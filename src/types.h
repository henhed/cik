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
typedef int64_t s64;

typedef uint8_t   u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

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

typedef enum
{
  // 0x00 Series: Generates a sucess response
  STATUS_OK                     = 0x00,

  // 0x10 Series: Errors that close the client connection
  MASK_INTERNAL_ERROR           = 0x10,
  STATUS_BUG                    = 0x11,
  STATUS_CONNECTION_CLOSED      = 0x12,
  STATUS_NETWORK_ERROR          = 0x13,

  // 0x20 Series: Errors that generate a failure response
  MASK_CLIENT_ERROR             = 0x20,
  STATUS_PROTOCOL_ERROR         = 0x21,

  // 0x40 Series: Non-errors that generate a failure response
  MASK_CLIENT_MESSAGE           = 0x40,
  STATUS_NOT_FOUND              = 0x41,
  STATUS_EXPIRED                = 0x42,
  STATUS_OUT_OF_MEMORY          = 0x43
} StatusCode;

static inline const char *
get_status_code_name (StatusCode code)
{
  switch (code)
    {
    case STATUS_OK:                 return "OK";
    case MASK_INTERNAL_ERROR:       return "[MASK_INTERNAL_ERROR]";
    case STATUS_BUG:                return "BUG!";
    case STATUS_CONNECTION_CLOSED:  return "Connection closed";
    case STATUS_NETWORK_ERROR:      return "Network error";
    case MASK_CLIENT_ERROR:         return "[MASK_CLIENT_ERROR]";
    case STATUS_PROTOCOL_ERROR:     return "Protocol error";
    case MASK_CLIENT_MESSAGE:       return "[MASK_CLIENT_MESSAGE]";
    case STATUS_NOT_FOUND:          return "Not found";
    case STATUS_EXPIRED:            return "Expired";
    case STATUS_OUT_OF_MEMORY:      return "Out of memory";
    default:                        return "Unknown";
    }
}

////////////////////////////////////////////////////////////////////////////////
// PROTOCOL
//

// GET
// char[3]      0               'CiK' (Sanity)
// char         3               'g'   (OP code)
// u8           4               Key length
// u8           5               Flags
// u8[10]       6               Padding
// ..data       16              (key)

// SET
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
#define CMD_BYTE_GET   0x67 // 'g'
#define CMD_BYTE_SET   0x73 // 's'
#define SUCCESS_BYTE   0x74 // 't'
#define FAILURE_BYTE   0x66 // 'f'

#define GET_FLAG_NONE          0x00
#define GET_FLAG_IGNORE_EXPIRY 0x01

typedef struct __attribute__((packed))
{
  s8 cik[3];
  s8 op;
  union __attribute__((packed))
  {
    struct __attribute__((packed))
    {
      u8 klen;
      u8 flags;
      u8 _padding[10];
    } g;
    struct __attribute__((packed))
    {
      u8 klen;
      u8 tlen[3];
      u32 vlen;
      u32 ttl;
    } s;
  };
} Request;

#define IS_REQUEST_STRUCT_VALID(request)        \
  ((sizeof (request) == 16)                     \
   && (sizeof (request.cik) == 3)               \
   && (sizeof (request.op) == 1)                \
   && (sizeof (request.g.klen) == 1)            \
   && (sizeof (request.g.flags) == 1)           \
   && (sizeof (request.g._padding) == 10)       \
   && (sizeof (request.s.klen) == 1)            \
   && (sizeof (request.s.tlen) == 3)            \
   && (sizeof (request.s.vlen) == 4)            \
   && (offsetof (Request, cik) == 0)            \
   && (offsetof (Request, op) == 3)             \
   && (offsetof (Request, g.klen) == 4)         \
   && (offsetof (Request, g.flags) == 5)        \
   && (offsetof (Request, g._padding) == 6)     \
   && (offsetof (Request, s.klen) == 4)         \
   && (offsetof (Request, s.tlen) == 5)         \
   && (offsetof (Request, s.vlen) == 8))

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

#define IS_RESPONSE_STRUCT_VALID(response)      \
  ((sizeof (response) == 8)                     \
   && (sizeof (response.cik) == 3)              \
   && (sizeof (response.status) == 1)           \
   && (sizeof (response.payload_size) == 4)     \
   && (sizeof (response.error_code) == 4)       \
   && (offsetof (Response, cik) == 0)           \
   && (offsetof (Response, status) == 3)        \
   && (offsetof (Response, payload_size) == 4)  \
   && (offsetof (Response, error_code) == 4))

#define MAKE_SUCCESS_RESPONSE(size) (Response) {    \
  .cik = {                                          \
    [0] = CONTROL_BYTE_1,                           \
    [1] = CONTROL_BYTE_2,                           \
    [2] = CONTROL_BYTE_3                            \
  },                                                \
  .status = SUCCESS_BYTE,                           \
  .payload_size = htonl ((u32) size)                \
}

#define MAKE_FAILURE_RESPONSE(error) (Response) {   \
  .cik = {                                          \
    [0] = CONTROL_BYTE_1,                           \
    [1] = CONTROL_BYTE_2,                           \
    [2] = CONTROL_BYTE_3                            \
  },                                                \
  .status = FAILURE_BYTE,                           \
  .error_code = htonl ((u32) error)                 \
}

#endif /* ! TYPES_H */
