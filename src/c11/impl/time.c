/*
 * Copyright 2022 Yonggang Luo
 * SPDX-License-Identifier: MIT
 *
 * C11 <time.h> implementation
 */

#include "c11/time.h"

#ifndef HAVE_TIMESPEC_GET

#if defined(_WIN32) && !defined(__CYGWIN__)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>

int
timespec_get(struct timespec *ts, int base)
{
/* difference between 1970 and 1601 */
#define _TIMESPEC_IMPL_UNIX_EPOCH_IN_TICKS 116444736000000000ull
/* 1 tick is 100 nanoseconds */
#define _TIMESPEC_IMPL_TICKS_PER_SECONDS 10000000ull
    if (!ts)
        return 0;
    if (base == TIME_UTC) {
        FILETIME ft;
        ULARGE_INTEGER date;
        LONGLONG ticks;

        GetSystemTimeAsFileTime(&ft);
        date.HighPart = ft.dwHighDateTime;
        date.LowPart = ft.dwLowDateTime;
        ticks = (LONGLONG)(date.QuadPart - _TIMESPEC_IMPL_UNIX_EPOCH_IN_TICKS);
        ts->tv_sec = ticks / _TIMESPEC_IMPL_TICKS_PER_SECONDS;
        ts->tv_nsec = (ticks % _TIMESPEC_IMPL_TICKS_PER_SECONDS) * 100;
        return base;
    }
    return 0;
#undef _TIMESPEC_IMPL_UNIX_EPOCH_IN_TICKS
#undef _TIMESPEC_IMPL_TICKS_PER_SECONDS
}

#else

int
timespec_get(struct timespec *ts, int base)
{
    if (!ts)
        return 0;
    if (base == TIME_UTC) {
        clock_gettime(CLOCK_REALTIME, ts);
        return base;
    }
    return 0;
}
#endif

#endif /* !HAVE_TIMESPEC_GET */
