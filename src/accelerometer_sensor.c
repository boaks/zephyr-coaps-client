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

#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "accelerometer_sensor.h"
#include "io_job_queue.h"
#include "power_manager.h"

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

static const struct device *accelerometer_dev = DEVICE_DT_GET_OR_NULL(DT_ALIAS(accelerometer_sensor));

static accelerometer_handler_t accelerometer_handler = NULL;

#if !defined(CONFIG_LIS2DH_TRIGGER_NONE)

static void accelerometer_read(const struct device *dev, accelerometer_handler_t handler)
{
   int err = 0;
   struct sensor_value data[ACCELEROMETER_CHANNELS];
   struct accelerometer_evt evt;

   if (sensor_sample_fetch(dev) < 0) {
      LOG_ERR("Sample fetch error");
      return;
   }

   err = sensor_channel_get(dev, SENSOR_CHAN_ACCEL_X, &data[0]);
   err += sensor_channel_get(dev, SENSOR_CHAN_ACCEL_Y, &data[1]);
   err += sensor_channel_get(dev, SENSOR_CHAN_ACCEL_Z, &data[2]);

   if (err) {
      LOG_ERR("sensor_channel_get, error: %d", err);
      return;
   }

   evt.values[0] = sensor_value_to_double(&data[0]);
   evt.values[1] = sensor_value_to_double(&data[1]);
   evt.values[2] = sensor_value_to_double(&data[2]);

   if (handler) {
      handler(&evt);
      return;
   }

   LOG_INF("Accelerometer x %.02f, y %.02f, z %.02f", evt.values[0], evt.values[1], evt.values[2]);
}

static void accelerometer_enable_fn(struct k_work *work)
{
   accelerometer_read(accelerometer_dev, NULL);
   accelerometer_enable(true);
}

static K_WORK_DELAYABLE_DEFINE(accelerometer_enable_work, accelerometer_enable_fn);

static void accelerometer_trigger_handler(const struct device *dev,
                                          const struct sensor_trigger *trig)
{
   switch (trig->type) {
      case SENSOR_TRIG_DELTA:
      case SENSOR_TRIG_MOTION:
         LOG_INF("Accelerometer trigger %d", trig->type);
         accelerometer_read(dev, accelerometer_handler);
         accelerometer_enable(false);
         work_reschedule_for_io_queue(&accelerometer_enable_work, K_MSEC(5000));
         break;
      default:
         LOG_ERR("Unknown trigger %d", trig->type);
   }
}
#endif

int accelerometer_init(accelerometer_handler_t handler)
{
   if (!device_is_ready(accelerometer_dev)) {
      LOG_ERR("Accelerometer device is not ready!");
      return -ENOTSUP;
   }
   if (handler) {
      accelerometer_handler = handler;
   } else {
      power_manager_add(accelerometer_dev);
   }
   return 0;
}

int accelerometer_enable(bool enable)
{
   int rc = -ENOTSUP;

#ifdef CONFIG_ADXL362
   if (device_is_ready(accelerometer_dev)) {
      struct sensor_trigger trig = {
          .chan = SENSOR_CHAN_ACCEL_XYZ,
          .type = SENSOR_TRIG_MOTION};

      rc = sensor_trigger_set(accelerometer_dev, &trig, enable ? accelerometer_trigger_handler : NULL);
      if (rc) {
         LOG_ERR("Accelerometer error: could not set motion trigger for device %s, %d / %s",
                 accelerometer_dev->name, rc, strerror(-rc));
      } else {
         LOG_INF("Accelerometer-motion-trigger: %s", enable ? "enabled" : "disabled");
      }
   }
#endif /* CONFIG_ADXL362 */
#if defined(CONFIG_LIS2DH) && defined(CONFIG_LIS2DH_TRIGGER)
   if (device_is_ready(accelerometer_dev)) {

      struct sensor_trigger trig = {
          .chan = SENSOR_CHAN_ACCEL_XYZ,
          .type = SENSOR_TRIG_DELTA};

      // 1.5 m/s
      struct sensor_value value = {
          .val1 = 0,
          .val2 = 500000,
      };

      rc = sensor_attr_set(accelerometer_dev, SENSOR_CHAN_ACCEL_XYZ,
                           SENSOR_ATTR_SLOPE_TH,
                           &value);
      if (rc) {
         LOG_ERR("Accelerometer error: could not set threshold for device %s, %d / %s",
                 accelerometer_dev->name, rc, strerror(-rc));
      }

      // duration 0
      value.val1 = 0;
      value.val2 = 0;
      rc = sensor_attr_set(accelerometer_dev, SENSOR_CHAN_ACCEL_XYZ,
                           SENSOR_ATTR_SLOPE_DUR,
                           &value);
      if (rc) {
         LOG_ERR("Accelerometer error: could not set duration for device %s, %d / %s",
                 accelerometer_dev->name, rc, strerror(-rc));
      }
#ifdef CONFIG_LIS2DH_ACCEL_HP_FILTERS
      value.val1 = enable ? 1 : 0;
      rc = sensor_attr_set(accelerometer_dev, SENSOR_CHAN_ACCEL_XYZ,
                           SENSOR_ATTR_CONFIGURATION,
                           &value);
      if (rc) {
         LOG_ERR("Accelerometer error: could not set high-pass filter for device %s, %d / %s",
                 accelerometer_dev->name, rc, strerror(-rc));
      }
#endif

      rc = sensor_trigger_set(accelerometer_dev, &trig, enable ? accelerometer_trigger_handler : NULL);
      if (rc) {
         LOG_ERR("Accelerometer error: could not set delta trigger for device %s, %d / %s",
                 accelerometer_dev->name, rc, strerror(-rc));
      } else {
         LOG_INF("Accelerometer-delta-trigger: %s", enable ? "enabled" : "disabled");
      }
   }
#endif /* CONFIG_LIS2DH */
   return rc;
}
