#ifndef ENTRY_H
#define ENTRY_H 1

#include "types.h"

#define CACHE_ENTRY_INIT (CacheEntry) { \
  .k     = NULL,                        \
  .klen  = 0,                           \
  .v     = NULL,                        \
  .vlen  = 0,                           \
  .guard = ATOMIC_FLAG_INIT             \
}

#define LOCK_ENTRY(e) do {} while (atomic_flag_test_and_set (&(e)->guard))
#define UNLOCK_ENTRY(e) atomic_flag_clear (&(e)->guard)

void        init_cache_entry_map     (CacheEntryHashMap *);
CacheEntry *lock_and_get_cache_entry (CacheEntryHashMap *, u8 *, u32);
bool        set_cache_entry          (CacheEntryHashMap *, u8 *, u32, u8 *, u32);

#endif /* ! ENTRY_H */
