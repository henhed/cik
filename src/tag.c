#include <stdlib.h>
#include <string.h>

#include "tag.h"
#include "memory.h"
#include "print.h"
#include "util.h"

#define LOCK_KEYS(t) \
  do {} while (atomic_flag_test_and_set_explicit (&(t)->keys_lock, memory_order_acquire))
#define UNLOCK_KEYS(t) \
  atomic_flag_clear_explicit (&(t)->keys_lock, memory_order_release)
#define TRY_LOCK_KEYS(t) \
  (!atomic_flag_test_and_set_explicit (&(t)->keys_lock, memory_order_acquire))

#define LOCK_KEYS_AND_LOG_SPIN(t)                                         \
  do {                                                                    \
    if (!TRY_LOCK_KEYS (t))                                               \
      {                                                                   \
        err_print ("SPINNING \"%.*s\"\n", (t)->tag.nmemb, (t)->tag.base); \
        LOCK_KEYS (t);                                                    \
        err_print ("GOT LOCK \"%.*s\"\n", (t)->tag.nmemb, (t)->tag.base); \
      }                                                                   \
  } while (0)

#define TAG_NODE_LEFT(t)  atomic_load (&(t)->left)
#define TAG_NODE_RIGHT(t) atomic_load (&(t)->right)

static u8 root_value[] = { 'R', 'O', 'O', 'T' };
static TagNode root = {
  .tag.base  = root_value,
  .tag.nmemb = sizeof (root_value),
  .left      = NULL,
  .right     = NULL
};

static bool
keys_are_equal (CacheKey a, CacheKey b)
{
  if (a.nmemb != b.nmemb)
    return false;
  if (a.base == b.base)
    return true;
  return (0 == memcmp (a.base, b.base, a.nmemb)) ? true : false;
}

static KeyElem *
create_key_elem (CacheKey key)
{
  KeyElem *elem;
  elem = reserve_memory (sizeof (KeyElem) + (sizeof (u8) * key.nmemb));
  if (elem)
    {
      *elem = (KeyElem) { 0 };
      elem->key.base = (u8 *) (elem + 1);
      elem->key.nmemb = key.nmemb;
      memcpy (elem->key.base, key.base, key.nmemb);
    }
  return elem;
}

static TagNode *
create_tag_node (CacheTag tag)
{
  TagNode *node;
  node = reserve_memory (sizeof (TagNode) + (sizeof (u8) * tag.nmemb));
  cik_assert (node != NULL);
  *node = (TagNode) { 0 };
  node->tag.base = (u8 *) (node + 1);
  node->tag.nmemb = tag.nmemb;
  memcpy (node->tag.base, tag.base, tag.nmemb);
  node->keys_lock = (atomic_flag) ATOMIC_FLAG_INIT;
  return node;
}

static bool
insert_if_unique (KeyElem **list, CacheKey key)
{
  KeyElem *elem;

  cik_assert (list != NULL);

  for (elem = *list; elem != NULL; elem = elem->next)
    {
      if (keys_are_equal (elem->key, key))
        return false;
    }

  elem = create_key_elem (key);
  cik_assert (elem != NULL);

  if (elem != NULL)
    {
      elem->next = *list;
      *list = elem;
      return true;
    }

  return false;
}

static int
compare_tags (CacheTag a, CacheTag b)
{
  int result;
  u8  size = a.nmemb > b.nmemb ? b.nmemb : a.nmemb;
  if (size == 0)
    result = 0;
  else
    result = memcmp (a.base, b.base, size);
  if (result == 0)
    {
      if (a.nmemb < b.nmemb)
        return -1;
      else if (b.nmemb < a.nmemb)
        return 1;
    }
  return result;
}

static TagNode *
get_or_create_node (CacheTag tag)
{
  TagNode *parent = &root;

  for (;;)
    {
      int diff = compare_tags (tag, parent->tag);
      if (diff < 0)
        {
          TagNode *left = TAG_NODE_LEFT (parent);
          if (left == NULL)
            {
              TagNode *expected = NULL;
              left = create_tag_node (tag);
              if (!atomic_compare_exchange_strong (&parent->left, &expected, left))
                {
                  // We lost a race, release node
                  release_memory (left);
                  left = TAG_NODE_LEFT (parent);
                  dbg_print ("Race to add tag: %.*s\n", tag.nmemb, tag.base);
                  cik_assert (left != NULL);
                }
            }
          parent = left;
        }
      else if (diff > 0)
        {
          TagNode *right = TAG_NODE_RIGHT (parent);
          if (right == NULL)
            {
              TagNode *expected = NULL;
              right = create_tag_node (tag);
              if (!atomic_compare_exchange_strong (&parent->right, &expected, right))
                {
                  // We lost a race, release node
                  release_memory (right);
                  right = TAG_NODE_RIGHT (parent);
                  dbg_print ("Race to add tag: %.*s\n", tag.nmemb, tag.base);
                  cik_assert (right != NULL);
                }
            }
          parent = right;
        }
      else
        {
          return parent;
        }
    }

  cik_assert (false);
  return NULL;
}

