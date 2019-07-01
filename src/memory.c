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

#define PADDING(s)                                         \
  ((alignof (max_align_t) - ((s) % alignof (max_align_t))) \
   % alignof (max_align_t))

#define TPADDING(T) PADDING (sizeof (T))

#define PADDED(s)  ((s) + PADDING (s))
#define TPADDED(T) (sizeof (T) + TPADDING (T))

#define NEW_MEMORY_SYSTEM 1

#if NEW_MEMORY_SYSTEM

typedef struct _Partition Partition;
typedef struct _Bucket    Bucket;

struct _Bucket
{
  u8        *data;
  Partition *partition;
  _Atomic (Bucket *) next;
};

struct _Partition
{
  u32        size;
  Partition *next;
  _Atomic (Bucket *) free_buckets;
  _Atomic (u32) num_used;
  _Atomic (u32) num_free;
  _Atomic (u32) num_reused;
};

CacheEntryHashMap **entry_maps = NULL;

static Partition *partitions = NULL;

static void *main_memory = NULL;
static atomic_uintptr_t memory_cursor = 0;
static size_t total_system_size = 0;
static size_t total_partition_size = 0;
static size_t total_hash_map_array_size = 0;
static size_t total_hash_maps_size = 0;

static void *
push_memory (u32 size)
{
  uintptr_t ptr = atomic_fetch_add (&memory_cursor, size);
  if ((ptr + size) > ((uintptr_t) main_memory + MAX_TOTAL_MEMORY))
    return NULL;
  return (void *) ptr;
}

