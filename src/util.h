#ifndef UTIL_H
#define UTIL_H 1

#include "types.h"

int         init_util   (void);
const char *key2str     (CacheKey);
const char *tag2str     (CacheTag);

static inline void
reverse_bytes (u8 *base, u32 nmemb)
{
  for (u8 *i = base, *j = base + (nmemb - 1); i < j; ++i, --j)
    {
      *i ^= *j;
      *j ^= *i;
      *i ^= *j;
    }
}

#endif /* ! UTIL_H */