static TagNode *
get_tag_if_exists (CacheTag tag)
{
  TagNode *current = &root;

  while (current)
    {
      int diff = compare_tags (tag, current->tag);
      if (diff == 0)
        return current;

      if (diff < 0)
        current = TAG_NODE_LEFT (current);
      else
        current = TAG_NODE_RIGHT (current);
    }

  return NULL;
}

static KeyElem *
copy_key_list (KeyElem *list)
{
  KeyElem *new_list = NULL;
  for (KeyElem **elem = &list, **copy = &new_list;
       *elem;
       elem = &(*elem)->next, copy = &(*copy)->next)
    {
      *copy = create_key_elem ((*elem)->key);
    }
  return new_list;
}

static KeyElem *
get_key_list_copy_by_tag (CacheTag tag)
{
  KeyElem *copy = NULL;
  TagNode *node = get_tag_if_exists (tag);
  if (!node)
    return NULL;
  LOCK_KEYS_AND_LOG_SPIN (node);
  copy = copy_key_list (node->keys);
  UNLOCK_KEYS (node);
  return copy;
}

void
add_key_to_tag (CacheTag tag, CacheKey key)
{
  TagNode *node = get_or_create_node (tag);

  cik_assert (node != NULL);

  if (node == NULL)
    return;

  LOCK_KEYS_AND_LOG_SPIN (node);

  insert_if_unique (&node->keys, key);

  UNLOCK_KEYS (node);
}

void
remove_key_from_tag (CacheTag tag, CacheKey key)
{
  TagNode *node = get_tag_if_exists (tag);
  if (node == NULL)
    return;

  LOCK_KEYS_AND_LOG_SPIN (node);

  for (KeyElem **elem = &node->keys; *elem; elem = &(*elem)->next)
    {
      if (keys_are_equal ((*elem)->key, key))
        {
          KeyElem *found = *elem;
          *elem = found->next;
          release_memory (found);
          break; // We assume there are no dupes (`insert_if_unique').
        }
    }

  UNLOCK_KEYS (node);
}

void
walk_all_tags (CacheTagWalkCb callback, void *user_data)
{
  u32 stack_cap = 1024; // @Incomplete: This will not be enough
  u32 stack_nmemb = 0;
  TagNode *stack[stack_cap];
  TagNode *current = &root;

  cik_assert (callback);

  while ((current != NULL) || (stack_nmemb > 0))
    {
      while (current != NULL)
        {
          cik_assert (stack_nmemb < stack_cap);
          stack[stack_nmemb++] = current;
          current = TAG_NODE_LEFT (current);
        }

      cik_assert (stack_nmemb > 0);
      current = stack[--stack_nmemb];

      LOCK_KEYS_AND_LOG_SPIN (current);
      bool has_keys = (current->keys != NULL);
      UNLOCK_KEYS (current);
      if (has_keys)
        {
          bool result = callback (current->tag, user_data);
          (void) result;
          // Return value ignored for now. `false' could mean `remove' like it
          // does for entry walk but that would add a whole lot of complexity
          // with regards to MT-safety. At the moment we never remove any tags
          // or balance the tree.
        }

      current = TAG_NODE_RIGHT (current);
    }
}

void
release_key_list (KeyElem *elem)
{
  while (elem)
    {
      KeyElem *next = elem->next;
      release_memory (elem);
      elem = next;
    }
}

KeyElem *
get_keys_matching_any_tag (CacheTag *tags, u8 ntags)
{
  KeyElem *found_keys = NULL;

  if (ntags == 0)
    return NULL;

  cik_assert (tags != NULL);

  found_keys = get_key_list_copy_by_tag (tags[0]);

  for (u8 t = 1; t < ntags; ++t)
    {
      KeyElem *keys = get_key_list_copy_by_tag (tags[t]);

      for (KeyElem **tag_key = &keys;
           *tag_key;
           tag_key = &(*tag_key)->next)
        {
          insert_if_unique (&found_keys, (*tag_key)->key);
        }

      release_key_list (keys);
    }

  return found_keys;
}

