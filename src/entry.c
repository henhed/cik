#include <stdlib.h>
#include <string.h>

#include "entry.h"

#if DEBUG
# include <assert.h>
# include <stdio.h>
#endif

static inline u32
get_hash (const u8 *base, u32 nmemb)
{
  u32 hash = 5381;
  for (; nmemb > 0; --nmemb)
    hash = ((hash << 5) + hash) ^ *(base++);
  return hash;
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
      map->hashes[i] = 0;
      map->entries[i] = CACHE_ENTRY_INIT;
    }
}

CacheEntry *
lock_and_get_cache_entry (CacheEntryHashMap *map, u8 *k, u32 klen)
{
  u32 hash, slot, pos;

#if DEBUG
  assert (map);
  assert (k);
  assert (klen > 0);
#endif

  hash = get_hash (k, klen);
  slot = hash % MAX_NUM_CACHE_ENTRIES;
  pos  = slot;

  do
    {
      LOCK_ENTRY (&map->entries[pos]);
      if (map->mask[pos])
        {
          if ((map->hashes[pos] == hash)
              && (map->entries[pos].key.nmemb == klen)
              && ((map->entries[pos].key.base == k) ||
                  (0 == memcmp (map->entries[pos].key.base, k, klen))))
            {
              return &map->entries[pos]; // Caller is now responsible for guard
            }
        }
      UNLOCK_ENTRY (&map->entries[pos]);
      ++pos;
      if (pos >= MAX_NUM_CACHE_ENTRIES)
        pos = 0;
    }
  while (pos != slot);

  return NULL;
}

bool
set_cache_entry (CacheEntryHashMap *map, u8 *k, u32 klen, u8 *v, u32 vlen)
{
  bool found = false;
  u32 hash, slot, pos;

#if DEBUG
  assert (map);
  assert (k);
  assert (klen > 0);
  assert (v);
  assert (vlen > 0);
#endif

  if (atomic_load (&map->nmemb) >= MAX_NUM_CACHE_ENTRIES)
    {
#if DEBUG
      fprintf (stderr, "[%s] We're full!\n", __FUNCTION__);
#endif
      return false;
    }

  hash = get_hash (k, klen);
  slot = hash % MAX_NUM_CACHE_ENTRIES;
  pos  = slot;

  do
    {
      LOCK_ENTRY (&map->entries[pos]);
      if (map->mask[pos])
        {
          if ((map->hashes[pos] == hash)
              && (map->entries[pos].key.nmemb == klen)
              && ((map->entries[pos].key.base == k) ||
                  (0 == memcmp (map->entries[pos].key.base, k, klen))))
            {
#if DEBUG
              u8 str[klen + 1];
              memcpy (str, k, klen);
              str[klen] = '\0';
              printf ("OVERWRITING \"%s\"\n", str);
#endif
              found = true;
              break; // entry flag is still set
            }
          UNLOCK_ENTRY (&map->entries[pos]);
          ++pos;
          if (pos >= MAX_NUM_CACHE_ENTRIES)
            pos = 0;
        }
      else
        {
          found = true;
          atomic_fetch_add (&map->nmemb, 1);
          break; // entry flag is still set
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
  map->entries[pos].key.base = k;
  map->entries[pos].key.nmemb = klen;
  map->entries[pos].value.base = v;
  map->entries[pos].value.nmemb = vlen;
  UNLOCK_ENTRY (&map->entries[pos]);

  return true;
}

void
debug_print_entry (CacheEntry *entry)
{
  bool expires = (entry->expiry != CACHE_EXPIRY_INIT);
  printf ("%s: Content is: {\n"
          " TTL: %ld\n"
          " KEY: \"%.*s\"\n"
          " TAG: \"%.*s\"\n"
          " TAG: \"%.*s\"\n"
          " TAG: \"%.*s\"\n"
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
