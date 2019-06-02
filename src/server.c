#include <errno.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#include "server.h"
#include "memory.h"
#include "entry.h"

typedef struct
{
  int fd;
  struct sockaddr_in addr;
  int epfd;
} Server;

typedef struct
{
  int fd;
  struct sockaddr_in addr;
  socklen_t addrlen;
  Response response;
  CacheEntry *response_entry; // @Temporary! We need output buffer per client
} Client;

static Server server = { 0 };
static Client clients[MAX_NUM_CLIENTS] = {{ 0 }};
static size_t num_clients = 0;

static void close_client (Client *);
static void debug_print_client (Client *);

int
start_server ()
{
  server.fd = socket (AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (server.fd < 0)
    return errno;

  server.epfd = epoll_create (MAX_NUM_CLIENTS); // Size is actually ignored here
  if (server.epfd < 0)
    {
      close (server.fd);
      return errno;
    }

  server.addr.sin_family = AF_INET;
  server.addr.sin_addr.s_addr = INADDR_ANY;
  server.addr.sin_port = htons (SERVER_PORT);

  if (0 > bind (server.fd, (struct sockaddr *) &server.addr, sizeof (server.addr)))
    {
      close (server.fd);
      close (server.epfd);
      return errno;
    }

  if (0 > listen (server.fd, SERVER_BACKLOG))
    {
      close (server.fd);
      close (server.epfd);
      return errno;
    }

  return 0;
}

int
server_accept ()
{
  Client *client = NULL;
  struct epoll_event event = { 0 };

  if (num_clients < MAX_NUM_CLIENTS)
    client = &clients[num_clients++];
  else
    {
      for (u32 i = 0; i < MAX_NUM_CLIENTS; ++i)
        {
          int fd = clients[i].fd;
          if (fd == -1)
            client = &clients[i];
        }
    }

  if (client == NULL)
    {
      fprintf (stderr, "%s: We're full\n", __FUNCTION__);
      return -EAGAIN;
    }

  client->addrlen = sizeof (client->addr);
  client->fd = accept (server.fd,
                       (struct sockaddr *) &client->addr,
                       &client->addrlen);
  if (client->fd < 0)
    return errno;

  event.events = EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP;
  event.data.ptr = client;
  if (0 > epoll_ctl (server.epfd, EPOLL_CTL_ADD, client->fd, &event))
    {
      close_client (client);
      return errno;
    }

  debug_print_client (client);

  return 0;
}

static int
handle_get_request (Client *client, Request *request)
{
  CacheEntry *entry = NULL;

  u8 klen  = request->g.klen;
  u8 flags = request->g.flags;

  CacheKey key;
  ssize_t nread;
  u8 tmp_key_data[0xFF];

  // Read key
  nread = read (client->fd, tmp_key_data, klen);
  if (nread < 0)
    return errno;
  if (nread != klen)
    return EINVAL;

  key.base = tmp_key_data;
  key.nmemb = klen;

  entry = lock_and_get_cache_entry (entry_map, key);
  if (!entry)
    return ENODATA;

  if (~flags & GET_FLAG_IGNORE_EXPIRY)
    {
      if (entry->expiry != CACHE_EXPIRY_INIT)
        {
          time_t now = time (NULL);
          if (entry->expiry < now)
            {
              UNLOCK_ENTRY (entry);
              return ENODATA; // @Revisit: Use different code for expired entry?
            }
        }
    }

  // @Temporary! Don't know what to do about outbut buffer allocation yet.
  // We should copy the entry value here, store it with the reponse and unlock the entry.
  client->response_entry = entry;

  UNLOCK_ENTRY (entry);

  return 0;
}

static int
handle_set_request (Client *client, Request *request)
{
  // @Incomplete: Read incoming payload from fd in worker thread?

  CacheEntry *entry = NULL, *old_entry = NULL;

  u8  klen  = request->s.klen;
  u8  tlen0 = request->s.tlen[0];
  u8  tlen1 = request->s.tlen[1];
  u8  tlen2 = request->s.tlen[2];
  u32 vlen  = request->s.vlen;
  u32 ttl   = request->s.ttl;

  ssize_t nread;
  u8     *payload;
  u8      tmp_key_data[0xFF];
  size_t  total_size = klen + tlen0 + tlen1 + tlen2 + vlen;
  size_t  remaining_size = total_size;

  // Read key
  nread = read (client->fd, tmp_key_data, klen);
  if (nread < 0)
    return errno;
  if (nread != klen)
    return EINVAL;
  remaining_size -= nread;

  // @Incomplete: Use key here and look if we have an existing entry
  // in the hash table already. If so, reuse it's memory if possible.
  // Right now we're always reserving new memory and releasing the old.

  entry = reserve_and_lock (total_size);
  if (entry == NULL)
    return ENOMEM;

  payload = (u8 *) (entry + 1);

  // Copy read key into reserved entry payload
  memcpy (payload, tmp_key_data, klen);
  entry->key.base = payload;
  entry->key.nmemb = klen;
  payload += klen;

  nread = read (client->fd, payload, remaining_size);
  if (nread < 0)
    {
      UNLOCK_ENTRY (entry);
      return errno;
    }
  if ((size_t) nread != remaining_size)
    {
      UNLOCK_ENTRY (entry);
      fprintf (stderr, "%s: Read %ld bytes, but expected %lu\n",
               __FUNCTION__, nread, remaining_size);
      return EINVAL;
    }
  remaining_size -= nread;

  entry->tags[0].base  = entry->key.base + klen;
  entry->tags[0].nmemb = tlen0;
  entry->tags[1].base  = entry->tags[0].base + tlen0;
  entry->tags[1].nmemb = tlen1;
  entry->tags[2].base  = entry->tags[1].base + tlen1;
  entry->tags[2].nmemb = tlen2;
  entry->value.base    = entry->tags[2].base + tlen2;
  entry->value.nmemb   = vlen;

  if (ttl != (u32) -1)
    entry->expiry = time (NULL) + ttl;

  set_locked_cache_entry (entry_map, entry, &old_entry);

  if (old_entry)
    {
      // @Revisit: Should we tell memory explicitly that this entry is reusable?
      old_entry->expiry = 0;
      UNLOCK_ENTRY (old_entry);
    }

  UNLOCK_ENTRY (entry);

  return 0;
}

static int
handle_request (Client *client, Request *request)
{
  if (!client || !request)
    return EINVAL;

  if ((request->cik[0] != CONTROL_BYTE_1)
      || (request->cik[1] != CONTROL_BYTE_2)
      || (request->cik[2] != CONTROL_BYTE_3))
    return EINVAL;

  switch (request->op)
    {
    case CMD_BYTE_GET:
      return handle_get_request (client, request);
    case CMD_BYTE_SET:
      request->s.vlen = ntohl (request->s.vlen);
      request->s.ttl  = ntohl (request->s.ttl);
      return handle_set_request (client, request);
    default:
      return ENOSYS;
    }
}

int
server_read ()
{
  int nrequests = 0;
  struct epoll_event events[MAX_NUM_EVENTS] = {{ 0 }};
  int nevents = epoll_wait (server.epfd, events, MAX_NUM_EVENTS, 0);
  if (nevents < 0)
    {
      fprintf (stderr, "%s: epoll_wait failed: %s\n",
               __FUNCTION__, strerror (errno));
      return nevents;
    }

  for (int i = 0; i < nevents; ++i)
    {
      struct epoll_event *event = &events[i];
      if (event->events & EPOLLIN)
        {
          Client *client = event->data.ptr;
          Request request = { 0 };
          ssize_t nread;
          int err;

          if (client->response.cik[0] != 0x00)
            continue; // This client has a pending response

          nread = read (client->fd, &request, sizeof (request));
          if (nread == 0)
            {
              fprintf (stderr, "%s: Connection closed (FD %d)\n",
                       __FUNCTION__, client->fd);
              // @Revisit: Thread safety. What happens if we have a thread
              // working on a response? Also, how to respond to client?
              close_client (client);
              continue;
            }
          else if (nread != sizeof (request))
            {
              fprintf (stderr, "%s: Invalid read (FD %d). Expected %lu, got %ld\n",
                       __FUNCTION__, client->fd, sizeof (request), nread);
              // @Revisit: Thread safety. What happens if we have a thread
              // working on a response? Also, how to respond to client?
              close_client (client);
              continue;
            }
          else if (0 != (err = handle_request (client, &request)))
            {
              u32 error_code = err < 0 ? -err : err;
              client->response.cik[0] = CONTROL_BYTE_1;
              client->response.cik[1] = CONTROL_BYTE_2;
              client->response.cik[2] = CONTROL_BYTE_3;
              client->response.status = FAILURE_BYTE;
              client->response.error_code = htonl (error_code);
              ++nrequests;
            }
          else
            {
              client->response.cik[0] = CONTROL_BYTE_1;
              client->response.cik[1] = CONTROL_BYTE_2;
              client->response.cik[2] = CONTROL_BYTE_3;
              client->response.status = SUCCESS_BYTE;
              if (client->response_entry != NULL)
                {
                  // @Temporary!!
                  CacheEntry *entry = client->response_entry;
                  client->response.payload_size = htonl (entry->value.nmemb);
                }
              else
                {
                  client->response.payload_size = htonl (0);
                }
              ++nrequests;
            }
        }

      if (event->events & EPOLLOUT)
        {
          Client *client = event->data.ptr;
          ssize_t nwritten;

          // @Incomplete: Response must have variable length.

          if (client->response.cik[0] == 0x00)
            continue; // This client has no pending response

          nwritten = write (client->fd, &client->response, sizeof (client->response));
          client->response.cik[0] = 0x00; // Invalidate response to accept new reads

          if (nwritten < 0)
            {
              fprintf (stderr, "%s: Failed to write to FD %d: %s\n",
                       __FUNCTION__, client->fd, strerror (errno));
              close_client (client);
              continue;
            }
          if (nwritten != sizeof (client->response))
            {
              fprintf (stderr, "%s: Wrote %ld but %lu was expected IF (%d)\n",
                       __FUNCTION__, nwritten, sizeof (client->response), client->fd);
              close_client (client);
              continue;
            }

          // @Temporary!!
          if (client->response_entry)
            {
              CacheEntry *entry = client->response_entry;
              nwritten = write (client->fd, entry->value.base, entry->value.nmemb);
              client->response_entry = NULL;
            }
        }

      if (event->events & (EPOLLERR | EPOLLHUP))
        {
          // @Revisit: Thread safety. What happens if we have a thread
          // working on a response? Also, how to respond to client?
          Client *client = event->data.ptr;
          close_client (client);
          fprintf (stderr, "%s: Got error event: 0x%X\n",
                   __FUNCTION__, event->events);
          continue;
        }
    }

  return nrequests;
}

void
stop_server ()
{
  for (u32 i = 0; i < MAX_NUM_CLIENTS; ++i)
    close_client (&clients[i]);
  close (server.epfd);
  close (server.fd);
}

static void
close_client (Client *client)
{
  if (!client || (client->fd < 0))
    return;
  printf ("%s: ", __FUNCTION__);
  debug_print_client (client);
  close (client->fd);
  client->fd = -1;
  client->response.cik[0] = 0x00; // Invalidate response to accept new reads
}

static void
debug_print_client (Client *client)
{
  printf ("[Client] %d.%d.%d.%d:%d (FD %d)\n",
          (client->addr.sin_addr.s_addr & 0x000000FF) >>  0,
          (client->addr.sin_addr.s_addr & 0x0000FF00) >>  8,
          (client->addr.sin_addr.s_addr & 0x00FF0000) >> 16,
          (client->addr.sin_addr.s_addr & 0xFF000000) >> 24,
          client->addr.sin_port,
          client->fd);
}

void
debug_print_clients ()
{
  for (u32 i = 0; i < MAX_NUM_CLIENTS; ++i)
    debug_print_client (&clients[i]);
}