KeyElem *
get_keys_matching_all_tags (CacheTag *tags, u8 ntags)
{
  KeyElem *found_keys = NULL;

  if (ntags == 0)
    return NULL;

  cik_assert (tags != NULL);

  found_keys = get_key_list_copy_by_tag (tags[0]);

  for (u8 t = 1; t < ntags; ++t)
    {
      KeyElem *keys = get_key_list_copy_by_tag (tags[t]);

      for (KeyElem **found_key = &found_keys;
           *found_key;
           found_key = &(*found_key)->next)
        {
          bool match;
        skip_advancement:
          match = false;
          for (KeyElem **tag_key = &keys;
               *tag_key;
               tag_key = &(*tag_key)->next)
            {
              if (keys_are_equal ((*found_key)->key, (*tag_key)->key))
                {
                  match = true;
                  break;
                }
            }
          if (!match)
            {
              KeyElem *not_found = *found_key;
              *found_key = not_found->next;
              release_memory (not_found);
              if (*found_key)
                goto skip_advancement;
              else
                break;
            }
        }

      release_key_list (keys);
    }

  return found_keys;
}

#if 0 // Print tree
static void
debug_print_tag (int fd, TagNode *node, u32 depth)
{
  u32 nentries = 0;
  dprintf (fd, "%*s'%.*s'",
           depth * 2,
           "* ",
           node->tag.nmemb,
           node->tag.base);
  for (KeyElem **list = &node->keys; *list != NULL; list = &(*list)->next)
    {
      if (++nentries < 2)
        dprintf (fd, " => '%.*s'",
                 (*list)->key.nmemb,
                 (*list)->key.base);
    }
  if (nentries > 2)
    dprintf (fd, " (+ %u more)", nentries);
  dprintf (fd, "\n");
  if (node->left)
    debug_print_tag (fd, node->left, depth + 1);
  if (node->right)
    debug_print_tag (fd, node->right, depth + 1);
}
#endif

typedef struct
{
  CacheTag tag;
  u32 tree_depth;
  u32 num_keys;
} DebugTag;

static int
cmp_debug_tag (const void *_a, const void *_b)
{
  const DebugTag *a = _a;
  const DebugTag *b = _b;
  if (a->num_keys < b->num_keys)
    return 1;
  else if (b->num_keys < a->num_keys)
    return -1;
  return 0;
}

void
debug_print_tags (int fd)
{
  u32 debug_tag_cap   = 1024; // @Incomplete: This will not be enough
  u32 debug_tag_nmemb = 0;
  DebugTag debug_tags[debug_tag_cap];

  u32 stack_cap = 1024; // @Incomplete: This will not be enough
  u32 stack_nmemb = 0;
  TagNode *stack[stack_cap];
  TagNode *current = &root;

  while ((current != NULL) || (stack_nmemb > 0))
    {
      while (current != NULL)
        {
          cik_assert (stack_nmemb < stack_cap);
          stack[stack_nmemb++] = current;
          current = TAG_NODE_LEFT (current);
        }

      cik_assert (stack_nmemb > 0);
      current = stack[--stack_nmemb];

      {
        DebugTag *dt = NULL;
        cik_assert (debug_tag_nmemb < debug_tag_cap);

        dt = &debug_tags[debug_tag_nmemb++];

        dt->tag        = current->tag;
        dt->tree_depth = stack_nmemb;
        dt->num_keys   = 0;

        LOCK_KEYS_AND_LOG_SPIN (current);
        for (KeyElem **elem = &current->keys; *elem; elem = &(*elem)->next)
          ++dt->num_keys;
        UNLOCK_KEYS (current);
      }

      current = TAG_NODE_RIGHT (current);
    }

  qsort (debug_tags, debug_tag_nmemb, sizeof (DebugTag), cmp_debug_tag);

  int count = 0;
  count = dprintf (fd, "TAGS (%u) ", debug_tag_nmemb);
  dprintf (fd, "%.*s\n", LINEWIDTH - count, HLINESTR);

  dprintf (fd, "NAME %66s  %s\n", "KEYS", "DEPTH");
  for (u32 i = 0; i < debug_tag_nmemb; ++i)
    {
      DebugTag *dt = &debug_tags[i];
      dprintf (fd, "%-64s %6u %6u\n",
               tag2str (dt->tag), dt->num_keys, dt->tree_depth);

      if (i == 9)
        break; // Only print top 10
    }

  dprintf (fd, "\n");

#if 0 // Print tree
  debug_print_tag (fd, &root, 2);
#endif
}
