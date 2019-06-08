#include <stdio.h>
#include <string.h>

#include "tag.h"
#include "memory.h"

#if DEBUG
# include <assert.h>
#endif

static u8 root_value = 0x4F; // Midddle of ASCII printable characters
static TagNode root = {
  .tag.base = &root_value,
  .tag.nmemb = 1,
  .left = NULL,
  .right = NULL
};

static bool
keys_are_equal (CacheKey a, CacheKey b)
{
  if (a.nmemb != b.nmemb)
    return false;
  if (a.base == b.base)
    return true;
  return (0 == memcmp (a.base, b.base, a.nmemb));
}

static KeyNode *
create_key_node (CacheKey key)
{
  KeyNode *node;
  node = reserve_memory (sizeof (KeyNode) + (sizeof (u8) * key.nmemb));
  if (node)
    {
      *node = (KeyNode) { 0 };
      node->key.base = (u8 *) (node + 1);
      node->key.nmemb = key.nmemb;
      memcpy (node->key.base, key.base, key.nmemb);
    }
  return node;
}

static bool
insert_if_unique (KeyNode **list, CacheKey key)
{
  KeyNode *node;
#if DEBUG
  assert (list != NULL);
#endif

  for (node = *list; node != NULL; node = node->next)
    {
      if (keys_are_equal (node->key, key))
        return false;
    }

  node = create_key_node (key);
#if DEBUG
  assert (node != NULL);
#endif

  if (node != NULL)
    {
      node->next = *list;
      *list = node;
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
get_or_create_node (TagNode *parent, CacheTag tag)
{
#if DEBUG
  assert (parent);
#endif

  for (;;)
    {
      int diff = compare_tags (tag, parent->tag);
      if (diff == 0)
        return parent;

      if (((diff < 0) && (parent->left == NULL))
          || ((diff > 0) && (parent->right == NULL)))
        {
          TagNode *node;
          node = reserve_memory (sizeof (TagNode) + (sizeof (u8) * tag.nmemb));
          assert (node);
          *node = (TagNode) { 0 };
          node->tag.base = (u8 *) (node + 1);
          node->tag.nmemb = tag.nmemb;
          memcpy (node->tag.base, tag.base, tag.nmemb);

          if (diff < 0)
            parent->left = node;
          else
            parent->right = node;

          return node;
        }

      if (diff < 0)
        parent = parent->left;
      else
        parent = parent->right;
    }

#if DEBUG
  assert (false);
#endif
  return NULL;
}

static KeyNode *
get_keys_by_tag (CacheTag tag)
{
  TagNode *current = &root;

  while (current)
    {
      int diff = compare_tags (tag, current->tag);
      if (diff == 0)
        return current->keys;

      if (diff < 0)
        current = current->left;
      else
        current = current->right;
    }

  return NULL;
}

static KeyNode *
copy_key_list (KeyNode *list)
{
  KeyNode *new_list = NULL;
  for (
       KeyNode **node = &list, **copy = &new_list;
       *node;
       node = &(*node)->next, copy = &(*copy)->next)
    {
      *copy = create_key_node ((*node)->key);
    }
  return new_list;
}

void
associate_key_with_tag (CacheTag tag, CacheKey key)
{
  // @Incomplete: MT-safety!
  TagNode *node = get_or_create_node (&root, tag);
#if DEBUG
  assert (node != NULL);
#endif
  insert_if_unique (&node->keys, key);
}

void
release_key_list (KeyNode *node)
{
  while (node)
    {
      KeyNode *next = node->next;
      release_memory (node);
      node = next;
    }
}

KeyNode *
get_keys_matching_any_tag (CacheTag *tags, u8 ntags)
{
  // @Incomplete: MT-safety!

#if DEBUG
  assert (tags != NULL);
#endif

  KeyNode *found_keys = NULL;

  if (ntags == 0)
    return NULL;

  found_keys = get_keys_by_tag (tags[0]); // Maybe `get_and_lock_keys_by_tag'
  found_keys = copy_key_list   (found_keys); // .. then release lock after copy

  for (u8 t = 1; t < ntags; ++t)
    {
      KeyNode *keys = get_keys_by_tag (tags[t]); // .. and lock here too

      for (KeyNode **tag_key = &keys;
           *tag_key;
           tag_key = &(*tag_key)->next)
        {
          insert_if_unique (&found_keys, (*tag_key)->key);
        }
    }

  return found_keys;
}

KeyNode *
get_keys_matching_all_tags (CacheTag *tags, u8 ntags)
{
  // @Incomplete: MT-safety!

#if DEBUG
  assert (tags != NULL);
#endif

  KeyNode *found_keys = NULL;

  if (ntags == 0)
    return NULL;

  found_keys = get_keys_by_tag (tags[0]); // Maybe `get_and_lock_keys_by_tag'
  found_keys = copy_key_list   (found_keys); // .. then release lock after copy

  for (u8 t = 1; t < ntags; ++t)
    {
      KeyNode *keys = get_keys_by_tag (tags[t]); // .. and lock here too

      for (KeyNode **found_key = &found_keys;
           *found_key;
           found_key = &(*found_key)->next)
        {
          bool match;
        skip_advancement:
          match = false;
          for (KeyNode **tag_key = &keys;
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
              KeyNode *not_found = *found_key;
              *found_key = not_found->next;
              release_memory (not_found);
              if (*found_key)
                goto skip_advancement;
              else
                break;
            }
        }
    }

  return found_keys;
}

void
clear_entries_matching_any_tag (CacheTag *tags, u8 ntags)
{
  (void) tags;
  (void) ntags;
  fprintf (stderr, "%s: NOT IMPLEMENTED\n", __FUNCTION__);
}

void
clear_entries_not_matching_any_tag (CacheTag *tags, u8 ntags)
{
  (void) tags;
  (void) ntags;
  fprintf (stderr, "%s: NOT IMPLEMENTED\n", __FUNCTION__);
}

static void
debug_print_tag (int fd, TagNode *node, u32 depth)
{
  u32 nentries = 0;
  dprintf (fd, "%*s'%.*s'\n",
           depth * 2,
           "* ",
           node->tag.nmemb,
           node->tag.base);
  for (KeyNode **list = &node->keys; *list != NULL; list = &(*list)->next)
    {
      if (++nentries < 3)
        dprintf (fd, "%*s'%.*s'\n",
                 (depth + 1) * 2,
                 "- ",
                 (*list)->key.nmemb,
                 (*list)->key.base);
    }
  if (nentries > 2)
    dprintf (fd, "%*s %u more ..\n",
             (depth + 1) * 2,
             "- ",
             nentries - 2);
  /* u32 nentries = 0; */
  /* dprintf (fd, "%*s'%.*s'", */
  /*          depth * 2, */
  /*          "* ", */
  /*          node->tag.nmemb, */
  /*          node->tag.base); */
  /* for (KeyNode **list = &node->keys; *list != NULL; list = &(*list)->next) */
  /*   { */
  /*     if (++nentries < 2) */
  /*       dprintf (fd, " => '%.*s'", */
  /*                (*list)->key.nmemb, */
  /*                (*list)->key.base); */
  /*   } */
  /* if (nentries > 2) */
  /*   dprintf (fd, " (+ %u more)", nentries); */
  /* dprintf (fd, "\n"); */
  if (node->left)
    debug_print_tag (fd, node->left, depth + 1);
  if (node->right)
    debug_print_tag (fd, node->right, depth + 1);
}

void
debug_print_tags (int fd)
{
  dprintf (fd, "TAGS:\n");
  debug_print_tag (fd, &root, 2);
}
