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
 * @file suspend.c
 *
 * @brief Suspend/Resume logic to conserve battery when device is idle.
 *
 */

/***
 * PwrEvent State Machine.
 */

#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

#include <syslog.h>

#include "suspend.h"
#include "clock.h"
#include "wait.h"
#include "machine.h"
#include "sleepd_debug.h"
#include "main.h"
#include "timersource.h"
#include "activity.h"
#include "logging.h"
#include "client.h"
#include "timesaver.h"
#include "init.h"
#include "timeout_alarm.h"
#include "reference_time.h"
#include "sleepd_config.h"
#include "sawmill_logger.h"
#include "status_parse.h"
#include "sysfs.h"
#include "nyx/nyx_client.h"

#include <json.h>
#include <luna-service2/lunaservice.h>

#define LOG_DOMAIN "PWREVENT-SUSPEND: "

#define kPowerBatteryCheckReasonSysfs "/sys/power/batterycheck_wakeup"
#define kPowerWakeupSourcesSysfs      "/sys/power/wakeup_event_list"

#define MIN_IDLE_SEC 5

/*
 * @brief Power States
 */

enum
{
    kPowerStateOn,
    kPowerStateOnIdle,
    kPowerStateSuspendRequest,
    kPowerStatePrepareSuspend,
    kPowerStateSleep,
    kPowerStateKernelResume,
    kPowerStateActivityResume,
    kPowerStateAbortSuspend,
    kPowerStateLast
};
typedef int PowerState;

enum
{
    kResumeTypeKernel,
    kResumeTypeActivity,
    kResumeAbortSuspend
};

static const char *resume_type_descriptions[] =
{
    [kResumeTypeKernel]   = "kernel",
    [kResumeTypeActivity] = "pwrevent_activity",
    [kResumeAbortSuspend] = "abort_suspend",
};

// A PowerStateProc processes the current state and returns the next state
typedef PowerState(*PowerStateProc)(void);

typedef struct
{
    PowerState     state; // currently unused
    PowerStateProc function;
} PowerStateNode;

/*
 * @brief State Functions
 */

static PowerState StateOn(void);
static PowerState StateOnIdle(void);
static PowerState StateSuspendRequest(void);
static PowerState StatePrepareSuspend(void);
static PowerState StateSleep(void);
static PowerState StateKernelResume(void);
static PowerState StateActivityResume(void);
static PowerState StateAbortSuspend(void);

/**
 * @defgroup SuspendLogic   Suspend/Resume State Machine
 * @ingroup PowerEvents
 * @brief Suspend/Resume state machine:
 *
 * A separate thread "SuspendThread" handles the device suspend/resume logic, and maintains
 * a state machine with the following states:
 *
 * 1. On: This is the first state , in which the device stays as long as display is on, or
 * some activity is active or the device has been awake for less than after_resume_idle_ms.
 *
 * 2. OnIdle: The device goes into this state from "On" state, if the IdleCheck thread thinks that
 * the device can now suspend. However if the device is connected to charger and the "suspend_with_charger"
 * option is "false", the device will again go back to the "On" state, else the device will go into the next
 * state i.e the "SuspendRequest" state.
 *
 * 3. SuspendRequest: In this state the device will broadcast the "SuspendRequest" signal, to which all the
 * registered clients are supposed to respond back with an ACK / NACK. The device will stay in this state for
 * a max of 30 sec waiting for all responses. If all clients respond back with an ACK or it timesout, it will go
 * to the next state i.e "PrepareSuspend" state. However if any client responds back with a NACK it goes back
 * to the "On" state again.
 *
 * 4. PrepareSuspend: In this state, the device will broadcast the "PrepareSuspend" signal, with a max wait
 * of 5 sec for all responses. If all clients respond back with an ACK or it timesout, it will go
 * to the next state i.e "Sleep" state. However if any client responds back with NACK, it goes to the
 * "AbortSuspend" state.
 *
 * 5. Sleep: In this state it will first send the "Suspended" signal to everybody. If any activity is active
 * at this point it will go resume by going to the "ActivityResume" state. Otherwise it arms the RTC for the
 * next wakeup timeout and calls MachineSleep(), which blocks for as long as the kernel is suspended. When
 * it returns true the kernel has been down and is back up: the next state is "KernelResume". When it
 * returns false the kernel never suspended (a wakeup source raced the write, or the platform refused):
 * the next state is "AbortSuspend".
 *
 * 6. KernelResume: Reached, in the same pass through the state loop, right after the kernel wakes up. It
 * tells the platform to resume, broadcasts the "Resume" signal, schedules the next IdleCheck
 * after_resume_idle_ms later and goes to the "On" state.
 *
 * 7. ActivityResume: It will broadcast the "Resume" signal schedule the next IdleCheck sequence and go back
 * to "On" state.
 *
 * 8. AbortSuspend: It will broadcast the "Resume" signal, schedule the next IdleCheck after_resume_idle_ms
 * later (which is the retry loop for a raced suspend) and go back to the "On" state.
 */

/**
 * @addtogroup SuspendLogic
 * @{
 */


