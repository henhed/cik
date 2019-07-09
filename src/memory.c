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
#include "log.h"

#define PADDING(s)                                         \
  ((alignof (max_align_t) - ((s) % alignof (max_align_t))) \
   % alignof (max_align_t))

#define TPADDING(T) PADDING (sizeof (T))

#define PADDED(s)  ((s) + PADDING (s))
#define TPADDED(T) (sizeof (T) + TPADDING (T))

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
  dbg_print ("Reserving %u bytes\n", MAX_TOTAL_MEMORY);
  main_memory = mmap ((void *) 0, MAX_TOTAL_MEMORY,
                      PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (main_memory == MAP_FAILED)
    {
      err_print ("Failed to map %u bytes: %s\n",
                 MAX_TOTAL_MEMORY, strerror (errno));
      return errno;
    }

  dbg_print ("Committing %u bytes\n", MAX_TOTAL_MEMORY);
  if (0 != mprotect (main_memory, MAX_TOTAL_MEMORY, PROT_READ | PROT_WRITE))
    {
      err_print ("Failed to commit %u bytes: %s\n",
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
  cik_assert (payload != NULL);
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
      err_print ("Failed to unmap %u bytes: %s\n",
                 MAX_TOTAL_MEMORY, strerror (errno));
    }
}

void
populate_nfo_response (NFOResponsePayload *nfo)
{
  cik_assert (nfo != NULL);

  nfo->server.bytes_reserved = MAX_TOTAL_MEMORY - total_system_size;
  nfo->server.bytes_used     = 0;
  nfo->server.bytes_free     = 0;
  nfo->server.bytes_reused   = 0;

  for (Partition **p = &partitions; *p; p = &(*p)->next)
    {
      Partition *partition = *p;
      u32 num_used   = atomic_load (&partition->num_used);
      u32 num_free   = atomic_load (&partition->num_free);
      u32 num_reused = atomic_load (&partition->num_reused);
      nfo->server.bytes_used   += partition->size * num_used;
      nfo->server.bytes_free   += partition->size * num_free;
      nfo->server.bytes_reused += partition->size * num_reused;
    }
}

void
write_memory_stats (int fd)
{
  u32 memory_left = (((uintptr_t) main_memory + MAX_TOTAL_MEMORY)
                     - atomic_load (&memory_cursor));

  dprintf (fd, "%s\t%s\t%s\t%s\t%s\n",
           "Size", "Used", "Free", "Reused", "Available");

  for (Partition **p = &partitions; *p; p = &(*p)->next)
    {
      Partition *partition = *p;
      u32 num_used   = atomic_load (&partition->num_used);
      u32 num_free   = atomic_load (&partition->num_free);
      u32 num_reused = atomic_load (&partition->num_reused);
      dprintf (fd, "%u\t%u\t%u\t%u\t%u\n", partition->size, num_used, num_free,
               num_reused, memory_left / partition->size);
    }
}
