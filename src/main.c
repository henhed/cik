#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#ifdef HAVE_SYSTEMD
# include <systemd/sd-daemon.h>
#endif

#include "controller.h"
#include "memory.h"
#include "entry.h"
#include "tag.h"
#include "server.h"
#include "profiler.h"
#include "util.h"
#include "log.h"

atomic_bool quit;
atomic_bool do_write_stats;
static thrd_t logging_thread;

static int run_logging_thread (const char *);
static void sigint_handler (int);
static void sigterm_handler (int);
static void sigusr1_handler (int);
static bool write_entry_as_set_request_callback (CacheEntry *, int *);
static void write_stats (RuntimeConfig *);
static void unlock_and_close_fd_ptr (int *);

int
main (int argc, char **argv)
{
  RuntimeConfig *config = parse_args (argc, argv);
  int pid_fd         __attribute__ ((__cleanup__ (unlock_and_close_fd_ptr))) = -1;
  int persistence_fd __attribute__ ((__cleanup__ (unlock_and_close_fd_ptr))) = -1;

  if (config == NULL)
    return EXIT_FAILURE;

  pid_fd = open (config->pid_filename,
                 O_WRONLY | O_CREAT | O_TRUNC,
                 S_IRUSR | S_IWUSR | S_IRGRP);
  if (pid_fd < 0)
    {
      err_print ("Could not open %s: %s\n", config->pid_filename,
                 strerror (errno));
      return EXIT_FAILURE;
    }
  if (flock (pid_fd, LOCK_EX | LOCK_NB) < 0)
    {
      err_print ("Could not lock %s: %s\n", config->pid_filename,
                 strerror (errno));
      return EXIT_FAILURE;
    }
  dprintf (pid_fd, "%d", getpid ());

#ifdef HAVE_SYSTEMD
  sd_notifyf (0, "MAINPID=%lu", (unsigned long) getpid ());
#endif

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
  // Try to cleanly exit given SIGTERM
  signal (SIGTERM, sigterm_handler);
  // Ignore broken pipe so logger can write to fifo w/o listeners
  signal (SIGPIPE, SIG_IGN);
  // Print stats when we get this signal
  signal (SIGUSR1, sigusr1_handler);
  // Ignore to avoid killing by typo. Might be used later for GC or sth maybe.
  signal (SIGUSR2, SIG_IGN);

  if (0 > init_memory ())
    return EXIT_FAILURE;

  atomic_init (&quit, false);
  atomic_init (&do_write_stats, false);

  if (0 != init_util ())
    {
      err_print ("Failed to init utilities: %s\n", strerror (errno));
      return EXIT_FAILURE;
    }

  if (0 != init_log ())
    {
      err_print ("Failed to init logging: %s\n", strerror (errno));
      return EXIT_FAILURE;
    }

  if (0 != init_controller ())
    {
      err_print ("Failed to init controller: %s\n", strerror (errno));
      return EXIT_FAILURE;
    }

  for (u32 i = 0; i < NUM_CACHE_ENTRY_MAPS; ++i)
    init_cache_entry_map (entry_maps[i]);

  nfo_print ("Starting server on %d.%d.%d.%d:%d\n",
             (ntohl (config->listen_address) & 0xFF000000) >> 24,
             (ntohl (config->listen_address) & 0x00FF0000) >> 16,
             (ntohl (config->listen_address) & 0x0000FF00) >>  8,
             (ntohl (config->listen_address) & 0x000000FF) >>  0,
             ntohs (config->listen_port));
  if (0 != start_server (config->listen_address, config->listen_port))
    {
      err_print ("Failed to start server: %s\n", strerror (errno));
      return EXIT_FAILURE;
    }

  persistence_fd = open (config->persistence_filename,
                         O_RDWR | O_CREAT,
                         S_IRUSR | S_IWUSR | S_IRGRP);
  if (persistence_fd < 0)
    {
      err_print ("Could not open %s: %s\n", config->persistence_filename,
                 strerror (errno));
      return EXIT_FAILURE;
    }
  if (flock (persistence_fd, LOCK_EX | LOCK_NB) < 0)
    {
      err_print ("Could not lock %s: %s\n", config->persistence_filename,
                 strerror (errno));
      return EXIT_FAILURE;
    }

  ////////////////////////////////////////
  // ... Profit

  if (thrd_create (&logging_thread, (thrd_start_t) run_logging_thread,
                   (void *) config->log_filename) != thrd_success)
    err_print ("%s\n", strerror (errno));

  load_request_log (persistence_fd);

#ifdef HAVE_SYSTEMD
  sd_notify (0, "READY=1");
#endif

  while (!atomic_load (&quit))
    {
      if (atomic_load (&do_write_stats))
        {
          atomic_store (&do_write_stats, false);
          write_stats (config);
        }
      sleep (1);
    }

  ////////////////////////////////////////
  // Clean up

#ifdef HAVE_SYSTEMD
  sd_notify (0, "STOPPING=1");
#endif

  nfo_print ("Shutting down %s\n", "..");

  stop_server ();

  if (0 > thrd_join (logging_thread, NULL))
    err_print ("%s\n", strerror (errno));

  // Persist current state
  ftruncate (persistence_fd, 0);
  lseek (persistence_fd, SEEK_SET, 0);
  for (u32 i = 0; i < NUM_CACHE_ENTRY_MAPS; ++i)
    {
      walk_entries (entry_maps[i],
                    (CacheEntryWalkCb) write_entry_as_set_request_callback,
                    &persistence_fd);
    }

  release_all_memory ();

  return EXIT_SUCCESS;
}

