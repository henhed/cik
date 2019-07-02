#include <string.h>

#include "controller.h"
#include "entry.h"
#include "memory.h"
#include "print.h"
#include "profiler.h"
#include "server.h"
#include "tag.h"
#include "util.h"

#if __BIG_ENDIAN__
# define htonll(x) (x)
# define ntohll(x) (x)
#else
# define htonll(x) (((u64) htonl ((x) & 0xFFFFFFFF) << 32) | htonl ((x) >> 32))
# define ntohll(x) (((u64) ntohl ((x) & 0xFFFFFFFF) << 32) | ntohl ((x) >> 32))
#endif

static inline CacheEntryHashMap *
get_map_for_key (CacheKey key)
{
  u32 map_index = 0;
  if (key.nmemb >= sizeof (u64))
    map_index = (*(u64 *) key.base) % NUM_CACHE_ENTRY_MAPS;
  else if (key.nmemb >= sizeof (u32))
    map_index = (*(u32 *) key.base) % NUM_CACHE_ENTRY_MAPS;
  else if (key.nmemb >= sizeof (u16))
    map_index = (*(u16 *) key.base) % NUM_CACHE_ENTRY_MAPS;
  else if (key.nmemb >= sizeof (u8))
    map_index = (*(u8 *)  key.base) % NUM_CACHE_ENTRY_MAPS;
  return entry_maps[map_index];
}

static StatusCode
read_request_key (Client *client, CacheKey key)
{
  StatusCode status;

  cik_assert (client != NULL);
  cik_assert (key.base != NULL);

  status = read_request_payload (client, key.base, key.nmemb);
  if (status != STATUS_OK)
    return status;

  // Both keys and tags tend to be prefixed and so in general they have more
  // entropy at the end. Hence we store them in reverse byte order to improve
  // hash map distribution and early exit memcmp.
  reverse_bytes (key.base, key.nmemb);

  return STATUS_OK;
}

static StatusCode
read_tags_using_payload_buffer (Client *client, CacheTag *tags, u8 ntags)
{
  StatusCode status;
  u8        *buffer;
  u32        buffer_cap;

  buffer = client->worker->payload_buffer.base;
  buffer_cap = client->worker->payload_buffer.cap;

  for (u8 t = 0; t < ntags; ++t)
    {
      CacheTag *tag = &tags[t];
      status = read_request_payload (client, &tag->nmemb, sizeof (tag->nmemb));
      if (status != STATUS_OK)
        return status;
      if (tag->nmemb > buffer_cap)
        return STATUS_OUT_OF_MEMORY; // @Cleanup: Drain input stream
      status = read_request_payload (client, buffer, tag->nmemb);
      if (status != STATUS_OK)
        return status;
      tag->base = buffer;
      buffer += tag->nmemb;
      buffer_cap -= tag->nmemb;

      reverse_bytes (tag->base, tag->nmemb);
    }

  client->worker->payload_buffer.nmemb = (client->worker->payload_buffer.cap
                                          - buffer_cap);

  return STATUS_OK;
}

static StatusCode
handle_get_request (Client *client, Request *request, Payload **response_payload)
{
  PROFILE (PROF_HANDLE_GET_REQUEST);

  StatusCode status;
  CacheEntry *entry          = NULL;
  Payload    *payload_buffer = &client->worker->payload_buffer;

  u8 klen  = request->g.klen;
  u8 flags = request->g.flags;

  CacheKey key;
  u8 tmp_key_data[0xFF];

  key.base  = tmp_key_data;
  key.nmemb = klen;

  status = read_request_key (client, key);
  if (status != STATUS_OK)
    return status;

  entry = lock_and_get_cache_entry (get_map_for_key (key), key);
  if (!entry)
    {
      dbg_print (RED ("GET") "[%X]: '%s'\n", client->worker->id, key2str (key));
      ++client->counters.get_miss;
      return STATUS_NOT_FOUND;
    }

  dbg_print (GREEN ("GET") "[%X]: '%s'\n", client->worker->id, key2str (key));

  if (~flags & GET_FLAG_IGNORE_EXPIRES)
    {
      if (entry->expires != CACHE_EXPIRES_INIT)
        {
          time_t now = time (NULL);
          if (entry->expires < now)
            {
              UNLOCK_ENTRY (entry);
              return STATUS_EXPIRED;
            }
        }
    }

  ++client->counters.get_hit;

  if (entry->value.nmemb > payload_buffer->cap)
    {
      UNLOCK_ENTRY (entry);
      return STATUS_BUG; // We should always have a buffer big enough
    }

  if (entry->value.nmemb > 0)
    {
      // We copy the entry value to a payload buffer so we don't have to keep
      // the entity locked while writing it's data to the client.
      payload_buffer->nmemb = entry->value.nmemb;
      memcpy (payload_buffer->base, entry->value.base, payload_buffer->nmemb);
      *response_payload = payload_buffer;
    }

  UNLOCK_ENTRY (entry);

  return STATUS_OK;
}

