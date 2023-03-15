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

#include "accelerometer_sensor.h"
#include "power_manager.h"

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

#define TEMPERATURE_OFFSET 22.0

const struct device *accelerometer_dev = DEVICE_DT_GET_OR_NULL(DT_ALIAS(accelerometer_sensor));

static accelerometer_handler_t accelerometer_handler = NULL;

#if !defined(CONFIG_LIS2DH_TRIGGER_NONE)
static void accelerometer_trigger_handler(const struct device *dev,
                                          const struct sensor_trigger *trig)
{
   int err = 0;
   struct sensor_value data[ACCELEROMETER_CHANNELS];
   struct accelerometer_evt evt;

   switch (trig->type) {
      case SENSOR_TRIG_DELTA:
      case SENSOR_TRIG_MOTION:
         LOG_INF("Accelerometer trigger %d", trig->type);

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

         if (accelerometer_handler) {
            accelerometer_handler(&evt);
            return;
         }

         LOG_INF("Accelerometer x %.02f, y %.02f, z %.02f", evt.values[0], evt.values[1], evt.values[2]);

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
#if defined(CONFIG_LIS2DH) && !defined(CONFIG_LIS2DH_TRIGGER_NONE)
   if (device_is_ready(accelerometer_dev)) {
      struct sensor_trigger trig = {
          .chan = SENSOR_CHAN_ACCEL_XYZ,
          .type = SENSOR_TRIG_DELTA};

      struct sensor_value threshold = {
          .val1 = 3,
          .val2 = 500000,
      };

      struct sensor_value duration = {
          .val1 = 10,
      };

      rc = sensor_attr_set(accelerometer_dev, SENSOR_CHAN_ACCEL_XYZ,
                           SENSOR_ATTR_SLOPE_TH,
                           &threshold);
      if (rc) {
         LOG_ERR("Accelerometer error: could not set threshold for device %s, %d / %s",
                 accelerometer_dev->name, rc, strerror(-rc));
      }

      rc = sensor_attr_set(accelerometer_dev, SENSOR_CHAN_ACCEL_XYZ,
                           SENSOR_ATTR_SLOPE_DUR,
                           &duration);
      if (rc) {
         LOG_ERR("Accelerometer error: could not set duration for device %s, %d / %s",
                 accelerometer_dev->name, rc, strerror(-rc));
      }

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