// Mapping from state to function handling state.
static const PowerStateNode kStateMachine[kPowerStateLast] =
{
    [kPowerStateOn]             = { kPowerStateOn,               StateOn },
    [kPowerStateOnIdle]         = { kPowerStateOnIdle,           StateOnIdle },
    [kPowerStateSuspendRequest] = { kPowerStateSuspendRequest,   StateSuspendRequest },
    [kPowerStatePrepareSuspend] = { kPowerStatePrepareSuspend,   StatePrepareSuspend },
    [kPowerStateSleep]          = { kPowerStateSleep,            StateSleep },
    [kPowerStateKernelResume]   = { kPowerStateKernelResume,     StateKernelResume },
    [kPowerStateActivityResume] = { kPowerStateActivityResume,   StateActivityResume },
    [kPowerStateAbortSuspend]   = { kPowerStateAbortSuspend,     StateAbortSuspend }
};

// current state
static PowerStateNode gCurrentStateNode;
//static PowerState gCurrentState;

WaitObj gWaitResumeMessage;

PowerEvent gSuspendEvent = kPowerEventNone;

GTimerSource *idle_scheduler = NULL;

GMainLoop *suspend_loop = NULL;

WaitObj gWaitSuspendResponse;
WaitObj gWaitPrepareSuspend;

struct timespec sTimeOnStartSuspend;
struct timespec sTimeOnSuspended;
struct timespec sTimeOnWake;

struct timespec sSuspendRTC;
struct timespec sWakeRTC;

/*
 * What com.palm.display last told us. Unknown counts as on: with no display
 * manager to ask, staying awake is the safe answer.
 */
bool gDisplayIsOn = true;
static LSMessageToken sDisplayStatusToken = LSMESSAGE_TOKEN_INVALID;
static void *sDisplayServerStatusCookie = NULL;

/* whether the suspend cycle in progress was started by forceSuspend */
static bool gForcedSuspend = false;

/*
 * Set once a shutdown or reboot has started; read from the suspend thread
 * and the main loop. A suspend that lands in the middle of the shutdown
 * sequence leaves the device dark with the sequence half done.
 */
static gint gShutdownInProgress = 0;
#define SHUTDOWN_WAKELOCK_NAME "sleepd_shutdown"

void SuspendIPCInit(void);
int SendSuspendRequest(const char *message);
int SendPrepareSuspend(const char *message);
int SendResume(int resumetype, char *message);
int SendSuspended(const char *message);

const char* StateToStr(PowerState state)
{
    switch (state)
    {
    case kPowerStateOn:
        return "on";
    case kPowerStateOnIdle:
        return "on-idle";
    case kPowerStateSuspendRequest:
        return "suspend-request";
    case kPowerStatePrepareSuspend:
        return "prepare-suspend";
    case kPowerStateSleep:
        return "sleep";
    case kPowerStateKernelResume:
        return "kernel-resume";
    case kPowerStateActivityResume:
        return "activity-resume";
    case kPowerStateAbortSuspend:
        return "abort-suspend";
    default:
        return "unknown";
    }
}

void
StateLoopShutdown(void)
{
    WaitObjectSignal(&gWaitSuspendResponse);
    WaitObjectSignal(&gWaitPrepareSuspend);
}

/**
 * @brief Schedule the IdleCheck thread after interval_ms from fromPoll
 */

void
ScheduleIdleCheck(int interval_ms, bool fromPoll)
{
    if (idle_scheduler)
    {
        SLEEPDLOG_DEBUG("Scheduling new idle check in %d ms", interval_ms);

        if (interval_ms <= 0)
        {
            /*
             * "Check as soon as possible" (activity.c asks for this whenever an
             * activity starts or ends). It must not be expressed as a zero
             * interval: dispatch() re-arms the source from interval_ms, so the
             * source would be ready again the moment it was dispatched, and
             * IdleCheck() only reschedules itself on the display-off path - with
             * the display on it returns straight to the loop. The result was a
             * permanent busy loop that ran IdleCheck tens of thousands of times
             * a second and cost ~25% of a CPU core.
             *
             * Fire now, but leave the repeat interval at the configured poll
             * period so the automatic re-arm is sane.
             */
            g_timer_source_set_interval(idle_scheduler,
                                        gSleepConfig.wait_idle_ms, fromPoll);
            g_timer_source_fire_now(idle_scheduler, fromPoll);
        }
        else
        {
            g_timer_source_set_interval(idle_scheduler, interval_ms, fromPoll);
        }
    }
    else
    {
        SLEEPDLOG_DEBUG("idle_scheduler not yet initialized");
    }
}

/**
 * @brief Get display status using NYX interface.
 */
static bool
IsDisplayOn(void)
{
    return gDisplayIsOn;
}

/**
 * @brief Thread that's scheduled periodically to check if the system has been idle for
 * specified time, to trigger the next state in the state machine.
 */

