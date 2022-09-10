/*
 * Copyright (c) 2022 Achim Kraus CloudCoap.net
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

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "environment_sensor.h"

#ifdef ENVIRONMENT_SENSOR

#if (CONFIG_ENVIRONMENT_HISTORY_SIZE > 0)

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

static K_MUTEX_DEFINE(environment_history_mutex);
static int64_t s_temperature_next = 0;
static uint8_t s_temperature_size = 0;
static double s_temperature_history[CONFIG_ENVIRONMENT_HISTORY_SIZE];

int environment_get_temperature_history(double *values, uint8_t size)
{
   uint8_t index;

   k_mutex_lock(&environment_history_mutex, K_FOREVER);
   if (s_temperature_size < size) {
      size = s_temperature_size;
   }
   for (index = 0; index < size; ++index) {
      values[index] = s_temperature_history[index];
   }
   k_mutex_unlock(&environment_history_mutex);

   return size;
}

void environment_init_temperature_history(void)
{
   uint8_t index;
   k_mutex_lock(&environment_history_mutex, K_FOREVER);
   s_temperature_size = 0;
   s_temperature_next = 0;
   for (index = 0; index < CONFIG_ENVIRONMENT_HISTORY_SIZE; ++index) {
      s_temperature_history[index] = 0.0;
   }
   k_mutex_unlock(&environment_history_mutex);
}

void environment_add_temperature_history(double value, bool force)
{
   uint8_t index;
   int64_t now = k_uptime_get();

   k_mutex_lock(&environment_history_mutex, K_FOREVER);
   if (force || (now - s_temperature_next) >= 0) {
      if (s_temperature_size < CONFIG_ENVIRONMENT_HISTORY_SIZE) {
         ++s_temperature_size;
      }
      for (index = s_temperature_size - 1; index > 0; --index) {
         s_temperature_history[index] = s_temperature_history[index - 1];
      }
      s_temperature_history[0] = value;
      s_temperature_next = now + CONFIG_ENVIRONMENT_HISTORY_INTERVAL_S * MSEC_PER_SEC;
   }
   k_mutex_unlock(&environment_history_mutex);
}

#endif /* CONFIG_ENVIRONMENT_HISTORY_SIZE > 0 */

#endif /* ENVIRONMENT_SENSOR */
