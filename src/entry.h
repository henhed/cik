#ifndef ENTRY_H
#define ENTRY_H 1

#include "types.h"

#define CACHE_KEY_INIT    { 0 }
#define CACHE_VALUE_INIT  { 0 }
#define CACHE_TAGS_INIT   { { 0 } }
#define CACHE_EXPIRY_INIT ((time_t) -1)

#define CACHE_ENTRY_INIT (CacheEntry) { \
  .key    = CACHE_KEY_INIT,             \
  .value  = CACHE_VALUE_INIT,           \
  .tags   = CACHE_TAGS_INIT,            \
  .expiry = CACHE_EXPIRY_INIT,          \
  .waste  = 0,                          \
  .guard  = ATOMIC_FLAG_INIT            \
}

#define LOCK_ENTRY(e) do {} while (atomic_flag_test_and_set (&(e)->guard))
#define UNLOCK_ENTRY(e) atomic_flag_clear (&(e)->guard)

void        init_cache_entry_map     (CacheEntryHashMap *);
CacheEntry *lock_and_get_cache_entry (CacheEntryHashMap *, CacheKey);
bool        set_locked_cache_entry   (CacheEntryHashMap *, CacheEntry *, CacheEntry **);
void        debug_print_entry        (CacheEntry *);

#endif /* ! ENTRY_H */