gboolean
IdleCheck(gpointer ctx)
{
    bool suspend_active;
    bool activity_idle;

    struct timespec now;
    int next_idle_ms = 0;

    if (SuspendInhibited())
    {
        SLEEPDLOG_DEBUG("IdleCheck: shutdown in progress; stopping idle checks");
        return G_SOURCE_REMOVE;
    }

    /*
     * With the display on there is nothing to decide, and this runs at
     * 2 Hz for as long as the screen is lit; only narrate the display-off
     * polls, where the outcome varies.
     */
    if (!IsDisplayOn())
    {
        SLEEPDLOG_DEBUG("IdleCheck: state %s", StateToStr(gCurrentStateNode.state));
    }

    /*
     * Drop activities that have outlived their duration whatever the display
     * is doing. Each one holds a kernel wakelock that _activity_stop_activity()
     * is the only thing that releases, and the sole call to this used to sit
     * inside the display-off branch below - so with the display on, or merely
     * believed to be on, a one-second activity kept its wakelock indefinitely.
     *
     * Observed on a PinePhone Pro: com.webos.service.alarm.timeout_fired asks
     * for TIMEOUT_KEEP_ALIVE_MS (1000ms) and its wakelock was still held
     * minutes later, released only when the next timeout fired and
     * _activity_start() stopped the previous instance by name.
     */
    ClockGetTime(&now);
    PwrEventActivityRemoveExpired(&now);

    if (!IsDisplayOn())
    {
        SLEEPDLOG_DEBUG("IdleCheck: display off");

        ClockGetTime(&now);

        /*
         * Enforce that the minimum time awake must be at least
         * after_resume_idle_ms.
         */
        struct timespec last_wake;
        last_wake.tv_sec = sTimeOnWake.tv_sec;
        last_wake.tv_nsec = sTimeOnWake.tv_nsec;

        ClockAccumMs(&last_wake, gSleepConfig.after_resume_idle_ms);

        if (!ClockTimeIsGreater(&last_wake, &now))
        {
            /*
             * Do not sleep if any activity is still active
             */

            activity_idle = PwrEventActivityCanSleep(&now);

            if (!activity_idle)
            {
                SLEEPDLOG_DEBUG("Can't sleep because an activity is active: ");
            }

            if (PwrEventActivityCount(&sTimeOnWake))
            {
                SLEEPDLOG_DEBUG("Activities since wake: ");
                PwrEventActivityPrintFrom(&sTimeOnWake);
            }

            PwrEventActivityRemoveExpired(&now);
            {
                time_t expiry = 0;
                gchar *app_id = NULL;
                gchar *key = NULL;

                if (timeout_get_next_wakeup(&expiry, &app_id, &key))
                {
                    g_free(app_id);
                    g_free(key);
                    int next_wake = expiry - reference_time();

                    if (next_wake >= 0 && next_wake <= gSleepConfig.wait_alarms_s)
                    {
                        SLEEPDLOG_DEBUG("Not going to sleep because an alarm is about to fire in %d sec\n",
                                        next_wake);
                        goto resched;
                    }
                }
            }

            // temporary hack, to be removed once compositor starts registering with com.webos.service.power
#if 1
            suspend_active = (access("/tmp/suspend_active", R_OK) == 0);

            if (suspend_active && activity_idle)
            {
                TriggerSuspend("device is idle.", kPowerEventIdleEvent);
            }

#endif
        }
        else
        {
            struct timespec diff;
            ClockDiff(&diff, &last_wake, &now);
            next_idle_ms = ClockGetMs(&diff);
        }

resched:
        {
            long wait_idle_ms = gSleepConfig.wait_idle_ms;
            long max_duration_ms = PwrEventActivityGetMaxDuration(&now);

            if (max_duration_ms > wait_idle_ms)
            {
                wait_idle_ms = max_duration_ms;
            }

            if (next_idle_ms > wait_idle_ms)
            {
                wait_idle_ms = next_idle_ms;
            }

            ScheduleIdleCheck(wait_idle_ms, true);
        }
    }

    return TRUE;
}

static gboolean
SuspendStateUpdate(PowerEvent power_event)
{
    gSuspendEvent = power_event;
    PowerState next_state = kPowerStateLast;

    SLEEPDLOG_DEBUG("%s: state %s", __PRETTY_FUNCTION__, StateToStr(gCurrentStateNode.state));

    do
    {
        SLEEPDLOG_DEBUG("In state '%s'", StateToStr(gCurrentStateNode.state));
        next_state = gCurrentStateNode.function();
        SLEEPDLOG_DEBUG("Next state will be '%s'", StateToStr(next_state));

        if (next_state >= 0 && next_state < kPowerStateLast)
        {
            gCurrentStateNode = kStateMachine[next_state];
        }
    }
    while (next_state != kPowerStateLast);

    return FALSE;
}

/**
 * @brief Suspend state machine is run in this thread.
 *
 * @param  ctx
 *
 * @retval
 */
void *
SuspendThread(void *ctx)
{
    GMainContext *context;
    context = g_main_context_new();

    suspend_loop = g_main_loop_new(context, FALSE);

    idle_scheduler = g_timer_source_new(
                         gSleepConfig.wait_idle_ms, gSleepConfig.wait_idle_granularity_ms);

    g_source_set_callback((GSource *)idle_scheduler,
                          IdleCheck, NULL, NULL);
    g_source_attach((GSource *)idle_scheduler,
                    g_main_loop_get_context(suspend_loop));

    g_main_loop_run(suspend_loop);
    g_source_unref((GSource *)idle_scheduler);
    g_main_loop_unref(suspend_loop);
    g_main_context_unref(context);

    return NULL;
}

