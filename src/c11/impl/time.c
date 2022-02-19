/*
 * Copyright 2022 Yonggang Luo
 * SPDX-License-Identifier: MIT
 *
 * C11 <time.h> implementation
 */

#include "c11/time.h"

#ifndef HAVE_TIMESPEC_GET

#if defined(_WIN32) && !defined(__CYGWIN__)

#include <assert.h>

int
timespec_get(struct timespec *ts, int base)
{
    assert(ts != NULL);
    if (base == TIME_UTC) {
        ts->tv_sec = time(NULL);
        ts->tv_nsec = 0;
        return base;
    }
    return 0;
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