static int
run_logging_thread (const char *logfile)
{
  struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000}; // 1ms
  int rd_fd = open (logfile, O_RDONLY | O_NONBLOCK);
  int wr_fd = open (logfile, O_WRONLY | O_NONBLOCK);

  if (wr_fd == -1)
    {
      wrn_print ("Could not open %s: %s\n", logfile, strerror (errno));
      return thrd_error;
    }

  // We don't actually care about `rd_fd' but we need to have it opened to be
  // able to open a FIFO for writing in non-blocking mode.
  close (rd_fd);

  while (!atomic_load (&quit))
    {
      flush_worker_logs (wr_fd);
      thrd_sleep (&delay, NULL);
    }

  close (wr_fd);

  return thrd_success;
}

static void
unlock_and_close_fd_ptr (int *fd)
{
  cik_assert (fd != NULL);
  if (fd && (*fd >= 0))
    {
      flock (*fd, LOCK_UN);
      close (*fd);
    }
}

static void
sigint_handler (int sig)
{
  signal (sig, SIG_DFL);
  atomic_store (&quit, true);
}

static void
sigterm_handler (int sig)
{
  signal (sig, SIG_DFL);
  atomic_store (&quit, true);
}

static void
sigusr1_handler (int sig)
{
  signal (sig, sigusr1_handler);
  atomic_store (&do_write_stats, true);
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

static void
write_entry_stats_proxy (int fd)
{
  write_entry_stats (fd, entry_maps, NUM_CACHE_ENTRY_MAPS);
}

static void
write_stats (RuntimeConfig *config)
{
  struct {
    const char *filename;
    void (*write_fn) (int);
  } writer_map[] = {
    {config->entry_stats_filename,  write_entry_stats_proxy},
    {config->tag_stats_filename,    write_tag_stats},
    {config->memory_stats_filename, write_memory_stats},
    {config->client_stats_filename, write_client_stats},
    {config->worker_stats_filename, write_workers_stats}
  };
  for (u8 i = 0; i < sizeof (writer_map) / sizeof (writer_map[0]); ++i)
    {
      FILE *file;
      u64 start_tick = get_performance_counter (), num_ticks;
      if (!writer_map[i].filename || !writer_map[i].write_fn)
        continue;

      file = fopen (writer_map[i].filename, "w");
      if (file)
        {
          writer_map[i].write_fn (fileno (file));
          fclose (file);
          num_ticks = get_performance_counter () - start_tick;
          nfo_print ("Wrote %s in %.3f s\n", writer_map[i].filename,
                     (double) num_ticks / get_performance_frequency ());
        }
      else
        err_print ("Failed to open %s: %s\n", writer_map[i].filename,
                   strerror (errno));
    }
}
