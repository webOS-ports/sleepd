// Copyright (c) 2011-2018 LG Electronics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

/**
 * @file sysfs.c
 *
 * @brief GTimerSource - a source needed because the typical GSources do not have necessary features.
 * I write this utility with the intention that this might be contributed back to glib
 * in the future.
 *
 * GTimerSource
 * 1) Can be forced to expire.
 * 2) The expiration interval may be changed.
 * 3) Uses a montonic clock.
 *
 */

#include <glib.h>

#include "timersource.h"
#include "clock.h"
#include "logging.h"

struct _GTimerSource
{
    GSource  source;
    gint64   expiration_us;   /* monotonic clock, microseconds */
    guint    interval_ms;     /* In milisecs */
    guint    granularity;
};

static gboolean g_timer_source_prepare(GSource *source, gint *timeout_ms);
static gboolean g_timer_source_check(GSource *source);
static gboolean g_timer_source_dispatch(GSource *source, GSourceFunc callback,
                                        gpointer user_data);

GSourceFuncs g_timer_source_funcs =
{
    .prepare  = g_timer_source_prepare,
    .check    = g_timer_source_check,
    .dispatch = g_timer_source_dispatch,
    .finalize = NULL,
};

#define USECS_PER_SEC 1000000
#define USECS_PER_MSEC 1000

/* Current monotonic time in microseconds (same clock as ClockGetTime). */
static gint64
g_timer_get_now_us(void)
{
    // TODO: We should do a time_is_current and skip syscalls
    struct timespec tv;
    ClockGetTime(&tv);

    return (gint64)tv.tv_sec * USECS_PER_SEC + tv.tv_nsec / 1000;
}

static void
g_timer_set_expiration(GTimerSource *rsource, gint64 now_us)
{
    guint interval_ms = rsource->interval_ms;

    /*
     * Never let the expiration land on "now". dispatch() re-arms through here,
     * so a zero interval produces a source that is ready again the instant it
     * is dispatched: prepare() returns TRUE, check() returns TRUE, the callback
     * runs, and round it goes with no poll in between. Callers that mean "as
     * soon as possible" get the next millisecond instead of a busy loop.
     */
    if (interval_ms == 0)
    {
        interval_ms = 1;
    }

    rsource->expiration_us = now_us + (gint64)interval_ms * USECS_PER_MSEC;

    if (rsource->granularity)
    {
        gint64 gran = (gint64)rsource->granularity * USECS_PER_MSEC;
        gint64 remainder = rsource->expiration_us % gran;

        if (remainder >= gran / 4)
        {
            rsource->expiration_us += gran;
        }

        rsource->expiration_us -= remainder;
    }
}

static gboolean
g_timer_source_prepare(GSource    *source,
                       gint       *timeout_ms)
{
    GTimerSource *tsource = (GTimerSource *)source;

    gint64 remaining_us = tsource->expiration_us - g_timer_get_now_us();
    gint64 msec;

    if (remaining_us <= 0)
    {
        *timeout_ms = 0;
        return TRUE;
    }

    /*
     * Round up, never down: a source due in 800us is not due yet, and
     * reporting "ready, poll timeout 0" for it makes the main loop poll with
     * no timeout, find check() false, and spin until the millisecond passes.
     * The 1ms floor in g_timer_set_expiration() depends on this to stay a
     * floor.
     */
    msec = (remaining_us + 999) / 1000;

    if (msec > G_MAXINT)
    {
        msec = G_MAXINT;
    }

    *timeout_ms = (gint)msec;

    return FALSE;
}

static gboolean
g_timer_source_check(GSource *source)
{
    GTimerSource *tsource = (GTimerSource *)source;

    return tsource->expiration_us <= g_timer_get_now_us();
}

static gboolean
g_timer_source_dispatch(GSource *source,
                        GSourceFunc callback, gpointer user_data)
{
    GTimerSource *tsource = (GTimerSource *)source;

    if (!callback)
    {
        SLEEPDLOG_DEBUG("Timeout source dispatched without callback, Call g_source_set_callback()");
        return FALSE;
    }

    if (callback(user_data))
    {
        g_timer_set_expiration(tsource, g_timer_get_now_us());
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}

/** Public Functions */

/**
* @brief A create a timer with 100 ms resolution.
*
* @param  interval_ms
*
* @retval
*/
GTimerSource *
g_timer_source_new(guint interval_ms, guint granularity_ms)
{
    GSource *source;
    GTimerSource *tsource;

    source = g_source_new(&g_timer_source_funcs, sizeof(GTimerSource));
    tsource = (GTimerSource *)source;

    tsource->interval_ms = interval_ms;
    tsource->granularity = granularity_ms;

    g_timer_set_expiration(tsource, g_timer_get_now_us());

    return tsource;
}

GTimerSource *
g_timer_source_new_seconds(guint interval_sec)
{
    GSource *source;
    GTimerSource *tsource;

    source = g_source_new(&g_timer_source_funcs, sizeof(GTimerSource));
    tsource = (GTimerSource *)source;

    tsource->interval_ms = 1000 * interval_sec;
    tsource->granularity = 1000;

    g_timer_set_expiration(tsource, g_timer_get_now_us());

    return tsource;
}

void
g_timer_source_set_interval_seconds(GTimerSource *tsource, guint interval_sec,
                                    gboolean from_poll)
{
    g_timer_source_set_interval(tsource, interval_sec * 1000, from_poll);
}

void
g_timer_source_fire_now(GTimerSource *tsource, gboolean from_poll)
{
    tsource->expiration_us = g_timer_get_now_us();

    if (!from_poll)
    {
        GMainContext *context = g_source_get_context((GSource *)tsource);

        if (!context)
        {
            SLEEPDLOG_DEBUG("Cannot get context for timer_source. Maybe you didn't call g_source_attach()");
            return;
        }

        g_main_context_wakeup(context);
    }
}

void
g_timer_source_set_interval(GTimerSource *tsource, guint interval_ms,
                            gboolean from_poll)
{
    tsource->interval_ms = interval_ms;
    g_timer_set_expiration(tsource, g_timer_get_now_us());

    if (!from_poll)
    {
        GMainContext *context =  g_source_get_context((GSource *)tsource);

        if (!context)
        {
            SLEEPDLOG_DEBUG("Cannot get context for timer_source. Maybe you didn't call g_source_attach()");
            return;
        }

        g_main_context_wakeup(context);
    }
}

guint
g_timer_source_get_interval_ms(GTimerSource *tsource)
{
    return tsource->interval_ms;
}


