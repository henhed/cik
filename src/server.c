#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "server.h"
#include "memory.h"
#include "entry.h"
#include "profiler.h"

#if DEBUG
# include <assert.h>
#endif

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
  CacheEntry *response_entry; // @Temporary! We need output buffer per client
} Client;

typedef enum
{
  // 0x00 Series: Generates a sucess response
  STATUS_OK                     = 0x00,

  // 0x10 Series: Errors that close the client connection
  MASK_INTERNAL_ERROR           = 0x10,
  STATUS_BUG                    = 0x11,
  STATUS_CONNECTION_CLOSED      = 0x12,
  STATUS_NETWORK_ERROR          = 0x13,

  // 0x20 Series: Errors that generate a failure response
  MASK_CLIENT_ERROR             = 0x20,
  STATUS_PROTOCOL_ERROR         = 0x21,

  // 0x40 Series: Non-errors that generate a failure response
  MASK_CLIENT_MESSAGE           = 0x40,
  STATUS_NOT_FOUND              = 0x41,
  STATUS_EXPIRED                = 0x42,
  STATUS_OUT_OF_MEMORY          = 0x43
} StatusCode;

static const char *status_code_names[] = {
  [STATUS_OK]                = "OK",
  [STATUS_BUG]               = "BUG!",
  [STATUS_CONNECTION_CLOSED] = "Connection closed",
  [STATUS_NETWORK_ERROR]     = "Network error",
  [STATUS_PROTOCOL_ERROR]    = "Protocol error",
  [STATUS_NOT_FOUND]         = "Not found",
  [STATUS_OUT_OF_MEMORY]     = "Out of memory"
};

static Server server = { 0 };
static Client clients[MAX_NUM_CLIENTS] = {{ 0 }};
static size_t num_clients = 0;

static StatusCode read_request           (Client *, Request *);
static StatusCode read_request_payload   (Client *, u8 *, u32);
static StatusCode write_response         (Client *, Response *);
static StatusCode write_response_payload (Client *, u8 *, u32);
static void       close_client           (Client *);
static void       debug_print_client     (Client *);