/**
 * @brief This is the first state , in which the device stays as long as display is on, or
 * some activity is active or the device has been awake for less than after_resume_idle_ms.
 *
 * @retval PowerState Next state
 */

static PowerState
StateOn(void)
{
    PowerState next_state;

    switch (gSuspendEvent)
    {
        case kPowerEventForceSuspend:
            next_state = kPowerStateSuspendRequest;
            break;

        case kPowerEventIdleEvent:
            next_state = kPowerStateOnIdle;
            break;

        case kPowerEventNone:
        default:
            next_state = kPowerStateLast;
            break;
    }

    /*
     * gSuspendEvent is consumed here, so later states cannot tell a forced
     * cycle from an idle one by looking at it; remember it for StateSleep,
     * where forceSuspend is meant to override the charger and activity vetoes.
     */
    gForcedSuspend = (gSuspendEvent == kPowerEventForceSuspend);
    gSuspendEvent = kPowerEventNone;

    return next_state;
}

/**
 * @brief The device goes into this state from "On" state, if the IdleCheck thread thinks that
 * the device can now suspend. However if the device is connected to charger and the "suspend_with_charger"
 * option is "false", the device will again go back to the "On" state, else the device will go into the next
 * state i.e the "SuspendRequest" state.
 *
 * @retval PowerState Next state
 */

static PowerState
StateOnIdle(void)
{
    if (!MachineCanSleep())
    {
        return kPowerStateOn;
    }

    return kPowerStateSuspendRequest;
}

#define START_LOG_COUNT 8
#define MAX_LOG_COUNT_INCREASE_RATE 512

/**
 * @brief In this state the device will broadcast the "SuspendRequest" signal, to which all the
 * registered clients are supposed to respond back with an ACK / NACK. The device will stay in this state for
 * a max of 30 sec waiting for all responses. If all clients respond back with an ACK or it timesout, it will go
 * to the next state i.e "PrepareSuspend" state. However if any client responds back with a NACK it goes back
 * to the "On" state again.
 *
 * @retval PowerState Next state.
 */

static PowerState
StateSuspendRequest(void)
{
    int timeout = 0;
    static int successive_ons = 0;
    static int log_count = START_LOG_COUNT;
    PowerState ret;

    ClockGetTime(&sTimeOnStartSuspend);

    WaitObjectLock(&gWaitSuspendResponse);

    PwrEventVoteInit();

    SendSuspendRequest("");

    // send msg to ask for permission to sleep
    SLEEPDLOG_DEBUG("Sent \"suspend request\", waiting up to %dms",
                    gSleepConfig.wait_suspend_response_ms);

    if (!PwrEventClientsApproveSuspendRequest())
    {
        // wait for the message to arrive
        timeout = WaitObjectWait(&gWaitSuspendResponse,
                                 gSleepConfig.wait_suspend_response_ms);
    }

    WaitObjectUnlock(&gWaitSuspendResponse);

    PwrEventClientTablePrint(G_LOG_LEVEL_DEBUG);

    if (timeout)
    {
        gchar *silent_clients = PwrEventGetSuspendRequestNORSPList();
        SLEEPDLOG_DEBUG("We timed-out waiting for daemons (%s) to acknowledge SuspendRequest.",
                        silent_clients);
        g_free(silent_clients);
        ret = kPowerStatePrepareSuspend;
    }
    else if (PwrEventClientsApproveSuspendRequest())
    {
        PMLOG_TRACE("Suspend response: go to prepare_suspend");
        ret = kPowerStatePrepareSuspend;
    }
    else
    {
        PMLOG_TRACE("Suspend response: stay awake");
        ret = kPowerStateOn;
    }

    if (ret == kPowerStateOn)
    {
        successive_ons++;

        if (successive_ons >= log_count)
        {
            SLEEPDLOG_DEBUG("%d successive votes to NACK SuspendRequest since previous suspend",
                            successive_ons);
            PwrEventClientTablePrint(G_LOG_LEVEL_WARNING);

            if (log_count >= MAX_LOG_COUNT_INCREASE_RATE)
            {
                log_count += MAX_LOG_COUNT_INCREASE_RATE;
            }
            else
            {
                log_count *= 2;
            }

            SLEEPDLOG_DEBUG("SuspendRequest - next count before logging is %d", log_count);
        }
    }
    else
    {
        // reset the exponential counter
        successive_ons = 0;
        log_count = START_LOG_COUNT;
    }

    return ret;
}

/**
 * @brief In this state, the device will broadcast the "PrepareSuspend" signal, with a max wait of 5 sec
 * for all responses. If all clients respond back with an ACK or it timesout, it will go to the next state
 * i.e "Sleep" state. However if any client responds back with NACK, it goes to the "AbortSuspend" state.
 *
 * @retval PowerState Next state.
 */

