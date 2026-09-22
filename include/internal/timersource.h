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


#ifndef _TIMERSOURCE_H_
#define _TIMERSOURCE_H_

#include <stdbool.h>

typedef struct _GTimerSource GTimerSource;

GTimerSource *g_timer_source_new(guint interval_ms, guint granularity_ms);

GTimerSource *g_timer_source_new_seconds(guint interval_seconds);

void g_timer_source_set_interval_seconds(GTimerSource *tsource,
        guint interval_sec, gboolean from_poll);

void g_timer_source_set_interval(GTimerSource *tsource, guint interval,
                                 gboolean from_poll);

guint g_timer_source_get_interval_ms(GTimerSource *tsource);

/**
 * @brief When the source is next due, on the monotonic clock in microseconds.
 */
gint64 g_timer_source_get_expiration_us(GTimerSource *tsource);

/**
 * @brief Whether the source has been due for longer than grace_us without
 * being dispatched and re-armed - the signature of a source whose loop is
 * not running, or that was lost.
 */
gboolean g_timer_source_is_overdue(GTimerSource *tsource, gint64 grace_us);

/*
 * Make the source fire as soon as the loop next runs, without disturbing its
 * repeat interval. Use this instead of setting the interval to zero: dispatch()
 * re-arms from the interval, so a zero there means the source is ready again
 * the instant it is dispatched, which is a busy loop rather than "run now".
 */
void g_timer_source_fire_now(GTimerSource *tsource, gboolean from_poll);

#endif
