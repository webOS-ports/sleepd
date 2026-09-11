#!/bin/sh
# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# SPDX-License-Identifier: Apache-2.0
# run-all.sh [sleepd-binary] [adb-serial]
#
# Runs every host-side layer of the harness; optionally also audits a built
# binary and drives the on-device suite over adb.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
fail=0

echo "==== unit tests"
make -C "$HERE/unit" check || fail=1

echo "==== static analysis"
"$HERE/hardening/static-analysis.sh" || fail=1

if [ $# -ge 1 ]; then
    echo "==== binary hardening: $1"
    "$HERE/hardening/check-binary.sh" "$1" || fail=1
fi

if [ $# -ge 2 ]; then
    echo "==== on-device suite (adb $2)"
    adb -s "$2" push "$HERE/on-device/sleepd-smoke.sh" /tmp/sleepd-smoke.sh >/dev/null &&
    adb -s "$2" shell sh /tmp/sleepd-smoke.sh || fail=1
fi

exit $fail
