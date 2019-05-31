#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <sys/mman.h>

#include "entry.h"

int main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  CacheEntryHashMap *entry_map = NULL;

  void  *memory = NULL;
  size_t length = 0;

  ////////////////////////////////////////
  // Calculate how much memory we need
  length += sizeof (CacheEntryHashMap);

  ////////////////////////////////////////
  // Try to allocate memory
  printf ("Reserving %lu bytes\n", length);
  memory = mmap ((void *) 0, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (memory == MAP_FAILED)
    {
      fprintf (stderr, "Failed to map %lu bytes: %s\n", length, strerror (errno));
      return EXIT_FAILURE;
    }

  printf ("Committing %lu bytes\n", length);
  if (0 != mprotect (memory, length, PROT_READ | PROT_WRITE))
    {
      fprintf (stderr, "Failed to commit %lu bytes: %s\n", length, strerror (errno));
      return EXIT_FAILURE;
    }

  ////////////////////////////////////////
  // ... Profit

  entry_map = memory;
  init_cache_entry_map (entry_map);

  {
    CacheEntry *entry = NULL;

    char mykey0[] = "mykey0";
    char myval0[] = "myval0";
    char mykey1[] = "mykey1";
    char myval1[] = "myval1";
    char mykey2[] = "mykey2";
    char myval2[] = "myval2";

    set_cache_entry (entry_map,
                     (u8 *) mykey0, strlen (mykey0),
                     (u8 *) myval0, strlen (myval0));
    set_cache_entry (entry_map,
                     (u8 *) mykey1, strlen (mykey1),
                     (u8 *) myval1, strlen (myval1));
    set_cache_entry (entry_map,
                     (u8 *) mykey2, strlen (mykey2),
                     (u8 *) myval2, strlen (myval2));
    set_cache_entry (entry_map,
                     (u8 *) mykey1, strlen (mykey1),
                     (u8 *) myval1, strlen (myval1));
    
    entry = lock_and_get_cache_entry (entry_map, (u8 *) "mykey", strlen ("mykey"));
    assert (entry == NULL);
    entry = lock_and_get_cache_entry (entry_map, (u8 *) "mykey1", strlen ("mykey1"));
    assert (entry != NULL);
    assert (atomic_flag_test_and_set (&entry->guard));

    {
      u8 key[entry->klen + 1];
      u8 val[entry->vlen + 1];
      memcpy (key, entry->k, entry->klen);
      memcpy (val, entry->v, entry->vlen);
      key[entry->klen] = '\0';
      val[entry->vlen] = '\0';
      printf ("GOT \"%s\" => \"%s\"\n", key, val);
    }

    UNLOCK_ENTRY (entry);
    assert (!atomic_flag_test_and_set (&entry->guard));
    UNLOCK_ENTRY (entry);
  }

  ////////////////////////////////////////
  // Clean up
  if (0 != munmap (memory, length))
    {
      fprintf (stderr, "Failed to unmap %lu bytes: %s\n", length, strerror (errno));
      return EXIT_FAILURE;
    }

  return EXIT_SUCCESS;
}
