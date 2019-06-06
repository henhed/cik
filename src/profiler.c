#include <assert.h>
#include <stdio.h>
#include <threads.h>

#include "profiler.h"

typedef struct
{
  Profiler profiler;
  Profiler parent;
  u64 num_invocations;
  u64 start_tick;
  u64 num_ticks;
  u64 num_child_ticks;
  u64 total_num_ticks;
  u64 total_self_cost;
  bool is_open;
} ProfilerData;

static const char *profiler_names[NUM_PROFILERS] = {
  [PROF_MAIN]               = "main",
  [PROF_MAIN_LOOP]          = "main_loop",
  [PROF_SERVER_ACCEPT]      = "server_accept",
  [PROF_HANDLE_GET_REQUEST] = "handle_get_request",
  [PROF_HANDLE_SET_REQUEST] = "handle_set_request",
  [PROF_HANDLE_DEL_REQUEST] = "handle_del_request",
  [PROF_HANDLE_REQUEST]     = "handle_request",
  [PROF_SERVER_READ]        = "server_read",
  [PROF_CLOSE_CLIENT]       = "close_client"
};

static thread_local ProfilerData profiler_data[NUM_PROFILERS] = { 0 };
static thread_local Profiler current_profiler = PROF_MAIN;

Profiler
profile_scope_enter (Profiler profiler)
{
  ProfilerData *data = &profiler_data[profiler];

  assert (profiler != PROF_MAIN);

  if (data->profiler != profiler)
    {
      data->profiler = profiler;
      data->parent = PROF_MAIN;
      data->is_open = false;
      data->num_invocations = 0;
      data->total_num_ticks = 0;
      data->total_self_cost = 0;
    }

  if (data->is_open)
    assert (false); // We don't support recursing
  data->is_open = true;

  data->parent = current_profiler;
  current_profiler = profiler;

  data->num_invocations += 1;
  data->start_tick = get_performance_counter ();
  data->num_child_ticks = 0;

  return profiler;
}

void
profile_scope_exit (Profiler *profiler)
{
  ProfilerData *data = &profiler_data[*profiler];

  assert (*profiler == current_profiler);

  data->num_ticks = get_performance_counter () - data->start_tick;
  if (data->parent != PROF_MAIN)
    {
      ProfilerData *parent = &profiler_data[data->parent];
      parent->num_child_ticks += data->num_ticks;
    }

  data->total_num_ticks += data->num_ticks;
  data->total_self_cost += data->num_ticks - data->num_child_ticks;

  data->is_open = false;
  current_profiler = data->parent;
}

void
debug_print_profilers (int fd)
{
  float to_seconds = 1.f / (float) get_performance_frequency ();

  dprintf (fd, "%-20s:%-12s%-12s%-12s\n", "Name", "Count", "Total Cost", "Self Cost");

  for (Profiler p = PROF_MAIN + 1; p < NUM_PROFILERS; ++p)
    {
      ProfilerData *data = &profiler_data[p];
      const char *name = profiler_names[p];
      u64 count = data->num_invocations;
      float total_cost = (float) data->total_num_ticks * to_seconds;
      float self_cost  = (float) data->total_self_cost * to_seconds;
      dprintf (fd, "%-20s:%-12lu%-12.2f%-12.2f\n", name, count, total_cost, self_cost);
    }

  dprintf (fd, "------------------------------------------------------\n");
}

u64
get_performance_counter ()
{
  u64 ticks;
  struct timespec now;
  clock_gettime (CLOCK_MONOTONIC, &now);
  ticks = now.tv_sec;
  ticks *= 1000000000;
  ticks += now.tv_nsec;
  return ticks;
}

u64
get_performance_frequency ()
{
  return 1000000000;
}
