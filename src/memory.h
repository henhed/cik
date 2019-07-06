#ifndef MEMORY_H
#define MEMORY_H 1

#include "types.h"

CacheEntryHashMap **entry_maps;

int         init_memory                         (void);
void       *reserve_memory                      (u32);
void        release_memory                      (void *);
CacheEntry *reserve_and_lock_entry              (size_t);
bool        reserve_biggest_possible_payload    (Payload *);
void        release_all_memory                  (void);
void        populate_nfo_response               (NFOResponsePayload *);
void        debug_print_memory                  (int);

#endif /* ! MEMORY_H */
