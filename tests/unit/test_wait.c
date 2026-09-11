// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0
/*
 * Unit tests for src/utils/wait.c
 *
 * Regression focus: WaitObjectWait() used to convert the sub-second part of
 * the wait with (ms % 1000) * 1e9 instead of 1e6. Any wait that was not a
 * whole multiple of 1000 ms produced an invalid timespec, so
 * pthread_cond_timedwait() failed with EINVAL and the g_error() call in
 * WaitObjectWaitAbsTime() aborted the daemon. With the bug present this
 * test binary aborts; with the fix it times out in the expected window.
 */
#include <glib.h>
#include <time.h>
#include <stdio.h>

#include "wait.h"

static gint64 now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (gint64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* a sub-second wait must time out, not error/abort, and must not overshoot */
static void test_subsecond_wait_times_out(void)
{
    WaitObj obj;
    WaitObjectInit(&obj);

    WaitObjectLock(&obj);
    gint64 start = now_ms();
    int ret = WaitObjectWait(&obj, 250);
    gint64 elapsed = now_ms() - start;
    WaitObjectUnlock(&obj);

    g_assert_cmpint(ret, ==, 1);          /* timed out */
    g_assert_cmpint(elapsed, >=, 200);
    g_assert_cmpint(elapsed, <=, 5000);   /* not the 250-second bug */
}

/* a wait with both second and sub-second parts (the worst case pre-fix) */
static void test_mixed_wait_times_out(void)
{
    WaitObj obj;
    WaitObjectInit(&obj);

    WaitObjectLock(&obj);
    gint64 start = now_ms();
    int ret = WaitObjectWait(&obj, 1500);
    gint64 elapsed = now_ms() - start;
    WaitObjectUnlock(&obj);

    g_assert_cmpint(ret, ==, 1);
    g_assert_cmpint(elapsed, >=, 1400);
    g_assert_cmpint(elapsed, <=, 10000);  /* pre-fix this waited ~500 s */
}

typedef struct { WaitObj *obj; } SignalCtx;

static gpointer signaler(gpointer data)
{
    SignalCtx *ctx = data;
    g_usleep(200 * 1000);
    WaitObjectSignal(ctx->obj);
    return NULL;
}

/* a signal must wake the waiter with ret == 0 */
static void test_signal_wakes_waiter(void)
{
    WaitObj obj;
    WaitObjectInit(&obj);

    SignalCtx ctx = { .obj = &obj };

    WaitObjectLock(&obj);
    GThread *t = g_thread_new("signaler", signaler, &ctx);
    int ret = WaitObjectWait(&obj, 10000);
    WaitObjectUnlock(&obj);
    g_thread_join(t);

    g_assert_cmpint(ret, ==, 0);          /* woken, not timed out */
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/wait/subsecond-times-out", test_subsecond_wait_times_out);
    g_test_add_func("/wait/mixed-times-out", test_mixed_wait_times_out);
    g_test_add_func("/wait/signal-wakes", test_signal_wakes_waiter);
    return g_test_run();
}
