/*
 * Copyright (c) 2023 Achim Kraus CloudCoap.net
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0
 *
 * SPDX-License-Identifier: EPL-2.0
 */

#include <time.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

#include "appl_time.h"

/* last time in milliseconds since 1.1.1970 */
static uint64_t appl_time = 0;
/* uptime of last time exchange */
static int64_t appl_uptime = 0;

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

void appl_get_now(int64_t *now)
{
   *now = appl_time;
   // adjust current time
   if (appl_uptime) {
      *now += (k_uptime_get() - appl_uptime);
   }
}

void appl_set_now(int64_t now)
{
   appl_time = now;
   appl_uptime = k_uptime_get();
}

int appl_format_time(int64_t time_millis, char *buf, size_t len)
{
   time_t time = time_millis / MSEC_PER_SEC;
   if (time > 0) {
      // Format time, "yyyy-mm-ddThh:mm:ssZ"
      return strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", gmtime(&time));
   } else {
      return 0;
   }
}

