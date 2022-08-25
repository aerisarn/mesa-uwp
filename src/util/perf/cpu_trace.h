/*
 * Copyright 2022 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef CPU_TRACE_H
#define CPU_TRACE_H

/* NOTE: for now disable atrace for C++ to workaround a ndk bug with ordering
 * between stdatomic.h and atomic.h.  See:
 *
 *   https://github.com/android/ndk/issues/1178
 */
#if defined(ANDROID) && !defined(__cplusplus)

#include <cutils/trace.h>

#define MESA_TRACE_BEGIN(name) atrace_begin(ATRACE_TAG_GRAPHICS, name)
#define MESA_TRACE_END() atrace_end(ATRACE_TAG_GRAPHICS)

#else

/* XXX we would like to use perfetto, but it lacks a C header */
#define MESA_TRACE_BEGIN(name)
#define MESA_TRACE_END()

#endif /* ANDROID */

#if __has_attribute(cleanup) && __has_attribute(unused)

#define MESA_TRACE_SCOPE(name)                                                \
   int _mesa_trace_scope_##__LINE__                                           \
      __attribute__((cleanup(mesa_trace_scope_end), unused)) =                \
         mesa_trace_scope_begin(name)

static inline int
mesa_trace_scope_begin(const char *name)
{
   MESA_TRACE_BEGIN(name);
   return 0;
}

static inline void
mesa_trace_scope_end(int *scope)
{
   MESA_TRACE_END();
}

#else

#define MESA_TRACE_SCOPE(name)

#endif /* __has_attribute(cleanup) && __has_attribute(unused) */

#define MESA_TRACE_FUNC() MESA_TRACE_SCOPE(__func__)

#endif /* CPU_TRACE_H */