int
start_server ()
{
  for (u32 i = 0; i < MAX_NUM_CLIENTS; ++i)
    clients[i].fd = -1;

  server.fd = socket (AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (server.fd < 0)
    return errno;

  int on = 1;
  if (0 > setsockopt (server.fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof (on)))
    printf ("Could not enable TCP_NODELAY: %s\n", strerror (errno));

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
  PROFILE (PROF_SERVER_ACCEPT);

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

static StatusCode
handle_get_request (Client *client, Request *request)
{
  PROFILE (PROF_HANDLE_GET_REQUEST);

  CacheEntry *entry = NULL;

  u8 klen  = request->g.klen;
  u8 flags = request->g.flags;

  CacheKey key;
  ssize_t nread;
  u8 tmp_key_data[0xFF];

  // Read key
  nread = read (client->fd, tmp_key_data, klen);
  if (nread < 0)
    return STATUS_NETWORK_ERROR;
  if (nread != klen)
    return STATUS_NETWORK_ERROR;

  key.base = tmp_key_data;
  key.nmemb = klen;

  entry = lock_and_get_cache_entry (entry_map, key);
  if (!entry)
    return STATUS_NOT_FOUND;

  if (~flags & GET_FLAG_IGNORE_EXPIRY)
    {
      if (entry->expiry != CACHE_EXPIRY_INIT)
        {
          time_t now = time (NULL);
          if (entry->expiry < now)
            {
              UNLOCK_ENTRY (entry);
              return STATUS_EXPIRED;
            }
        }
    }

  // @Temporary! Don't know what to do about outbut buffer allocation yet.
  // We should copy the entry value here, store it with the reponse and unlock the entry.
  client->response_entry = entry;

  UNLOCK_ENTRY (entry);

  return STATUS_OK;
}

static StatusCode
handle_set_request (Client *client, Request *request)
{
  PROFILE (PROF_HANDLE_SET_REQUEST);

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
    return STATUS_NETWORK_ERROR;
  if (nread != klen)
    return STATUS_NETWORK_ERROR;
  remaining_size -= nread;

  // @Incomplete: Use key here and look if we have an existing entry
  // in the hash table already. If so, reuse it's memory if possible.
  // Right now we're always reserving new memory and releasing the old.

  entry = reserve_and_lock (total_size);
  if (entry == NULL)
    return STATUS_OUT_OF_MEMORY;

  payload = (u8 *) (entry + 1);

  // Copy read key into reserved entry payload
  memcpy (payload, tmp_key_data, klen);
  entry->key.base = payload;
  entry->key.nmemb = klen;
  payload += klen;

  do
    {
      u8 *buffer = payload + (total_size - klen) - remaining_size;
      nread = read (client->fd, buffer, remaining_size);
      if (nread < 0)
        {
          UNLOCK_ENTRY (entry);
          return STATUS_NETWORK_ERROR;
        }
      remaining_size -= nread;
    }
  while (remaining_size > 0);

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

  return STATUS_OK;
}

static StatusCode
handle_request (Client *client, Request *request)
{
  PROFILE (PROF_HANDLE_REQUEST);

  if (!client || !request)
    return STATUS_BUG;

  if ((request->cik[0] != CONTROL_BYTE_1)
      || (request->cik[1] != CONTROL_BYTE_2)
      || (request->cik[2] != CONTROL_BYTE_3))
    return STATUS_PROTOCOL_ERROR;

  switch (request->op)
    {
    case CMD_BYTE_GET:
      return handle_get_request (client, request);
    case CMD_BYTE_SET:
      request->s.vlen = ntohl (request->s.vlen);
      request->s.ttl  = ntohl (request->s.ttl);
      return handle_set_request (client, request);
    default:
      return STATUS_PROTOCOL_ERROR;
    }
}

int
server_read ()
{
  PROFILE (PROF_SERVER_READ);

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
      if ((event->events & (EPOLLIN | EPOLLOUT)) == (EPOLLIN | EPOLLOUT))
        {
          Client *client = event->data.ptr;
          Request request = { 0 };
          Response response = { 0 };
          StatusCode status;

          errno = 0;
          status = read_request (client, &request);
          if (status & MASK_INTERNAL_ERROR)
            {
              fprintf (stderr, "%s: (FD %d) %s [%s]\n", __FUNCTION__,
                       client->fd, status_code_names[status], strerror (errno));
              close_client (client);
              continue;
            }

          errno = 0;
          status = handle_request (client, &request);
          if (status & MASK_INTERNAL_ERROR)
            {
              fprintf (stderr, "%s: (FD %d) %s [%s]\n", __FUNCTION__,
                       client->fd, status_code_names[status], strerror (errno));
              close_client (client);
              continue;
            }
          if (status & (MASK_CLIENT_ERROR | MASK_CLIENT_MESSAGE))
            {
              response = MAKE_FAILURE_RESPONSE (status);
            }
          else
            {
#if DEBUG
              assert (status == STATUS_OK);
#endif
              u32 size = 0;
              if (client->response_entry != NULL)
                size = client->response_entry->value.nmemb; // @Temporary!!
              response = MAKE_SUCCESS_RESPONSE (size);
            }

          errno = 0;
          status = write_response (client, &response);
          if (status & MASK_INTERNAL_ERROR)
            {
              fprintf (stderr, "%s: (FD %d) %s [%s]\n", __FUNCTION__,
                       client->fd, status_code_names[status], strerror (errno));
              close_client (client);
              continue;
            }

          // @Temporary!!
          if (client->response_entry)
            {
              CacheEntry *entry = client->response_entry;
              status = write_response_payload (client,
                                               entry->value.base,
                                               entry->value.nmemb);
              client->response_entry = NULL;
            }
        }

      if (event->events & (EPOLLERR | EPOLLHUP))
        {
          Client *client = event->data.ptr;
          close_client (client);
#if DEBUG
          fprintf (stderr, "%s: Got error event: 0x%X\n",
                   __FUNCTION__, event->events);
#endif
          continue;
        }
    }

  return nevents;
}

void
stop_server ()
{
  for (u32 i = 0; i < MAX_NUM_CLIENTS; ++i)
    close_client (&clients[i]);
  close (server.epfd);
  close (server.fd);
}

static StatusCode
read_request (Client *client, Request *request)
{
  return read_request_payload (client, (u8 *) request, sizeof (Request));
}

static StatusCode
read_request_payload (Client *client, u8 *base, u32 nmemb)
{
  ssize_t nread = 0, remaining_size = nmemb;
#if DEBUG
  assert (client);
  assert (base);
#endif

  do
    {
      nread = read (client->fd, base, remaining_size);
      if (nread < 0)
        return STATUS_NETWORK_ERROR;
      if (nread == 0)
        return STATUS_CONNECTION_CLOSED;
      base += nread;
      remaining_size -= nread;
    }
  while (remaining_size > 0);

  return STATUS_OK;
}

static StatusCode
write_response (Client *client, Response *response)
{
  return write_response_payload (client, (u8 *) response, sizeof (Response));
}

static StatusCode
write_response_payload (Client *client, u8 *base, u32 nmemb)
{
  ssize_t nsent = 0, remaining_size = nmemb;
#if DEBUG
  assert (client);
  assert (base);
#endif

  do
    {
      nsent = send (client->fd, base, remaining_size, MSG_NOSIGNAL);
      if (nsent < 0)
        return STATUS_NETWORK_ERROR;
      if (nsent == 0)
        return STATUS_CONNECTION_CLOSED;
      base += nsent;
      remaining_size -= nsent;
    }
  while (remaining_size > 0);

  return STATUS_OK;
}

static void
close_client (Client *client)
{
  PROFILE (PROF_CLOSE_CLIENT);

  if (!client || (client->fd < 0))
    return;
  printf ("%s: ", __FUNCTION__);
  debug_print_client (client);
  close (client->fd);
  client->fd = -1;
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
