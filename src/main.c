#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "entry.h"
#include "memory.h"
#include "server.h"

volatile bool quit;

static void sigint_handler (int);
static void test_hash_map (void);

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  ////////////////////////////////////////
  // Sanity checks
  {
    Request request;
    assert (sizeof (request)               == 16);
    assert (sizeof (request.cik)           == 3);
    assert (sizeof (request.op)            == 1);
    assert (sizeof (request.g.klen)        == 1);
    assert (sizeof (request.g.flags)       == 1);
    assert (sizeof (request.g._padding)    == 10);
    assert (sizeof (request.s.klen)        == 1);
    assert (sizeof (request.s.tlen)        == 3);
    assert (sizeof (request.s.vlen)        == 4);
    assert (offsetof (Request, cik)        == 0);
    assert (offsetof (Request, op)         == 3);
    assert (offsetof (Request, g.klen)     == 4);
    assert (offsetof (Request, g.flags)    == 5);
    assert (offsetof (Request, g._padding) == 6);
    assert (offsetof (Request, s.klen)     == 4);
    assert (offsetof (Request, s.tlen)     == 5);
    assert (offsetof (Request, s.vlen)     == 8);

    Response response;
    assert (sizeof (response)                 == 8);
    assert (sizeof (response.cik)             == 3);
    assert (sizeof (response.status)          == 1);
    assert (sizeof (response.payload_size)    == 4);
    assert (sizeof (response.error_code)      == 4);
    assert (offsetof (Response, cik)          == 0);
    assert (offsetof (Response, status)       == 3);
    assert (offsetof (Response, payload_size) == 4);
    assert (offsetof (Response, error_code)   == 4);

    // We use errno codes in responses, don't know how platform specific these are!
    assert (ENODATA == 61);
  }

  ////////////////////////////////////////
  // Init

  // Ignore SIGPIPE so we don't get signalled when we write to a client that
  // has closed the connection. Instead write () should return -EPIPE.
  signal (SIGPIPE, SIG_IGN);
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

  ////////////////////////////////////////
  // ... Profit
  init_cache_entry_map (entry_map);
  test_hash_map (); // @Temporary

  while (!quit)
    {
      int handled_requests;
      server_accept ();
      handled_requests = server_read ();
      if (handled_requests > 0)
        {
          /* debug_print_memory (); */
        }
      sleep (0);
    }

  ////////////////////////////////////////
  // Clean up

  printf ("\nShutting down ..\n");
  stop_server ();
  release_memory ();

  return EXIT_SUCCESS;
}

static void
sigint_handler (int sig)
{
  signal (sig, SIG_DFL);
  quit = true;
}

static void
test_hash_map ()
{
  CacheKey   mykey0 = {(u8 *) "mykey0", strlen ("mykey0")};
  CacheValue myval0 = {(u8 *) "myval0", strlen ("myval0")};
  CacheKey   mykey1 = {(u8 *) "mykey1", strlen ("mykey1")};
  CacheValue myval1 = {(u8 *) "myval1", strlen ("myval1")};

  CacheEntry *entry0, *old_entry;
  entry0 = reserve_and_lock (mykey0.nmemb + myval0.nmemb);
  entry0->key = mykey0; // !! Not pointing to reserved memory
  entry0->value = myval0; // !! Not pointing to reserved memory
  old_entry = NULL;
  set_locked_cache_entry (entry_map, entry0, &old_entry);
  UNLOCK_ENTRY (entry0);
  assert (old_entry == NULL);

  CacheEntry *entry1;
  entry1 = reserve_and_lock (mykey0.nmemb + myval0.nmemb);
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
  entry2 = reserve_and_lock (mykey1.nmemb + myval1.nmemb);
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
  UNLOCK_ENTRY (entry3);

  CacheEntry *entry4;
  CacheKey newkey = {(u8 *) "marklar", strlen ("marklar")};
  entry4 = lock_and_get_cache_entry (entry_map, newkey);
  assert (entry4 == NULL);
}