static PowerState
StatePrepareSuspend(void)
{
    int timeout = 0;
    static int successive_ons = 0;
    static int log_count = START_LOG_COUNT;

    WaitObjectLock(&gWaitPrepareSuspend);

    // send suspend request to all power-aware daemons.
    SendPrepareSuspend("");

    PMLOG_TRACE("Sent \"prepare suspend\", waiting up to %dms",
                gSleepConfig.wait_prepare_suspend_ms);

    if (!PwrEventClientsApprovePrepareSuspend())
    {

        timeout = WaitObjectWait(&gWaitPrepareSuspend,
                                 gSleepConfig.wait_prepare_suspend_ms);
    }

    WaitObjectUnlock(&gWaitPrepareSuspend);

    PwrEventClientTablePrint(G_LOG_LEVEL_DEBUG);

    if (timeout)
    {
        gchar *silent_clients = PwrEventGetPrepareSuspendNORSPList();
        SLEEPDLOG_DEBUG("We timed-out waiting for daemons (%s) to acknowledge PrepareSuspend.",
                        silent_clients);
        gchar *clients = PwrEventGetClientTable();

        SLEEPDLOG_DEBUG("== NORSP clients ==\n %s\n == client table ==\n %s",
                        silent_clients, clients);
        g_free(clients);
        g_free(silent_clients);

        // reset the exponential counter
        successive_ons = 0;
        log_count = START_LOG_COUNT;
        return kPowerStateSleep;
    }
    else if (PwrEventClientsApprovePrepareSuspend())
    {
        PMLOG_TRACE("Clients all approved prepare_suspend");
        // reset the exponential counter
        successive_ons = 0;
        log_count = START_LOG_COUNT;
        return kPowerStateSleep;
    }
    else
    {
        // if any daemons nacked, quit suspend...
        PMLOG_TRACE("Some daemon nacked prepare_suspend: stay awake");
        successive_ons++;

        if (successive_ons >= log_count)
        {
            SLEEPDLOG_DEBUG("%d successive votes to NACK PrepareSuspend since previous suspend",
                            successive_ons);
            PwrEventClientTablePrint(G_LOG_LEVEL_WARNING);

            if (log_count >= MAX_LOG_COUNT_INCREASE_RATE)
            {
                log_count += MAX_LOG_COUNT_INCREASE_RATE;
            }
            else
            {
                log_count *= 2;
            }

            SLEEPDLOG_DEBUG("PrepareSuspend - next count before logging is %d", log_count);
        }

        return kPowerStateAbortSuspend;
    }
}

/**
 * @brief Instrument how much time it took to sleep.
 */
void
InstrumentOnSleep(void)
{
    struct timespec diff;
    struct timespec diffAwake;

    ClockGetTime(&sTimeOnSuspended);
    get_time_now(&sSuspendRTC);

    ClockDiff(&diff, &sTimeOnSuspended, &sTimeOnStartSuspend);

    ClockDiff(&diffAwake, &sTimeOnSuspended, &sTimeOnWake);

    GString *str = g_string_new("");

    g_string_append_printf(str, "PWREVENT-SLEEP after ");
    ClockStr(str, &diffAwake);
    g_string_append(str, "... decision took ");
    ClockStr(str, &diff);

    SLEEPDLOG_DEBUG(" Clock String : %s", str->str);

    g_string_free(str, TRUE);

    /* Rate-limited print NACK sources. */
    PwrEventClientPrintNACKRateLimited();

    sawmill_logger_record_sleep(diffAwake);
}

/**
 * @brief Instrument how much time it took to wake back up.
 */
void
InstrumentOnWake(int resumeType)
{
    ClockGetTime(&sTimeOnWake);
    get_time_now(&sWakeRTC);

    struct timespec diffAsleep;
    ClockDiff(&diffAsleep, &sWakeRTC, &sSuspendRTC);

    struct tm tm;
    gmtime_r(&(diffAsleep.tv_sec), &tm);

    if (tm.tm_year >= 70)
    {
        tm.tm_year -= 70;    // EPOCH returned by time() starts at 1970
    }

    GString *str = g_string_new("PWREVENT-WOKE after ");
    g_string_append_printf(str, "%lds : ", diffAsleep.tv_sec);

    if (tm.tm_year > 0)
    {
        g_string_append_printf(str, "%d years, ", tm.tm_year);
    }

    g_string_append_printf(str, "%d days, %dh-%dm-%ds\n", tm.tm_yday,
                           tm.tm_hour, tm.tm_min, tm.tm_sec);

    SLEEPDLOG_DEBUG("%s (%s)", str->str, resume_type_descriptions[resumeType]);

    g_string_free(str, TRUE);

    sawmill_logger_record_wake(diffAsleep);
}

static bool
CheckActivitiesActive(struct timespec *now)
{
    if (MachineSupportsWakelocks())
    {
        return PwrEventActivityCheckActivitiesActive(now);
    }

    return PwrEventFreezeActivities(now);
}

/**
 * @brief In this state it will first send the "Suspended" signal to everybody. If any activity is active
 * at this point it will go resume by going to the "ActivityResume" state, else it arms the wakeup alarm and
 * lets the machine sleep.
 *
 * MachineSleep() blocks until the kernel has suspended and resumed again, so on a true return the device
 * has already been through a full suspend cycle and the next state is "KernelResume". On a false return
 * the kernel never went down - a wakeup source raced the suspend write, or the platform refused - and the
 * next state is "AbortSuspend", which schedules the retry.
 *
 * A forced suspend (forceSuspend over luna) skips the charger and activity vetoes, as its documentation
 * has always promised; it does not skip the client vote, the wakeup alarm, or kernel wakelocks.
 *
 * @retval PowerState Next state.
 */

