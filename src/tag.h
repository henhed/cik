#ifndef TAG_H
#define TAG_H 1

#include "types.h"

void associate_key_with_tag (CacheTag, CacheKey);
void clear_entries_matching_all_tags    (CacheTag *, u8);
void clear_entries_matching_any_tag     (CacheTag *, u8);
void clear_entries_not_matching_any_tag (CacheTag *, u8);
void debug_print_tags       (int);

#endif /* ! TAG_H */
