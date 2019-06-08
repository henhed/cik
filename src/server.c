#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <threads.h>
#include <unistd.h>

#include "server.h"
#include "memory.h"
#include "entry.h"
#include "tag.h"
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
} Client;

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
    fprintf (stderr, "Could not enable TCP_NODELAY: %s\n", strerror (errno));

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

  /* debug_print_client (client); */

  return 0;
}

static thread_local u32     payload_buffer_cap = 0;
static thread_local Payload payload_buffer     = { .base = NULL, .nmemb = 0 };

static StatusCode
init_payload_buffer ()
{
  if (payload_buffer.base == NULL)
    {
      if (!reserve_biggest_possible_payload (&payload_buffer))
        return STATUS_OUT_OF_MEMORY;
      // We may overwrite nmemb later so we save initial value here
      payload_buffer_cap = payload_buffer.nmemb;
    }
  return STATUS_OK;
}

static StatusCode
handle_get_request (Client *client, Request *request, Payload **response_payload)
{
  PROFILE (PROF_HANDLE_GET_REQUEST);

  StatusCode status;
  CacheEntry *entry = NULL;

  u8 klen  = request->g.klen;
  u8 flags = request->g.flags;

  CacheKey key;
  u8 tmp_key_data[0xFF];

  status = init_payload_buffer ();
  if (status != STATUS_OK)
    return status;

  // Read key
  status = read_request_payload (client, tmp_key_data, klen);
  if (status != STATUS_OK)
    return status;

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

  if (entry->value.nmemb > payload_buffer_cap)
    return STATUS_BUG; // We should always have a buffer big enough

  if (entry->value.nmemb > 0)
    {
      // We copy the entry value to a payload buffer so we don't have to keep
      // the entity locked while writing it's data to the client.
      payload_buffer.nmemb = entry->value.nmemb;
      memcpy (payload_buffer.base, entry->value.base, payload_buffer.nmemb);
      *response_payload = &payload_buffer;
    }

  UNLOCK_ENTRY (entry);

  return STATUS_OK;
}

static StatusCode
handle_set_request (Client *client, Request *request)
{
  PROFILE (PROF_HANDLE_SET_REQUEST);

  StatusCode status;
  CacheEntry *entry = NULL, *old_entry = NULL;

  u8  klen  = request->s.klen;
  u8  tlen0 = request->s.tlen[0];
  u8  tlen1 = request->s.tlen[1];
  u8  tlen2 = request->s.tlen[2];
  u32 vlen  = request->s.vlen;
  u32 ttl   = request->s.ttl;

  u8     *payload;
  u8      tmp_key_data[0xFF];
  size_t  total_size = klen + tlen0 + tlen1 + tlen2 + vlen;

  // Read key
  status = read_request_payload (client, tmp_key_data, klen);
  if (status != STATUS_OK)
    return status;

  // @Incomplete: Use key here and look if we have an existing entry
  // in the hash table already. If so, reuse it's memory if possible.
  // Right now we're always reserving new memory and releasing the old.
  // But who knows, maybe reserving new memory will be faster in the end.

  // @Speed: Both keys and tags tend to be prefixed and so in general they
  // should have more entropy at near end. If we store keys and tags in reverse
  // byte order we should help memcpy to exit early.

  entry = reserve_and_lock_entry (total_size);
  if (entry == NULL)
    return STATUS_OUT_OF_MEMORY;

  payload = (u8 *) (entry + 1);

  // Copy read key into reserved entry payload
  memcpy (payload, tmp_key_data, klen);
  entry->key.base = payload;
  entry->key.nmemb = klen;
  payload += klen;

  status = read_request_payload (client, payload, total_size - klen);
  if (status != STATUS_OK)
    {
      UNLOCK_ENTRY (entry);
      release_memory (entry);
      return status;
    }

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

  if (!set_locked_cache_entry (entry_map, entry, &old_entry))
    {
#if DEBUG
      assert (old_entry == NULL);
      fprintf (stderr, "%s: TODO: Evict something (%.*s)\n", __FUNCTION__,
               entry->key.nmemb, entry->key.base);
#endif
      UNLOCK_ENTRY (entry);
      release_memory (entry);
      return STATUS_OUT_OF_MEMORY;
    }

  if (old_entry)
    {
      UNLOCK_ENTRY (old_entry);
      release_memory (old_entry);
    }

#if DEBUG
  assert (MAX_NUM_TAGS_PER_ENTRY == 3);
#endif
  for (u8 i = 0; i < MAX_NUM_TAGS_PER_ENTRY; ++i)
    {
      if (entry->tags[i].nmemb > 0)
        associate_key_with_tag (entry->tags[i], entry->key);
      else
        break;
    }

  UNLOCK_ENTRY (entry);

  return STATUS_OK;
}

static StatusCode
delete_entry_by_key (CacheKey key)
{
  CacheEntry *entry = NULL;

#if DEBUG
  fprintf (stderr, "Deleting entry '%.*s'\n", key.nmemb, key.base);
#endif

  // Unmap entry
  entry = lock_and_unset_cache_entry (entry_map, key);
  if (!entry)
    return STATUS_NOT_FOUND;

  // @Incomplete: Remove tag associations

  do
    {
      // Release memory. We loop untill we get NULL back from map. See note
      // about @Bug in `set_locked_cache_entry'.
      UNLOCK_ENTRY (entry);
      release_memory (entry);
      entry = lock_and_unset_cache_entry (entry_map, key);
    }
  while (entry != NULL);

  return STATUS_OK;
}

