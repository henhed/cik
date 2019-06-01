#include <errno.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#include "server.h"

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

int
start_server ()
{
  server.fd = socket (AF_INET, SOCK_STREAM/* | SOCK_NONBLOCK*/, 0);
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
      close (client->fd);
      return errno;
    }

  printf ("Accepted connection from %d.%d.%d.%d:%d over FD %d\n",
          (client->addr.sin_addr.s_addr & 0x000000FF) >>  0,
          (client->addr.sin_addr.s_addr & 0x0000FF00) >>  8,
          (client->addr.sin_addr.s_addr & 0x00FF0000) >> 16,
          (client->addr.sin_addr.s_addr & 0xFF000000) >> 24,
          client->addr.sin_port,
          client->fd);

  return 0;
}

static int
handle_set_request (Client *client, Request *request)
{
  u8  klen  = request->s.klen;
  u8  tlen0 = request->s.tlen[0];
  u8  tlen1 = request->s.tlen[1];
  u8  tlen2 = request->s.tlen[2];
  u32 vlen  = request->s.vlen;
  size_t total_size = klen + tlen0 + tlen1 + tlen2 + vlen;
  char buffer[total_size + 1];
  ssize_t nread;

  // @Temporary
  printf ("%s: Got SET request:\n\t{"
          "klen = %u, "
          "len[0] = %u, "
          "len[1] = %u, "
          "len[2] = %u, "
          "vlen = %u}\n",
          __FUNCTION__, klen, tlen0, tlen1, tlen2, vlen);

  nread = read (client->fd, buffer, total_size);
  if (nread < 0)
    return errno;
  if ((size_t) nread < total_size)
    {
      fprintf (stderr, "%s: Read %ld bytes, but expected %lu\n",
               __FUNCTION__, nread, total_size);
      return -EINVAL;
    }

  buffer[total_size] = '\0';
  printf ("%s: Content is:\n"
          "\tKEY: %.*s\n"
          "\tTAG: %.*s\n"
          "\tTAG: %.*s\n"
          "\tTAG: %.*s\n"
          "\tVAL: %.*s\n\n",
          __FUNCTION__,
          klen,
          buffer + 0,
          tlen0,
          buffer + klen,
          tlen1,
          buffer + klen + tlen0,
          tlen2,
          buffer + klen + tlen0 + tlen1,
          vlen,
          buffer + klen + tlen0 + tlen1 + tlen2
          );

  return 0;
}

static int
handle_request (Client *client, Request *request)
{
  if (!client || !request)
    return -EINVAL;

  if ((request->cik[0] != 0x43)     // 'C'
      || (request->cik[1] != 0x69)  // 'i'
      || (request->cik[2] != 0x4B)) // 'K'
    return -EINVAL;

  if (request->op == 0x73) // 's'
    {
      request->s.vlen = ntohl (request->s.vlen);
      return handle_set_request (client, request);
    }

  return -ENOSYS;
}

void
server_read ()
{
  struct epoll_event events[MAX_NUM_EVENTS] = {{ 0 }};
  int nevents = epoll_wait (server.epfd, events, MAX_NUM_EVENTS, 0);
  if (nevents < 0)
    {
      fprintf (stderr, "%s: epoll_wait failed: %s\n",
               __FUNCTION__, strerror (errno));
      return;
    }

  for (int i = 0; i < nevents; ++i)
    {
      struct epoll_event *event = &events[i];
      if (event->events & EPOLLIN)
        {
          Client *client = event->data.ptr;
          Request request = { 0 };
          ssize_t nread = read (client->fd, &request, sizeof (request));

          if (nread == 0)
            {
              fprintf (stderr, "%s: Connection closed (FD %d)\n",
                       __FUNCTION__, client->fd);
              // @Revisit: Thread safety. What happens if we have a thread
              // working on a response? Also, how to respond to client?
              close (client->fd);
              client->fd = -1;
              continue;
            }
          else if (nread != sizeof (request))
            {
              fprintf (stderr, "%s: Invalid read (FD %d). Expected %lu, got %ld\n",
                       __FUNCTION__, client->fd, sizeof (request), nread);
              // @Revisit: Thread safety. What happens if we have a thread
              // working on a response? Also, how to respond to client?
              close (client->fd);
              client->fd = -1;
              continue;
            }
          else if (0 > handle_request (client, &request))
            {
              fprintf (stderr, "%s: Invalid request (FD %d).\n",
                       __FUNCTION__, client->fd);
              // @Revisit: Thread safety. What happens if we have a thread
              // working on a response? Also, how to respond to client?
              close (client->fd);
              client->fd = -1;
              continue;
            }
        }

      if (event->events & (EPOLLERR | EPOLLHUP))
        {
          // @Revisit: Thread safety. What happens if we have a thread
          // working on a response? Also, how to respond to client?
          Client *client = event->data.ptr;
          close (client->fd);
          client->fd = -1;
          fprintf (stderr, "%s: Got error event: 0x%X",
                   __FUNCTION__, event->events);
          continue;
        }
    }
}

void
stop_server ()
{
  for (u32 i = 0; i < MAX_NUM_CLIENTS; ++i)
    close (clients[i].fd);
  close (server.epfd);
  close (server.fd);
}
