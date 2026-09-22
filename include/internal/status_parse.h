// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
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

#ifndef _STATUS_PARSE_H_
#define _STATUS_PARSE_H_

/**
 * @file status_parse.h
 *
 * Pure parsers for the luna payloads sleepd consumes from other services.
 * They touch nothing but the payload string, so they can be unit tested on
 * the host without a bus.
 */

/**
 * What a com.palm.display/control/status reply or notification says about
 * the display.
 */
typedef enum
{
    /** Not a usable reply: unparsable, returnValue false, or a hub error
     *  (service down, call not permitted). The display state is unknown. */
    DisplayStatusError = -1,
    /** A valid message that carries no display state (for instance the
     *  "changedTimeout" or "blockedDisplay" events). Keep what we had. */
    DisplayStatusUnchanged = 0,
    DisplayStatusOn,
    DisplayStatusOff,
} DisplayStatus;

DisplayStatus DisplayStatusParse(const char *payload);

/**
 * Charger presence from a com.webos.service.battery payload: the
 * chargerConnected ({"connected":bool}) and chargerStatus ({"type","name",
 * "connected",...}) signals on /com/palm/power, and the chargerStatusQuery
 * reply ({"USBConnected":bool,"DockConnected":bool,...}).
 *
 * @retval 1  a charger is connected
 * @retval 0  no charger is connected
 * @retval -1 the payload says nothing about the charger (unparsable, an
 *            error reply, or none of the known fields present)
 */
int ChargerStatusParse(const char *payload);

#endif