static PowerState
StateSleep(void)
{
    int nextState = kPowerStateAbortSuspend;

    PMLOG_TRACE("State Sleep, We will try to go to sleep now");

    SendSuspended("attempting to suspend (We are trying to sleep)");

    {
        time_t expiry = 0;
        gchar *app_id = NULL;
        gchar *key = NULL;

        if (timeout_get_next_wakeup(&expiry, &app_id, &key))
        {
            SLEEPDLOG_DEBUG("waking in %ld seconds for %s", expiry - reference_time(), key);
        }

        g_free(app_id);
        g_free(key);
    }

    InstrumentOnSleep();

    // save the current time to disk in case battery is pulled.
    timesaver_save();

    // if any activities were started, abort suspend.
    if (!gForcedSuspend && !CheckActivitiesActive(&sTimeOnSuspended))
    {
        SLEEPDLOG_DEBUG("aborting sleep because of current activity");
        PwrEventActivityPrintFrom(&sTimeOnSuspended);
        nextState = kPowerStateActivityResume;
    }
    else if (!gForcedSuspend && !MachineCanSleep())
    {
        SLEEPDLOG_DEBUG("We couldn't sleep because charger was connected");
    }
    else if (SuspendInhibited())
    {
        SLEEPDLOG_DEBUG("We couldn't sleep because a shutdown is in progress");
    }
    else if (!queue_next_wakeup())
    {
        SLEEPDLOG_DEBUG("We couldn't sleep because we can't setup the wakeup alarm");
    }
    else
    {
        SLEEPDLOG_DEBUG("Going to sleep now%s", gForcedSuspend ? " (forced)" : "");

        if (MachineSleep())
        {
            SLEEPDLOG_DEBUG("Kernel resumed");
            nextState = kPowerStateKernelResume;
        }
        else
        {
            SLEEPDLOG_DEBUG("We couldn't sleep because the suspend request failed (wakeup source raced, or platform refused)");
        }
    }

    if (nextState != kPowerStateActivityResume)
    {
        // Back from the kernel, or never went: either way activities may run again.
        PwrEventThawActivities();
    }

    gForcedSuspend = false;

    SLEEPDLOG_DEBUG("Leaving sleep state");
    return nextState;
}

/**
 * @brief In this state the "Resume" signal will be broadcasted and the device will go back to the "On" state.
 *
 * The next idle check is pushed out by after_resume_idle_ms, as after a real resume: a suspend that a
 * wakeup source raced is retried at that pace rather than at the idle poll rate, and the source that
 * raced it gets that long to finish what it woke up for.
 *
 * @retval PowerState Next state.
 */

static PowerState
StateAbortSuspend(void)
{
    PMLOG_TRACE("State Abort suspend");
    if (!MachineSupportsWakelocks())
    {
        PwrEventThawActivities();
    }
    SendResume(kResumeAbortSuspend, "resume (suspend aborted)");

    ClockGetTime(&sTimeOnWake);
    ScheduleIdleCheck(gSleepConfig.after_resume_idle_ms, false);

    return kPowerStateOn;
}


/**
 * @brief Broadcast the resume signal when we wake up ( due to kernel sleep or activity)
 *
 * @retval PowerState Next state
 */

static PowerState
_stateResume(int resumeType)
{
    PMLOG_TRACE("We awoke");

    MachineWakeup();

    if (!MachineSupportsWakelocks())
    {
        PwrEventThawActivities();
    }

    char *resumeDesc = g_strdup_printf("resume (%s)",
                                       resume_type_descriptions[resumeType]);
    SendResume(resumeType, resumeDesc);
    g_free(resumeDesc);

#ifdef ASSERT_ON_BUG
    WaitObjectSignal(&gWaitSuspendResponse);
#endif

    InstrumentOnWake(resumeType);

    // if we are inactive in 1s, go back to sleep.
    ScheduleIdleCheck(gSleepConfig.after_resume_idle_ms, false);

    return kPowerStateOn;
}

/**
 * @brief This is the default state in which the system will be after waking up from sleep. It will
 * broadcast the "Resume" signal , schedule the next IdleCheck sequence and go to the "On" state.
 *
 * @retval PowerState Next state
 */

static PowerState
StateKernelResume(void)
{
    return _stateResume(kResumeTypeKernel);
}

/**
 * @brief A reply or notification on our com.palm.display/control/status subscription.
 *
 * The first reply carries "state"; later ones carry "event". Anything that
 * is not a good reply - a hub error because the display manager went away
 * or refused the call, returnValue false - ends the subscription, so the
 * state becomes unknown and is treated as on until the next successful
 * subscribe (which the server-status watch issues when the display manager
 * is next seen up).
 */