int
init_memory ()
{
  for (u32 size = MIN_BUCKET_SIZE; size <= MAX_BUCKET_SIZE; size <<= 1)
    total_partition_size += sizeof (Partition);
  total_partition_size += PADDING (total_partition_size);

  total_hash_map_array_size  = NUM_CACHE_ENTRY_MAPS * sizeof (CacheEntryHashMap *);
  total_hash_map_array_size += PADDING (total_hash_map_array_size);
  total_hash_maps_size       = NUM_CACHE_ENTRY_MAPS * (sizeof (CacheEntryHashMap)
                                                       + TPADDING (CacheEntryHashMap));

  total_system_size = (total_partition_size
                       + total_hash_map_array_size
                       + total_hash_maps_size);

  // Try to allocate memory
  printf ("Reserving %lu bytes\n", MAX_TOTAL_MEMORY);
  main_memory = mmap ((void *) 0, MAX_TOTAL_MEMORY,
                      PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (main_memory == MAP_FAILED)
    {
      fprintf (stderr, "Failed to map %lu bytes: %s\n",
               MAX_TOTAL_MEMORY, strerror (errno));
      return errno;
    }

  printf ("Committing %lu bytes\n", MAX_TOTAL_MEMORY);
  if (0 != mprotect (main_memory, MAX_TOTAL_MEMORY, PROT_READ | PROT_WRITE))
    {
      fprintf (stderr, "Failed to commit %lu bytes: %s\n",
               MAX_TOTAL_MEMORY, strerror (errno));
      return errno;
    }

  atomic_init (&memory_cursor, (uintptr_t) main_memory);
  assert (((intptr_t) memory_cursor % alignof (max_align_t)) == 0);

  for (u32 size = MAX_BUCKET_SIZE; size >= MIN_BUCKET_SIZE; size >>= 1)
    {
      Partition *partition = push_memory (sizeof (Partition));
      partition->size = size;
      partition->next = partitions;
      partitions = partition;
      atomic_init (&partition->free_buckets, NULL);
      atomic_init (&partition->num_used, 0);
      atomic_init (&partition->num_free, 0);
      atomic_init (&partition->num_reused, 0);
    }
  push_memory (PADDING (total_partition_size));

  entry_maps = push_memory (total_hash_map_array_size);
  for (u32 i = 0; i < NUM_CACHE_ENTRY_MAPS; ++i)
    {
      entry_maps[i] = push_memory (sizeof (CacheEntryHashMap)
                                   + TPADDING (CacheEntryHashMap));
      assert (((intptr_t) memory_cursor % alignof (max_align_t)) == 0);
    }

  // Make sure all allocations are accounted for
  assert ((size_t) (memory_cursor - (uintptr_t) main_memory) == total_system_size);

  return 0;
}

#define MARK_POINTER(p) \
  (!((uintptr_t) atomic_fetch_or ((atomic_uintptr_t *) p, (uintptr_t) 0x1) & 0x1))
#define UNMARK_POINTER(p) \
  atomic_fetch_and ((atomic_uintptr_t *) p, ~(uintptr_t) 0x1)
#define READ_POINTER(p) \
  (void *) ((uintptr_t) atomic_load (p) & ~(uintptr_t) 0x1)
#define WRITE_POINTER(p, v) \
  atomic_store ((p), (v))

void *
reserve_memory (u32 size)
{
  Bucket    *bucket    = NULL;
  Partition *partition = NULL;
  _Atomic (Bucket *) *free_list = NULL;

  for (Partition **p = &partitions; *p; p = &(*p)->next)
    {
      if ((*p)->size >= size)
        {
          partition = *p;
          break;
        }
    }

  if (!partition)
    return NULL;

  free_list = &partition->free_buckets;

  if (MARK_POINTER (free_list))
    {
      bucket = READ_POINTER (free_list);
      if (bucket)
        {
          WRITE_POINTER (free_list, bucket->next);
          atomic_fetch_sub_explicit (&partition->num_free, 1,
                                     memory_order_relaxed);
          atomic_fetch_add_explicit (&partition->num_reused, 1,
                                     memory_order_relaxed);
        }
      else
        UNMARK_POINTER (free_list);
    }

  // We push new memory if the free-list is empty
  // .. or if we lost a @Race to pop from the free-list.

  if (!bucket)
    {
      bucket = push_memory (TPADDED (Bucket) + PADDED (partition->size));
      bucket->data = ((u8 *) bucket) + TPADDED (Bucket);
      bucket->partition = partition;
    }

  atomic_fetch_add_explicit (&partition->num_used, 1, memory_order_relaxed);

  return bucket->data;
}

void
release_memory (void *memory)
{
  Partition *partition = NULL;
  Bucket    *bucket    = NULL;
  _Atomic (Bucket *) *free_list = NULL;

  cik_assert (memory != NULL);

  bucket    = (Bucket *) (((u8 *) memory) - TPADDED (Bucket));
  partition = bucket->partition;

  free_list = &partition->free_buckets;

  do
    {
      bucket->next = READ_POINTER (free_list);
    }
  while (!atomic_compare_exchange_weak (free_list,
                                        (Bucket **) &bucket->next,
                                        bucket));

  atomic_fetch_add_explicit (&partition->num_free, 1, memory_order_relaxed);
  atomic_fetch_sub_explicit (&partition->num_used, 1, memory_order_relaxed);
}

CacheEntry *
reserve_and_lock_entry (size_t payload_size)
{
  CacheEntry *entry = NULL;
  size_t total_size = sizeof (CacheEntry) + payload_size;

  entry = reserve_memory (total_size);
  if (entry == NULL)
    return NULL;

  *entry = CACHE_ENTRY_INIT;
  LOCK_ENTRY (entry);

  return entry;
}

bool
reserve_biggest_possible_payload (Payload *payload)
{
#if DEBUG
  assert (payload != NULL);
#endif
  payload->base = reserve_memory (MAX_BUCKET_SIZE);
  if (payload->base != NULL)
    {
      payload->nmemb = 0;
      payload->cap = MAX_BUCKET_SIZE;
      return true;
    }
  return false;
}

void
release_all_memory ()
{
  if (0 != munmap (main_memory, MAX_TOTAL_MEMORY))
    {
      fprintf (stderr, "Failed to unmap %lu bytes: %s\n",
               MAX_TOTAL_MEMORY, strerror (errno));
    }
}

void
debug_print_memory (int fd)
{
  int count = 0;
  u32 memory_left = atomic_load (&memory_cursor) - (uintptr_t) main_memory;

  static u32 kb = 1024;
  static u32 mb = 1024 * 1024;

  count = dprintf (fd, "MEMORY PARTITIONS (%.2f%%) ",
                   100.f * ((float) memory_left / MAX_TOTAL_MEMORY));
  dprintf (fd, "%.*s\n", LINEWIDTH - count, HLINESTR);

  dprintf (fd, "%-24s%-24s%-24s%s\n", "Item Size", "Used", "Free", "Reused");

  for (Partition **p = &partitions; *p; p = &(*p)->next)
    {
      Partition *partition = *p;
      u32 num_used   = atomic_load (&partition->num_used);
      u32 num_free   = atomic_load (&partition->num_free);
      u32 num_reused = atomic_load (&partition->num_reused);

      dprintf (fd, "%8u%s",
               ((partition->size >= mb)
                ? (partition->size / mb)
                : (partition->size >= kb
                   ? (partition->size / kb)
                   : partition->size)),
               ((partition->size >= mb)
                ? "M"
                : (partition->size >= kb ? "K" : "B")));
      dprintf (fd, "         %10u", num_used);
      dprintf (fd, "              %10u", num_free);
      dprintf (fd, "                %10u", num_reused);
      dprintf (fd, "\n");
    }

  dprintf (fd, "\n");
}

#else

typedef struct
{
  u32 size;
  u32 cap;
  _Atomic u32 nmemb;
  u8 *base;
  atomic_flag *occupancy_mask;
} Bucket;

CacheEntryHashMap **entry_maps = NULL;

static Bucket buckets[MAX_NUM_BUCKETS] = { 0 };
static size_t num_buckets = 0;

static void  *main_memory = NULL;
static void  *memory_cursor = NULL;
static size_t total_memory_size = 0;
static size_t total_bucket_size = 0;
static size_t total_hash_map_array_size = 0;
static size_t total_hash_maps_size = 0;

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

  total_hash_map_array_size  = NUM_CACHE_ENTRY_MAPS * sizeof (CacheEntryHashMap *);
  total_hash_map_array_size += PADDING (total_hash_map_array_size);
  total_hash_maps_size       = NUM_CACHE_ENTRY_MAPS * (sizeof (CacheEntryHashMap)
                                                       + TPADDING (CacheEntryHashMap));

  total_memory_size = (total_bucket_size
                       + total_hash_map_array_size
                       + total_hash_maps_size);

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

  entry_maps = memory_cursor;
  memory_cursor += total_hash_map_array_size + PADDING (total_hash_map_array_size);

  for (u32 i = 0; i < NUM_CACHE_ENTRY_MAPS; ++i)
    {
      entry_maps[i] = memory_cursor;
      memory_cursor += sizeof (CacheEntryHashMap) + TPADDING (CacheEntryHashMap);
      assert (((intptr_t) memory_cursor % alignof (max_align_t)) == 0);
    }

  // Make sure all allocations are accounted for
  assert ((size_t) (memory_cursor - main_memory) == total_memory_size);

  return 0;
}

void *
reserve_memory (u32 size)
{
  u32 head_size = sizeof (atomic_flag *) + TPADDING (atomic_flag *);
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
  u32 head_size = sizeof (atomic_flag *) + TPADDING (atomic_flag *);
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
    return NULL;

  *entry = CACHE_ENTRY_INIT;
  LOCK_ENTRY (entry);

  return entry;
}

bool
reserve_biggest_possible_payload (Payload *payload)
{
  // @Revisit: Maybe this head_size should be baked into the bucket somehow?
  // It seem error prone to calculate it everywhere.
  u32 head_size = sizeof (atomic_flag *) + TPADDING (atomic_flag *);
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
          payload->nmemb = 0;
          payload->cap   = size;
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

#endif
