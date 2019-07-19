#ifndef THRD_COMPAT_H
#define THRD_COMPAT_H 1

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

extern void    *reserve_memory  (uint32_t);
extern void     release_memory  (void *);

typedef pthread_t thrd_t;
typedef pthread_key_t tss_t;
typedef void (* tss_dtor_t) (void *);
typedef int  (* thrd_start_t) (void *);

enum
{
  thrd_success  = 0,
  thrd_busy     = 1,
  thrd_error    = 2,
  thrd_nomem    = 3,
  thrd_timedout = 4
};

static inline void *
tss_get (tss_t tss_key)
{
  return pthread_getspecific (tss_key);
}

static inline int
tss_create (tss_t *tss_key, tss_dtor_t destructor)
{
  int err = pthread_key_create (tss_key, destructor);
  return (err == 0) ? thrd_success : thrd_error;
}

static inline int
tss_set (tss_t tss_key, void *val)
{
  int err = pthread_setspecific (tss_key, val);
  return (err == 0) ? thrd_success : thrd_error;
}

struct _thrd_start_routine_arg {
  thrd_start_t func;
  void *arg;
  int res;
};

static void *
_thrd_start_routine (void *arg)
{
  struct _thrd_start_routine_arg *wrapper = arg;
  wrapper->res = wrapper->func (wrapper->arg);
  return wrapper;
}

static inline int
thrd_create (thrd_t *thr, thrd_start_t func, void *arg)
{
  int err;
  struct _thrd_start_routine_arg *wrapper;
  wrapper = reserve_memory (sizeof (*wrapper));
  assert (wrapper != NULL);
  wrapper->func = func;
  wrapper->arg = arg;
  wrapper->res = thrd_success;
  err = pthread_create (thr, NULL, _thrd_start_routine, wrapper);
  if (err == ENOMEM)
    return thrd_nomem;
  return (err == 0) ? thrd_success : thrd_error;
}

static inline int
thrd_join (thrd_t thr, int *res)
{
  int err;
  void *arg;
  struct _thrd_start_routine_arg *wrapper;

  err = pthread_join (thr, &arg);
  if (err != 0)
    return thrd_error;

  wrapper = arg;

  if (res)
    *res = wrapper->res;

  release_memory (wrapper);

  return thrd_success;
}

static inline int
thrd_sleep (const struct timespec* duration,
            struct timespec* remaining)
{
  int err = nanosleep (duration, remaining);
  if (err == 0)
    return 0;
  if (errno == EINTR)
    return -1;
  return -errno;
}

static inline void
thrd_yield ()
{
  (void) pthread_yield ();
}

#endif /* ! THRD_COMPAT_H */