static StatusCode
handle_set_request (Client *client, Request *request)
{
  PROFILE (PROF_HANDLE_SET_REQUEST);

  StatusCode status;
  CacheEntry *entry = NULL, *old_entry = NULL;

  u8  klen  = request->s.klen;
  u32 tlen  = 0;
  u32 vlen  = ntohl (request->s.vlen);
  u8  ntags = request->s.ntags;
  u32 ttl   = ntohl (request->s.ttl);
  u8  flags = request->s.flags;

  u8      *payload;
  u8       tmp_key_data[0xFF];
  size_t   total_size;
  CacheTag tags[ntags];
  CacheKey key = { .base = tmp_key_data, .nmemb = klen };

  // Read key
  status = read_request_key (client, key);

  ++client->counters.set;

  dbg_print (BLUE ("SET") "[%X]: '%s'\n", client->worker->id, key2str (key));

  // @Revisit: Use key here and look if we have an existing entry
  // in the hash table already. If so, reuse it's memory if possible.
  // Right now we're always reserving new memory and releasing the old.
  // But who knows, maybe reserving new memory will be faster in the end.

  if (flags & SET_FLAG_ONLY_TTL)
    {
      // Just renew expiry time for entry, ignore tags and value
      entry = lock_and_get_cache_entry (get_map_for_key (key), key);
      if (!entry)
        return STATUS_NOT_FOUND;

      if (ttl == (u32) -1)
        entry->expires = CACHE_EXPIRES_INIT;
      else
        entry->expires = time (NULL) + ttl;

      UNLOCK_ENTRY (entry);
      return STATUS_OK;
    }

  status = read_tags_using_payload_buffer (client, tags, ntags);
  if (status != STATUS_OK)
    return status;

  tlen = sizeof (tags);
  for (u8 t = 0; t < ntags; ++t)
    tlen += tags[t].nmemb;

  total_size = tlen + key.nmemb + vlen;

  entry = reserve_and_lock_entry (total_size);
  if (entry == NULL)
    return STATUS_OUT_OF_MEMORY;

  payload = (u8 *) (entry + 1);

  // Copy read tags into reserved entry payload
  entry->tags.base = (CacheTag *) payload;
  entry->tags.nmemb = ntags;
  payload += sizeof (tags);
  for (u8 t = 0; t < entry->tags.nmemb; ++t)
    {
      CacheTag *tag = &entry->tags.base[t];
      tag->base = payload;
      tag->nmemb = tags[t].nmemb;
      memcpy (tag->base, tags[t].base, tag->nmemb);
      payload += tag->nmemb;
    }

  // Copy read key into reserved entry payload
  memcpy (payload, key.base, key.nmemb);
  entry->key.base = payload;
  entry->key.nmemb = key.nmemb;
  payload += key.nmemb;
  entry->value.base = payload;
  entry->value.nmemb = vlen;
  payload += vlen;

  cik_assert ((u32) (payload - (u8 *) (entry + 1)) == total_size);

  status = read_request_payload (client, entry->value.base, vlen);
  if (status != STATUS_OK)
    {
      UNLOCK_ENTRY (entry);
      release_memory (entry);
      return status;
    }

  entry->mtime = time (NULL);

  if (ttl != (u32) -1)
    entry->expires = entry->mtime + ttl;

  if (!set_locked_cache_entry (get_map_for_key (entry->key), entry, &old_entry))
    {
      cik_assert (old_entry == NULL);
      UNLOCK_ENTRY (entry);
      release_memory (entry);
      return STATUS_OUT_OF_MEMORY;
    }

  if (old_entry)
    {
      // @Speed: Only remove keys missing in new entry
      for (u8 t = 0; t < old_entry->tags.nmemb; ++t)
        remove_key_from_tag (old_entry->tags.base[t], old_entry->key);
      UNLOCK_ENTRY (old_entry);
      release_memory (old_entry);
    }

  // @Speed: Only add tags missing in old entry
  for (u8 t = 0; t < entry->tags.nmemb; ++t)
    add_key_to_tag (entry->tags.base[t], entry->key);

  UNLOCK_ENTRY (entry);

  return STATUS_OK;
}

