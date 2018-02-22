/* @@@LICENSE
*
*      Copyright (c) 2011-2013 LG Electronics, Inc.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* LICENSE@@@ */


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

#include "sysfs.h"

#include "machine.h"
#include "sleepd_debug.h"
#include "logging.h"
#include "suspend.h"
#include "config.h"

static char *machineName = NULL;

/**
 * Holds the current state of whether or not we're being supplied with power from a charger of any sort.
 */
bool chargerIsConnected = false;

/**
 * Holds the current state of whether or not we're being supplied with power from a USB charger.
 */
bool usbconn = false;

/**
 * Holds the current state of whether or not we're being supplied with power from a dock charger.
 */
bool dockconn = false;

bool machineSupportsWakelocks = false;

/**
 * Obtains the machine specific release name.
 * For e.g. If uname -r returns "2.6.22.1-11-palm-joplin-2430",
 * then GetMachineName should return "palm-joplin-2430"
 */

char *
MachineGetName(void)
{
	if (machineName)
	{
		return machineName;
	}

	struct utsname un;

	int ret;

	ret = uname(&un);

	if (ret < 0)
	{
		goto unknown;
	}

	// find first string after '-' that is not a digit
	char *machine_id = un.release;

	do
	{
		machine_id = strchr(machine_id, '-');

		if (!machine_id)
		{
			goto unknown;
		}

		machine_id++; // skip the '-'
	}
	while (g_ascii_isdigit(*machine_id) == TRUE);

	if (strlen(machine_id) == 0)
	{
		goto unknown;
	}

	machineName = g_strdup(machine_id);
	return machineName;

unknown:
	machineName = "unknown";
	return machineName;
}

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

#ifdef REBOOT_TAKES_REASON

	if (gSleepConfig.fasthalt)
	{
		nyx_system_shutdown(GetNyxSystemDevice(), NYX_SYSTEM_EMERG_SHUTDOWN, reason);
	}
	else
	{
		nyx_system_shutdown(GetNyxSystemDevice(), NYX_SYSTEM_NORMAL_SHUTDOWN, reason);
	}

#else

	if (gSleepConfig.fasthalt)
	{
		nyx_system_shutdown(GetNyxSystemDevice(), NYX_SYSTEM_EMERG_SHUTDOWN);
	}
	else
	{
		nyx_system_shutdown(GetNyxSystemDevice(), NYX_SYSTEM_NORMAL_SHUTDOWN);
	}

#endif
}

void
MachineForceReboot(const char *reason)
{
	SLEEPDLOG_INFO(MSGID_FRC_REBOOT, 1, PMLOGKS("Reason", reason),
	               "Pwrevents rebooting system");

#ifdef REBOOT_TAKES_REASON

	if (gSleepConfig.fasthalt)
	{
		nyx_system_reboot(GetNyxSystemDevice(), NYX_SYSTEM_EMERG_SHUTDOWN, reason);
	}
	else
	{
		nyx_system_reboot(GetNyxSystemDevice(), NYX_SYSTEM_NORMAL_SHUTDOWN, reason);
	}

#else

	if (gSleepConfig.fasthalt)
	{
		nyx_system_reboot(GetNyxSystemDevice(), NYX_SYSTEM_EMERG_SHUTDOWN);
	}
	else
	{
		nyx_system_reboot(GetNyxSystemDevice(), NYX_SYSTEM_NORMAL_SHUTDOWN);
	}

#endif
}


void
TurnBypassOn(void)
{
	// 0 > level means on.
	SysfsWriteString("/sys/user_hw/pins/power/chg_bypass/level", "0");
}

void
TurnBypassOff(void)
{
	// 1 > level means off.
	SysfsWriteString("/sys/user_hw/pins/power/chg_bypass/level", "1");
}

int
MachineGetToken(const char *token_name, char *buf, int len)
{
	char *file_name = g_build_filename("/dev/tokens", token_name, NULL);
	int fd = open(file_name, O_RDONLY);

	if (fd < 0)
	{
		return -1;
	}

	int ret;

	do
	{
		ret = read(fd, buf, len);
	}
	while (ret < 0 && errno == -EINTR);

	buf[ret] = '\0';

	close(fd);
	g_free(file_name);

	if (ret < 0)
	{
		return -1;
	}

	return 0;
}

/**
 * Handler for events from com.palm.power telling us when the charger is plugged/unplugged
 *
 * This function is registered with the com.palm.power service in {@link main()}
 * to allow us to track the state of the charger and adjust our power plan
 * accordingly.
 *
 * The "message" parameter will be given to us as a JSON object formatted as
 * follows:
 *
 * <code>
 * {
 *   "Charging" : {
 *       "USBConnected" : true || false, //whether power is currently being supplied via a USB charger
 *       "DockPower" : true || false //whether power is currently being supplied via a dock
 *     }
 * }
 * </code>
 *
 * {@link chargerIsConnected} will be changed in response to this message to
 * indicate the current state of charging.  If power is being supplied to us
 * by either charging method, {@link chargerIsConnected} is set to true.
 *
 * @param   sh      Pointer to service handle.
 * @param   message     JSON message which contains the latest information about the state of methods of charging.
 * @param   user_data   Currently unused.
 *
 * @return          Always returns true.
 */
bool ChargerStatus(LSHandle *sh,
                   LSMessage *message, void *user_data)
{
	struct json_object *object;
	object = json_tokener_parse(LSMessageGetPayload(message));

	if (object)
	{
		if (json_object_object_get(object, "Charging"))
		{
			usbconn = json_object_get_boolean(json_object_object_get(object,
			                                  "USBConnected"));
			dockconn = json_object_get_boolean(json_object_object_get(object, "DockPower"));
			SLEEPDLOG_DEBUG("Charger connected/disconnected, usb : %s, dock : %s",
			                usbconn ? "true" : "false", dockconn ? "true" : "false");
			chargerIsConnected = usbconn | dockconn;
		}
	}

	if (object)
	{
		json_object_put(object);
	}

	return true;
}
