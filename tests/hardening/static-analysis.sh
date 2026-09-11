#!/bin/sh
# static-analysis.sh [srcdir]
#
# Static-analysis gate for sleepd. Fails on any cppcheck error/warning
# severity finding. clang-tidy runs too when a compile database is
# available (point SLEEPD_COMPILE_DB at a directory containing
# compile_commands.json from any build of this tree).
set -u

SRC="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
fail=0

if command -v cppcheck >/dev/null 2>&1; then
    echo "== cppcheck"
    OUT=$(cppcheck --enable=warning,portability --inconclusive --std=c99 \
                   --force --inline-suppr \
                   --suppress=missingIncludeSystem --suppress=unmatchedSuppression \
                   -I "$SRC/include/internal" "$SRC/src" 2>&1 >/dev/null | \
          grep -E ': (error|warning):') || true
    if [ -n "$OUT" ]; then
        echo "$OUT"
        fail=1
    else
        echo "clean"
    fi
else
    echo "cppcheck not installed; skipping" >&2
fi

if command -v clang-tidy >/dev/null 2>&1 && [ -n "${SLEEPD_COMPILE_DB:-}" ]; then
    echo "== clang-tidy"
    OUT=$(clang-tidy -p "$SLEEPD_COMPILE_DB" --quiet \
            --checks='clang-analyzer-*,bugprone-*,concurrency-*,-bugprone-easily-swappable-parameters,-bugprone-narrowing-conversions,-bugprone-reserved-identifier,-bugprone-assignment-in-if-condition,-bugprone-unsafe-functions,-bugprone-implicit-widening-of-multiplication-result' \
            "$SRC"/src/*.c "$SRC"/src/*/*.c 2>/dev/null | grep "warning:") || true
    if [ -n "$OUT" ]; then
        echo "$OUT"
        fail=1
    else
        echo "clean"
    fi
else
    echo "clang-tidy skipped (not installed or SLEEPD_COMPILE_DB unset)"
fi

exit $fail
