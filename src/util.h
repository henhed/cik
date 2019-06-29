#ifndef UTIL_H
#define UTIL_H 1

static inline void
reverse_data (u8 *base, u32 nmemb)
{
  for (u8 *i = base, *j = base + (nmemb - 1); i < j; ++i, --j)
    {
      *i ^= *j;
      *j ^= *i;
      *i ^= *j;
    }
}

static thread_local char key2str_buffer[0x100];

static inline const char *
key2str (CacheKey key)
{
  key2str_buffer[key.nmemb] = '\0';
  for (u8 i = 0; i < key.nmemb; ++i)
    key2str_buffer[i] = key.base[(key.nmemb - 1) - i];
  return key2str_buffer;
}

static thread_local char tag2str_buffer[0x100];

static inline const char *
tag2str (CacheTag tag)
{
  tag2str_buffer[tag.nmemb] = '\0';
  for (u8 i = 0; i < tag.nmemb; ++i)
    tag2str_buffer[i] = tag.base[(tag.nmemb - 1) - i];
  return tag2str_buffer;
}

#endif /* ! UTIL_H */
