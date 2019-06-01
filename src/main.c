#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>

#include "entry.h"
#include "server.h"

typedef struct
{
  u32 size;
  u32 nmemb;
  u8 *base;
} Bucket;

static Bucket buckets[MAX_NUM_BUCKETS] = { 0 };
static size_t num_buckets = 0;

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

  CacheEntryHashMap *entry_map = NULL;

  void  *memory = NULL;
  size_t total_memory_size = 0;
  size_t total_bucket_size = 0;

  ////////////////////////////////////////
  // Calculate how much memory we need
  for (u32 size = MIN_BUCKET_SIZE, nmemb = MAX_BUCKET_ENTRY_COUNT;
       size <= MAX_BUCKET_SIZE;
       size <<= 1, nmemb >>= 1)
    {
      Bucket *bucket;
      if (num_buckets >= MAX_NUM_BUCKETS)
        break;

      printf ("Adding bucket %u x %u Bytes\n", nmemb, size);

      bucket = &buckets[num_buckets++];
      bucket->size  = size;
      bucket->nmemb = nmemb;
      bucket->base  = NULL;

      total_bucket_size += size * nmemb;
    }
  printf ("Total bucket memory is %lu\n", total_bucket_size);

  total_memory_size = (total_bucket_size
                       + sizeof (CacheEntryHashMap));

  ////////////////////////////////////////
  // Try to allocate memory
  printf ("Reserving %lu bytes\n", total_memory_size);
  memory = mmap ((void *) 0, total_memory_size,
                 PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (memory == MAP_FAILED)
    {
      fprintf (stderr, "Failed to map %lu bytes: %s\n",
               total_memory_size, strerror (errno));
      return EXIT_FAILURE;
    }

  printf ("Committing %lu bytes\n", total_memory_size);
  if (0 != mprotect (memory, total_memory_size, PROT_READ | PROT_WRITE))
    {
      fprintf (stderr, "Failed to commit %lu bytes: %s\n",
               total_memory_size, strerror (errno));
      return EXIT_FAILURE;
    }

  printf ("Starting server on port %u\n", SERVER_PORT);
  if (0 != start_server ())
    {
      fprintf (stderr, "Failed to start server: %s\n", strerror (errno));
      return EXIT_FAILURE;
    }

  ////////////////////////////////////////
  // ... Profit

  void *next_memory_location = memory;
  entry_map = next_memory_location;
  next_memory_location += sizeof (CacheEntryHashMap);
  init_cache_entry_map (entry_map);

  for (u32 b = 0; b < num_buckets; ++b)
    {
      Bucket *bucket = &buckets[b];
      size_t bucket_size = bucket->size * bucket->nmemb;
      bucket->base = next_memory_location;
      next_memory_location += bucket_size;
    }

  assert ((size_t) (next_memory_location - memory) == total_memory_size);

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

  if (0 != munmap (memory, total_memory_size))
    {
      fprintf (stderr, "Failed to unmap %lu bytes: %s\n",
               total_memory_size, strerror (errno));
      return EXIT_FAILURE;
    }

  return EXIT_SUCCESS;
}
