#ifndef TYPES_H
#define TYPES_H 1

#include "config.h"

#include <netinet/in.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

typedef int8_t   s8;
typedef int16_t s16;
typedef int32_t s32;

typedef uint8_t   u8;
typedef uint16_t u16;
typedef uint32_t u32;

typedef _Atomic volatile u32 u32atom;

typedef struct
{
  u8 *base;
  u32 nmemb;
} CacheKey;

typedef struct
{
  u8 *base;
  u32 nmemb;
} CacheValue;

typedef struct
{
  u8 *base;
  u8 nmemb;
} CacheTag;

typedef CacheTag CacheTagList[MAX_NUM_TAGS_PER_ENTRY];

typedef struct
{
  CacheKey key;
  CacheValue value;
  CacheTagList tags;
  time_t expiry;
  u32 waste;
  atomic_flag guard;
} CacheEntry;

typedef struct
{
  u32atom nmemb;
  bool mask[MAX_NUM_CACHE_ENTRIES];
  atomic_flag guards[MAX_NUM_CACHE_ENTRIES];
  u32 hashes[MAX_NUM_CACHE_ENTRIES];
  CacheEntry *entries[MAX_NUM_CACHE_ENTRIES];
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
// u32          12              TTL in seconds
// ..data       16              (key + tags + value)

#define CONTROL_BYTE_1 0x43 // 'C'
#define CONTROL_BYTE_2 0x69 // 'i'
#define CONTROL_BYTE_3 0x4B // 'K'
#define CMD_BYTE_SET   0x73 // 's'
#define SUCCESS_BYTE   0x74 // 't'
#define FAILURE_BYTE   0x66 // 'f'

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
      u32 ttl;
    } s;
  };
} Request;

typedef struct __attribute__((packed))
{
  s8 cik[3]; // Always ASCII 'CiK'
  s8 status; // ASCII 't' for success of 'f' for error
  union __attribute__((packed))
  {
    u32 payload_size; // Indicates size of body if status = t
    u32 error_code;   // Error code if status = f
  };
} Response;

#endif /* ! TYPES_H */
