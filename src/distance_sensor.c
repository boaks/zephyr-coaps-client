/*
 * Copyright (c) 2024 Achim Kraus CloudCoap.net
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

#include <stdio.h>

#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "distance_sensor.h"
#include "io_job_queue.h"
#include "power_manager.h"

#include "sh_cmd.h"

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

static const struct device *distance_dev = DEVICE_DT_GET_OR_NULL(DT_ALIAS(distance_sensor));

static int distance_read(const struct device *dev, int32_t *val, int32_t *sigma)
{
   int err = 0;
   struct sensor_value data = {0, 0};

   //   err = sensor_sample_fetch(dev);
   err = sensor_sample_fetch_chan(dev, SENSOR_CHAN_DISTANCE);
   if (err < 0) {
      LOG_ERR("Sample fetch, error: %d", err);
      return err;
   }

   err = sensor_channel_get(dev, SENSOR_CHAN_DISTANCE, &data);

   if (err) {
      LOG_ERR("sensor_channel_get, error: %d", err);
      return err;
   }

   if (val) {
#ifdef CONFIG_VL53L1X
      *val = data.val1;
#elif defined(CONFIG_VL53L0X)
      *val = data.val1 * 1000 + (data.val2 / 1000);
#endif
   }
   if (sigma) {
#ifdef CONFIG_VL53L1X
      *sigma = data.val2;
#else
      *sigma = 0;
#endif
   }
   return err;
}

#define IMIN(A, B) (((int)A) < ((int)B) ? (A) : (B))
#define IMAX(A, B) (((int)A) > ((int)B) ? (A) : (B))

int distance_meter_config(unsigned int m, unsigned int r)
{
   int rc = 0;

   if (!device_is_ready(distance_dev)) {
      if (distance_dev) {
         LOG_ERR("Distancemeter device %s is not ready!", distance_dev->name);
      } else {
         LOG_ERR("Distancemeter device is not available!");
      }
      rc = -ENOTSUP;
   } else {
#ifdef CONFIG_VL53L1X
      struct sensor_value mode = {
          .val1 = m,
          .val2 = 0,
      };

      struct sensor_value roi = {
          .val1 = 0,
          .val2 = 0,
      };
      int x = 8;
      int y = 8;

      rc = sensor_attr_set(distance_dev, SENSOR_CHAN_DISTANCE,
                           SENSOR_ATTR_CONFIGURATION,
                           &mode);
      if (rc) {
         LOG_ERR("Distancemeter: configuration error %d!", rc);
      } else {
         LOG_ERR("Distancemeter: configured %d.", mode.val1);
      }

      rc = sensor_attr_get(distance_dev, SENSOR_CHAN_DISTANCE,
                           SENSOR_ATTR_CALIB_TARGET,
                           &roi);
      if (rc) {
         LOG_ERR("Distancemeter: get calibration error %d!", rc);
      } else {
         if (roi.val1 & 0x10000) {
            // requires patch!
            x = (roi.val1 & 0xf000) / (256 * 16);
            y = (roi.val2 & 0xf000) / (256 * 16);
            LOG_INF("Distancemeter: get center (%d,%d)", x, y);
         }
      }
      roi.val1 = IMIN((y + (int)r), 15) * 16 + IMAX((x - (int)r), 0);
      roi.val2 = IMAX((y - (int)r), 0) * 16 + IMIN((x + (int)r), 15);

      rc = sensor_attr_set(distance_dev, SENSOR_CHAN_DISTANCE,
                           SENSOR_ATTR_CALIB_TARGET,
                           &roi);
      if (rc) {
         LOG_ERR("Distancemeter: calibration error %d!", rc);
      } else {
         LOG_INF("Distancemeter: calibration (%d,%d),(%d,%d)",
                 (roi.val1 / 16) & 0xf, roi.val1 & 0xf,
                 (roi.val2 / 16) & 0xf, roi.val2 & 0xf);
      }
#endif
   }
   return rc;
}

static int distance_meter_init(void)
{
   int rc = -ENOTSUP;
   if (!device_is_ready(distance_dev)) {
      if (distance_dev) {
         LOG_ERR("Distancemeter device %s is not ready!", distance_dev->name);
      } else {
         LOG_ERR("Distancemeter device is not available!");
      }
   } else {
      rc = distance_meter_config(3, 2);
   }
   return rc;
}

SYS_INIT(distance_meter_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#define MAX_MEASUREMENT_LOOPS 10
#define MEASUREMENT_LOOPS 5

int distance_meter_get(int32_t *distance_mm)
{
   int rc = -ENOTSUP;

   if (device_is_ready(distance_dev)) {
      int index = 0;
      int32_t value;
      int32_t sigma;
      int32_t values[MEASUREMENT_LOOPS];
      int32_t sigmas[MEASUREMENT_LOOPS];

      for (int i = 0; i < MAX_MEASUREMENT_LOOPS; ++i) {
         rc = distance_read(distance_dev, &value, &sigma);
         if (rc) {
            break;
         }
         if (value <= 0) {
            continue;
         }
         values[index] = value;
         sigmas[index] = sigma;
         for (int j = index; j > 0; --j) {
            if (values[j - 1] > value) {
               values[j] = values[j - 1];
               sigmas[j] = sigmas[j - 1];
               values[j - 1] = value;
               sigmas[j - 1] = sigma;
            }
         }
         if (++index >= MEASUREMENT_LOOPS) {
            break;
         }
      }
      if (!rc && index > 0) {
         for (int i = 0; i < index; ++i) {
#ifdef CONFIG_VL53L1X
            LOG_INF("Distance: %d. %d mm, %d", i, values[i], sigmas[i] >> 16);
#else
            LOG_INF("Distance: %d. %d mm", i, values[i]);
#endif
         }
         LOG_INF("Distance: %d mm", values[index / 2]);
         if (distance_mm) {
            *distance_mm = values[index / 2];
         }
      } else {
         LOG_INF("Distance: n.a.");
      }
   } else {
      if (distance_dev) {
         LOG_ERR("Distancemeter device %s is not ready!", distance_dev->name);
      } else {
         LOG_ERR("Distancemeter device is not available!");
      }
   }

   return rc;
}

#ifdef CONFIG_SH_CMD

static int sh_cmd_distance(const char *parameter)
{
   int res = 0;
   unsigned int mode = 0;
   unsigned int roi = 0;

   int err = sscanf(parameter, "%u %u", &mode, &roi);
   if (err == 2) {
      res = distance_meter_config(mode, roi);
   } else {
      int32_t distance = 0;
      res = distance_meter_get(&distance);
      if (!res) {
         LOG_INF("Distance: %d mm", distance);
      }
   }
   return res;
}

#ifdef CONFIG_VL53L1X

static void sh_cmd_distance_help(void)
{
   LOG_INF("> help dist:");
   LOG_INF("  dist mode roi : configure mode and roi.");
   LOG_INF("  dist          : measure distance.");
}

SH_CMD(dist, NULL, "distance sensor.", sh_cmd_distance, sh_cmd_distance_help, 0);
#else
SH_CMD(dist, NULL, "measure distance.", sh_cmd_distance, NULL, 0);
#endif

#endif
