#ifndef MEMORY_H
#define MEMORY_H 1

#include "types.h"

CacheEntryHashMap *entry_map;

int  init_memory    (void);
void release_memory (void);

#endif /* ! MEMORY_H */
