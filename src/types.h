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

typedef struct
{
  u8 *base;
  u32 nmemb;
} Payload;

// Linked list
typedef struct _KeyNode
{
  CacheKey key;
  struct _KeyNode *next;
} KeyNode;

// Binary tree
typedef struct _TagNode
{
  CacheTag tag;
  KeyNode *keys;
  struct _TagNode *left;
  struct _TagNode *right;
} TagNode;

typedef struct
{
  CacheTag *base;
  u8 nmemb;
} CacheTagArray;

typedef struct
{
  CacheKey key;
  CacheTagArray tags;
  CacheValue value;
  time_t expiry;
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
  CLEAR_MODE_ALL        = 0x00,
  CLEAR_MODE_OLD        = 0x01,
  CLEAR_MODE_MATCH_ALL  = 0x02,
  CLEAR_MODE_MATCH_NONE = 0x03,
  CLEAR_MODE_MATCH_ANY  = 0x04
} ClearMode;

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

// Size         Offset          Value

// :GET
// char[3]      0               'CiK' (Sanity)
// char         3               'g'   (OP code)
// u8           4               Key length
// u8           5               Flags
// u8[10]       6               Padding
// ..data       16              (key)

// :SET
// char[3]      0               'CiK' (Sanity)
// char         3               's'   (OP code)
// u8           4               Key length
// u8           5               Tag count
// u8[2]        6               Padding
// u32          8               Value length
// u32          12              TTL in seconds
// ..data       16              (key + tags + value)

// :DEL
// char[3]      0               'CiK' (Sanity)
// char         3               'd'   (OP code)
// u8           4               Key length
// u8[11]       6               Padding
// void *       16              (key)

// :CLR
// char[3]      0               'CiK' (Sanity)
// char         3               'c'   (OP code)
// u8           4               ClearMode
// u8           5               Tag count
// u8[10]       6               Padding
// void *       10              (tags)

#define CONTROL_BYTE_1 0x43 // 'C'
#define CONTROL_BYTE_2 0x69 // 'i'
#define CONTROL_BYTE_3 0x4B // 'K'
#define CMD_BYTE_GET   0x67 // 'g'
#define CMD_BYTE_SET   0x73 // 's'
#define CMD_BYTE_DEL   0x64 // 'd'
#define CMD_BYTE_CLR   0x63 // 'c'
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
      u8 ntags;
      u8 _padding[2];
      u32 vlen;
      u32 ttl;
    } s;
    struct __attribute__((packed))
    {
      u8 klen;
      u8 _padding[11];
    } d;
    struct __attribute__((packed))
    {
      u8 mode;
      u8 ntags;
      u8 _padding[10];
    } c;
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
   && (sizeof (request.s.ntags) == 1)           \
   && (sizeof (request.s.vlen) == 4)            \
   && (sizeof (request.s._padding) == 2)        \
   && (sizeof (request.d.klen) == 1)            \
   && (sizeof (request.d._padding) == 11)       \
   && (sizeof (request.c.mode) == 1)            \
   && (sizeof (request.c.ntags) == 1)           \
   && (sizeof (request.c._padding) == 10)       \
   && (offsetof (Request, cik) == 0)            \
   && (offsetof (Request, op) == 3)             \
   && (offsetof (Request, g.klen) == 4)         \
   && (offsetof (Request, g.flags) == 5)        \
   && (offsetof (Request, g._padding) == 6)     \
   && (offsetof (Request, s.klen) == 4)         \
   && (offsetof (Request, s.ntags) == 5)        \
   && (offsetof (Request, s._padding) == 6)     \
   && (offsetof (Request, s.vlen) == 8)         \
   && (offsetof (Request, d.klen) == 4)         \
   && (offsetof (Request, d._padding) == 5)     \
   && (offsetof (Request, c.mode) == 4)         \
   && (offsetof (Request, c.ntags) == 5)        \
   && (offsetof (Request, c._padding) == 6)     \
   )

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