static StatusCode
delete_entry_by_key (CacheKey key)
{
  CacheEntry *entry = NULL;

  dbg_print (YELLOW ("DEL") ": '%s'\n", key2str (key));

  // Unmap entry
  entry = lock_and_unset_cache_entry (get_map_for_key (key), key);
  if (!entry)
    return STATUS_NOT_FOUND;

  for (u8 t = 0; t < entry->tags.nmemb; ++t)
    remove_key_from_tag (entry->tags.base[t], entry->key);

  do
    {
      // Release memory. We loop untill we get NULL back from map. See note
      // about @Bug in `set_locked_cache_entry'.
      UNLOCK_ENTRY (entry);
      release_memory (entry);
      entry = lock_and_unset_cache_entry (get_map_for_key (key), key);
    }
  while (entry != NULL);

  return STATUS_OK;
}

static StatusCode
handle_del_request (Client *client, Request *request)
{
  PROFILE (PROF_HANDLE_DEL_REQUEST);

  StatusCode status;
  u8 tmp_key_data[0xFF];
  CacheKey key = {
    .base  = tmp_key_data,
    .nmemb = request->d.klen
  };

  status = read_request_key (client, key);
  if (status != STATUS_OK)
    return status;

  ++client->counters.del;

  return delete_entry_by_key (key);
}

static bool
clear_all_callback (CacheEntry *entry, void *user_data)
{
  (void) user_data;

  cik_assert (entry);

  dbg_print (YELLOW ("DEL") ": '%s'\n", key2str (entry->key));

  for (u8 t = 0; t < entry->tags.nmemb; ++t)
    remove_key_from_tag (entry->tags.base[t], entry->key);

  UNLOCK_ENTRY (entry);
  release_memory (entry);

  return true; // 'true' tells map to unset the entry
}

static bool
clear_old_callback (CacheEntry *entry, time_t *now)
{
  cik_assert (entry);
  cik_assert (now);

  if ((entry->expires == CACHE_EXPIRES_INIT)
      || (entry->expires >= *now))
    return false;

  return clear_all_callback (entry, NULL);
}

static bool
clear_non_matching_callback (CacheEntry *entry, CacheTagArray *tags)
{
  cik_assert (entry);
  cik_assert (tags);

  for (u8 i = 0; i < tags->nmemb; ++i)
    {
      CacheTag *want = &tags->base[i];
      for (u8 j = 0; j < entry->tags.nmemb; ++j)
        {
          CacheTag *have = &entry->tags.base[j];
          if ((want->nmemb == have->nmemb)
              && (memcmp (want->base, have->base, have->nmemb) == 0))
            {
              return false;
            }
        }
    }

  return clear_all_callback (entry, NULL);
}

static void
walk_all_entries (void *cb, void *data)
{
  for (u32 i = 0; i < NUM_CACHE_ENTRY_MAPS; ++i)
    walk_entries (entry_maps[i], (CacheEntryWalkCb) cb, data);
}

