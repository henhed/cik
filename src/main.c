#include <assert.h>
#include <errno.h>
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

volatile atomic_bool quit;

static void sigint_handler (int);
static void test_hash_map (void);

static int server_accept_thread (void *);

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  ////////////////////////////////////////
  // Sanity checks
  {
    Request request;
    Response response;
    assert (IS_REQUEST_STRUCT_VALID (request));
    assert (IS_RESPONSE_STRUCT_VALID (response));

    // We use errno codes in responses, don't know how platform specific these are!
    assert (ENODATA == 61);
  }

  ////////////////////////////////////////
  // Init

  // Try to cleanly exit given SIGINT
  signal (SIGINT, sigint_handler);

  printf ("Starting server on port %u\n", SERVER_PORT);
  if (0 != start_server ())
    {
      fprintf (stderr, "Failed to start server: %s\n", strerror (errno));
      return EXIT_FAILURE;
    }

  if (0 > init_memory ())
    return EXIT_FAILURE;

  FILE *info_file = fopen ("info.txt", "w");
  int   info_fd   = fileno (info_file);

  ////////////////////////////////////////
  // ... Profit
  init_cache_entry_map (entry_map);
  test_hash_map (); // @Temporary

  float time_elapsed = 0.f;

  thrd_t thread;
  int err = thrd_create (&thread, server_accept_thread, NULL);
  if (err < 0)
    fprintf (stderr, "%s:%d: %s\n", __FUNCTION__, __LINE__, strerror (errno));

  while (!atomic_load (&quit))
    {
      u64 start_tick = get_performance_counter ();
      {
        PROFILE (PROF_MAIN_LOOP);
        server_read ();
        sleep (0);
      }
      time_elapsed += ((float) (get_performance_counter () - start_tick)
                       / get_performance_frequency ());
      if (time_elapsed > 1.f)
        {
          time_elapsed -= 1.f;
          rewind (info_file);
          debug_print_profilers (info_fd);
          debug_print_memory (info_fd);
          debug_print_tags (info_fd);
        }
    }

  err = thrd_join (thread, NULL);
  if (err < 0)
    fprintf (stderr, "%s:%d: %s\n", __FUNCTION__, __LINE__, strerror (errno));

  ////////////////////////////////////////
  // Clean up

  printf ("\nShutting down ..\n");

  fclose (info_file);
  stop_server ();
  release_all_memory ();

  return EXIT_SUCCESS;
}

static int
server_accept_thread (void *user_data)
{
  (void) user_data;

  printf ("Starting thread %s[0x%lX]\n", __FUNCTION__, thrd_current ());

  while (!atomic_load (&quit))
    {
      struct timespec delay = {.tv_sec = 0, .tv_nsec = 0};
      server_accept ();
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

static void
test_hash_map ()
{
  CacheKey   mykey0 = {(u8 *) "mykey0", strlen ("mykey0")};
  CacheValue myval0 = {(u8 *) "myval0", strlen ("myval0")};
  CacheKey   mykey1 = {(u8 *) "mykey1", strlen ("mykey1")};
  CacheValue myval1 = {(u8 *) "myval1", strlen ("myval1")};

  CacheEntry *entry0, *old_entry;
  entry0 = reserve_and_lock_entry (mykey0.nmemb + myval0.nmemb);
  entry0->key = mykey0; // !! Not pointing to reserved memory
  entry0->value = myval0; // !! Not pointing to reserved memory
  old_entry = NULL;
  set_locked_cache_entry (entry_map, entry0, &old_entry);
  UNLOCK_ENTRY (entry0);
  assert (old_entry == NULL);

  CacheEntry *entry1;
  entry1 = reserve_and_lock_entry (mykey0.nmemb + myval0.nmemb);
  entry1->key = mykey0; // !! Not pointing to reserved memory
  entry1->value = myval0; // !! Not pointing to reserved memory

  old_entry = NULL;
  set_locked_cache_entry (entry_map, entry1, &old_entry);
  UNLOCK_ENTRY (entry1);
  assert (old_entry == entry0);
  UNLOCK_ENTRY (old_entry);

  old_entry = NULL;
  LOCK_ENTRY (entry1);
  set_locked_cache_entry (entry_map, entry1, &old_entry);
  UNLOCK_ENTRY (entry1);
  assert (old_entry == NULL);

  CacheEntry *entry2;
  entry2 = reserve_and_lock_entry (mykey1.nmemb + myval1.nmemb);
  entry2->key = mykey1; // !! Not pointing to reserved memory
  entry2->value = myval1; // !! Not pointing to reserved memory
  set_locked_cache_entry (entry_map, entry2, NULL);
  UNLOCK_ENTRY (entry2);

  CacheEntry *entry3;
  entry3 = lock_and_get_cache_entry (entry_map, mykey1);
  assert (entry3 == entry2);
  assert (atomic_flag_test_and_set (&entry3->guard));
  UNLOCK_ENTRY (entry3);
  assert (!atomic_flag_test_and_set (&entry3->guard));
  UNLOCK_ENTRY (entry3);
  // Try to get same entry again to test internal slot locking
  entry3 = lock_and_get_cache_entry (entry_map, mykey1);
  assert (entry3 != NULL);
  UNLOCK_ENTRY (entry3);

  CacheEntry *entry4;
  CacheKey newkey = {(u8 *) "marklar", strlen ("marklar")};
  entry4 = lock_and_get_cache_entry (entry_map, newkey);
  assert (entry4 == NULL);

  CacheEntry *unset = lock_and_unset_cache_entry (entry_map, mykey1);
  assert (unset != NULL);
  UNLOCK_ENTRY (unset);
  unset = lock_and_unset_cache_entry (entry_map, mykey1);
  assert (unset == NULL);
}