static bool
DisplayStatusCb(LSHandle *handle, LSMessage *message, void *user_data)
{
    const char *payload = LSMessageGetPayload(message);
    bool was_on = gDisplayIsOn;

    switch (DisplayStatusParse(payload))
    {
        case DisplayStatusOn:
            gDisplayIsOn = true;
            break;

        case DisplayStatusOff:
            gDisplayIsOn = false;
            break;

        case DisplayStatusUnchanged:
            break;

        case DisplayStatusError:
        default:
            /* the payload is JSON itself, so it cannot go into a PmLog kv */
            SLEEPDLOG_WARNING(MSGID_SUBSCRIBE_DISP_MGR_FAIL, 0,
                              "Display status subscription ended; assuming the display is on");
            SLEEPDLOG_DEBUG("Display status reply was: %s", payload ? payload : "(null)");
            gDisplayIsOn = true;
            sDisplayStatusToken = LSMESSAGE_TOKEN_INVALID;
            break;
    }

    if (was_on != gDisplayIsOn)
    {
        SLEEPDLOG_DEBUG("Display status is now %s", gDisplayIsOn ? "on" : "off");

        if (!gDisplayIsOn)
        {
            /* the idle countdown starts from here, not from the next poll */
            ScheduleIdleCheck(0, false);
        }
    }

    return true;
}

/**
 * @brief (Re)subscribe to the display state. The reply to the subscribe call
 * itself carries the current state, so this doubles as the initial query.
 */
static void
DisplayStatusSubscribe(void)
{
    LSError lserror;
    LSErrorInit(&lserror);

    if (sDisplayStatusToken != LSMESSAGE_TOKEN_INVALID)
    {
        if (!LSCallCancel(GetLunaServiceHandle(), sDisplayStatusToken, &lserror))
        {
            LSErrorFree(&lserror);
            LSErrorInit(&lserror);
        }

        sDisplayStatusToken = LSMESSAGE_TOKEN_INVALID;
    }

    if (!LSCall(GetLunaServiceHandle(), "luna://com.palm.display/control/status",
                "{\"subscribe\":true}", DisplayStatusCb, NULL,
                &sDisplayStatusToken, &lserror))
    {
        SLEEPDLOG_WARNING(MSGID_SUBSCRIBE_DISP_MGR_FAIL, 1,
                          PMLOGKS(ERRTEXT, lserror.message),
                          "Failed to subscribe for display status updates");
        LSErrorFree(&lserror);
        sDisplayStatusToken = LSMESSAGE_TOKEN_INVALID;
        gDisplayIsOn = true;
        return;
    }

    SLEEPDLOG_DEBUG("Subscribed to com.palm.display/control/status");
}

/**
 * @brief com.palm.display came up or went down.
 *
 * A single subscribe at startup was not enough: if the display manager was
 * not up yet, or restarted later, the subscription silently died and
 * gDisplayIsOn kept whatever it last was - on, from initialisation, so
 * sleepd never suspended again. Subscribe on every up event, and treat a
 * down display manager as an unknown, i.e. on, display.
 */
static bool
DisplayServerStatusCb(LSHandle *sh, const char *serviceName, bool connected,
                      void *ctx)
{
    SLEEPDLOG_DEBUG("%s is %s", serviceName, connected ? "up" : "down");

    if (connected)
    {
        DisplayStatusSubscribe();
    }
    else
    {
        sDisplayStatusToken = LSMESSAGE_TOKEN_INVALID;
        gDisplayIsOn = true;
    }

    return true;
}

/**
 * @brief We are in this state if the system did not really sleep but had to prevent the sleep because
 * an activity as active. It does so by broadcasting the "Resume" signal , schedule the next IdleCheck
 * sequence and go to the "On" state.
 *
 * @retval PowerState Next state
 */

static PowerState
StateActivityResume(void)
{
    return _stateResume(kResumeTypeActivity);
}

/**
 * @brief Initialize the Suspend/Resume state machine.
 */

static int
SuspendInit(void)
{
    pthread_t suspend_tid;

    // initialize wake time.
    ClockGetTime(&sTimeOnWake);

    WaitObjectInit(&gWaitSuspendResponse);
    WaitObjectInit(&gWaitPrepareSuspend);

    WaitObjectInit(&gWaitResumeMessage);

    com_palm_suspend_lunabus_init();
    PwrEventClientTableCreate();

    SuspendIPCInit();

    gCurrentStateNode = kStateMachine[kPowerStateOn];
    if(gSleepConfig.enable_idle_check_thread)
    {
        LSError lserror;
        LSErrorInit(&lserror);

        /*
         * The up callback fires right away if the display manager is already
         * registered, and again after every (re)start of it. The subscribe
         * itself happens there.
         */
        if (!LSRegisterServerStatusEx(GetLunaServiceHandle(), "com.palm.display",
                                      DisplayServerStatusCb, NULL,
                                      &sDisplayServerStatusCookie, &lserror))
        {
            SLEEPDLOG_WARNING(MSGID_SUBSCRIBE_DISP_MGR_FAIL, 1,
                              PMLOGKS(ERRTEXT, lserror.message),
                              "Failed to watch com.palm.display; display assumed on");
            LSErrorFree(&lserror);
        }

        if (pthread_create(&suspend_tid, NULL, SuspendThread, NULL))
        {
            SLEEPDLOG_CRITICAL(MSGID_PTHREAD_CREATE_FAIL, 0,
                               "Could not create SuspendThread\n");
            abort();
        }
    }

    return 0;
}

