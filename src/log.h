#ifndef LOG_H
#define LOG_H 1

#include "print.h"
#include "types.h"
#include "util.h"

#define LOG_QUEUE_INIT (LogQueue) { \
  .mask = NUM_LOG_QUEUE_ELEMS - 1,  \
  .read = ATOMIC_VAR_INIT (0),      \
  .write = ATOMIC_VAR_INIT (0),     \
  .elems = { { 0 } }                \
}

bool enqueue_log_entry (LogQueue *, LogEntry *);
bool dequeue_log_entry (LogQueue *, LogEntry *);

bool log_request_get_hit        (Client *, CacheKey);
bool log_request_get_miss       (Client *, CacheKey);
bool log_request_set            (Client *, CacheKey);
bool log_request_del            (Client *, CacheKey);
bool log_request_clr_all        (Client *);
bool log_request_clr_old        (Client *);
bool log_request_clr_match_none (Client *, CacheTag *, u8);
bool log_request_clr_match_all  (Client *, CacheTag *, u8);
bool log_request_clr_match_any  (Client *, CacheTag *, u8);

void print_log_entry (LogEntry *);

#endif /* ! LOG_H */
