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
#include "print.h"
#include "profiler.h"

#if DEBUG
# include <assert.h>
#endif

typedef struct sockaddr    sockaddr_t;
typedef struct sockaddr_in sockaddr_in_t;
typedef struct epoll_event epoll_event_t;

typedef struct
{
  int fd;
  int epfd;
  atomic_bool is_running;
  thrd_t accept_thread;
  sockaddr_in_t addr;
} Server;

typedef struct
{
  int fd;
  sockaddr_in_t addr;
  socklen_t addrlen;
  struct {
    u32 get_hit;
    u32 get_miss;
    u32 set;
    u32 del;
    u32 clr;
  } counters;
} Client;

typedef struct
{
  thrd_t thread;
  u32    id;
  int    epfd;
} Worker;

static Server server = { 0 };
static Client clients[MAX_NUM_CLIENTS] = {{ 0 }};
static size_t num_clients = 0;
static Worker workers[NUM_WORKERS] = {{ 0 }};
static thread_local Worker *current_worker = NULL;

static int        run_worker             (Worker *);
static int        run_accept_thread      (Server *);
static StatusCode read_request           (Client *, Request *);
static StatusCode read_request_payload   (Client *, u8 *, u32);
static StatusCode write_response         (Client *, Response *);
static StatusCode write_response_payload (Client *, u8 *, u32);
static void       close_client           (Client *);

int
start_server ()
{
  epoll_event_t event = { 0 };
  int enable_tcp_nodelay = 1;

  for (u32 i = 0; i < MAX_NUM_CLIENTS; ++i)
    clients[i].fd = -1;

  server.fd = socket (AF_INET, SOCK_STREAM, 0);
  if (server.fd < 0)
    return errno;

  if (0 > setsockopt (server.fd, IPPROTO_TCP, TCP_NODELAY, &enable_tcp_nodelay,
                      sizeof (enable_tcp_nodelay)))
    fprintf (stderr, "Could not enable TCP_NODELAY: %s\n", strerror (errno));

  server.addr.sin_family = AF_INET;
  server.addr.sin_addr.s_addr = INADDR_ANY;
  server.addr.sin_port = htons (SERVER_PORT);

  if (0 > bind (server.fd, (sockaddr_t *) &server.addr, sizeof (server.addr)))
    {
      close (server.fd);
      return errno;
    }

  if (0 > listen (server.fd, SERVER_BACKLOG))
    {
      close (server.fd);
      return errno;
    }

  server.epfd = epoll_create (1); // Size is actually ignored here
  if (0 > server.epfd)
    {
      close (server.fd);
      return errno;
    }

  event.events = EPOLLIN;
  event.data.ptr = &server;
  if (0 > epoll_ctl (server.epfd, EPOLL_CTL_ADD, server.fd, &event))
    {
      close (server.epfd);
      close (server.fd);
      return errno;
    }

  atomic_init (&server.is_running, true);

  for (u32 id = 0; id < NUM_WORKERS; ++id)
    {
      Worker *worker = &workers[id];
      worker->id = id;
      if (thrd_create (&worker->thread, (thrd_start_t) run_worker, worker) < 0)
        {
          fprintf (stderr, "%s:%d: %s\n", __FUNCTION__, __LINE__,
                   strerror (errno));
        }
    }

  if (thrd_create (&server.accept_thread,
                   (thrd_start_t) run_accept_thread,
                   &server) < 0)
    {
      fprintf (stderr, "%s:%d: %s\n", __FUNCTION__, __LINE__, strerror (errno));
    }

  return 0;
}

static int
wait_for_new_connection (Server *server)
{
  PROFILE (PROF_SERVER_ACCEPT);

  static thread_local u32 worker_id = 0;

  Worker *worker = NULL;
  Client *client = NULL;
  epoll_event_t event = { 0 };
  int nevents;

  nevents = epoll_wait (server->epfd, &event, 1, WORKER_EPOLL_TIMEOUT);
  if (nevents < 0)
    {
      fprintf (stderr, "%s: epoll_wait failed: %s\n",
               __FUNCTION__, strerror (errno));
      return -nevents;
    }
  else if (nevents == 0)
    {
      return 0;
    }
  else if (~event.events & EPOLLIN)
    {
      fprintf (stderr, "%s: Unexpected epoll event: 0x%X\n",
               __FUNCTION__, event.events);
      return 0;
    }
#if DEBUG
  assert (event.data.ptr == server); // Sanity check
#endif

  if (num_clients < MAX_NUM_CLIENTS)
    client = &clients[num_clients++];
  else
    {
      // @Incomplete: MT-safety!
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
      return EAGAIN;
    }

  client->addrlen = sizeof (client->addr);
  client->fd = accept (server->fd,
                       (sockaddr_t *) &client->addr,
                       &client->addrlen);
  if (client->fd < 0)
    return errno;

  memset (&client->counters, 0, sizeof (client->counters));

  worker = &workers[worker_id];

  event = (epoll_event_t) { 0 };
  event.events = EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP;
  event.data.ptr = client;
  if (0 > epoll_ctl (worker->epfd, EPOLL_CTL_ADD, client->fd, &event))
    {
      close_client (client);
      return errno;
    }

  worker_id = (worker_id + 1) % NUM_WORKERS; // Round robin

  return 0;
}

