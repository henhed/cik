#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>

#include "memory.h"
#include "entry.h"
#include "tag.h"
#include "server.h"
#include "profiler.h"
#include "util.h"
#include "log.h"

volatile atomic_bool quit;
static thrd_t logging_thread;

static int run_logging_thread (void *);
static void sigint_handler (int);
static bool write_entry_as_set_request_callback (CacheEntry *, int *);

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  ////////////////////////////////////////
  // Sanity checks
  {
    Request  request;
    Response response;
    assert (IS_REQUEST_STRUCT_VALID  (request));
    assert (IS_RESPONSE_STRUCT_VALID (response));
    assert ((NUM_LOG_QUEUE_ELEMS & (NUM_LOG_QUEUE_ELEMS - 1)) == 0);
  }

  ////////////////////////////////////////
  // Init

  // Try to cleanly exit given SIGINT
  signal (SIGINT, sigint_handler);

  if (0 > init_memory ())
    return EXIT_FAILURE;

  for (u32 i = 0; i < NUM_CACHE_ENTRY_MAPS; ++i)
    init_cache_entry_map (entry_maps[i]);

  dbg_print ("Starting server on port %u\n", SERVER_PORT);
  if (0 != start_server ())
    {
      err_print ("Failed to start server: %s\n", strerror (errno));
      return EXIT_FAILURE;
    }

  FILE *info_file = fopen  ("info.txt", "w");
  int   info_fd   = fileno (info_file);

  int persistence_fd = open ("persistent.requestlog",
                             O_RDWR | O_CREAT,
                             S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  if (persistence_fd < 0)
    {
      err_print ("Could not open persistent.requestlog: %s\n", strerror (errno));
      return EXIT_FAILURE;
    }

  ////////////////////////////////////////
  // ... Profit

  if (0 > thrd_create (&logging_thread, run_logging_thread, NULL))
    err_print ("%s\n", strerror (errno));

  load_request_log (persistence_fd);

  while (!atomic_load (&quit))
    {
      ftruncate (info_fd, 0);
      lseek (info_fd, SEEK_SET, 0);
      debug_print_profilers (info_fd);
      debug_print_memory (info_fd);
      debug_print_tags (info_fd);
      debug_print_clients (info_fd);
      debug_print_workers (info_fd);
      sleep (1);
    }

  ////////////////////////////////////////
  // Clean up

  dbg_print ("\nShutting down ..\n");

  stop_server ();

  if (0 > thrd_join (logging_thread, NULL))
    err_print ("%s\n", strerror (errno));

  fclose (info_file);

  // Persist current state
  ftruncate (persistence_fd, 0);
  lseek (persistence_fd, SEEK_SET, 0);
  for (u32 i = 0; i < NUM_CACHE_ENTRY_MAPS; ++i)
    {
      walk_entries (entry_maps[i],
                    (CacheEntryWalkCb) write_entry_as_set_request_callback,
                    &persistence_fd);
    }
  close (persistence_fd);

  release_all_memory ();

  return EXIT_SUCCESS;
}

static int
run_logging_thread (void *user_data)
{
  struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000}; // 1ms

  (void) user_data;

  while (!atomic_load (&quit))
    {
      flush_worker_logs ();
      thrd_sleep (&delay, NULL);
    }

  return thrd_success;
}

static void
sigint_handler (int sig)
{
  signal (sig, SIG_DFL);
  atomic_store (&quit, true);
}

static bool
write_entry_as_set_request_callback (CacheEntry *entry, int *fd)
{
  Request request;
  request.cik[0]        = CONTROL_BYTE_1;
  request.cik[1]        = CONTROL_BYTE_2;
  request.cik[2]        = CONTROL_BYTE_3;
  request.op            = CMD_BYTE_SET;
  request.s.klen        = entry->key.nmemb;
  request.s.ntags       = entry->tags.nmemb;
  request.s.flags       = SET_FLAG_NONE;
  request.s._padding[0] = 0;
  request.s.vlen        = htonl (entry->value.nmemb);
  request.s.ttl         = htonl (0xFFFFFFFF);

  if (entry->expires != CACHE_EXPIRES_INIT)
    {
      time_t now = time (NULL);
      if (entry->expires < now)
        return false;
      request.s.ttl = htonl (entry->expires - now);
    }

  write (*fd, &request, sizeof (request));
  reverse_bytes (entry->key.base, entry->key.nmemb);
  write (*fd, entry->key.base, entry->key.nmemb);
  for (u8 t = 0; t < entry->tags.nmemb; ++t)
    {
      u8 tlen = entry->tags.base[t].nmemb;
      write (*fd, &tlen, sizeof (tlen));
      reverse_bytes (entry->tags.base[t].base, tlen);
      write (*fd, entry->tags.base[t].base, tlen);
    }
  write (*fd, entry->value.base, entry->value.nmemb);

  return true;
}
