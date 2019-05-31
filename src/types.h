#ifndef TYPES_H
#define TYPES_H 1

#include "config.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>

typedef int8_t   s8;
typedef int16_t s16;
typedef int32_t s32;

typedef uint8_t   u8;
typedef uint16_t u16;
typedef uint32_t u32;

typedef _Atomic volatile u32 u32atom;

typedef struct
{
  u8 *k;
  u32 klen;
  u8 *v;
  u32 vlen;
  atomic_flag guard;
} CacheEntry;

typedef struct
{
  u32atom nmemb;
  bool mask[MAX_NUM_CACHE_ENTRIES];
  u32 hashes[MAX_NUM_CACHE_ENTRIES];
  CacheEntry entries[MAX_NUM_CACHE_ENTRIES];
} CacheEntryHashMap;

#endif /* ! TYPES_H */
