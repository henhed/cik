#include <errno.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#if DEBUG
# include <assert.h>
#endif

#include "server.h"
#include "controller.h"
#include "memory.h"
#include "print.h"
#include "profiler.h"

static Server server = { 0 };
static Client clients[MAX_NUM_CLIENTS] = {{ 0 }};
static size_t num_clients = 0;
static Worker workers[NUM_WORKERS] = {{ 0 }};

static int run_worker        (Worker *);
static int run_accept_thread (Server *);

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
      reserve_biggest_possible_payload (&worker->payload_buffer);
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

  client->worker = &workers[worker_id];

  event = (epoll_event_t) { 0 };
  event.events = EPOLLIN | EPOLLERR | EPOLLHUP;
  event.data.ptr = client;
  if (0 > epoll_ctl (client->worker->epfd, EPOLL_CTL_ADD, client->fd, &event))
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

void
load_request_log (int fd)
{
  // This is assumed to be called from the main thread!

  Client     client;
  Worker     worker;
  Request    request;
  StatusCode status;

  client.fd = fd;
  client.worker = &worker;
  worker.id = (u32) -1;
  reserve_biggest_possible_payload (&worker.payload_buffer);

  while (0 != read (fd, &request, sizeof (request)))
    {
      Payload *ignored = NULL;
      status = handle_request (&client, &request, &ignored);
#if DEBUG
      assert (status == STATUS_OK);
#else
      (void) status;
#endif
    }

  release_memory (worker.payload_buffer.base);
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

      if (event->events & EPOLLIN)
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
  worker->epfd = epoll_create (MAX_NUM_CLIENTS); // Size is actually ignored here
  if (worker->epfd < 0)
    {
      fprintf (stderr, "%s:%d: %s\n", __FUNCTION__, __LINE__, strerror (errno));
      return thrd_error;
    }

  while (atomic_load (&server.is_running))
    process_worker_events (worker);

  close (worker->epfd);
  release_memory (worker->payload_buffer.base);
  worker->payload_buffer = (Payload) { 0 };

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

StatusCode
read_request (Client *client, Request *request)
{
  return read_request_payload (client, (u8 *) request, sizeof (Request));
}

StatusCode
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

StatusCode
write_response (Client *client, Response *response)
{
  return write_response_payload (client, (u8 *) response, sizeof (Response));
}

StatusCode
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

void
close_client (Client *client)
{
  PROFILE (PROF_CLOSE_CLIENT);

  if (!client || (client->fd < 0))
    return;
  close (client->fd);
  client->fd = -1;
  client->worker = NULL;
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

  dprintf (fd, "%.*s", 19 - count, BLANKSTR);
  dprintf (fd, "%4d", client->fd);
  dprintf (fd, GREEN  ("      %6u"),  client->counters.get_hit);
  dprintf (fd, RED    ("       %6u"), client->counters.get_miss);
  dprintf (fd, BLUE   ("%6u"),        client->counters.set);
  dprintf (fd, YELLOW ("%6u"),        client->counters.del);
  dprintf (fd, YELLOW ("%6u"),        client->counters.clr);
  dprintf (fd, GREEN  ("%6u"),        client->counters.lst);
  dprintf (fd, GREEN  ("%6u"),        client->counters.nfo);

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
  dprintf (fd, "%19s", "FD");
  dprintf (fd, "%12s", "GET (HIT)");
  dprintf (fd, "%13s", "GET (MISS)");
  dprintf (fd, "%6s",  "SET");
  dprintf (fd, "%6s",  "DEL");
  dprintf (fd, "%6s",  "CLR");
  dprintf (fd, "%6s",  "LST");
  dprintf (fd, "%6s",  "NFO");
  dprintf (fd, "\n");

  for (u32 i = 0; i < MAX_NUM_CLIENTS; ++i)
    {
      if (clients[i].fd != -1)
        debug_print_client (fd, &clients[i]);
    }

  dprintf (fd, "\n");
}

void
debug_print_workers (int fd)
{
  int count;
  float to_ms = 1000.f / get_performance_frequency ();

  count = dprintf (fd, "WORKERS (%d) ", NUM_WORKERS);
  dprintf (fd, "%.*s\n", LINEWIDTH - count, HLINESTR);
  dprintf (fd, "      GET    ");
  dprintf (fd, " |    SET    ");
  dprintf (fd, " |    DEL    ");
  dprintf (fd, " |    CLR    ");
  dprintf (fd, " |    LST    ");
  dprintf (fd, " |    NFO    ");
  dprintf (fd, "\n");

  for (u32 i = 0; i < NUM_WORKERS; ++i)
    {
      Worker *worker = &workers[i];
      float seconds;
      float seconds_avg;

      seconds = to_ms * worker->timers.get;
      seconds_avg = worker->counters.get ? (seconds / worker->counters.get) : 0.f;
      dprintf (fd, " %5u", worker->counters.get);
      dprintf (fd, " %6.2f", seconds_avg);

      seconds = to_ms * worker->timers.set;
      seconds_avg = worker->counters.set ? (seconds / worker->counters.set) : 0.f;
      dprintf (fd, " %5u", worker->counters.set);
      dprintf (fd, " %6.2f", seconds_avg);

      seconds = to_ms * worker->timers.del;
      seconds_avg = worker->counters.del ? (seconds / worker->counters.del) : 0.f;
      dprintf (fd, " %5u", worker->counters.del);
      dprintf (fd, " %6.2f", seconds_avg);

      seconds = to_ms * worker->timers.clr;
      seconds_avg = worker->counters.clr ? (seconds / worker->counters.clr) : 0.f;
      dprintf (fd, " %5u", worker->counters.clr);
      dprintf (fd, " %6.2f", seconds_avg);

      seconds = to_ms * worker->timers.lst;
      seconds_avg = worker->counters.lst ? (seconds / worker->counters.lst) : 0.f;
      dprintf (fd, " %5u", worker->counters.lst);
      dprintf (fd, " %6.2f", seconds_avg);

      seconds = to_ms * worker->timers.nfo;
      seconds_avg = worker->counters.nfo ? (seconds / worker->counters.nfo) : 0.f;
      dprintf (fd, " %5u", worker->counters.nfo);
      dprintf (fd, " %6.2f", seconds_avg);

      dprintf (fd, "\n");
    }

  dprintf (fd, "\n");
}
