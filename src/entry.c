#include <stdlib.h>
#include <string.h>

#include "entry.h"
#include "print.h"

#if DEBUG
# include <assert.h>
# include <stdio.h>
#endif

#define LOCK_SLOT(m, s) \
  do {} while (atomic_flag_test_and_set_explicit (&(map)->guards[s], \
                                                  memory_order_acquire))
#define UNLOCK_SLOT(m, s) \
  atomic_flag_clear_explicit (&(m)->guards[s], memory_order_release)

static inline u32
get_hash (const u8 *base, u32 nmemb)
{
  u32 hash = 5381;
  for (; nmemb > 0; --nmemb)
    hash = ((hash << 5) + hash) ^ *(base++);
  return hash;
}

static inline u32
get_key_hash (CacheKey key)
{
  return get_hash (key.base, key.nmemb);
}

void
init_cache_entry_map (CacheEntryHashMap *map)
{
#if DEBUG
  assert (map);
#endif

  for (u32 i = 0; i < CACHE_ENTRY_MAP_SIZE; ++i)
    {
      map->mask[i] = false;
      map->guards[i] = (atomic_flag) ATOMIC_FLAG_INIT;
      map->hashes[i] = 0;
      map->entries[i] = NULL;
    }
}

// @Note: Segfaults if any key base ptr is NULL
#define CMP_KEYS(a, b)                                  \
  ((a.nmemb == b.nmemb)                                 \
   && ((a.base == b.base)                               \
       || (0 == memcmp (a.base, b.base, a.nmemb))))

// @Note: Evaluates to false if both entries are NULL
#define CMP_ENTRY_KEYS(a, b) (((a) && (b)) && CMP_KEYS((a)->key, (b)->key))

CacheEntry *
lock_and_get_cache_entry (CacheEntryHashMap *map, CacheKey key)
{
  u32 hash, slot, pos;

#if DEBUG
  assert (map);
  assert (key.base);
#endif

  hash = get_key_hash (key);
  slot = hash % CACHE_ENTRY_MAP_SIZE;
  pos  = slot;

  do
    {
      LOCK_SLOT (map, pos);
      if (map->mask[pos])
        {
          if (map->hashes[pos] == hash)
            {
              if (!TRY_LOCK_ENTRY (map->entries[pos]))
                {
#if DEBUG
                  fprintf (stderr, "%s: SPINNING \"%.*s\"\n", __FUNCTION__,
                           map->entries[pos]->key.nmemb, map->entries[pos]->key.base);
#endif
                  LOCK_ENTRY (map->entries[pos]);
#if DEBUG
                  fprintf (stderr, "%s: GOT LOCK \"%.*s\"\n", __FUNCTION__,
                           map->entries[pos]->key.nmemb, map->entries[pos]->key.base);
#endif
                }
              if (CMP_KEYS (map->entries[pos]->key, key))
                {
                  UNLOCK_SLOT (map, pos);
                  return map->entries[pos]; // Caller now owns entry lock
                }
              UNLOCK_ENTRY (map->entries[pos]);
            }
        }
      UNLOCK_SLOT (map, pos);
      ++pos;
      if (pos >= CACHE_ENTRY_MAP_SIZE)
        pos = 0;
    }
  while (pos != slot);

  return NULL;
}

CacheEntry *
lock_and_unset_cache_entry (CacheEntryHashMap *map, CacheKey key)
{
  u32 hash, slot, pos;

#if DEBUG
  assert (map);
  assert (key.base);
#endif

  hash = get_key_hash (key);
  slot = hash % CACHE_ENTRY_MAP_SIZE;
  pos  = slot;

  do
    {
      LOCK_SLOT (map, pos);
      if (map->mask[pos])
        {
          if (map->hashes[pos] == hash)
            {
              if (!TRY_LOCK_ENTRY (map->entries[pos]))
                {
#if DEBUG
                  fprintf (stderr, "%s: SPINNING \"%.*s\"\n", __FUNCTION__,
                           map->entries[pos]->key.nmemb, map->entries[pos]->key.base);
#endif
                  LOCK_ENTRY (map->entries[pos]);
#if DEBUG
                  fprintf (stderr, "%s: GOT LOCK \"%.*s\"\n", __FUNCTION__,
                           map->entries[pos]->key.nmemb, map->entries[pos]->key.base);
#endif
                }
              if (CMP_KEYS (map->entries[pos]->key, key))
                {
                  CacheEntry *entry = map->entries[pos];
                  map->mask[pos] = false;
                  map->hashes[pos] = 0;
                  map->entries[pos] = NULL;
                  UNLOCK_SLOT (map, pos);
                  return entry; // Caller now owns entry lock
                }
              UNLOCK_ENTRY (map->entries[pos]);
            }
        }
      UNLOCK_SLOT (map, pos);
      ++pos;
      if (pos >= CACHE_ENTRY_MAP_SIZE)
        pos = 0;
    }
  while (pos != slot);

  return NULL;
}