/**
 * @brief Iterate through the suspend state machine
 */
void
TriggerSuspend(const char *reason, PowerEvent event)
{
    SLEEPDLOG_DEBUG("%s: state %s", __PRETTY_FUNCTION__, StateToStr(gCurrentStateNode.state));

    if (!suspend_loop)
    {
        SLEEPDLOG_DEBUG("Suspend thread not running; ignoring %s", reason);
        return;
    }

    if (SuspendInhibited())
    {
        SLEEPDLOG_DEBUG("Shutdown in progress; ignoring suspend trigger (%s)", reason);
        return;
    }

    GSource *source = g_idle_source_new();
    g_source_set_callback(source,
        (GSourceFunc)SuspendStateUpdate, GINT_TO_POINTER(event), NULL);
    g_source_attach(source, g_main_loop_get_context(suspend_loop));

    g_source_unref(source);
}

 /**
 * @brief Run the state machine with no event.
 *
 * MachineSleep() blocks, so the machine is never left parked in a suspended
 * state waiting for this; after a kernel resume it drives itself through
 * KernelResume back to On in the same pass. What remains of this is a
 * harmless poke from the activityStart path and the RTC alarm callback: in
 * the On state with no event it is a no-op. The "resume" luna method goes
 * through ForceResume() instead, which also broadcasts.
 */
void
TriggerResume(const char *reason, PowerEvent event)
{
    if (!suspend_loop)
    {
        return;
    }

    /*
     * In the On state with no event the machine has nothing to do, and
     * every activityStart lands here: skip the four-line no-op cycle it
     * would otherwise log.
     */
    if (event == kPowerEventNone && gCurrentStateNode.state == kPowerStateOn)
    {
        return;
    }

    SLEEPDLOG_DEBUG("%s: state %s (%s)", __PRETTY_FUNCTION__,
                    StateToStr(gCurrentStateNode.state), reason ? reason : "");

    GSource *source = g_idle_source_new();
    g_source_set_callback(source,
                          (GSourceFunc)SuspendStateUpdate, GINT_TO_POINTER(event), NULL);
    g_source_attach(source, g_main_loop_get_context(suspend_loop));

    g_source_unref(source);
}

/**
 * @brief Check if the device is currently in a low power mode
 *
 * @return True, if device is currently suspended, False otherwise.
 */
/**
 * @brief Resume on request even when the kernel never went down.
 *
 * A client that entered a suspended state on prepareSuspend is waiting for the
 * resume signal to leave it again. If the suspend is still pending, or was
 * aborted, IsSuspended() is false and the old code answered the resume request
 * with an error and broadcast nothing - leaving that client stuck in a state
 * only the resume signal can end. luna-displaymanager is exactly such a client:
 * its DisplayOffSuspended records where to restore to and waits, so the display
 * stayed off and the power key did nothing at all until the process restarted.
 *
 * Drive the state machine as a normal resume does, and broadcast regardless, so
 * asking to wake up always results in subscribers being told the device is
 * awake.
 */
void
ForceResume(const char *reason)
{
    SLEEPDLOG_DEBUG("%s: state %s, reason %s", __PRETTY_FUNCTION__,
                    StateToStr(gCurrentStateNode.state), reason ? reason : "(none)");

    TriggerResume(reason, kPowerEventNone);
    SendResume(kResumeAbortSuspend, (char *) (reason ? reason : "resume requested"));
}

void
SuspendInhibitForShutdown(const char *reason)
{
    if (!g_atomic_int_compare_and_exchange(&gShutdownInProgress, 0, 1))
    {
        return;
    }

    SLEEPDLOG_DEBUG("Shutdown in progress (%s): suspend inhibited, idle checks stopped",
                    reason ? reason : "(none)");

    /*
     * Also veto it in the kernel, for the window between a vote already in
     * flight and the state write. Never released: the process ends with the
     * shutdown, and a wakelock whose owner has exited is dropped with it.
     */
    if (MachineSupportsWakelocks() &&
        SysfsWriteString("/sys/power/wake_lock", SHUTDOWN_WAKELOCK_NAME) < 0)
    {
        SLEEPDLOG_WARNING(MSGID_WAKE_LOCK_FAILED, 1,
                          PMLOGKS("wakelock", SHUTDOWN_WAKELOCK_NAME),
                          "Could not take the shutdown wakelock");
    }
}

bool
SuspendInhibited(void)
{
    return g_atomic_int_get(&gShutdownInProgress) != 0;
}

bool
IsSuspended(void)
{
    SLEEPDLOG_DEBUG("%s: state %s", __PRETTY_FUNCTION__, StateToStr(gCurrentStateNode.state));
    /* the suspend thread is inside MachineSleep() for the whole of StateSleep */
    return (gCurrentStateNode.state == kPowerStateSleep);
}

INIT_FUNC(INIT_FUNC_END, SuspendInit);

/* @} END OF SuspendLogic */