static int
run_accept_thread (Server *server)
{
  while (atomic_load (&server->is_running))
    wait_for_new_connection (server);

  return thrd_success;
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
read_tags_using_payload_buffer (Client *client, CacheTag *tags, u8 ntags)
{
  StatusCode status;
  u8        *buffer;
  u32        buffer_cap;

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
    {
#if DEBUG
      printf (RED ("GET") "[%X]: '%.*s'\n",
              current_worker->id, klen, tmp_key_data);
#endif
      ++client->counters.get_miss;
      return STATUS_NOT_FOUND;
    }

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

#if DEBUG
  printf (GREEN ("GET") "[%X]: '%.*s'\n",
          current_worker->id, klen, tmp_key_data);
#endif

  ++client->counters.get_hit;

  return STATUS_OK;
}

static StatusCode
handle_set_request (Client *client, Request *request)
{
  PROFILE (PROF_HANDLE_SET_REQUEST);

  StatusCode status;
  CacheEntry *entry = NULL, *old_entry = NULL;

  u8  klen  = request->s.klen;
  u32 tlen  = 0;
  u32 vlen  = request->s.vlen;
  u8  ntags = request->s.ntags;
  u32 ttl   = request->s.ttl;

  u8      *payload;
  u8       tmp_key_data[0xFF];
  size_t   total_size;
  CacheTag tags[ntags];

  // Read key
  status = read_request_payload (client, tmp_key_data, klen);
  if (status != STATUS_OK)
    return status;

  // @Incomplete: Use key here and look if we have an existing entry
  // in the hash table already. If so, reuse it's memory if possible.
  // Right now we're always reserving new memory and releasing the old.
  // But who knows, maybe reserving new memory will be faster in the end.

  status = read_tags_using_payload_buffer (client, tags, ntags);
  if (status != STATUS_OK)
    return status;

  tlen = sizeof (tags);
  for (u8 t = 0; t < ntags; ++t)
    tlen += tags[t].nmemb;

  total_size = tlen + klen + vlen;

  // @Speed: Both keys and tags tend to be prefixed and so in general they
  // should have more entropy at near end. If we store keys and tags in reverse
  // byte order we should help memcpy to exit early.

  entry = reserve_and_lock_entry (total_size);
  if (entry == NULL)
    return STATUS_OUT_OF_MEMORY;

  payload = (u8 *) (entry + 1);

  // Copy read tags into reserved entry payload
  entry->tags.base = (CacheTag *) payload;
  entry->tags.nmemb = ntags;
  payload += sizeof (tags);
  for (u8 t = 0; t < entry->tags.nmemb; ++t)
    {
      CacheTag *tag = &entry->tags.base[t];
      tag->base = payload;
      tag->nmemb = tags[t].nmemb;
      memcpy (tag->base, tags[t].base, tag->nmemb);
      payload += tag->nmemb;
    }

  // Copy read key into reserved entry payload
  memcpy (payload, tmp_key_data, klen);
  entry->key.base = payload;
  entry->key.nmemb = klen;
  payload += klen;
  entry->value.base = payload;
  entry->value.nmemb = vlen;
  payload += vlen;

#if DEBUG
  assert ((u32) (payload - (u8 *) (entry + 1)) == total_size);
#endif

  status = read_request_payload (client, entry->value.base, vlen);
  if (status != STATUS_OK)
    {
      UNLOCK_ENTRY (entry);
      release_memory (entry);
      return status;
    }

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
      // @Incomplete: Remove tag associations for old entry
      UNLOCK_ENTRY (old_entry);
      release_memory (old_entry);
    }

  for (u8 t = 0; t < entry->tags.nmemb; ++t)
    {
      // Maybe we don't need to keep the entry locked for this?
      add_key_to_tag (entry->tags.base[t], entry->key);
    }

  UNLOCK_ENTRY (entry);

#if DEBUG
  printf (BLUE ("SET") "[%X]: '%.*s'\n",
          current_worker->id, klen, tmp_key_data);
#endif

  ++client->counters.set;

  return STATUS_OK;
}

