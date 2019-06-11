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

volatile atomic_bool quit;

static void sigint_handler (int);
static void test_hash_map (void);
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
  }

  ////////////////////////////////////////
  // Init

  // Try to cleanly exit given SIGINT
  signal (SIGINT, sigint_handler);

  if (0 > init_memory ())
    return EXIT_FAILURE;

  init_cache_entry_map (entry_map);

  printf ("Starting server on port %u\n", SERVER_PORT);
  if (0 != start_server ())
    {
      fprintf (stderr, "Failed to start server: %s\n", strerror (errno));
      return EXIT_FAILURE;
    }

  FILE *info_file = fopen  ("info.txt", "w");
  int   info_fd   = fileno (info_file);

  int persistence_fd = open ("persistent.requestlog", O_RDWR | O_CREAT);
  if (persistence_fd < 0)
    {
      fprintf (stderr, "Could not open persistent.requestlog: %s\n",
               strerror (errno));
      return EXIT_FAILURE;
    }

  ////////////////////////////////////////
  // ... Profit
  test_hash_map (); // @Temporary

  load_request_log (persistence_fd);

  while (!atomic_load (&quit))
    {
      ftruncate (info_fd, 0);
      lseek (info_fd, SEEK_SET, 0);
      debug_print_profilers (info_fd);
      debug_print_memory (info_fd);
      debug_print_tags (info_fd);
      debug_print_clients (info_fd);
      sleep (1);
    }

  ////////////////////////////////////////
  // Clean up

  printf ("\nShutting down ..\n");

  // Persist current state
  ftruncate (persistence_fd, 0);
  lseek (persistence_fd, SEEK_SET, 0);
  walk_entries (entry_map,
                (CacheEntryWalkCb) write_entry_as_set_request_callback,
                &persistence_fd);
  close (persistence_fd);

  fclose (info_file);
  stop_server ();
  release_all_memory ();

  return EXIT_SUCCESS;
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
  request.s._padding[0] = 0;
  request.s._padding[1] = 0;
  request.s.vlen        = htonl (entry->value.nmemb);
  request.s.ttl         = htonl (0xFFFFFFFF);

  if (entry->expiry != CACHE_EXPIRY_INIT)
    {
      time_t now = time (NULL);
      if (entry->expiry < now)
        return false;
      request.s.ttl = htonl (entry->expiry - now);
    }

  write (*fd, &request, sizeof (request));
  write (*fd, entry->key.base, entry->key.nmemb);
  for (u8 t = 0; t < entry->tags.nmemb; ++t)
    {
      u8 tlen = entry->tags.base[t].nmemb;
      write (*fd, &tlen, sizeof (tlen));
      write (*fd, entry->tags.base[t].base, tlen);
    }
  write (*fd, entry->value.base, entry->value.nmemb);

  return false;
}

static bool
test_walk_entries (CacheEntry *entry, void *user_data)
{
  (void) user_data;
  assert (entry);
  fprintf (stderr, "%s: got called: %.*s\n", __FUNCTION__,
           entry->key.nmemb, entry->key.base);
  return false;
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

  walk_entries (entry_map, test_walk_entries, NULL);
}
