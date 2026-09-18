// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0
/*
 * Unit tests for src/config.c
 *
 * config.c is included directly (with stub headers on the include path) so
 * the static config_init() and the CONFIG_GET_* macros are testable. The
 * config path is redirected to a temp dir via -DWEBOS_INSTALL_DEFAULTCONFDIR
 * pointing at TEST_CONF_DIR, set up in main().
 *
 * Regression focus: wait_alarms_ms was read with CONFIG_GET_BOOL into the
 * wait_alarms_s field, so the shipped "wait_alarms_ms = 5000" never parsed
 * and a literal "true" set the field to 1 (second).
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>

#include "../../src/config.c"

static gchar *conf_dir;

static void write_conf(const char *contents)
{
    gchar *path = g_build_filename(conf_dir, "sleepd.conf", NULL);
    g_assert_true(g_file_set_contents(path, contents, -1, NULL));
    g_free(path);
}

static void reset_config(void)
{
    /* restore the compiled-in defaults the daemon starts with */
    gSleepConfig.wait_idle_ms = 500;
    gSleepConfig.wait_idle_granularity_ms = 100;
    gSleepConfig.after_resume_idle_ms = 1000;
    gSleepConfig.wait_suspend_response_ms = 30000;
    gSleepConfig.wait_prepare_suspend_ms = 5000;
    gSleepConfig.wait_alarms_s = 5;
    gSleepConfig.suspend_with_charger = 0;
    gSleepConfig.enable_idle_check_thread = 0;
    gSleepConfig.debug = 0;
}

static void test_full_conf_parses(void)
{
    reset_config();
    write_conf(
        "[general]\n"
        "debug = 1\n"
        "\n"
        "[suspend]\n"
        "wait_idle_ms = 750\n"
        "wait_idle_granularity_ms = 50\n"
        "after_resume_idle_ms = 2000\n"
        "wait_suspend_response_ms = 15000\n"
        "wait_prepare_suspend_ms = 4000\n"
        "wait_alarms_ms = 7000\n"
        "suspend_with_charger = true\n"
        "enable_idle_check_thread = true\n");

    g_assert_cmpint(config_init(), ==, 0);

    g_assert_cmpint(gSleepConfig.debug, ==, 1);
    g_assert_cmpint(gSleepConfig.wait_idle_ms, ==, 750);
    g_assert_cmpint(gSleepConfig.wait_idle_granularity_ms, ==, 50);
    g_assert_cmpint(gSleepConfig.after_resume_idle_ms, ==, 2000);
    g_assert_cmpint(gSleepConfig.wait_suspend_response_ms, ==, 15000);
    g_assert_cmpint(gSleepConfig.wait_prepare_suspend_ms, ==, 4000);
    g_assert_cmpint(gSleepConfig.wait_alarms_s, ==, 7);   /* ms -> s */
    g_assert_true(gSleepConfig.suspend_with_charger);
    g_assert_true(gSleepConfig.enable_idle_check_thread);
}

/* the exact conf shipped in files/conf/sleepd.conf must land as 5 s */
static void test_shipped_wait_alarms_value(void)
{
    reset_config();
    write_conf("[suspend]\nwait_alarms_ms = 5000\n");

    g_assert_cmpint(config_init(), ==, 0);
    g_assert_cmpint(gSleepConfig.wait_alarms_s, ==, 5);
}

/* wait_idle_granularity_ms was not read from the file at all */
static void test_granularity_is_configurable(void)
{
    reset_config();
    write_conf("[suspend]\nwait_idle_granularity_ms = 250\n");

    g_assert_cmpint(config_init(), ==, 0);
    g_assert_cmpint(gSleepConfig.wait_idle_granularity_ms, ==, 250);
    g_assert_cmpint(gSleepConfig.wait_idle_ms, ==, 500);
}

/* a missing/garbage key must keep the compiled-in default, not corrupt it */
static void test_bad_values_keep_defaults(void)
{
    reset_config();
    write_conf(
        "[suspend]\n"
        "wait_idle_ms = notanumber\n"
        "enable_idle_check_thread = maybe\n");

    g_assert_cmpint(config_init(), ==, 0);
    g_assert_cmpint(gSleepConfig.wait_idle_ms, ==, 500);
    g_assert_false(gSleepConfig.enable_idle_check_thread);
}

/* no conf file at all: defaults survive */
static void test_missing_conf_keeps_defaults(void)
{
    reset_config();
    gchar *path = g_build_filename(conf_dir, "sleepd.conf", NULL);
    g_unlink(path);
    g_free(path);

    g_assert_cmpint(config_init(), ==, 0);
    g_assert_cmpint(gSleepConfig.wait_alarms_s, ==, 5);
    g_assert_false(gSleepConfig.enable_idle_check_thread);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    conf_dir = g_dir_make_tmp("sleepd-conf-test-XXXXXX", NULL);
    g_assert_nonnull(conf_dir);
    g_setenv("SLEEPD_TEST_CONF_DIR", conf_dir, TRUE);

    /* config_init() also mkdirs preference_dir; point it into the tmp dir */
    gchar *pref = g_build_filename(conf_dir, "prefs", NULL);
    gSleepConfig.preference_dir = pref;

    g_test_add_func("/config/full-conf", test_full_conf_parses);
    g_test_add_func("/config/shipped-wait-alarms", test_shipped_wait_alarms_value);
    g_test_add_func("/config/granularity-configurable", test_granularity_is_configurable);
    g_test_add_func("/config/bad-values-keep-defaults", test_bad_values_keep_defaults);
    g_test_add_func("/config/missing-conf-keeps-defaults", test_missing_conf_keeps_defaults);

    return g_test_run();
}
