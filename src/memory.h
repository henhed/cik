#ifndef MEMORY_H
#define MEMORY_H 1

#include "types.h"

CacheEntryHashMap *entry_map;

int         init_memory        (void);
CacheEntry *reserve_and_lock   (size_t);
void        release_memory     (void);
void        debug_print_memory (void);

#endif /* ! MEMORY_H */
