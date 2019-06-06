#include <stdlib.h>
#include <string.h>

#include "entry.h"

#if DEBUG
# include <assert.h>
# include <stdio.h>
#endif

#define LOCK_SLOT(m, s) do {} while (atomic_flag_test_and_set (&(map)->guards[slot]))
#define UNLOCK_SLOT(m, s) atomic_flag_clear (&(m)->guards[slot])

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

  atomic_init (&map->nmemb, 0);

  for (u32 i = 0; i < MAX_NUM_CACHE_ENTRIES; ++i)
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
  assert (key.nmemb > 0);
#endif

  hash = get_key_hash (key);
  slot = hash % MAX_NUM_CACHE_ENTRIES;
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
      if (pos >= MAX_NUM_CACHE_ENTRIES)
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
  assert (key.nmemb > 0);
#endif

  hash = get_key_hash (key);
  slot = hash % MAX_NUM_CACHE_ENTRIES;
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
                  atomic_fetch_sub (&map->nmemb, 1);
                  UNLOCK_SLOT (map, pos);
                  return entry; // Caller now owns entry lock
                }
              UNLOCK_ENTRY (map->entries[pos]);
            }
        }
      UNLOCK_SLOT (map, pos);
      ++pos;
      if (pos >= MAX_NUM_CACHE_ENTRIES)
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
  assert (entry->key.nmemb > 0);
  assert (entry->value.base);
  assert (entry->value.nmemb > 0);
#endif

  if (atomic_load (&map->nmemb) >= MAX_NUM_CACHE_ENTRIES)
    {
      if (old_entry == NULL)
        {
          // We refuse to overwrite existing entries if caller won't take care of it
#if DEBUG
          fprintf (stderr, "[%s] We're full!\n", __FUNCTION__);
#endif
          return false;
        }
    }

  hash = get_key_hash (entry->key);
  slot = hash % MAX_NUM_CACHE_ENTRIES;
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
                  if (old_entry != NULL)
                    {
#if DEBUG
                      assert (*old_entry != entry);
#endif
                      // If caller wants the old entry they are responsible for
                      // unlocking it
                      *old_entry = occupant;
                      found = true;
                      break; // slot is still locked
                    }
                  else
                    {
                      // If caller doesn't want the old entry we reject the new one.
                      // We don't want duplicate keys. There is a @Bug here though;
                      // since the same key can get different pos'es depending on
                      // what else is currently in the map we /can/ get duplicate
                      // keys. But in that case the most recently added entry will
                      // be closest to the initially calculated pos and hence
                      // matched before the older duplicate. This inconsistency has
                      // to be handled in a higher layer for now.
                      // Ensuring consistency here would impact @Speed
                      UNLOCK_ENTRY (occupant);
                      UNLOCK_SLOT (map, pos);
#if DEBUG
                      assert (false); // This case is untested
#endif
                      break;
                    }
                }
              UNLOCK_ENTRY (occupant);
            }
          UNLOCK_SLOT (map, pos);
          ++pos;
          if (pos >= MAX_NUM_CACHE_ENTRIES)
            pos = 0;
        }
      else
        {
          found = true;
          atomic_fetch_add (&map->nmemb, 1);
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
debug_print_entry (CacheEntry *entry)
{
  bool expires = (entry->expiry != CACHE_EXPIRY_INIT);
  printf ("%s: Content is: {\n"
          " TTL: %ld\n"
          " KEY: \"%.*s\"\n"
          " TAGS: [\"%.*s\", \"%.*s\", \"%.*s\"]\n"
          " VAL: \"%.*s\"\n}\n",
          __FUNCTION__,
          expires ? (entry->expiry - time (NULL)) : -1,
          entry->key.nmemb,     entry->key.base,
          entry->tags[0].nmemb, entry->tags[0].base,
          entry->tags[1].nmemb, entry->tags[1].base,
          entry->tags[2].nmemb, entry->tags[2].base,
          entry->value.nmemb,   entry->value.base
          );
}
