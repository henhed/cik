#include <assert.h>
#include <errno.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <sys/mman.h>

#include "memory.h"
#include "entry.h"
#include "print.h"

typedef struct
{
  u32 size;
  u32 cap;
  _Atomic u32 nmemb;
  u8 *base;
  atomic_flag *occupancy_mask;
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
  for (u32 size = MIN_BUCKET_SIZE, cap = MAX_BUCKET_ENTRY_COUNT;
       size <= MAX_BUCKET_SIZE;
       size <<= 1, cap >>= 1)
    {
      Bucket *bucket;
      if (num_buckets >= MAX_NUM_BUCKETS)
        break;

      printf ("Adding bucket %u x %u Bytes\n", cap, size);

      bucket = &buckets[num_buckets++];
      bucket->size  = size;
      bucket->cap   = cap;
      bucket->base  = NULL;
      bucket->occupancy_mask = NULL;
      atomic_init (&bucket->nmemb, 0);

      total_bucket_size += (size + sizeof (atomic_flag)) * cap;
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
  assert (((intptr_t) memory_cursor % alignof (max_align_t)) == 0);

  for (u32 b = 0; b < num_buckets; ++b)
    {
      Bucket *bucket = &buckets[b];
      size_t bucket_size = bucket->size * bucket->cap;
      size_t mask_size = sizeof (atomic_flag) * bucket->cap;

      bucket->base = memory_cursor;
      memory_cursor += bucket_size;
      assert (((intptr_t) memory_cursor % alignof (max_align_t)) == 0);

      bucket->occupancy_mask = memory_cursor;
      for (u32 f = 0; f < bucket->cap; ++f)
        bucket->occupancy_mask[f] = (atomic_flag) ATOMIC_FLAG_INIT;
      memory_cursor += mask_size;
      assert (((intptr_t) memory_cursor % alignof (max_align_t)) == 0);
    }

  entry_map = memory_cursor;
  memory_cursor += sizeof (CacheEntryHashMap);
  assert (((intptr_t) memory_cursor % alignof (max_align_t)) == 0);

  // Make sure all allocations are accounted for
  assert ((size_t) (memory_cursor - main_memory) == total_memory_size);

  return 0;
}

#define PADDING(T) \
  ((alignof (max_align_t) - (sizeof (T) % alignof (max_align_t))) \
   % alignof (max_align_t))

void *
reserve_memory (u32 size)
{
  u32 head_size = sizeof (atomic_flag *) + PADDING (atomic_flag *);
  u32 total_size = head_size + size;

  if ((UINT32_MAX - head_size) < size)
    return NULL; // `total_size' has overflowed

  for (u32 i = 0; i < num_buckets; ++i)
    {
      Bucket *bucket = &buckets[i];
      if (bucket->size < total_size)
        continue;

      u32 m = atomic_fetch_add (&bucket->nmemb, 1);
      if (m < bucket->cap)
        {
          u8 *memory = NULL;
#if DEBUG
          assert (atomic_flag_test_and_set (&bucket->occupancy_mask[m]) == false);
#else
          atomic_flag_test_and_set (&bucket->occupancy_mask[m]);
#endif
          memory = bucket->base + (m * bucket->size);
          *(atomic_flag **) memory = &bucket->occupancy_mask[m];
          memory += head_size;
          return memory;
        }

      // @Race: We can increment nmemb past cap so we'll have to be careful not
      // to trust it too much elsewhere.
      atomic_store (&bucket->nmemb, bucket->cap);

      for (u32 m = 0, cap = bucket->cap; m < cap; ++m)
        {
          u8 *memory = NULL;
          if (atomic_flag_test_and_set (&bucket->occupancy_mask[m]))
            continue;
          memory = bucket->base + (m * bucket->size);
          *(atomic_flag **) memory = &bucket->occupancy_mask[m];
          memory += head_size;
          return memory;
        }
    }

  return NULL;
}

void
release_memory (void *memory)
{
  u32 head_size = sizeof (atomic_flag *) + PADDING (atomic_flag *);
#if DEBUG
  assert (memory != NULL);
#endif
  memory -= head_size;
#if DEBUG
  assert (atomic_flag_test_and_set (*(atomic_flag **) memory));
#endif
  // The atomic_flag here is a pointer to an occupancy_mask entry
  atomic_flag_clear (*(atomic_flag **) memory);
}

CacheEntry *
reserve_and_lock_entry (size_t payload_size)
{
  CacheEntry *entry = NULL;
  size_t total_size = sizeof (CacheEntry) + payload_size;

  entry = reserve_memory (total_size);
  if (entry == NULL)
    {
      // @Incomplete: Evict something old if entry is NULL here we should never
      // fail to allocate an entry I think.
      assert (false);
      return NULL;
    }

  *entry = CACHE_ENTRY_INIT;
  LOCK_ENTRY (entry);

  return entry;
}

bool
reserve_biggest_possible_payload (Payload *payload)
{
  // @Revisit: Maybe this head_size should be baked into the bucket somehow?
  // It seem error prone to calculate it everywhere.
  u32 head_size = sizeof (atomic_flag *) + PADDING (atomic_flag *);
#if DEBUG
  assert (payload != NULL);
#endif
  for (u32 i = num_buckets - 1; (s32) i >= 0; --i)
    {
      Bucket *bucket = &buckets[i];
      u32 size = bucket->size - head_size;
      payload->base = reserve_memory (size);
      if (payload->base != NULL)
        {
          payload->nmemb = size;
          return true;
        }
    }
  return false;
}

void
release_all_memory ()
{
  if (0 != munmap (main_memory, total_memory_size))
    {
      fprintf (stderr, "Failed to unmap %lu bytes: %s\n",
               total_memory_size, strerror (errno));
    }
}

void
debug_print_memory (int fd)
{
  int count = 0;

  count = dprintf (fd, "MEMORY BUCKETS (%lu) ", num_buckets);
  dprintf (fd, "%.*s\n", LINEWIDTH - count, HLINESTR);

  dprintf (fd, "%-24s%-24s%-24s%s\n", "Item Size", "Capacity", "Members", "Usage%");

  static u32 kb = 1024;
  static u32 mb = 1024 * 1024;

  for (u32 b = 0; b < num_buckets; ++b)
    {
      Bucket *bucket = &buckets[b];
      float usage = 100.f * ((float) bucket->nmemb / bucket->cap);
      dprintf (fd, "%8u%s",
               ((bucket->size >= mb)
                ? (bucket->size / mb)
                : (bucket->size >= kb ? (bucket->size / kb) : bucket->size)),
               ((bucket->size >= mb)
                ? "M"
                : (bucket->size >= kb ? "K" : "B")));
      dprintf (fd, "             %10u", bucket->cap);
      dprintf (fd, "             %10u", bucket->nmemb);
      if (usage > 50.f)
        dprintf (fd, "             "    RED ("%10.2f"), usage);
      else if (usage > 25.f)
        dprintf (fd, "             " YELLOW ("%10.2f"), usage);
      else
        dprintf (fd, "             "  GREEN ("%10.2f"), usage);
      dprintf (fd, "\n");
    }

  dprintf (fd, "\n");
}
