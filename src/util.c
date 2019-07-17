#include "util.h"
#include "memory.h"

static tss_t key2str_buffer = (tss_t) -1;
static tss_t tag2str_buffer = (tss_t) -1;

int init_util ()
{
  int err;
  err = tss_create (&key2str_buffer, release_memory);
  cik_assert (err == thrd_success);
  cik_assert (key2str_buffer != (tss_t) -1);
  if (err != thrd_success)
    return err;

  tss_set (key2str_buffer, NULL);

  err = tss_create (&tag2str_buffer, release_memory);
  cik_assert (err == thrd_success);
  cik_assert (tag2str_buffer != (tss_t) -1);
  if (err != thrd_success)
    return err;

  tss_set (tag2str_buffer, NULL);

  return 0;
}

const char *
key2str (CacheKey key)
{
  char *buffer;

  cik_assert (key2str_buffer != (tss_t) -1);

  buffer = tss_get (key2str_buffer);
  if (!buffer)
    {
      buffer = reserve_memory (sizeof (char) * 0x100);
      tss_set (key2str_buffer, buffer);
    }

  cik_assert (buffer != NULL);
  if (!buffer)
    return NULL;

  buffer[key.nmemb] = '\0';
  for (u8 i = 0; i < key.nmemb; ++i)
    buffer[i] = key.base[(key.nmemb - 1) - i];
  return buffer;
}

const char *
tag2str (CacheTag tag)
{
  char *buffer;

  cik_assert (tag2str_buffer != (tss_t) -1);

  buffer = tss_get (tag2str_buffer);
  if (!buffer)
    {
      buffer = reserve_memory (sizeof (char) * 0x100);
      tss_set (tag2str_buffer, buffer);
    }

  cik_assert (buffer != NULL);
  if (!buffer)
    return NULL;

  buffer[tag.nmemb] = '\0';
  for (u8 i = 0; i < tag.nmemb; ++i)
    buffer[i] = tag.base[(tag.nmemb - 1) - i];
  return buffer;
}
