#ifndef LOG_H
#define LOG_H 1

#include <stdio.h>
#include <stdarg.h>

#include "config.h"
#include "types.h"
#include "util.h"

#define BEGIN_BLUE     "\e[0;34m"
#define BEGIN_GREEN    "\e[0;32m"
#define BEGIN_RED      "\e[1;31m"
#define BEGIN_YELLOW   "\e[1;33m"
#define BEGIN_GRAY     "\e[38;5;244m"
#define RESET_ANSI_FMT "\e[0m"

#define BLUE(string)   BEGIN_BLUE   string RESET_ANSI_FMT
#define GREEN(string)  BEGIN_GREEN  string RESET_ANSI_FMT
#define RED(string)    BEGIN_RED    string RESET_ANSI_FMT
#define YELLOW(string) BEGIN_YELLOW string RESET_ANSI_FMT
#define GRAY(string)   BEGIN_GRAY   string RESET_ANSI_FMT

#define LINEWIDTH 80
#define HLINESTR "----------------------------------------" \
                 "----------------------------------------"
#define BLANKSTR "                                        " \
                 "                                        "

#if DEBUG
# define dbg_print printf
#else
# define dbg_print(fmt, ...)
#endif

#define wrn_print(fmt, ...)                                     \
  fprintf (stderr, YELLOW ("W") " %s:%d: %s: " fmt,             \
           __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)

#define err_print(fmt, ...)                                     \
  fprintf (stderr, RED ("E") " %s:%d: %s: " fmt,                \
           __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)

#define LOG_QUEUE_INIT (LogQueue) { \
  .mask = NUM_LOG_QUEUE_ELEMS - 1,  \
  .read = ATOMIC_VAR_INIT (0),      \
  .write = ATOMIC_VAR_INIT (0),     \
  .elems = { { 0 } }                \
}

extern thread_local LogQueue *current_log_queue;

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

void print_log_entry (LogEntry *, int);

static inline bool
log_entry (LogEntry *e)
{
  if (current_log_queue == NULL)
    return false;
  return enqueue_log_entry (current_log_queue, e);
}

static inline bool
logprintf (const char *fmt, ...)
{
  LogEntry entry = {
    .type = LOG_TYPE_STRING,
    .data = { 0 }
  };
  va_list ap;
  va_start (ap, fmt);
  vsnprintf ((char *) entry.data, 0xFF, fmt, ap);
  va_end(ap);
  return log_entry (&entry);
}

#endif /* ! LOG_H */
