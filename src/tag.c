#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "memory.h"
#include "tag.h"
#include "util.h"

#define LOCK_KEYS(t) \
  do {} while (atomic_flag_test_and_set_explicit (&(t)->keys_lock, memory_order_acquire))
#define UNLOCK_KEYS(t) \
  atomic_flag_clear_explicit (&(t)->keys_lock, memory_order_release)
#define TRY_LOCK_KEYS(t) \
  (!atomic_flag_test_and_set_explicit (&(t)->keys_lock, memory_order_acquire))

#define LOCK_KEYS_AND_LOG_SPIN(t)                                       \
  do {                                                                  \
    if (!TRY_LOCK_KEYS (t))                                             \
      {                                                                 \
        err_print ("SPINNING \"%s\"\n", tag2str ((t)->tag));            \
        LOCK_KEYS (t);                                                  \
        err_print ("GOT LOCK \"%s\"\n", tag2str ((t)->tag));            \
      }                                                                 \
  } while (0)

#define TAG_NODE_LEFT(t)  atomic_load (&(t)->left)
#define TAG_NODE_RIGHT(t) atomic_load (&(t)->right)

static u8 root_value[] = { 'R', 'O', 'O', 'T' };
static TagNode root = {
  .tag.base  = root_value,
  .tag.nmemb = sizeof (root_value),
  .keys      = NULL,
  .keys_lock = ATOMIC_FLAG_INIT,
  .num_keys  = ATOMIC_VAR_INIT (0),
  .left      = ATOMIC_VAR_INIT (NULL),
  .right     = ATOMIC_VAR_INIT (NULL)
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
      *elem = (KeyElem) {};
      elem->key.base = (u8 *) (elem + 1);
      elem->key.nmemb = key.nmemb;
      memcpy (elem->key.base, key.base, key.nmemb);
      elem->next = NULL;
    }
  return elem;
}

static TagNode *
create_tag_node (CacheTag tag)
{
  TagNode *node;
  node = reserve_memory (sizeof (TagNode) + (sizeof (u8) * tag.nmemb));
  cik_assert (node != NULL); // @Incomplete: Bubble out-of-memory status
  *node = (TagNode) {};
  node->tag.base = (u8 *) (node + 1);
  node->tag.nmemb = tag.nmemb;
  memcpy (node->tag.base, tag.base, tag.nmemb);
  node->keys = NULL;
  node->keys_lock = (atomic_flag) ATOMIC_FLAG_INIT;
  atomic_init (&node->num_keys, 0);
  atomic_init (&node->left, NULL);
  atomic_init (&node->right, NULL);
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

  if (insert_if_unique (&node->keys, key))
    atomic_fetch_add_explicit (&node->num_keys, 1, memory_order_relaxed);

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
          atomic_fetch_sub_explicit (&node->num_keys, 1, memory_order_relaxed);
          break; // We assume there are no dupes (`insert_if_unique').
        }
    }

  UNLOCK_KEYS (node);
}

void
walk_all_tags (CacheTagWalkCb callback, void *user_data)
{
  struct TagElem {
    TagNode *node;
    struct TagElem *next;
  };

  struct TagElem *stack = NULL;
  TagNode *current = &root;

  cik_assert (callback);

  while ((current != NULL) || (stack != NULL))
    {
      struct TagElem *tmp;

      while (current != NULL)
        {
          tmp = reserve_memory (sizeof (struct TagElem));
          cik_assert (tmp != NULL);
          tmp->node = current;
          tmp->next = stack;
          stack = tmp;
          current = TAG_NODE_LEFT (current);
        }

      cik_assert (stack != NULL);
      current = stack->node;
      tmp = stack;
      stack = tmp->next;
      release_memory (tmp);

      LOCK_KEYS_AND_LOG_SPIN (current);
      bool has_keys = (current->keys != NULL);
      UNLOCK_KEYS (current);
      if (has_keys)
        callback (current->tag, user_data);

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

#define MAX_DEBUG_TAG_DEPTH 1024 // Recursion overflow protection

static void
write_tag_tree_stats (TagNode *node, u32 depth, int fd)
{
  u32 num_keys = 0;

  cik_assert (depth <= MAX_DEBUG_TAG_DEPTH);
  if (node == NULL)
    return;

  if (depth > MAX_DEBUG_TAG_DEPTH)
    {
      err_print ("Max recursion depth exceeded (%u)\n", MAX_DEBUG_TAG_DEPTH);
      return;
    }

  LOCK_KEYS_AND_LOG_SPIN (node);
  for (KeyElem **elem = &node->keys; *elem; elem = &(*elem)->next)
    ++num_keys;
  UNLOCK_KEYS (node);

  dprintf (fd, "%u\t%u\t%s\n",
           atomic_load (&node->num_keys), depth, tag2str (node->tag));

  write_tag_tree_stats (TAG_NODE_LEFT  (node), depth + 1, fd);
  write_tag_tree_stats (TAG_NODE_RIGHT (node), depth + 1, fd);
}

void
write_tag_stats (int fd)
{
  dprintf (fd, "%s\t%s\t%s\n", "Keys", "Depth", "Tag");
  write_tag_tree_stats (TAG_NODE_LEFT  (&root), 1, fd);
  write_tag_tree_stats (TAG_NODE_RIGHT (&root), 1, fd);
}
