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

#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "environment_sensor.h"

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

static K_MUTEX_DEFINE(environment_mutex);
static uint8_t s_temperature_size = 0;
static double s_temperature_history[CONFIG_ENVIRONMENT_HISTORY_SIZE];

int environment_get_temperature_history(double *values, uint8_t size)
{
   uint8_t index;

   k_mutex_lock(&environment_mutex, K_FOREVER);
   if (s_temperature_size < size) {
      size = s_temperature_size;
   }
   for (index = 0; index < size; ++index) {
      values[index] = s_temperature_history[index];
   }
   k_mutex_unlock(&environment_mutex);

   return size;
}

static void environment_init_temperature_history(void)
{
   uint8_t index;
   k_mutex_lock(&environment_mutex, K_FOREVER);
   s_temperature_size = 0;
   for (index = 0; index < CONFIG_ENVIRONMENT_HISTORY_SIZE; ++index) {
      s_temperature_history[index] = 0.0;
   }
   k_mutex_unlock(&environment_mutex);
}

static void environment_add_temperature_history(double value)
{
   uint8_t index;
   if (s_temperature_size < CONFIG_ENVIRONMENT_HISTORY_SIZE) {
      ++s_temperature_size;
   }
   for (index = s_temperature_size; index > 0; --index) {
      s_temperature_history[index] = s_temperature_history[index - 1];
   }
   s_temperature_history[0] = value;
}

struct environment_sensor {
   const enum sensor_channel channel;
   const struct device *dev;
};

static const struct environment_sensor temperature_sensor = {
    .channel = SENSOR_CHAN_AMBIENT_TEMP,
    .dev = DEVICE_DT_GET(DT_ALIAS(temperature_sensor_2))};

static const struct environment_sensor humidity_sensor = {
    .channel = SENSOR_CHAN_HUMIDITY,
    .dev = DEVICE_DT_GET(DT_ALIAS(humidity_sensor_2))};

static const struct environment_sensor *all_sensors[] = {
    &temperature_sensor,
    &humidity_sensor};

static const unsigned int all_sensors_size = sizeof(all_sensors) / sizeof(void *);

static void environment_history_work_fn(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(environment_history_work, environment_history_work_fn);

static int environment_sensor_fetch(bool force)
{
   static int64_t environment_sensor_next_fetch = 0;
   static int err = 0;
   int64_t now = k_uptime_get();
   if (force || (now - environment_sensor_next_fetch) >= 0) {
      environment_sensor_next_fetch = now + 5 * MSEC_PER_SEC;
      for (int index1 = 0; index1 < all_sensors_size; ++index1) {
         const struct device *dev = all_sensors[index1]->dev;
         for (int index2 = 0; index2 < index1; ++index2) {
            if (dev == all_sensors[index2]->dev) {
               dev = NULL;
               break;
            }
         }
         if (dev) {
            err = sensor_sample_fetch_chan(dev, SENSOR_CHAN_ALL);
            if (err) {
               break;
            }
         }
      }
   }
   return err;
}

static void environment_history_work_fn(struct k_work *work)
{
   double temperature = 0.0;

   environment_sensor_fetch(true);
   if (environment_get_temperature(&temperature) == 0) {
      k_mutex_lock(&environment_mutex, K_FOREVER);
      environment_add_temperature_history(temperature);
      k_mutex_unlock(&environment_mutex);
   }
   k_work_schedule(&environment_history_work, K_SECONDS(CONFIG_ENVIRONMENT_HISTORY_INTERVAL_S));
}

static int environment_sensor_init(const struct device *dev)
{
   if (!device_is_ready(dev)) {
      LOG_ERR("%s device is not ready", dev->name);
      return -ENOTSUP;
   }
   return 0;
}

int environment_init(void)
{
   int err = 0;

   LOG_INF("SHT31 initialize, %d s minimum interval", 5);

   for (int index = 0; index < all_sensors_size; ++index) {
      err = environment_sensor_init(all_sensors[index]->dev);
      if (err) {
         return err;
      }
   }
   environment_sensor_fetch(true);
   environment_init_temperature_history();
   k_work_schedule(&environment_history_work, K_SECONDS(2));

   return 0;
}

static int environment_sensor_read(const struct environment_sensor *sensor, double *value, int32_t *high_value, int32_t *low_value)
{
   int err;
   struct sensor_value data = {0};

   err = environment_sensor_fetch(false);
   if (err) {
      LOG_ERR("Failed to fetch data from %s, error: %d",
              sensor->dev->name, err);
      return -ENODATA;
   }

   err = sensor_channel_get(sensor->dev, sensor->channel, &data);
   if (err) {
      LOG_ERR("Failed to read data from %s, error: %d",
              sensor->dev->name, err);
      return -ENODATA;
   }

   if (value) {
      *value = sensor_value_to_double(&data);
   } else {
      if (high_value) {
         *high_value = data.val1;
      }
      if (low_value) {
         *low_value = data.val2;
      }
   }
   return 0;
}

int environment_get_temperature(double *value)
{
   int err = environment_sensor_read(&temperature_sensor, value, NULL, NULL);
   return err;
}

int environment_get_humidity(double *value)
{
   return environment_sensor_read(&humidity_sensor, value, NULL, NULL);
}

int environment_get_pressure(double *value)
{
   return -ENODATA;
}

int environment_get_gas(int32_t *value)
{
   return -ENODATA;
}

int environment_get_iaq(int32_t *value)
{
   return -ENODATA;
}