static StatusCode
handle_clr_request (Client *client, Request *request)
{
  PROFILE (PROF_HANDLE_CLR_REQUEST);

  StatusCode status;
  ClearMode  mode  = (ClearMode) request->c.mode;
  u8         ntags = request->c.ntags;
  CacheTag   tags[ntags];

  status = read_tags_using_payload_buffer (client, tags, ntags);
  if (status != STATUS_OK)
    return status;

  ++client->counters.clr;

  switch (mode)
    {
    case CLEAR_MODE_ALL:
      dbg_print (YELLOW ("CLR") "[%X]: (MATCH ALL)\n", client->worker->id);
      walk_all_entries (clear_all_callback, NULL);
      return STATUS_OK;
    case CLEAR_MODE_OLD:
      {
        time_t now = time (NULL);
        dbg_print (YELLOW ("CLR") "[%X]: (MATCH OLD)\n", client->worker->id);
        walk_all_entries (clear_old_callback, &now);
        return STATUS_OK;
      }
    case CLEAR_MODE_MATCH_NONE:
      {
        CacheTagArray tag_array = { .base = tags, .nmemb = ntags };
#if DEBUG
        dbg_print (YELLOW ("CLR") "[%X]: (MATCH NONE)", client->worker->id);
        for (u8 t = 0; t < ntags; ++t)
          dbg_print (" '%s'", tag2str (tags[t]));
        dbg_print ("\n");
#endif
        walk_all_entries (clear_non_matching_callback, &tag_array);
        return STATUS_OK;
      }
    case CLEAR_MODE_MATCH_ALL: // Intentional fallthrough
    case CLEAR_MODE_MATCH_ANY:
      {
        KeyElem *keys = NULL;
#if DEBUG
        dbg_print (YELLOW ("CLR") "[%X]: (MATCH %s)", client->worker->id,
                (mode == CLEAR_MODE_MATCH_ALL) ? "ALL" : "ANY");
        for (u8 t = 0; t < ntags; ++t)
          dbg_print (" '%s'", tag2str (tags[t]));
        dbg_print ("\n");
#endif
        keys = (mode == CLEAR_MODE_MATCH_ALL)
          ? get_keys_matching_all_tags (tags, ntags)
          : get_keys_matching_any_tag  (tags, ntags);

        for (KeyElem **key = &keys; *key; key = &(*key)->next)
          delete_entry_by_key ((*key)->key);
        release_key_list (keys);

        return STATUS_OK;
      }
    default:
      return STATUS_PROTOCOL_ERROR;
    }

  return STATUS_OK;
}

struct _ListAllKeysCallbackData
{
  StatusCode status;
  Payload   *payload;
};

static bool
list_all_keys_callback (CacheEntry *entry, struct _ListAllKeysCallbackData *data)
{
  CacheKey *key     = &entry->key;
  Payload  *payload = data->payload;

  if (data->status != STATUS_OK)
    return false;

  if ((payload->nmemb + 1 + key->nmemb) > payload->cap)
    {
      data->status = STATUS_OUT_OF_MEMORY;
      return false;
    }

  payload->base[payload->nmemb++] = key->nmemb;
  memcpy (&payload->base[payload->nmemb], key->base, key->nmemb);
  reverse_bytes (&payload->base[payload->nmemb], key->nmemb);
  payload->nmemb += key->nmemb;

  return false;
}

struct _ListNonMatchingKeysCallbackData
{
  struct _ListAllKeysCallbackData base;
  CacheTagArray tags;
};

static bool
list_non_matching_callback (CacheEntry *entry,
                            struct _ListNonMatchingKeysCallbackData *data)
{
  if (data->base.status != STATUS_OK)
    return false;

  for (u8 i = 0; i < data->tags.nmemb; ++i)
    {
      CacheTag *want = &data->tags.base[i];
      for (u8 j = 0; j < entry->tags.nmemb; ++j)
        {
          CacheTag *have = &entry->tags.base[j];
          if ((want->nmemb == have->nmemb)
              && (memcmp (want->base, have->base, have->nmemb) == 0))
            {
              return false;
            }
        }
    }

