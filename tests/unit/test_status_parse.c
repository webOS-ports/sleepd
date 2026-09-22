// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// SPDX-License-Identifier: Apache-2.0
/*
 * Unit tests for src/utils/status_parse.c
 *
 * Pins the payload shapes sleepd's inputs actually have:
 *
 * - com.palm.display/control/status: the subscribe reply carries "state",
 *   later notifications carry "event"; hub errors and returnValue:false
 *   must not be read as "display off" (pre-fix: error replies were parsed
 *   as ordinary messages and silently kept whatever state was current).
 *
 * - com.webos.service.battery: chargerConnected/chargerStatus carry
 *   "connected"; the chargerStatusQuery reply does not (pre-fix: it was
 *   handed to the same callback, which only looked for "connected", so
 *   the initial charger state was never learned).
 */
#include <glib.h>

#include "status_parse.h"

static void test_display_subscribe_reply(void)
{
    g_assert_cmpint(DisplayStatusParse(
        "{\"returnValue\":true,\"event\":\"request\",\"state\":\"on\","
        "\"timeout\":120,\"blockDisplay\":\"false\",\"active\":true,\"subscribed\":true}"),
        ==, DisplayStatusOn);
    g_assert_cmpint(DisplayStatusParse(
        "{\"returnValue\":true,\"event\":\"request\",\"state\":\"off\",\"subscribed\":true}"),
        ==, DisplayStatusOff);
    g_assert_cmpint(DisplayStatusParse(
        "{\"returnValue\":true,\"event\":\"request\",\"state\":\"dimmed\",\"subscribed\":true}"),
        ==, DisplayStatusOn);
}

static void test_display_events(void)
{
    g_assert_cmpint(DisplayStatusParse("{\"returnValue\":true,\"event\":\"displayOff\"}"),
                    ==, DisplayStatusOff);
    g_assert_cmpint(DisplayStatusParse("{\"returnValue\":true,\"event\":\"displayOn\"}"),
                    ==, DisplayStatusOn);
    g_assert_cmpint(DisplayStatusParse(
        "{\"returnValue\":true,\"event\":\"displayOn\",\"dockMode\":true}"),
        ==, DisplayStatusOn);
    g_assert_cmpint(DisplayStatusParse("{\"returnValue\":true,\"event\":\"displayDimmed\"}"),
                    ==, DisplayStatusOn);

    /* events that say nothing about the panel must not touch the state */
    g_assert_cmpint(DisplayStatusParse(
        "{\"returnValue\":true,\"event\":\"changedTimeout\",\"timeout\":120}"),
        ==, DisplayStatusUnchanged);
    g_assert_cmpint(DisplayStatusParse("{\"returnValue\":true,\"event\":\"blockedDisplay\"}"),
                    ==, DisplayStatusUnchanged);
    g_assert_cmpint(DisplayStatusParse("{\"returnValue\":true,\"event\":\"displayActive\"}"),
                    ==, DisplayStatusUnchanged);
}

static void test_display_errors(void)
{
    /* what the hub sends when the service goes away or refuses the call */
    g_assert_cmpint(DisplayStatusParse(
        "{\"serviceName\":\"com.palm.display\",\"returnValue\":false,"
        "\"errorCode\":-1,\"errorText\":\"Service is down\"}"),
        ==, DisplayStatusError);
    g_assert_cmpint(DisplayStatusParse(
        "{\"errorCode\":-1,\"errorText\":\"Not permitted to call\"}"),
        ==, DisplayStatusError);
    g_assert_cmpint(DisplayStatusParse("{\"returnValue\":false,\"state\":\"off\"}"),
                    ==, DisplayStatusError);
    g_assert_cmpint(DisplayStatusParse("not json"), ==, DisplayStatusError);
    g_assert_cmpint(DisplayStatusParse("[1,2]"), ==, DisplayStatusError);
    g_assert_cmpint(DisplayStatusParse(""), ==, DisplayStatusError);
    g_assert_cmpint(DisplayStatusParse(NULL), ==, DisplayStatusError);

    /* a state we do not know is not an error, but tells us nothing */
    g_assert_cmpint(DisplayStatusParse("{\"returnValue\":true,\"state\":\"undefined\"}"),
                    ==, DisplayStatusUnchanged);
}

static void test_charger_signals(void)
{
    /* chargerConnected on /com/palm/power */
    g_assert_cmpint(ChargerStatusParse("{\"connected\":true}"), ==, 1);
    g_assert_cmpint(ChargerStatusParse("{\"connected\":false}"), ==, 0);

    /* chargerStatus on /com/palm/power */
    g_assert_cmpint(ChargerStatusParse(
        "{\"type\":\"usb\",\"name\":\"pc\",\"connected\":true,"
        "\"current_mA\":500,\"message_source\":\"batteryd\"}"), ==, 1);
    g_assert_cmpint(ChargerStatusParse(
        "{\"type\":\"none\",\"name\":\"none\",\"connected\":false,"
        "\"current_mA\":0,\"message_source\":\"batteryd\"}"), ==, 0);
}

static void test_charger_query_reply(void)
{
    /* chargerStatusQuery has no "connected"; USB and dock are separate */
    g_assert_cmpint(ChargerStatusParse(
        "{\"DockConnected\":false,\"DockPower\":false,\"DockSerialNo\":\"NULL\","
        "\"USBConnected\":true,\"USBName\":\"pc\",\"Charging\":true}"), ==, 1);
    g_assert_cmpint(ChargerStatusParse(
        "{\"DockConnected\":true,\"DockPower\":true,\"DockSerialNo\":\"NULL\","
        "\"USBConnected\":false,\"USBName\":\"none\",\"Charging\":true}"), ==, 1);
    g_assert_cmpint(ChargerStatusParse(
        "{\"DockConnected\":false,\"DockPower\":false,\"DockSerialNo\":\"NULL\","
        "\"USBConnected\":false,\"USBName\":\"none\",\"Charging\":false}"), ==, 0);
}

static void test_charger_unknown(void)
{
    /* addmatch acks, hub errors, unrelated signals: leave the state alone */
    g_assert_cmpint(ChargerStatusParse("{\"returnValue\":true}"), ==, -1);
    g_assert_cmpint(ChargerStatusParse(
        "{\"returnValue\":false,\"errorCode\":-1,\"errorText\":\"Service is down\"}"), ==, -1);
    g_assert_cmpint(ChargerStatusParse("{\"percent\":57}"), ==, -1);
    g_assert_cmpint(ChargerStatusParse("garbage"), ==, -1);
    g_assert_cmpint(ChargerStatusParse(NULL), ==, -1);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/status_parse/display/subscribe_reply", test_display_subscribe_reply);
    g_test_add_func("/status_parse/display/events", test_display_events);
    g_test_add_func("/status_parse/display/errors", test_display_errors);
    g_test_add_func("/status_parse/charger/signals", test_charger_signals);
    g_test_add_func("/status_parse/charger/query_reply", test_charger_query_reply);
    g_test_add_func("/status_parse/charger/unknown", test_charger_unknown);

    return g_test_run();
}
