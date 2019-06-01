#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "entry.h"
#include "memory.h"
#include "server.h"

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  ////////////////////////////////////////
  // Sanity checks
  Request request;
  printf ("[SET] Request is %lu Bytes {\n"
          "  cik  = %lu at %lu\n"
          "  op   = %lu at %lu\n"
          "  klen = %lu at %lu\n"
          "  tlen = %lu at %lu\n"
          "  vlen = %lu at %lu\n"
          "}\n",
          sizeof (request),
          sizeof (request.cik), offsetof (Request, cik),
          sizeof (request.op),  offsetof (Request, op),
          sizeof (request.s.klen),  offsetof (Request, s.klen),
          sizeof (request.s.tlen),  offsetof (Request, s.tlen),
          sizeof (request.s.vlen),  offsetof (Request, s.vlen)
          );
  assert (sizeof (request) == 16);

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

  {
    CacheEntry *entry = NULL;

    char mykey0[] = "mykey0";
    char myval0[] = "myval0";
    char mykey1[] = "mykey1";
    char myval1[] = "myval1";
    char mykey2[] = "mykey2";
    char myval2[] = "myval2";

    set_cache_entry (entry_map,
                     (u8 *) mykey0, strlen (mykey0),
                     (u8 *) myval0, strlen (myval0));
    set_cache_entry (entry_map,
                     (u8 *) mykey1, strlen (mykey1),
                     (u8 *) myval1, strlen (myval1));
    set_cache_entry (entry_map,
                     (u8 *) mykey2, strlen (mykey2),
                     (u8 *) myval2, strlen (myval2));
    set_cache_entry (entry_map,
                     (u8 *) mykey1, strlen (mykey1),
                     (u8 *) myval1, strlen (myval1));

    entry = lock_and_get_cache_entry (entry_map, (u8 *) "mykey", strlen ("mykey"));
    assert (entry == NULL);
    entry = lock_and_get_cache_entry (entry_map, (u8 *) "mykey1", strlen ("mykey1"));
    assert (entry != NULL);
    assert (atomic_flag_test_and_set (&entry->guard));

    {
      u8 key[entry->klen + 1];
      u8 val[entry->vlen + 1];
      memcpy (key, entry->k, entry->klen);
      memcpy (val, entry->v, entry->vlen);
      key[entry->klen] = '\0';
      val[entry->vlen] = '\0';
      printf ("GOT \"%s\" => \"%s\"\n", key, val);
    }

    UNLOCK_ENTRY (entry);
    assert (!atomic_flag_test_and_set (&entry->guard));
    UNLOCK_ENTRY (entry);
  }

  for (s32 countdown = 4; countdown > 0;)
    {
      int handled_requests;
      server_accept ();
      handled_requests = server_read ();
      if (handled_requests > 0)
        countdown -= handled_requests;
      sleep (0);
    }

  ////////////////////////////////////////
  // Clean up

  stop_server ();
  release_memory ();

  return EXIT_SUCCESS;
}
