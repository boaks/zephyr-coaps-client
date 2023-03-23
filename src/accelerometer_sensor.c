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

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

const struct device *accelerometer_dev = DEVICE_DT_GET(DT_ALIAS(accelerometer_sensor));

static accelerometer_handler_t accelerometer_handler;

static void accelerometer_trigger_handler(const struct device *dev,
                                          const struct sensor_trigger *trig)
{
   int err = 0;
   struct sensor_value data[ACCELEROMETER_CHANNELS];
   struct accelerometer_evt evt;

   switch (trig->type) {
      case SENSOR_TRIG_MOTION:

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
         LOG_ERR("Unknown trigger");
   }
}

int accelerometer_init(accelerometer_handler_t handler)
{
   if (handler) {
      accelerometer_handler = handler;
   }

   if (!device_is_ready(accelerometer_dev)) {
      LOG_ERR("Accelerometer device is not ready!");
      return ENOTSUP;
   }

   return 0;
}

int accelerometer_enable(bool enable)
{
   struct sensor_trigger trig = {
       .chan = SENSOR_CHAN_ACCEL_XYZ,
       .type = SENSOR_TRIG_MOTION};

   int err = sensor_trigger_set(accelerometer_dev, &trig, enable ? accelerometer_trigger_handler : NULL);
   if (err) {
      LOG_ERR("Accelerometer error: could not set trigger for device %s, error: %d",
              accelerometer_dev->name, err);
      return err;
   }
   LOG_INF("Accelerometer-threshold: %s", enable ? "enabled" : "disabled");

   return 0;
}
