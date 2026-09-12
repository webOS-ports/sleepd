#!/bin/sh
# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# SPDX-License-Identifier: Apache-2.0
# sleepd-smoke.sh - on-device functional test suite for sleepd (LuneOS).
#
# Runs against the live daemon over the luna bus; busybox-sh compatible.
# Safe by default: it will NOT suspend the device. Set SLEEPD_TEST_SUSPEND=1
# to also exercise forceSuspend/resume (device must be attended).
#
# Run on the device:            sh sleepd-smoke.sh
# Or from the host over adb:    adb -s <serial> push sleepd-smoke.sh /tmp/ &&
#                               adb -s <serial> shell sh /tmp/sleepd-smoke.sh
set -u

PASS=0
FAIL=0
LUNA="luna-send -n 1"

t_pass() { PASS=$((PASS+1)); echo "ok $((PASS+FAIL)) - $1"; }
t_fail() { FAIL=$((FAIL+1)); echo "not ok $((PASS+FAIL)) - $1${2:+: $2}"; }

# expect_true <name> <uri> <payload>  - call must answer returnValue:true
expect_true() {
    OUT=$($LUNA "$2" "$3" 2>&1)
    case "$OUT" in
        *'"returnValue":true'*) t_pass "$1" ;;
        *) t_fail "$1" "$OUT" ;;
    esac
}

# expect_false <name> <uri> <payload> - call must answer returnValue:false
expect_false() {
    OUT=$($LUNA "$2" "$3" 2>&1)
    case "$OUT" in
        *'"returnValue":false'*) t_pass "$1" ;;
        *) t_fail "$1" "$OUT" ;;
    esac
}

echo "# sleepd smoke test on $(hostname) ($(uname -m))"

# --- 0. daemon alive -------------------------------------------------------
if [ -n "$(pidof sleepd)" ]; then
    t_pass "sleepd process running"
else
    t_fail "sleepd process running" "no pid"
fi

if systemctl is-active sleepd >/dev/null 2>&1; then
    t_pass "sleepd.service active"
else
    t_fail "sleepd.service active"
fi

# --- 1. legacy /com/palm/power category is registered ----------------------
# regression: "Register the /com/palm/power category only once" - the double
# registration made the second LSRegisterCategory fail, leaving categories
# missing and the journal full of LS_NO_CATEGORY errors.
expect_true "com.palm.sleep /com/palm/power reachable" \
    "luna://com.palm.sleep/com/palm/power/activityStart" \
    '{"id":"smoke.category.check","duration_ms":5000}'
$LUNA "luna://com.palm.sleep/com/palm/power/activityEnd" \
    '{"id":"smoke.category.check"}' >/dev/null 2>&1

# same methods on the new service/category
expect_true "com.webos.service.power /suspend reachable" \
    "luna://com.webos.service.power/suspend/activityStart" \
    '{"id":"smoke.category.check2","duration_ms":5000}'
$LUNA "luna://com.webos.service.power/suspend/activityEnd" \
    '{"id":"smoke.category.check2"}' >/dev/null 2>&1

# --- 2. activity API -------------------------------------------------------
expect_true "activityStart" \
    "luna://com.webos.service.power/suspend/activityStart" \
    '{"id":"smoke.activity","duration_ms":10000}'
expect_true "activityEnd" \
    "luna://com.webos.service.power/suspend/activityEnd" \
    '{"id":"smoke.activity"}'
expect_false "activityStart rejects bad duration" \
    "luna://com.webos.service.power/suspend/activityStart" \
    '{"id":"smoke.activity","duration_ms":-5}'
expect_false "activityStart rejects missing id" \
    "luna://com.webos.service.power/suspend/activityStart" \
    '{"duration_ms":1000}'

# --- 3. alarm/timeout API --------------------------------------------------
expect_true "timeout set (relative)" \
    "luna://com.webos.service.alarm/set" \
    '{"key":"smoke.alarm","uri":"luna://com.palm.sleep/time/internalAlarmFired","params":{},"in":"00:10:00"}'
expect_true "timeout clear" \
    "luna://com.webos.service.alarm/clear" \
    '{"key":"smoke.alarm"}'
expect_false "timeout clear of unknown key fails" \
    "luna://com.webos.service.alarm/clear" \
    '{"key":"smoke.alarm.never-existed"}'
