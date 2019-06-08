#ifndef TAG_H
#define TAG_H 1

#include "types.h"

void     associate_key_with_tag       (CacheTag, CacheKey);
KeyNode *get_keys_matching_any_tag    (CacheTag *, u8); // A.K.A. union
KeyNode *get_keys_matching_all_tags   (CacheTag *, u8); // A.K.A. intersection
void     release_key_list             (KeyNode *);
void     debug_print_tags             (int);

#endif /* ! TAG_H */
