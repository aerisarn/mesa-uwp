/*
 * Copyright 2022 Yonggang Luo
 * SPDX-License-Identifier: MIT
 *
 * C11 <threads.h> emulation library
 */

#ifndef C11_THREADS_H_INCLUDED_
#define C11_THREADS_H_INCLUDED_

#include "c11/time.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#if defined(_WIN32) && !defined(__CYGWIN__)
#  include <io.h> /* close */
#  include <process.h> /* _exit */
#elif defined(HAVE_PTHREAD)
#  include <pthread.h>
#  include <unistd.h> /* close, _exit */
#else
#  error Not supported on this platform.
#endif

/*---------------------------- macros ---------------------------*/

#ifndef _Thread_local
#  if defined(__cplusplus)
     /* C++11 doesn't need `_Thread_local` keyword or macro */
#  elif !defined(__STDC_NO_THREADS__)
     /* threads are optional in C11, _Thread_local present in this condition */
#  elif defined(_MSC_VER)
#    define _Thread_local __declspec(thread)
#  elif defined(__GNUC__)
#    define _Thread_local __thread
#  else
     /* Leave _Thread_local undefined so that use of _Thread_local would not promote
      * to a non-thread-local global variable
      */
#  endif
#endif

#if !defined(__cplusplus)
   /*
    * C11 thread_local() macro
    * C++11 and above already have thread_local keyword
    */
#  ifndef thread_local
#    define thread_local _Thread_local
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------- types ----------------------------*/
typedef void (*tss_dtor_t)(void *);
typedef int (*thrd_start_t)(void *);

#if defined(_WIN32) && !defined(__CYGWIN__)
typedef struct
{
   void *Ptr;
} cnd_t;
typedef void *thrd_t;
typedef unsigned long tss_t;
typedef struct
{
   void *DebugInfo;
   long LockCount;
   long RecursionCount;
   void *OwningThread;
   void *LockSemaphore;
   uintptr_t SpinCount;
} mtx_t; /* Mock of CRITICAL_SECTION */
typedef struct
{
   volatile uintptr_t status;
} once_flag;
// FIXME: temporary non-standard hack to ease transition
#  define _MTX_INITIALIZER_NP {(void*)-1, -1, 0, 0, 0, 0}
#  define ONCE_FLAG_INIT {0}
#  define TSS_DTOR_ITERATIONS 1
#elif defined(HAVE_PTHREAD)
typedef pthread_cond_t  cnd_t;
typedef pthread_t       thrd_t;
typedef pthread_key_t   tss_t;
typedef pthread_mutex_t mtx_t;
typedef pthread_once_t  once_flag;
// FIXME: temporary non-standard hack to ease transition
#  define _MTX_INITIALIZER_NP PTHREAD_MUTEX_INITIALIZER
#  define ONCE_FLAG_INIT PTHREAD_ONCE_INIT
#  ifdef INIT_ONCE_STATIC_INIT
#    define TSS_DTOR_ITERATIONS PTHREAD_DESTRUCTOR_ITERATIONS
#  else
#    define TSS_DTOR_ITERATIONS 1  // assume TSS dtor MAY be called at least once.
#  endif
#else
#  error Not supported on this platform.
#endif

/*-------------------- enumeration constants --------------------*/
enum
{
   mtx_plain = 0,
   mtx_try = 1,
   mtx_timed = 2,
   mtx_recursive = 4
};

enum
{
   thrd_success = 0, // succeeded
   thrd_timedout,    // timed out
   thrd_error,       // failed
   thrd_busy,        // resource busy
   thrd_nomem        // out of memory
};

/*-------------------------- functions --------------------------*/

void call_once(once_flag *, void (*)(void));
int cnd_broadcast(cnd_t *);
void cnd_destroy(cnd_t *);
int cnd_init(cnd_t *);
int cnd_signal(cnd_t *);
int cnd_timedwait(cnd_t *__restrict, mtx_t *__restrict __mtx,
                  const struct timespec *__restrict);
int cnd_wait(cnd_t *, mtx_t *__mtx);
void mtx_destroy(mtx_t *__mtx);
int mtx_init(mtx_t *__mtx, int);
int mtx_lock(mtx_t *__mtx);
int mtx_timedlock(mtx_t *__restrict __mtx,
                  const struct timespec *__restrict);
int mtx_trylock(mtx_t *__mtx);
int mtx_unlock(mtx_t *__mtx);
int thrd_create(thrd_t *, thrd_start_t, void *);
thrd_t thrd_current(void);
int thrd_detach(thrd_t);
int thrd_equal(thrd_t, thrd_t);
#if defined(__cplusplus)
[[ noreturn ]]
#else
_Noreturn
#endif
void thrd_exit(int);
int thrd_join(thrd_t, int *);
int thrd_sleep(const struct timespec *, struct timespec *);
void thrd_yield(void);
int tss_create(tss_t *, tss_dtor_t);
void tss_delete(tss_t);
void *tss_get(tss_t);
int tss_set(tss_t, void *);

#ifdef __cplusplus
}
#endif

#endif /* C11_THREADS_H_INCLUDED_ */
