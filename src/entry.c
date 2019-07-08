#include <stdlib.h>
#include <string.h>

#include "entry.h"
#include "log.h"
#include "util.h"

#define LOCK_SLOT(m, s) \
  do {} while (atomic_flag_test_and_set_explicit (&(map)->guards[s], \
                                                  memory_order_acquire))
#define UNLOCK_SLOT(m, s) \
  atomic_flag_clear_explicit (&(m)->guards[s], memory_order_release)

#define LOCK_ENTRY_AND_LOG_SPIN(e)                              \
  do {                                                          \
    if (!TRY_LOCK_ENTRY (e))                                    \
      {                                                         \
        err_print ("SPINNING \"%s\"\n", key2str ((e)->key));    \
        LOCK_ENTRY (e);                                         \
        err_print ("GOT LOCK \"%s\"\n", key2str ((e)->key));    \
      }                                                         \
  } while (0)

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
  cik_assert (map);

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

  cik_assert (map);
  cik_assert (key.base);

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
              LOCK_ENTRY_AND_LOG_SPIN (map->entries[pos]);
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

  cik_assert (map);
  cik_assert (key.base);

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
              LOCK_ENTRY_AND_LOG_SPIN (map->entries[pos]);
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

  cik_assert (map);
  cik_assert (entry->key.base);
  cik_assert (entry->value.base);
  cik_assert (old_entry != NULL);

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
              cik_assert (occupant != NULL);
              if (occupant == entry)
                {
                  err_print ("ALREADY SET \"%.*s\"\n",
                             entry->key.nmemb, entry->key.base);
                  UNLOCK_SLOT (map, pos);
                  return true;
                }

              LOCK_ENTRY_AND_LOG_SPIN (occupant);
              if (CMP_ENTRY_KEYS (occupant, entry))
                {
                  cik_assert (*old_entry != entry);
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
      err_print ("No slot found for \"%.*s\"\n",
                 entry->key.nmemb, entry->key.base);
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
  cik_assert (map);
  cik_assert (callback);

  for (u32 pos = 0; pos < CACHE_ENTRY_MAP_SIZE; ++pos)
    {
      LOCK_SLOT (map, pos);
      if (map->mask[pos])
        {
          CacheEntry *entry = map->entries[pos];
          cik_assert (entry != NULL);

          LOCK_ENTRY_AND_LOG_SPIN (entry);

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

////////////////////////////////////////////////////////////////////////////////
// STATS / DEBUG

typedef struct
{
  int fd;
  time_t now;
  u32 map_index;
} StatsCbData;

static bool
write_entry_stats_cb (CacheEntry *entry, StatsCbData *data)
{
  time_t expires = entry->expires;
  if (expires != CACHE_EXPIRES_INIT)
    expires -= data->now;
  dprintf (data->fd, "%u\t%u\t%u\t%ld\t%ld\t%u\t%s\n",
           entry->nhits,
           entry->value.nmemb,
           entry->tags.nmemb,
           data->now - entry->mtime,
           expires,
           data->map_index,
           key2str (entry->key));
  return false;
}

void
write_entry_stats (int fd, CacheEntryHashMap **maps, u32 nmaps)
{
  StatsCbData data = {};
  data.fd = fd;
  data.now = time (NULL);

  dprintf (fd, "Hits\tSize\tTags\tAge\tTTL\tMap\tKey\n");

  for (u32 i = 0; i < nmaps; ++i)
    {
      data.map_index = i;
      walk_entries (maps[i], (CacheEntryWalkCb) write_entry_stats_cb, &data);
    }
}

void
debug_print_entry (CacheEntry *entry)
{
#if DEBUG
  bool expires = (entry->expires != CACHE_EXPIRES_INIT);
  dbg_print ("%s: Content is: {\n"
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
