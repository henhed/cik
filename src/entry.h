#ifndef ENTRY_H
#define ENTRY_H 1

#include "types.h"

#define CACHE_KEY_INIT    { 0 }
#define CACHE_VALUE_INIT  { 0 }
#define CACHE_TAGS_INIT   { 0 }
#define CACHE_EXPIRY_INIT ((time_t) -1)

#define CACHE_ENTRY_INIT (CacheEntry) { \
  .key    = CACHE_KEY_INIT,             \
  .tags   = CACHE_TAGS_INIT,            \
  .value  = CACHE_VALUE_INIT,           \
  .expiry = CACHE_EXPIRY_INIT,          \
  .guard  = ATOMIC_FLAG_INIT            \
}

#define LOCK_ENTRY(e) \
  do {} while (atomic_flag_test_and_set_explicit (&(e)->guard, memory_order_acquire))
#define UNLOCK_ENTRY(e) \
  atomic_flag_clear_explicit (&(e)->guard, memory_order_release)
#define TRY_LOCK_ENTRY(e) \
  (!atomic_flag_test_and_set_explicit (&(e)->guard, memory_order_acquire))

void        init_cache_entry_map        (CacheEntryHashMap *);
CacheEntry *lock_and_get_cache_entry    (CacheEntryHashMap *, CacheKey);
CacheEntry *lock_and_unset_cache_entry  (CacheEntryHashMap *, CacheKey);
bool        set_locked_cache_entry      (CacheEntryHashMap *, CacheEntry *,
                                         CacheEntry **);
void        debug_print_entry           (CacheEntry *);

#endif /* ! ENTRY_H */
