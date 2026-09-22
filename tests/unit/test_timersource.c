// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0
/*
 * Unit tests for src/utils/timersource.c
 *
 * The GSourceFuncs table g_timer_source_funcs is exported, so prepare() and
 * check() are reachable without a running main loop.
 */
#include <glib.h>

#include "timersource.h"

extern GSourceFuncs g_timer_source_funcs;

/* freshly armed timer: not ready, timeout close to the interval */
static void test_fresh_timer_not_ready(void)
{
    GTimerSource *ts = g_timer_source_new(1500, 100);
    gint timeout_ms = -1;

    gboolean ready = g_timer_source_funcs.prepare((GSource *)ts, &timeout_ms);

    g_assert_false(ready);
    g_assert_false(g_timer_source_funcs.check((GSource *)ts));
    g_assert_cmpint(timeout_ms, >, 1200);
    g_assert_cmpint(timeout_ms, <=, 1600);

    g_source_unref((GSource *)ts);
}

/*
 * A zero interval must not produce a source that is ready the instant it is
 * armed. dispatch() re-arms from the interval, so "ready immediately" plus
 * "re-arm to now" is a busy loop with no poll in between - that cost ~25% of a
 * CPU core in sleepd until ScheduleIdleCheck(0) stopped expressing "run now"
 * that way.
 */
static void test_zero_interval_does_not_spin(void)
{
    GTimerSource *ts = g_timer_source_new(0, 0);
    gint timeout_ms = -1;

    gboolean ready = g_timer_source_funcs.prepare((GSource *)ts, &timeout_ms);

    g_assert_false(ready);
    g_assert_cmpint(timeout_ms, >, 0);
    g_assert_false(g_timer_source_funcs.check((GSource *)ts));

    g_source_unref((GSource *)ts);
}

/*
 * The dispatch -> re-arm cycle must leave the source not-ready, whatever the
 * interval. This is the property that actually prevents the busy loop: a
 * callback returning TRUE re-arms through g_timer_set_expiration().
 */
static void test_rearm_after_dispatch_is_not_ready(void)
{
    guint intervals[] = { 0, 1, 100 };
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(intervals); i++)
    {
        GTimerSource *ts = g_timer_source_new(intervals[i], 0);
        gint timeout_ms = -1;

        /* Re-arm exactly the way dispatch() does for a callback returning TRUE */
        g_timer_source_set_interval(ts, intervals[i], TRUE);

        g_assert_false(g_timer_source_funcs.prepare((GSource *)ts, &timeout_ms));
        g_assert_cmpint(timeout_ms, >, 0);

        g_source_unref((GSource *)ts);
    }
}

/*
 * fire_now() is how a caller asks for "run as soon as possible" without
 * flattening the repeat interval to zero: the source becomes ready, but the
 * interval it re-arms with is untouched.
 */
static void test_fire_now_keeps_interval(void)
{
    GTimerSource *ts = g_timer_source_new(5000, 0);
    gint timeout_ms = -1;

    g_assert_false(g_timer_source_funcs.prepare((GSource *)ts, &timeout_ms));

    g_timer_source_fire_now(ts, TRUE);

    g_assert_true(g_timer_source_funcs.prepare((GSource *)ts, &timeout_ms));
    g_assert_cmpint(timeout_ms, ==, 0);
    g_assert_true(g_timer_source_funcs.check((GSource *)ts));
    g_assert_cmpuint(g_timer_source_get_interval_ms(ts), ==, 5000);

    g_source_unref((GSource *)ts);
}

/* seconds constructor stores ms */
static void test_seconds_constructor(void)
{
    GTimerSource *ts = g_timer_source_new_seconds(2);
    g_assert_cmpuint(g_timer_source_get_interval_ms(ts), ==, 2000);
    g_source_unref((GSource *)ts);
}

/* a huge interval must clamp the poll timeout instead of overflowing it:
 * regression for the gint64->gint conversion in prepare() */
static void test_huge_interval_clamps(void)
{
    /* 7 days, the MAX_WAKEUP_SECS ceiling used by the alarm code */
    GTimerSource *ts = g_timer_source_new(7 * 24 * 60 * 60 * 1000u, 1000);
    gint timeout_ms = -1;

    gboolean ready = g_timer_source_funcs.prepare((GSource *)ts, &timeout_ms);

    g_assert_false(ready);
    g_assert_cmpint(timeout_ms, >, 0);    /* pre-clamp this could go negative */

    g_source_unref((GSource *)ts);
}

/* set_interval re-arms relative to now */
static void test_set_interval_rearms(void)
{
    GTimerSource *ts = g_timer_source_new(60000, 100);
    gint timeout_ms = -1;

    g_timer_source_set_interval(ts, 100, true);
    g_timer_source_funcs.prepare((GSource *)ts, &timeout_ms);
    g_assert_cmpint(timeout_ms, <=, 200);

    g_usleep(250 * 1000);
    g_assert_true(g_timer_source_funcs.check((GSource *)ts));

    g_source_unref((GSource *)ts);
}

/*
 * The idle-check watchdog asks whether a source has been due for longer
 * than a grace period without being re-armed; a freshly armed source is
 * not overdue, one whose expiry is well in the past is.
 */
static void test_overdue(void)
{
    GTimerSource *ts = g_timer_source_new(100, 0);

    g_assert_false(g_timer_source_is_overdue(ts, 0));
    g_assert_false(g_timer_source_is_overdue(ts, 5 * G_USEC_PER_SEC));
    g_assert_cmpint(g_timer_source_get_expiration_us(ts), >, g_get_monotonic_time());

    /* re-arm exactly as dispatch() does, then let it go stale */
    g_timer_source_set_interval(ts, 1, TRUE);
    g_usleep(20 * 1000);
    g_assert_true(g_timer_source_is_overdue(ts, 0));
    g_assert_true(g_timer_source_is_overdue(ts, 10 * 1000));
    g_assert_false(g_timer_source_is_overdue(ts, 5 * G_USEC_PER_SEC));

    /* re-arming clears it */
    g_timer_source_set_interval(ts, 100, TRUE);
    g_assert_false(g_timer_source_is_overdue(ts, 0));

    g_source_unref((GSource *)ts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/timersource/fresh-not-ready", test_fresh_timer_not_ready);
    g_test_add_func("/timersource/zero-interval-does-not-spin", test_zero_interval_does_not_spin);
    g_test_add_func("/timersource/rearm-after-dispatch", test_rearm_after_dispatch_is_not_ready);
    g_test_add_func("/timersource/fire-now-keeps-interval", test_fire_now_keeps_interval);
    g_test_add_func("/timersource/seconds-constructor", test_seconds_constructor);
    g_test_add_func("/timersource/huge-interval-clamps", test_huge_interval_clamps);
    g_test_add_func("/timersource/set-interval-rearms", test_set_interval_rearms);
    g_test_add_func("/timersource/overdue", test_overdue);

    return g_test_run();
}
