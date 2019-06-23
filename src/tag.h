#ifndef TAG_H
#define TAG_H 1

#include "types.h"

void     add_key_to_tag               (CacheTag, CacheKey);
void     remove_key_from_tag          (CacheTag, CacheKey);
void     walk_all_tags                (CacheTagWalkCb, void *);
KeyElem *get_keys_matching_any_tag    (CacheTag *, u8); // A.K.A. union
KeyElem *get_keys_matching_all_tags   (CacheTag *, u8); // A.K.A. intersection
void     release_key_list             (KeyElem *);
void     debug_print_tags             (int);

#endif /* ! TAG_H */
