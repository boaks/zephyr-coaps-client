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

// #include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>

#include "environment_sensor.h"
#include "io_job_queue.h"

#if (defined CONFIG_ENVIRONMENT_SENSOR) || (defined CONFIG_SHT21)

#if (CONFIG_ENVIRONMENT_HISTORY_SIZE > 0)

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

#define SENSOR_HISTORY(T) sensor_history_##T
#define SENSOR_HISTORY_DEF(T, S) \
   typedef struct {              \
      int64_t next_time;         \
      uint8_t size;              \
      T history[S];              \
   } SENSOR_HISTORY(T)

static struct k_spinlock environment_history_lock;

SENSOR_HISTORY_DEF(double, CONFIG_ENVIRONMENT_HISTORY_SIZE);

static SENSOR_HISTORY(double) s_temperature_history;
static SENSOR_HISTORY(double) s_humidity_history;
static SENSOR_HISTORY(double) s_pressure_history;

#ifdef CONFIG_BME680_BSEC

SENSOR_HISTORY_DEF(uint16_t, CONFIG_ENVIRONMENT_HISTORY_SIZE);

static SENSOR_HISTORY(uint16_t) s_iaq_history;

static void environment_init_uint16_history(SENSOR_HISTORY(uint16_t) * history)
{
   uint8_t index;
   K_SPINLOCK(&environment_history_lock)
   {
      history->size = 0;
      history->next_time = 0;
      for (index = 0; index < CONFIG_ENVIRONMENT_HISTORY_SIZE; ++index) {
         history->history[index] = 0.0;
      }
   }
}

static int environment_get_uint16_history(SENSOR_HISTORY(uint16_t) * history, uint16_t *values, uint8_t size)
{
   uint8_t index;

   K_SPINLOCK(&environment_history_lock)
   {
      if (history->size < size) {
         size = history->size;
      }
      for (index = 0; index < size; ++index) {
         values[index] = history->history[index];
      }
   }

   return size;
}

static void environment_add_uint16_history(SENSOR_HISTORY(uint16_t) * history, uint16_t value, bool force)
{
   uint8_t index;
   int64_t now = k_uptime_get();

   K_SPINLOCK(&environment_history_lock)
   {
      if (force || (now - history->next_time) >= 0) {
         if (history->size < CONFIG_ENVIRONMENT_HISTORY_SIZE) {
            ++history->size;
         }
         for (index = history->size - 1; index > 0; --index) {
            history->history[index] = history->history[index - 1];
         }
         history->history[0] = value;
         history->next_time = now + CONFIG_ENVIRONMENT_HISTORY_INTERVAL_S * MSEC_PER_SEC;
      }
   }
}
#endif

#ifndef NO_ENVIRONMENT_HISTORY_WORKER
static void environment_history_work_fn(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(environment_history_work, environment_history_work_fn);

static void environment_history_work_fn(struct k_work *work)
{
   double value = 0.0;

   work_schedule_for_io_queue(&environment_history_work, K_SECONDS(CONFIG_ENVIRONMENT_HISTORY_INTERVAL_S));
   environment_sensor_fetch(true);
   if (environment_get_temperature(&value) == 0) {
      environment_add_temperature_history(value, true);
   }
   if (environment_get_humidity(&value) == 0) {
      environment_add_humidity_history(value, true);
   }
   if (environment_get_pressure(&value) == 0) {
      environment_add_pressure_history(value, true);
   }
}
#endif

static void environment_init_double_history(SENSOR_HISTORY(double) * history)
{
   uint8_t index;

   K_SPINLOCK(&environment_history_lock)
   {
      history->size = 0;
      history->next_time = 0;
      for (index = 0; index < CONFIG_ENVIRONMENT_HISTORY_SIZE; ++index) {
         history->history[index] = 0.0;
      }
   }
}

static int environment_get_double_history(SENSOR_HISTORY(double) * history, double *values, uint8_t size)
{
   uint8_t index;

   K_SPINLOCK(&environment_history_lock)
   {
      if (history->size < size) {
         size = history->size;
      }
      for (index = 0; index < size; ++index) {
         values[index] = history->history[index];
      }
   }

   return size;
}

static void environment_add_double_history(SENSOR_HISTORY(double) * history, double value, bool force)
{
   uint8_t index;
   int64_t now = k_uptime_get();

   K_SPINLOCK(&environment_history_lock)
   {
      if (force || (now - history->next_time) >= 0) {
         if (history->size < CONFIG_ENVIRONMENT_HISTORY_SIZE) {
            ++history->size;
         }
         for (index = history->size - 1; index > 0; --index) {
            history->history[index] = history->history[index - 1];
         }
         history->history[0] = value;
         history->next_time = now + CONFIG_ENVIRONMENT_HISTORY_INTERVAL_S * MSEC_PER_SEC;
      }
   }
}

int environment_get_temperature_history(double *values, uint8_t size)
{
   return environment_get_double_history(&s_temperature_history, values, size);
}

void environment_add_temperature_history(double value, bool force)
{
   environment_add_double_history(&s_temperature_history, value, force);
}

int environment_get_humidity_history(double *values, uint8_t size)
{
   return environment_get_double_history(&s_humidity_history, values, size);
}

void environment_add_humidity_history(double value, bool force)
{
   environment_add_double_history(&s_humidity_history, value, force);
}

int environment_get_pressure_history(double *values, uint8_t size)
{
   return environment_get_double_history(&s_pressure_history, values, size);
}

void environment_add_pressure_history(double value, bool force)
{
   environment_add_double_history(&s_pressure_history, value, force);
}

void environment_init_history(void)
{
   environment_init_double_history(&s_temperature_history);
   environment_init_double_history(&s_humidity_history);
   environment_init_double_history(&s_pressure_history);
#ifdef CONFIG_BME680_BSEC
   environment_init_uint16_history(&s_iaq_history);
#endif
#ifndef NO_ENVIRONMENT_HISTORY_WORKER
   work_schedule_for_io_queue(&environment_history_work, K_SECONDS(2));
#endif
}

int environment_get_iaq_history(uint16_t *values, uint8_t size)
{
#ifdef CONFIG_BME680_BSEC
   return environment_get_uint16_history(&s_iaq_history, values, size);
#else
   (void)values;
   (void)size;
   return 0;
#endif
}

void environment_add_iaq_history(uint16_t value, bool force)
{
#ifdef CONFIG_BME680_BSEC
   environment_add_uint16_history(&s_iaq_history, value, force);
#else
   (void)value;
   (void)force;
#endif
}

#endif /* CONFIG_ENVIRONMENT_HISTORY_SIZE > 0 */

#endif /* CONFIG_ENVIRONMENT_SENSOR || CONFIG_SHT21 */