static StatusCode
handle_del_request (Client *client, Request *request)
{
  PROFILE (PROF_HANDLE_DEL_REQUEST);

  StatusCode status;
  u8 tmp_key_data[0xFF];
  CacheKey key = {
    .base = tmp_key_data,
    .nmemb = request->d.klen
  };

  // Read key
  status = read_request_payload (client, key.base, key.nmemb);
  if (status != STATUS_OK)
    return status;

  return delete_entry_by_key (key);
}

static StatusCode
handle_clr_request (Client *client, Request *request)
{
  PROFILE (PROF_HANDLE_CLR_REQUEST);

  StatusCode status;
  ClearMode mode = (ClearMode) request->c.mode;
  u8 ntags = request->c.ntags;
  CacheTag tags[ntags];
  u8 *buffer;
  u32 buffer_cap;

  status = init_payload_buffer ();
  if (status != STATUS_OK)
    return status;

  buffer = payload_buffer.base;
  buffer_cap = payload_buffer_cap;

  for (u8 t = 0; t < ntags; ++t)
    {
      CacheTag *tag = &tags[t];
      status = read_request_payload (client, &tag->nmemb, sizeof (tag->nmemb));
      if (status != STATUS_OK)
        return status;
      if (tag->nmemb > buffer_cap)
        return STATUS_OUT_OF_MEMORY; // @Cleanup: Drain input stream
      status = read_request_payload (client, buffer, tag->nmemb);
      if (status != STATUS_OK)
        return status;
      tag->base = buffer;
      buffer += tag->nmemb;
      buffer_cap -= tag->nmemb;
    }

  switch (mode)
    {
    case CLEAR_MODE_MATCH_NONE: // Is there a good use case for this? (fallthrough)
    case CLEAR_MODE_ALL:
      return STATUS_BUG; // Not implemented
    case CLEAR_MODE_OLD:
      return STATUS_BUG; // Not implemented
    case CLEAR_MODE_MATCH_ALL: // Intentional fallthrough
    case CLEAR_MODE_MATCH_ANY:
      {
        KeyNode *keys = NULL;

        keys = (mode == CLEAR_MODE_MATCH_ALL)
          ? get_keys_matching_all_tags (tags, ntags)
          : get_keys_matching_any_tag  (tags, ntags);
#if DEBUG
        fprintf (stderr, "Clearing tags (mode %s):\n",
                 (mode == CLEAR_MODE_MATCH_ALL) ? "ALL" : "ANY");
        for (u8 t = 0; t < ntags; ++t)
          fprintf (stderr, "\t- '%.*s'\n", tags[t].nmemb, tags[t].base);
#endif
        for (KeyNode **key = &keys; *key; key = &(*key)->next)
          delete_entry_by_key ((*key)->key);
        release_key_list (keys);

        return STATUS_OK;
      }
    default:
      return STATUS_PROTOCOL_ERROR;
    }

  return STATUS_OK;
}

static StatusCode
handle_request (Client *client, Request *request, Payload **response_payload)
{
  PROFILE (PROF_HANDLE_REQUEST);

  if (!client || !request || !response_payload)
    return STATUS_BUG;

  if ((request->cik[0] != CONTROL_BYTE_1)
      || (request->cik[1] != CONTROL_BYTE_2)
      || (request->cik[2] != CONTROL_BYTE_3))
    return STATUS_PROTOCOL_ERROR;

  *response_payload = NULL;

  switch (request->op)
    {
    case CMD_BYTE_GET:
      return handle_get_request (client, request, response_payload);
    case CMD_BYTE_SET:
      request->s.vlen = ntohl (request->s.vlen);
      request->s.ttl  = ntohl (request->s.ttl);
      return handle_set_request (client, request);
    case CMD_BYTE_DEL:
      return handle_del_request (client, request);
    case CMD_BYTE_CLR:
      return handle_clr_request (client, request);
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

      if ((event->events & (EPOLLIN | EPOLLOUT)) == (EPOLLIN | EPOLLOUT))
        {
          Client *client = event->data.ptr;
          Request request = { 0 };
          Response response = { 0 };
          Payload *payload = NULL;
          StatusCode status;

          errno = 0;
          status = read_request (client, &request);
          if (status & MASK_INTERNAL_ERROR)
            {
              if (status != STATUS_CONNECTION_CLOSED)
                {
                  fprintf (stderr, "%s:%d (FD %d) %s [%s]\n", __FUNCTION__, __LINE__,
                           client->fd, get_status_code_name (status),
                           strerror (errno));
                }
              close_client (client);
              continue;
            }

          errno = 0;
          status = handle_request (client, &request, &payload);
          if (status & MASK_INTERNAL_ERROR)
            {
              fprintf (stderr, "%s:%d (FD %d) %s [%s]\n", __FUNCTION__, __LINE__,
                       client->fd, get_status_code_name (status), strerror (errno));
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
              u32 size = (payload == NULL) ? 0 : payload->nmemb;
              response = MAKE_SUCCESS_RESPONSE (size);
            }

          errno = 0;
          status = write_response (client, &response);
          if (status & MASK_INTERNAL_ERROR)
            {
              fprintf (stderr, "%s:%d (FD %d) %s [%s]\n", __FUNCTION__, __LINE__,
                       client->fd, get_status_code_name (status), strerror (errno));
              close_client (client);
              continue;
            }

          if ((payload != NULL) && (payload->nmemb > 0))
            {
              status = write_response_payload (client, payload->base, payload->nmemb);
              if (status & MASK_INTERNAL_ERROR)
                {
                  fprintf (stderr, "%s:%d (FD %d) %s [%s]\n", __FUNCTION__, __LINE__,
                           client->fd, get_status_code_name (status), strerror (errno));
                  close_client (client);
                  continue;
                }
            }
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

  if (nmemb == 0)
    return STATUS_OK;

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

  if (nmemb == 0)
    return STATUS_OK;

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
