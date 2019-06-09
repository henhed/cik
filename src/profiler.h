#ifndef PROFILING_H
#define PROFILING_H 1

#include "types.h"

#if DEBUG
# define CONCAT(x, y) x ## y
# define CONCAT2(x, y) CONCAT (x, y)
# define PROFILE(name) \
  __attribute__ ((unused)) Profiler CONCAT2 (_profile_, __COUNTER__) \
  __attribute__ ((__cleanup__ (profile_scope_exit))) \
  = profile_scope_enter (name);
#else
# define PROFILE(name)
#endif

typedef enum
{
  PROF_MAIN = 0,
  PROF_MAIN_LOOP,
  PROF_SERVER_ACCEPT,
  PROF_HANDLE_GET_REQUEST,
  PROF_HANDLE_SET_REQUEST,
  PROF_HANDLE_DEL_REQUEST,
  PROF_HANDLE_CLR_REQUEST,
  PROF_HANDLE_REQUEST,
  PROF_SERVER_READ,
  PROF_CLOSE_CLIENT,
  NUM_PROFILERS
} Profiler;

Profiler profile_scope_enter       (Profiler);
void     profile_scope_exit        (Profiler *);
u64      get_performance_counter   (void);
u64      get_performance_frequency (void);
void     debug_print_profilers     (int);

#endif /* ! PROFILING_H */
