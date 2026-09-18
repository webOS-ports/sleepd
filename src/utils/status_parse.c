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

/**
 * @file status_parse.c
 *
 * @brief Parsers for the display-status and charger payloads. See
 * status_parse.h for the message shapes each one accepts.
 */

#include <stdbool.h>
#include <string.h>
#include <json.h>

#include "status_parse.h"

/* returnValue:false, or a hub error (errorCode without returnValue) */
static bool
_is_error_reply(struct json_object *root)
{
    struct json_object *obj = NULL;

    if (json_object_object_get_ex(root, "returnValue", &obj))
    {
        return !json_object_get_boolean(obj);
    }

    return json_object_object_get_ex(root, "errorCode", &obj) ||
           json_object_object_get_ex(root, "errorText", &obj);
}

DisplayStatus
DisplayStatusParse(const char *payload)
{
    struct json_object *root;
    struct json_object *obj = NULL;
    DisplayStatus result = DisplayStatusUnchanged;

    if (!payload)
    {
        return DisplayStatusError;
    }

    root = json_tokener_parse(payload);

    if (!root || !json_object_is_type(root, json_type_object))
    {
        if (root)
        {
            json_object_put(root);
        }

        return DisplayStatusError;
    }

    if (_is_error_reply(root))
    {
        json_object_put(root);
        return DisplayStatusError;
    }

    /*
     * The reply to the subscribe call carries "state" (on / dimmed / off);
     * subsequent notifications carry "event" (displayOn, displayDimmed,
     * displayOff, or one of several that say nothing about the state).
     * A dimmed display is still lit, so it counts as on.
     */
    if (json_object_object_get_ex(root, "state", &obj))
    {
        const char *state = json_object_get_string(obj);

        if (state && strcmp(state, "off") == 0)
        {
            result = DisplayStatusOff;
        }
        else if (state && (strcmp(state, "on") == 0 || strcmp(state, "dimmed") == 0))
        {
            result = DisplayStatusOn;
        }
    }

    if (json_object_object_get_ex(root, "event", &obj))
    {
        const char *event = json_object_get_string(obj);

        if (event && strcmp(event, "displayOff") == 0)
        {
            result = DisplayStatusOff;
        }
        else if (event && (strcmp(event, "displayOn") == 0 ||
                           strcmp(event, "displayDimmed") == 0))
        {
            result = DisplayStatusOn;
        }
    }

    json_object_put(root);
    return result;
}

int
ChargerStatusParse(const char *payload)
{
    struct json_object *root;
    struct json_object *obj = NULL;
    int result = -1;

    if (!payload)
    {
        return -1;
    }

    root = json_tokener_parse(payload);

    if (!root || !json_object_is_type(root, json_type_object))
    {
        if (root)
        {
            json_object_put(root);
        }

        return -1;
    }

    if (_is_error_reply(root))
    {
        json_object_put(root);
        return -1;
    }

    if (json_object_object_get_ex(root, "connected", &obj))
    {
        /* chargerConnected and chargerStatus signals */
        result = json_object_get_boolean(obj) ? 1 : 0;
    }
    else
    {
        /* chargerStatusQuery reply: no "connected", one flag per source */
        bool usb = false, dock = false, known = false;

        if (json_object_object_get_ex(root, "USBConnected", &obj))
        {
            usb = json_object_get_boolean(obj);
            known = true;
        }

        if (json_object_object_get_ex(root, "DockConnected", &obj))
        {
            dock = json_object_get_boolean(obj);
            known = true;
        }

        if (known)
        {
            result = (usb || dock) ? 1 : 0;
        }
    }

    json_object_put(root);
    return result;
}