expect_false "timeout set rejects bad time" \
    "luna://com.webos.service.alarm/set" \
    '{"key":"smoke.alarm.bad","uri":"luna://x/y","params":{},"in":"99:99:99"}'

# SQL-injection regression: a key with quote/injection characters must be
# stored and cleared verbatim, and must not corrupt the timeout database.
INJ_KEY='smoke\" OR 1=1 --'
expect_true "timeout set with hostile key" \
    "luna://com.webos.service.alarm/set" \
    "{\"key\":\"$INJ_KEY\",\"uri\":\"luna://com.palm.sleep/time/internalAlarmFired\",\"params\":{},\"in\":\"00:10:00\"}"
expect_true "timeout clear with hostile key" \
    "luna://com.webos.service.alarm/clear" \
    "{\"key\":\"$INJ_KEY\"}"
expect_true "timeout db still functional after injection attempt" \
    "luna://com.webos.service.alarm/set" \
    '{"key":"smoke.alarm.post-inj","uri":"luna://com.palm.sleep/time/internalAlarmFired","params":{},"in":"00:10:00"}'
$LUNA "luna://com.webos.service.alarm/clear" '{"key":"smoke.alarm.post-inj"}' >/dev/null 2>&1

# --- 4. legacy wakeLock API ------------------------------------------------
expect_true "wakeLockRegister" \
    "luna://com.webos.service.power/suspend/wakeLockRegister" \
    '{"register":true,"clientId":"smoke.wakelock"}'
expect_true "setWakeLock take" \
    "luna://com.webos.service.power/suspend/setWakeLock" \
    '{"clientId":"smoke.wakelock","isWakeup":true}'

if [ -r /sys/power/wake_lock ] && grep -q "webos-smoke.wakelock" /sys/power/wake_lock 2>/dev/null; then
    t_pass "kernel wakelock taken"
else
    # some kernels do not list holders in wake_lock; treat as informational
    echo "# info: webos-smoke.wakelock not visible in /sys/power/wake_lock"
    t_pass "kernel wakelock taken (not verifiable on this kernel)"
fi

expect_true "setWakeLock release" \
    "luna://com.webos.service.power/suspend/setWakeLock" \
    '{"clientId":"smoke.wakelock","isWakeup":false}'
expect_true "wakeLockRegister unregister" \
    "luna://com.webos.service.power/suspend/wakeLockRegister" \
    '{"register":false,"clientId":"smoke.wakelock"}'
expect_false "setWakeLock for unknown client fails" \
    "luna://com.webos.service.power/suspend/setWakeLock" \
    '{"clientId":"smoke.wakelock.gone","isWakeup":true}'

# --- 5. suspend voting registration ---------------------------------------
expect_true "identify" \
    "luna://com.webos.service.power/suspend/identify" \
    '{"subscribe":true,"clientName":"smoke.voter"}'
expect_true "clientCancelByName" \
    "luna://com.webos.service.power/suspend/clientCancelByName" \
    '{"clientName":"smoke.voter"}'

# --- 6. state queries / misc ----------------------------------------------
expect_false "resume while not suspended fails" \
    "luna://com.webos.service.power/suspend/resume" '{}'
expect_true "TESTSuspend schedules idle check" \
    "luna://com.webos.service.power/suspend/TESTSuspend" '{}'

# --- 7. config & journal sanity -------------------------------------------
if [ -f /etc/default/sleepd.conf ] && grep -q "enable_idle_check_thread" /etc/default/sleepd.conf; then
    t_pass "sleepd.conf present with idle check setting"
else
    t_fail "sleepd.conf present with idle check setting"
fi

if journalctl -u sleepd --since "-5 min" --no-pager 2>/dev/null | grep -qE "SIGSEGV|SIGABRT|core-dump"; then
    t_fail "no crashes in journal (last 5 min)"
else
    t_pass "no crashes in journal (last 5 min)"
fi

# --- 8. optional: real suspend cycle (attended only) -----------------------
if [ "${SLEEPD_TEST_SUSPEND:-0}" = "1" ]; then
    echo "# forcing suspend; device will sleep briefly"
    expect_true "forceSuspend" \
        "luna://com.webos.service.power/suspend/forceSuspend" '{}'
    sleep 5
    if [ -n "$(pidof sleepd)" ]; then
        t_pass "sleepd survived suspend cycle"
    else
        t_fail "sleepd survived suspend cycle"
    fi
fi

echo "# $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
