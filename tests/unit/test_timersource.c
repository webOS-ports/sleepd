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

/* zero interval: immediately ready */
static void test_zero_interval_ready(void)
{
    GTimerSource *ts = g_timer_source_new(0, 0);
    gint timeout_ms = -1;

    gboolean ready = g_timer_source_funcs.prepare((GSource *)ts, &timeout_ms);

    g_assert_true(ready);
    g_assert_cmpint(timeout_ms, ==, 0);
    g_assert_true(g_timer_source_funcs.check((GSource *)ts));

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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/timersource/fresh-not-ready", test_fresh_timer_not_ready);
    g_test_add_func("/timersource/zero-interval-ready", test_zero_interval_ready);
    g_test_add_func("/timersource/seconds-constructor", test_seconds_constructor);
    g_test_add_func("/timersource/huge-interval-clamps", test_huge_interval_clamps);
    g_test_add_func("/timersource/set-interval-rearms", test_set_interval_rearms);
    return g_test_run();
}
