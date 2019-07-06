#include <string.h>

#include "log.h"

static char *verbs[NUM_LOG_TYPES] = {
  [LOG_TYPE_REQUEST_GET_HIT]          = GREEN  ("GET"),
  [LOG_TYPE_REQUEST_GET_MISS]         = RED    ("GET"),
  [LOG_TYPE_REQUEST_SET]              = BLUE   ("SET"),
  [LOG_TYPE_REQUEST_DEL]              = YELLOW ("DEL"),
  [LOG_TYPE_REQUEST_CLR_ALL]          = YELLOW ("CLR"),
  [LOG_TYPE_REQUEST_CLR_OLD]          = YELLOW ("CLR"),
  [LOG_TYPE_REQUEST_CLR_MATCH_NONE]   = YELLOW ("CLR"),
  [LOG_TYPE_REQUEST_CLR_MATCH_ALL]    = YELLOW ("CLR"),
  [LOG_TYPE_REQUEST_CLR_MATCH_ANY]    = YELLOW ("CLR")
};

bool
enqueue_log_entry (LogQueue *q, LogEntry *e)
{
  cik_assert (q != NULL);
  cik_assert (e != NULL);

  if (((q->write + 1) & q->mask) == atomic_load (&q->read))
    return false;

  q->elems[q->write] = *e;

  atomic_store (&q->write, (q->write + 1) & q->mask);

  return true;
}

bool
dequeue_log_entry (LogQueue *q, LogEntry *e)
{
  cik_assert (q != NULL);
  cik_assert (e != NULL);

  if (q->read == atomic_load (&q->write))
    return false;

  *e = q->elems[q->read];

  atomic_store (&q->read, (q->read + 1) & q->mask);

  return true;
}

static bool
log_request_with_no_args (LogEntryType type, Client *client)
{
  LogEntry entry = {
    .type        = type,
    .worker_id   = client->worker->id,
    .client_ip   = client->addr.sin_addr.s_addr,
    .client_port = client->addr.sin_port
  };
  return enqueue_log_entry (&client->worker->log_queue, &entry);
}

static bool
log_request_with_key (LogEntryType type, Client *client, CacheKey key)
{
  LogEntry entry = {
    .type        = type,
    .worker_id   = client->worker->id,
    .client_ip   = client->addr.sin_addr.s_addr,
    .client_port = client->addr.sin_port,
    .data        = { [0] = key.nmemb }
  };
  memcpy (&entry.data[1], key.base, key.nmemb);
  return enqueue_log_entry (&client->worker->log_queue, &entry);
}

static bool
log_request_with_tags (LogEntryType type, Client *client,
                       CacheTag *tags, u8 ntags)
{
  LogEntry entry = {
    .type        = type,
    .worker_id   = client->worker->id,
    .client_ip   = client->addr.sin_addr.s_addr,
    .client_port = client->addr.sin_port,
    .data        = { 0 }
  };
  u8 nmemb = 0;
  for (u8 t = 0; t < ntags; ++t)
    {
      if ((nmemb + 1 + tags[t].nmemb) > 0x100)
        break;
      entry.data[nmemb++] = tags[t].nmemb;
      memcpy (&entry.data[nmemb], tags[t].base, tags[t].nmemb);
      nmemb += tags[t].nmemb;
    }
  return enqueue_log_entry (&client->worker->log_queue, &entry);
}

bool
log_request_get_hit (Client *client, CacheKey key)
{
  return log_request_with_key (LOG_TYPE_REQUEST_GET_HIT, client, key);
}

bool
log_request_get_miss (Client *client, CacheKey key)
{
  return log_request_with_key (LOG_TYPE_REQUEST_GET_MISS, client, key);
}

bool
log_request_set (Client *client, CacheKey key)
{
  return log_request_with_key (LOG_TYPE_REQUEST_SET, client, key);
}

bool
log_request_del (Client *client, CacheKey key)
{
  return log_request_with_key (LOG_TYPE_REQUEST_DEL, client, key);
}

bool
log_request_clr_all (Client *client)
{
  return log_request_with_no_args (LOG_TYPE_REQUEST_CLR_ALL, client);
}

bool
log_request_clr_old (Client *client)
{
  return log_request_with_no_args (LOG_TYPE_REQUEST_CLR_OLD, client);
}

bool
log_request_clr_match_none (Client *client, CacheTag *tags, u8 ntags)
{
  return log_request_with_tags (LOG_TYPE_REQUEST_CLR_MATCH_NONE, client,
                                tags, ntags);
}

bool
log_request_clr_match_all (Client *client, CacheTag *tags, u8 ntags)
{
  return log_request_with_tags (LOG_TYPE_REQUEST_CLR_MATCH_ALL, client,
                                tags, ntags);
}

bool
log_request_clr_match_any (Client *client, CacheTag *tags, u8 ntags)
{
  return log_request_with_tags (LOG_TYPE_REQUEST_CLR_MATCH_ANY, client,
                                tags, ntags);
}

void
print_log_entry (LogEntry *e)
{
#if DEBUG
  cik_assert (e != NULL);
  cik_assert ((e->type >= 0) && (e->type < NUM_LOG_TYPES));

  dbg_print (GRAY ("%u.%u.%u.%u:%u[%X]") " %s ",
             (e->client_ip & 0x000000FF) >>  0,
             (e->client_ip & 0x0000FF00) >>  8,
             (e->client_ip & 0x00FF0000) >> 16,
             (e->client_ip & 0xFF000000) >> 24,
             e->client_port,
             e->worker_id,
             verbs [e->type]);

  switch (e->type)
    {
    case LOG_TYPE_REQUEST_GET_HIT: // Intentional fallthrough
    case LOG_TYPE_REQUEST_GET_MISS: // Intentional fallthrough
    case LOG_TYPE_REQUEST_SET: // Intentional fallthrough
    case LOG_TYPE_REQUEST_DEL:
      {
        CacheKey key = { .base = &e->data[1], .nmemb = e->data[0] };
        dbg_print ("'%s'", key2str (key));
        break;
      }
    case LOG_TYPE_REQUEST_CLR_ALL:
      dbg_print ("(ALL)");
      break;
    case LOG_TYPE_REQUEST_CLR_OLD:
      dbg_print ("(OLD)");
      break;
    case LOG_TYPE_REQUEST_CLR_MATCH_NONE:
    case LOG_TYPE_REQUEST_CLR_MATCH_ALL:
    case LOG_TYPE_REQUEST_CLR_MATCH_ANY:
      {
        CacheTag tag = { .base = &e->data[1], .nmemb = e->data[0] };
        dbg_print ("(MATCH %s)",
                   ((e->type == LOG_TYPE_REQUEST_CLR_MATCH_NONE)
                    ? "NONE"
                    : ((e->type == LOG_TYPE_REQUEST_CLR_MATCH_ALL)
                       ? "ALL"
                       : "ANY")));
        while (tag.nmemb > 0)
          {
            dbg_print (" '%s'", tag2str (tag));
            tag.base += tag.nmemb;
            tag.nmemb = *tag.base;
            ++tag.base;
          }
        break;
      }
    default:
      break; // NoOp
    }

  dbg_print ("\n");
#else
  (void) e;
  (void) verbs;
#endif
}
