#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <sys/mman.h>

#include "memory.h"

typedef struct
{
  u32 size;
  u32 nmemb;
  u8 *base;
} Bucket;

CacheEntryHashMap *entry_map = NULL;

static Bucket buckets[MAX_NUM_BUCKETS] = { 0 };
static size_t num_buckets = 0;

static void  *main_memory = NULL;
static void  *memory_cursor = NULL;
static size_t total_memory_size = 0;
static size_t total_bucket_size = 0;

int
init_memory ()
{
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

  // Try to allocate memory
  printf ("Reserving %lu bytes\n", total_memory_size);
  main_memory = mmap ((void *) 0, total_memory_size,
                      PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (main_memory == MAP_FAILED)
    {
      fprintf (stderr, "Failed to map %lu bytes: %s\n",
               total_memory_size, strerror (errno));
      return errno;
    }

  printf ("Committing %lu bytes\n", total_memory_size);
  if (0 != mprotect (main_memory, total_memory_size, PROT_READ | PROT_WRITE))
    {
      fprintf (stderr, "Failed to commit %lu bytes: %s\n",
               total_memory_size, strerror (errno));
      return errno;
    }

  memory_cursor = main_memory;

  for (u32 b = 0; b < num_buckets; ++b)
    {
      Bucket *bucket = &buckets[b];
      size_t bucket_size = bucket->size * bucket->nmemb;
      bucket->base = memory_cursor;
      memory_cursor += bucket_size;
    }

  entry_map = memory_cursor;
  memory_cursor += sizeof (CacheEntryHashMap);

  // Make sure all allocations are accounted for
  assert ((size_t) (memory_cursor - main_memory) == total_memory_size);

  return 0;
}

void
release_memory ()
{
  if (0 != munmap (main_memory, total_memory_size))
    {
      fprintf (stderr, "Failed to unmap %lu bytes: %s\n",
               total_memory_size, strerror (errno));
    }
}