static StatusCode
delete_entry_by_key (CacheKey key)
{
  CacheEntry *entry = NULL;

#if DEBUG
  printf (YELLOW ("DEL") "[%X]: '%.*s'\n",
          current_worker->id, key.nmemb, key.base);
#endif

  // Unmap entry
  entry = lock_and_unset_cache_entry (entry_map, key);
  if (!entry)
    return STATUS_NOT_FOUND;

  for (u8 t = 0; t < entry->tags.nmemb; ++t)
    {
      // Maybe we don't need to keep the entry locked for this?
      remove_key_from_tag (entry->tags.base[t], entry->key);
    }

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

  ++client->counters.del;

  return delete_entry_by_key (key);
}

static bool
clear_all_callback (CacheEntry *entry, void *user_data)
{
  (void) user_data;

#if DEBUG
  assert (entry);
  printf (YELLOW ("DEL") "[%X]: '%.*s'\n",
          current_worker->id, entry->key.nmemb, entry->key.base);
#endif

  for (u8 t = 0; t < entry->tags.nmemb; ++t)
    remove_key_from_tag (entry->tags.base[t], entry->key);

  UNLOCK_ENTRY (entry);
  release_memory (entry);

  return true; // 'true' tells map to unset the entry
}

static bool
clear_old_callback (CacheEntry *entry, time_t *now)
{
#if DEBUG
  assert (entry);
  assert (now);
#endif

  if ((entry->expiry == CACHE_EXPIRY_INIT)
      || (entry->expiry >= *now))
    return false;

  return clear_all_callback (entry, NULL);
}

static bool
clear_non_matching_callback (CacheEntry *entry, CacheTagArray *tags)
{
#if DEBUG
  assert (entry);
  assert (tags);
#endif

  for (u8 i = 0; i < tags->nmemb; ++i)
    {
      CacheTag *want = &tags->base[i];
      for (u8 j = 0; j < entry->tags.nmemb; ++j)
        {
          CacheTag *have = &entry->tags.base[j];
          if ((want->nmemb == have->nmemb)
              && (memcmp (want->base, have->base, have->nmemb) == 0))
            {
              return false;
            }
        }
    }

  return clear_all_callback (entry, NULL);
}