  return list_all_keys_callback (entry, &data->base);
}

struct _ListAllTagsCallbackData
{
  StatusCode status;
  Payload   *payload;
};

static void
list_all_tags_callback (CacheTag tag, struct _ListAllTagsCallbackData *data)
{
  Payload *payload = data->payload;

  if (data->status != STATUS_OK)
    return;

  if ((payload->nmemb + 1 + tag.nmemb) > payload->cap)
    {
      data->status = STATUS_OUT_OF_MEMORY;
      return;
    }

  payload->base[payload->nmemb++] = tag.nmemb;
  memcpy (&payload->base[payload->nmemb], tag.base, tag.nmemb);
  reverse_bytes (&payload->base[payload->nmemb], tag.nmemb);

  payload->nmemb += tag.nmemb;
}

static StatusCode
handle_lst_request (Client *client, Request *request, Payload **response_payload)
{
  StatusCode status = STATUS_OK;
  ListMode   mode   = (ListMode) request->c.mode;
  u8         ntags  = request->c.ntags;
  Payload   *buffer = &client->worker->payload_buffer;
  CacheTag   tags[ntags];

  status = read_tags_using_payload_buffer (client, tags, ntags);
  if (status != STATUS_OK)
    return status;

  ++client->counters.lst;

  switch (mode)
    {
    case LIST_MODE_ALL_KEYS:
      {
        struct _ListAllKeysCallbackData data = {
          .status = STATUS_OK,
          .payload = buffer
        };
        data.payload->nmemb = 0; // We don't care about input tags
        walk_all_entries (list_all_keys_callback, &data);
        *response_payload = data.payload;
        return data.status;
      }
    case LIST_MODE_ALL_TAGS:
      {
        struct _ListAllKeysCallbackData data = {
          .status = STATUS_OK,
          .payload = buffer
        };
        data.payload->nmemb = 0; // We don't care about input tags
        walk_all_tags ((CacheTagWalkCb) list_all_tags_callback, &data);
        *response_payload = data.payload;
        return data.status;
      }
    case LIST_MODE_MATCH_NONE:
      {
        // Make virtual payload since we've already used part of it for tags and
        // we need to pass them to our walk callback without them being overwritten.
        static thread_local Payload alt_payload;
        alt_payload = (Payload) {
          .base  = (buffer->base + buffer->nmemb),
          .nmemb = 0,
          .cap   = (buffer->cap - buffer->nmemb)
        };
        struct _ListNonMatchingKeysCallbackData data = {
          .base = {
            .status  = STATUS_OK,
            .payload = &alt_payload
          },
          .tags.base  = tags,
          .tags.nmemb = ntags
        };
        walk_all_entries (list_non_matching_callback, &data);
        *response_payload = data.base.payload;
        return data.base.status;
      }
    case LIST_MODE_MATCH_ALL: // Intentional fallthrough
    case LIST_MODE_MATCH_ANY:
      {
        KeyElem *list = (mode == LIST_MODE_MATCH_ALL)
          ? get_keys_matching_all_tags (tags, ntags)
          : get_keys_matching_any_tag  (tags, ntags);

        status = STATUS_OK;
        buffer->nmemb = 0; // We're done with `tags' now

        for (KeyElem **elem = &list; *elem; elem = &(*elem)->next)
          {
            CacheKey key = (*elem)->key;
            if ((buffer->nmemb + 1 + key.nmemb) > buffer->cap)
              {
                status = STATUS_OUT_OF_MEMORY;
                break;
              }

            buffer->base[buffer->nmemb++] = key.nmemb;
            memcpy (&buffer->base[buffer->nmemb], key.base, key.nmemb);
            reverse_bytes (&buffer->base[buffer->nmemb], key.nmemb);
            buffer->nmemb += key.nmemb;
          }

        release_key_list (list);

        *response_payload = buffer;
        return status;
      }
    default:
      return STATUS_PROTOCOL_ERROR;
    }

  return STATUS_BUG;
}

