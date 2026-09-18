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


#include <unistd.h>
#include <stdlib.h>
#include <glib.h>
#include <string.h>
#include <stdbool.h>
#include <syslog.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <json.h>
#include <luna-service2/lunaservice.h>

#include "main.h"
#include "suspend.h"
#include "defines.h"

#include "machine.h"
#include "sleepd_debug.h"
#include "logging.h"
#include "suspend.h"
#include "sleepd_config.h"
#include "status_parse.h"

/**
 * Holds the current state of whether or not we're being supplied with power from a charger of any sort.
 */
bool chargerIsConnected = false;

/**
 * Holds the current state of whether or not the device supports Wakelocks.
 */

bool machineSupportsWakelocks = false;

bool MachineSupportsWakelocks(void)
{
    static bool initialized = false;
    if (!initialized)
    {
        machineSupportsWakelocks =  g_file_test("/sys/power/wake_lock", (GFileTest)(G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR));
        SLEEPDLOG_DEBUG("System %s wakelocks", machineSupportsWakelocks ? "supports" : "does not support");
        initialized = true;
    }

    return machineSupportsWakelocks;
}

bool
MachineCanSleep(void)
{
    return (!chargerIsConnected || gSleepConfig.suspend_with_charger);
}

const char *
MachineCantSleepReason(void)
{
    static char reason[512];

    snprintf(reason, 512, "%s", chargerIsConnected ? "charger_present" : "");

    return reason;
}


bool MachineSleep(void)
{
    bool success = false;
    nyx_error_t error = NYX_ERROR_NONE;

    error = nyx_system_suspend_async(GetNyxSystemDevice(), &success);
    if (error != NYX_ERROR_NONE) {
        SLEEPDLOG_DEBUG("NYX: failed to suspend (error %d)", error);
        return false;
    }

    return success;
}

void MachineWakeup(void)
{
    bool success = false;

    nyx_system_resume(GetNyxSystemDevice(), &success);
}

void
MachineForceShutdown(const char *reason)
{
    SLEEPDLOG_INFO(MSGID_FRC_SHUTDOWN, 1, PMLOGKS("Reason", reason),
                   "Pwrevents shutting down system");

    SuspendInhibitForShutdown(reason);

    if (gSleepConfig.fasthalt)
    {
        nyx_system_shutdown(GetNyxSystemDevice(), NYX_SYSTEM_EMERG_SHUTDOWN, reason);
    }
    else
    {
        nyx_system_shutdown(GetNyxSystemDevice(), NYX_SYSTEM_NORMAL_SHUTDOWN, reason);
    }
}

void
MachineForceReboot(const char *reason)
{
    SLEEPDLOG_INFO(MSGID_FRC_REBOOT, 1, PMLOGKS("Reason", reason),
                   "Pwrevents rebooting system");

    SuspendInhibitForShutdown(reason);

    if (gSleepConfig.fasthalt)
    {
        nyx_system_reboot(GetNyxSystemDevice(), NYX_SYSTEM_EMERG_SHUTDOWN, reason);
    }
    else
    {
        nyx_system_reboot(GetNyxSystemDevice(), NYX_SYSTEM_NORMAL_SHUTDOWN, reason);
    }
}

/**
 * @brief Track charger presence from com.webos.service.battery.
 *
 * Fed by the chargerConnected and chargerStatus signals and by the reply to
 * chargerStatusQuery; see ChargerStatusParse() for the three shapes. A
 * payload that says nothing about the charger (an error reply, an addmatch
 * acknowledgement) leaves the state untouched.
 */
bool ChargerStatus(LSHandle *sh,
                   LSMessage *message, void *user_data)
{
    const char *payload = LSMessageGetPayload(message);
    int connected = ChargerStatusParse(payload);

    if (connected < 0)
    {
        SLEEPDLOG_DEBUG("Charger payload without charger state ignored: %s",
                        payload ? payload : "(null)");
        return true;
    }

    if (chargerIsConnected != (connected == 1))
    {
        SLEEPDLOG_DEBUG("Charger is now %s", connected ? "connected" : "disconnected");
    }
    else
    {
        SLEEPDLOG_DEBUG("Charger still %s", connected ? "connected" : "disconnected");
    }

    chargerIsConnected = (connected == 1);

    return true;
}
