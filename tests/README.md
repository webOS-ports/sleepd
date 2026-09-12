# sleepd test & hardening harness

Three layers, runnable independently or together via `./run-all.sh`.

## Layout

    tests/
    ├── run-all.sh              host entry point: unit + static analysis
    │                           (+ binary check / device suite when args given)
    ├── unit/                   host unit tests (glib-2.0 only, ASan+UBSan)
    │   ├── Makefile            make -C tests/unit          # build + run
    │   └── stubs/              PmLogLib/luna-service2 stand-ins
    ├── hardening/
    │   ├── static-analysis.sh  cppcheck gate; clang-tidy too when
    │   │                       SLEEPD_COMPILE_DB points at a build dir
    │   │                       with compile_commands.json
    │   └── check-binary.sh     ELF mitigation audit of a built sleepd:
    │                           PIE, full RELRO, canary, FORTIFY,
    │                           NX stack, no rpath (works on cross binaries)
    └── on-device/
        └── sleepd-smoke.sh     functional suite against the live daemon
                                over the luna bus (busybox-sh compatible)

## Host tests

    make -C tests/unit                                    # 12 unit tests
    tests/hardening/static-analysis.sh                    # cppcheck only
    SLEEPD_COMPILE_DB=/path/to/build tests/hardening/static-analysis.sh
    tests/hardening/check-binary.sh /path/to/build/sleepd

The unit tests compile the units under test straight out of `src/` against
the stub headers, so no webOS sysroot is needed. They pin regressions for:

- `WaitObjectWait` ms→ns conversion (pre-fix: EINVAL → `g_error()` abort)
- timersource arithmetic after the GTimeVal → gint64 rework, including
  poll-timeout clamping for week-long alarm intervals
- `wait_alarms_ms` config parsing (pre-fix: read as boolean, never applied)

## On-device suite

    adb -s <serial> push tests/on-device/sleepd-smoke.sh /tmp/
    adb -s <serial> shell sh /tmp/sleepd-smoke.sh

Safe by default: it never suspends the device. `SLEEPD_TEST_SUSPEND=1`
additionally exercises a real forceSuspend/resume cycle (attended only).

Covers: category registration on both service names (regression for the
double /com/palm/power registration), the activity API and its input
validation, alarm set/clear including an SQL-injection regression with a
hostile key, the legacy wakeLockRegister/setWakeLock pair down to the
kernel wakelock in /sys/power/wake_lock, identify/clientCancelByName,
error paths, config presence, and a crash scan of the journal.

## Cross-building against the OE tree

The OE workdirs under `tmp/work/*/sleepd/` provide the recipe sysroots and
toolchain env used by CI-style rebuilds; any build that produces
`compile_commands.json` feeds the clang-tidy gate.