static StatusCode
handle_nfo_request (Client *client, Request *request, Payload **response_payload)
{
  StatusCode          status;
  NFOResponsePayload *nfo            = NULL;
  u8                  klen           = request->n.klen;
  Payload            *payload_buffer = &client->worker->payload_buffer;

  nfo = (NFOResponsePayload *) payload_buffer->base;
  payload_buffer->nmemb = sizeof (*nfo);
  *response_payload = payload_buffer;

  ++client->counters.nfo;

  if (klen > 0)
    {
      CacheEntry *entry = NULL;
      CacheKey    key;
      u8          tmp_key_data[0xFF];
      u8         *tag_data = nfo->entry.stream_of_tags;

      key.base  = tmp_key_data;
      key.nmemb = klen;

      status = read_request_key (client, key);
      if (status != STATUS_OK)
        return status;

      entry = lock_and_get_cache_entry (get_map_for_key (key), key);
      if (!entry)
        return STATUS_NOT_FOUND;

      nfo->entry.expires = htonll (entry->expires);
      nfo->entry.mtime   = htonll (entry->mtime);

      for (u8 t = 0; t < entry->tags.nmemb; ++t)
        {
          CacheTag *tag = &entry->tags.base[t];

          if ((payload_buffer->nmemb + 1 + tag->nmemb) > payload_buffer->cap)
            {
              UNLOCK_ENTRY (entry);
              return STATUS_BUG; // We should always have a buffer big enough
            }

          *(tag_data++) = tag->nmemb;
          memcpy (tag_data, tag->base, tag->nmemb);
          tag_data += tag->nmemb;
          payload_buffer->nmemb += 1 + tag->nmemb;
        }

      UNLOCK_ENTRY (entry);
      return STATUS_OK;
    }
  else
    {
      // @Incomplete: Filling percentage etc.
      dbg_print (RED ("NFO") "[%X]: Not implemented for empty tag\n",
                 client->worker->id);
      return STATUS_BUG;
    }

  return STATUS_BUG;
}

StatusCode
handle_request (Client *client, Request *request, Payload **response_payload)
{
  PROFILE (PROF_HANDLE_REQUEST);

  StatusCode status;
  Worker *worker;
  u64 start_tick;

  if (!client || !request || !response_payload)
    return STATUS_BUG;

  worker = client->worker;
  if (!worker)
    return STATUS_BUG;

  if ((request->cik[0] != CONTROL_BYTE_1)
      || (request->cik[1] != CONTROL_BYTE_2)
      || (request->cik[2] != CONTROL_BYTE_3))
    return STATUS_PROTOCOL_ERROR;

  *response_payload = NULL;

  start_tick = get_performance_counter ();

  switch (request->op)
    {
    case CMD_BYTE_GET:
      {
        status = handle_get_request (client, request, response_payload);
        worker->timers.get   += (get_performance_counter () - start_tick);
        worker->counters.get += 1;
        return status;
      }
    case CMD_BYTE_SET:
      {
        status = handle_set_request (client, request);
        worker->timers.set   += (get_performance_counter () - start_tick);
        worker->counters.set += 1;
        return status;
      }
    case CMD_BYTE_DEL:
      {
        status = handle_del_request (client, request);
        worker->timers.del   += (get_performance_counter () - start_tick);
        worker->counters.del += 1;
        return status;
      }
    case CMD_BYTE_CLR:
      {
        status = handle_clr_request (client, request);
        worker->timers.clr   += (get_performance_counter () - start_tick);
        worker->counters.clr += 1;
        return status;
      }
    case CMD_BYTE_LST:
      {
        status = handle_lst_request (client, request, response_payload);
        worker->timers.lst   += (get_performance_counter () - start_tick);
        worker->counters.lst += 1;
        return status;
      }
    case CMD_BYTE_NFO:
      {
        status = handle_nfo_request (client, request, response_payload);
        worker->timers.nfo   += (get_performance_counter () - start_tick);
        worker->counters.nfo += 1;
        return status;
      }
    default:
      return STATUS_PROTOCOL_ERROR;
    }
}