bool
set_locked_cache_entry (CacheEntryHashMap *map, CacheEntry *entry,
                        CacheEntry **old_entry)
{
  bool found = false;
  u32 hash, slot, pos;

#if DEBUG
  assert (map);
  assert (entry->key.base);
  assert (entry->value.base);
  assert (old_entry != NULL);
#endif

  hash = get_key_hash (entry->key);
  slot = hash % CACHE_ENTRY_MAP_SIZE;
  pos  = slot;

  do
    {
      LOCK_SLOT (map, pos);
      if (map->mask[pos])
        {
          if (map->hashes[pos] == hash)
            {
              CacheEntry *occupant = map->entries[pos];
#if DEBUG
              assert (occupant != NULL);
#endif
              if (occupant == entry)
                {
#if DEBUG
                  fprintf (stderr, "ALREADY SET \"%.*s\"\n",
                           entry->key.nmemb, entry->key.base);
#endif
                  UNLOCK_SLOT (map, pos);
                  return true;
                }

              if (!TRY_LOCK_ENTRY (occupant))
                {
#if DEBUG
                  fprintf (stderr, "%s: SPINNING \"%.*s\"\n",
                           __FUNCTION__, entry->key.nmemb, entry->key.base);
#endif
                  LOCK_ENTRY (occupant);
#if DEBUG
                  fprintf (stderr, "%s: GOT LOCK \"%.*s\"\n",
                           __FUNCTION__, entry->key.nmemb, entry->key.base);
#endif
                }
              if (CMP_ENTRY_KEYS (occupant, entry))
                {
#if DEBUG
                  assert (*old_entry != entry);
#endif
                  *old_entry = occupant; // Caller now own entry lock
                  found = true;
                  break; // slot is still locked
                }
              UNLOCK_ENTRY (occupant);
            }
          UNLOCK_SLOT (map, pos);
          // @Bug: We can get duplicate keys.  Since the same key can get
          // different pos'es depending on what else is currently in the map we
          // /can/ get duplicate keys.  But in that case the most recently added
          // entry will be closest to the initially calculated pos and hence
          // matched before the older duplicate.  This inconsistency has to be
          // handled in a higher layer for now.  Ensuring consistency here would
          // impact @Speed.
          ++pos;
          if (pos >= CACHE_ENTRY_MAP_SIZE)
            pos = 0;
        }
      else
        {
          found = true;
          break; // slot is still locked
        }
    }
  while (pos != slot);

  if (!found)
    {
#if DEBUG
      fprintf (stderr, "[%s] We didn't find anything!\n", __FUNCTION__);
#endif
      return false;
    }

  map->mask[pos] = true;
  map->hashes[pos] = hash;
  map->entries[pos] = entry;
  UNLOCK_SLOT (map, pos);

  return true;
}

void
walk_entries (CacheEntryHashMap *map, CacheEntryWalkCb callback,
              void *user_data)
{
#if DEBUG
  assert (map);
  assert (callback);
#endif

  for (u32 pos = 0; pos < CACHE_ENTRY_MAP_SIZE; ++pos)
    {
      LOCK_SLOT (map, pos);
      if (map->mask[pos])
        {
          CacheEntry *entry = map->entries[pos];
#if DEBUG
          assert (entry != NULL);
#endif
          if (!TRY_LOCK_ENTRY (entry))
            {
#if DEBUG
              fprintf (stderr, "%s: SPINNING \"%.*s\"\n",
                       __FUNCTION__, entry->key.nmemb, entry->key.base);
#endif
              LOCK_ENTRY (entry);
#if DEBUG
              fprintf (stderr, "%s: GOT LOCK \"%.*s\"\n",
                       __FUNCTION__, entry->key.nmemb, entry->key.base);
#endif
            }

          if (callback (entry, user_data))
            {
              // Caller now owns entry lock
              map->mask[pos] = false;
              map->hashes[pos] = 0;
              map->entries[pos] = NULL;
            }
          else
            {
              UNLOCK_ENTRY (entry);
            }
        }
      UNLOCK_SLOT (map, pos);
    }
}

void
debug_print_entry (CacheEntry *entry)
{
#if DEBUG
  bool expires = (entry->expires != CACHE_EXPIRES_INIT);
  printf ("%s: Content is: {\n"
          " TTL: %ld\n"
          " MTIME: %ld\n"
          " TAGS: %u\n"
          " KEY: \"%.*s\"\n"
          " VAL: \"%.*s\"\n}\n",
          __FUNCTION__,
          expires ? (entry->expires - time (NULL)) : -1,
          entry->mtime,
          entry->tags.nmemb,
          entry->key.nmemb, entry->key.base,
          entry->value.nmemb, entry->value.base
          );
#else
  (void) entry;
#endif
}
