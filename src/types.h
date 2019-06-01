#ifndef TYPES_H
#define TYPES_H 1

#include "config.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>

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

////////////////////////////////////////////////////////////////////////////////
// PROTOCOL
//

// char[3]      0               'CiK' (Sanity)
// char         3               's'   (OP code)
// u8           4               Key length
// u8           5               Tag 0 length
// u8           6               Tag 1 length
// u8           7               Tag 2 length
// u32          8               Value length
// ..data       12              (key + tags + value)

typedef struct __attribute__((packed))
{
  s8 cik[3];
  s8 op;
  union __attribute__((packed))
  {
    struct __attribute__((packed))
    {
      u8 klen;
      u8 tlen[3];
      u32 vlen;
    } s;
  };
} Request;

#endif /* ! TYPES_H */
