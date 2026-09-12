// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0
/* Test stub for clock.h: the real implementations live in libluna-service2,
 * which is not available on the build host. Semantics mirror LS2's clock.c. */
#ifndef _TEST_STUB_CLOCK_H_
#define _TEST_STUB_CLOCK_H_

#include <stdbool.h>
#include <time.h>
#include <glib.h>

#define TEST_NSEC_PER_SEC 1000000000L

static inline void ClockGetTime(struct timespec *time)
{
    clock_gettime(CLOCK_MONOTONIC, time);
}

/* Single-carry normalization, as in luna-service2. A tv_nsec that is more
 * than one second over (like the old ms->ns bug produced) stays invalid,
 * which is exactly what the wait regression test relies on. */
static inline void ClockAccum(struct timespec *sum, struct timespec *b)
{
    sum->tv_sec += b->tv_sec;
    sum->tv_nsec += b->tv_nsec;

    if (sum->tv_nsec >= TEST_NSEC_PER_SEC)
    {
        sum->tv_sec++;
        sum->tv_nsec -= TEST_NSEC_PER_SEC;
    }
}

static inline void ClockAccumMs(struct timespec *sum, int duration_ms)
{
    struct timespec delta = {
        .tv_sec = duration_ms / 1000,
        .tv_nsec = (long)(duration_ms % 1000) * 1000000L,
    };
    ClockAccum(sum, &delta);
}

static inline bool ClockTimeIsGreater(struct timespec *a, struct timespec *b)
{
    return (a->tv_sec > b->tv_sec) ||
           (a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec);
}

static inline void ClockDiff(struct timespec *diff, struct timespec *a,
                             struct timespec *b)
{
    diff->tv_sec = a->tv_sec - b->tv_sec;
    diff->tv_nsec = a->tv_nsec - b->tv_nsec;

    if (diff->tv_nsec < 0)
    {
        diff->tv_sec--;
        diff->tv_nsec += TEST_NSEC_PER_SEC;
    }
}

static inline long ClockGetMs(struct timespec *t)
{
    return t->tv_sec * 1000 + t->tv_nsec / 1000000;
}

static inline void ClockStr(GString *str, struct timespec *t)
{
    g_string_append_printf(str, "%ld.%03lds ", t->tv_sec, t->tv_nsec / 1000000);
}

#endif