static StatusCode
handle_clr_request (Client *client, Request *request)
{
  PROFILE (PROF_HANDLE_CLR_REQUEST);

  StatusCode status;
  ClearMode  mode  = (ClearMode) request->c.mode;
  u8         ntags = request->c.ntags;
  CacheTag   tags[ntags];

  status = read_tags_using_payload_buffer (client, tags, ntags);
  if (status != STATUS_OK)
    return status;

  ++client->counters.clr;

  switch (mode)
    {
    case CLEAR_MODE_ALL:
#if DEBUG
        printf (YELLOW ("CLR") "[%X]: (MATCH ALL)\n", current_worker->id);
#endif
      walk_entries (entry_map, clear_all_callback, NULL);
      return STATUS_OK;
    case CLEAR_MODE_OLD:
      {
        time_t now = time (NULL);
#if DEBUG
        printf (YELLOW ("CLR") "[%X]: (MATCH OLD)\n", current_worker->id);
#endif
        walk_entries (entry_map, (CacheEntryWalkCb) clear_old_callback, &now);
        return STATUS_OK;
      }
    case CLEAR_MODE_MATCH_NONE:
      {
        CacheTagArray tag_array = { .base = tags, .nmemb = ntags };
#if DEBUG
        printf (YELLOW ("CLR") "[%X]: (MATCH NONE)", current_worker->id);
        for (u8 t = 0; t < ntags; ++t)
          printf (" '%.*s'", tags[t].nmemb, tags[t].base);
        printf ("\n");
#endif
        walk_entries (entry_map,
                      (CacheEntryWalkCb) clear_non_matching_callback,
                      &tag_array);
        return STATUS_OK;
      }
    case CLEAR_MODE_MATCH_ALL: // Intentional fallthrough
    case CLEAR_MODE_MATCH_ANY:
      {
        KeyNode *keys = NULL;
#if DEBUG
        printf (YELLOW ("CLR") "[%X]: (MATCH %s)", current_worker->id,
                (mode == CLEAR_MODE_MATCH_ALL) ? "ALL" : "ANY");
        for (u8 t = 0; t < ntags; ++t)
          printf (" '%.*s'", tags[t].nmemb, tags[t].base);
        printf ("\n");
#endif
        keys = (mode == CLEAR_MODE_MATCH_ALL)
          ? get_keys_matching_all_tags (tags, ntags)
          : get_keys_matching_any_tag  (tags, ntags);

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

void
load_request_log (int fd)
{
  // This is assumed to be called from the main thread!

  Client     client;
  Worker     worker;
  Request    request;
  StatusCode status;

  client.fd = fd;
  worker.id = (u32) -1;
  current_worker = &worker;

  status = init_payload_buffer ();
#if DEBUG
  assert (status == STATUS_OK);
#else
  (void) status;
#endif

  while (0 != read (fd, &request, sizeof (request)))
    {
      Payload *ignored = NULL;
      status = handle_request (&client, &request, &ignored);
#if DEBUG
      assert (status == STATUS_OK);
#endif
    }

  release_memory (payload_buffer.base);
  payload_buffer_cap = 0;
  payload_buffer     = (Payload) { .base = NULL, .nmemb = 0 };
}

static int
process_worker_events (Worker *worker)
{
  PROFILE (PROF_SERVER_READ);

  epoll_event_t events[MAX_NUM_EVENTS] = {{ 0 }};
  int nevents = epoll_wait (worker->epfd, events,
                            MAX_NUM_EVENTS, WORKER_EPOLL_TIMEOUT);
  if (nevents < 0)
    {
      fprintf (stderr, "%s: epoll_wait failed: %s\n",
               __FUNCTION__, strerror (errno));
      return nevents;
    }

  for (int i = 0; i < nevents; ++i)
    {
      epoll_event_t *event = &events[i];

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
          Client    *client   = event->data.ptr;
          Request    request  = { 0 };
          Response   response = { 0 };
          Payload   *payload  = NULL;
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

static int
run_worker (Worker *worker)
{
  current_worker = worker;

  worker->epfd = epoll_create (MAX_NUM_CLIENTS); // Size is actually ignored here
  if (worker->epfd < 0)
    {
      fprintf (stderr, "%s:%d: %s\n", __FUNCTION__, __LINE__, strerror (errno));
      return thrd_error;
    }

  while (atomic_load (&server.is_running))
    process_worker_events (worker);

  close (worker->epfd);

  return thrd_success;
}

void
stop_server ()
{
  atomic_store (&server.is_running, false);

  if (0 > thrd_join (server.accept_thread, NULL))
    fprintf (stderr, "%s:%d: %s\n", __FUNCTION__, __LINE__, strerror (errno));

  for (u32 w = 0; w < NUM_WORKERS; ++w)
    {
      if (0 > thrd_join (workers[w].thread, NULL))
        fprintf (stderr, "%s:%d: %s\n", __FUNCTION__, __LINE__,
                 strerror (errno));
    }

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
debug_print_client (int fd, Client *client)
{
  int count = 0;

  count = dprintf (fd, "%d.%d.%d.%d:%d",
                   (client->addr.sin_addr.s_addr & 0x000000FF) >>  0,
                   (client->addr.sin_addr.s_addr & 0x0000FF00) >>  8,
                   (client->addr.sin_addr.s_addr & 0x00FF0000) >> 16,
                   (client->addr.sin_addr.s_addr & 0xFF000000) >> 24,
                   client->addr.sin_port);

  dprintf (fd, "%.*s", 29 - count, BLANKSTR);
  dprintf (fd, "%6d", client->fd);
  dprintf (fd, GREEN  ("      %6u"),  client->counters.get_hit);
  dprintf (fd, RED    ("       %6u"), client->counters.get_miss);
  dprintf (fd, BLUE   ("%6u"),        client->counters.set);
  dprintf (fd, YELLOW ("%6u"),        client->counters.del);
  dprintf (fd, YELLOW ("%6u"),        client->counters.clr);

  dprintf (fd, "\n");
}

void
debug_print_clients (int fd)
{
  int count = 0;

  for (u32 i = 0; i < MAX_NUM_CLIENTS; ++i)
    {
      if (clients[i].fd != -1)
        ++count;
    }

  count = dprintf (fd, "CLIENTS (%d) ", count);
  dprintf (fd, "%.*s\n", LINEWIDTH - count, HLINESTR);

  dprintf (fd, "%s",   "HOST");
  dprintf (fd, "%31s", "FD");
  dprintf (fd, "%12s", "GET (HIT)");
  dprintf (fd, "%13s", "GET (MISS)");
  dprintf (fd, "%6s", "SET");
  dprintf (fd, "%6s", "DEL");
  dprintf (fd, "%6s", "CLR");
  dprintf (fd, "\n");

  for (u32 i = 0; i < MAX_NUM_CLIENTS; ++i)
    {
      if (clients[i].fd != -1)
        debug_print_client (fd, &clients[i]);
    }

  dprintf (fd, "\n");
}
