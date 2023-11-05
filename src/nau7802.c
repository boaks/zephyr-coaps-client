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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "appl_storage.h"
#include "appl_storage_config.h"
#include "appl_time.h"
#include "io_job_queue.h"
#include "nau7802.h"
#include "ui.h"

#include "uart_cmd.h"

LOG_MODULE_REGISTER(SCALE, CONFIG_SCALE_LOG_LEVEL);

#define NAU7802_ADDR 0x2A

#define NAU7802_PU_CTRL 0x00
#define NAU7802_CTRL1 0x01
#define NAU7802_CTRL2 0x02
#define NAU7802_OCAL1 0x03
#define NAU7802_I2C 0x11
#define NAU7802_ADC 0x12
#define NAU7802_ADC_CTRL 0x15
#define NAU7802_PGA_CTRL 0x1B
#define NAU7802_POWER_CTRL 0x1C

/*
  gain 0 => 1x
  gain 1 => 2x
  gain 2 => 4x
  ...
  gain 7 => 128x
*/
#define NAU7802_DEFAULT_GAIN 7
#define NAU7802_DEFAULT_AVREF 2400
#define NAU7802_CALIBRATE_ON_RESUME true
#define NAU7802_MAX_ADC_VALUE 0x7ffffd
#define NAU7802_NONE_ADC_VALUE 0xff800000

#define NAU7802_MIN_PAUSE_CALIBRATION_MS 200
#define NAU7802_MIN_PAUSE_SAMPLE_MS 100

static K_MUTEX_DEFINE(scale_mutex);

#define MAX_ADC_CHANNELS 2
#define MAX_ADC_LOOPS 12
#define MIN_ADC_SAMPLES 4
#define MAX_ADC_LOOPS_CALIBRATION 15
#define MIN_ADC_SAMPLES_CALIBRATION 8
#define MAX_ADC_HISTORY ((MIN_ADC_SAMPLES_CALIBRATION < MIN_ADC_SAMPLES) ? MIN_ADC_SAMPLES : MIN_ADC_SAMPLES_CALIBRATION)

#define MAX_ADC_DITHER 512
#define MIN_ADC_DIVIDER 2000

#define NORMALIZE_DIVIDER(DIV) ((DIV <= MIN_ADC_DIVIDER) ? 0 : DIV)

#define TEMPERATURE_DOUBLE(T) (((double)T) / 1000.0)

static const struct storage_config calibration_storage_configs[] = {
    {
        .storage_device = DEVICE_DT_GET_OR_NULL(DT_PHANDLE(DT_ALIAS(scale_a), calibration_storage)),
        .desc = "calibration",
        .is_flash_device = false,
        .id = CALIBRATION_A_ID,
        .magic = 0x03400560,
        .version = 2,
        .value_size = CALIBRATE_VALUE_SIZE,
        .pages = 4,
    },
    {
        .storage_device = DEVICE_DT_GET_OR_NULL(DT_PHANDLE(DT_ALIAS(scale_b), calibration_storage)),
        .desc = "calibration",
        .is_flash_device = false,
        .id = CALIBRATION_B_ID,
        .magic = 0x03400560,
        .version = 2,
        .value_size = CALIBRATE_VALUE_SIZE,
        .pages = 4,
    },
};

enum avdd_source {
   AVDD_UNKNOWN,
   AVDD_EXTERNAL,
   AVDD_INTERNAL,
};

const char *AVDD_DESCRIPTION[] = {"n.a", "ext.", "int."};

struct scale_config {
   const char *channel_name;
   const struct storage_config *storage_config;
   const struct device *i2c_device;
   bool ok;
   bool read_temperature;
   enum avdd_source source;
   uint8_t gain;
   uint16_t avref;
   int64_t resume_time;
   int32_t internal_offset;
   int32_t raw;
   int32_t weight;
   int32_t temperature;
   int32_t offset;
   int32_t divider;
   int32_t calibration_temperature;
   int32_t previous_offset;
   int32_t previous_divider;
};

static struct scale_config configs[MAX_ADC_CHANNELS] = {
    {
        .channel_name = "CHA",
        .storage_config = &calibration_storage_configs[0],
        .i2c_device = DEVICE_DT_GET_OR_NULL(DT_BUS(DT_ALIAS(scale_a))),
        .ok = false,
        .read_temperature = false,
        .source = AVDD_UNKNOWN,
        .gain = DT_PROP_OR(DT_ALIAS(scale_a), gain, NAU7802_DEFAULT_GAIN),
        .avref = DT_PROP_OR(DT_ALIAS(scale_a), avref_mv, NAU7802_DEFAULT_AVREF),
        .resume_time = 0,
        .raw = NAU7802_NONE_ADC_VALUE,
        .temperature = NAU7802_NONE_ADC_VALUE,
        .offset = NAU7802_NONE_ADC_VALUE,
        .divider = 0,
        .previous_offset = NAU7802_NONE_ADC_VALUE,
        .previous_divider = 0,
    },
    {
        .channel_name = "CHB",
        .storage_config = &calibration_storage_configs[1],
        .i2c_device = DEVICE_DT_GET_OR_NULL(DT_BUS(DT_ALIAS(scale_b))),
        .ok = false,
        .read_temperature = false,
        .source = AVDD_UNKNOWN,
        .gain = DT_PROP_OR(DT_ALIAS(scale_a), gain, NAU7802_DEFAULT_GAIN),
        .avref = DT_PROP_OR(DT_ALIAS(scale_b), avref_mv, NAU7802_DEFAULT_AVREF),
        .resume_time = 0,
        .raw = NAU7802_NONE_ADC_VALUE,
        .temperature = NAU7802_NONE_ADC_VALUE,
        .offset = NAU7802_NONE_ADC_VALUE,
        .divider = 0,
        .previous_offset = NAU7802_NONE_ADC_VALUE,
        .previous_divider = 0,
    }};

static volatile enum calibrate_phase current_calibrate_phase = CALIBRATE_NONE;
static volatile enum calibrate_phase next_calibrate_phase = CALIBRATE_START;

static bool scale_wait_uptime(int64_t *time)
{
   int64_t delay = *time;
   if (delay) {
      delay -= k_uptime_get();
      if (delay > 0) {
         k_sleep(K_MSEC(delay));
      }
      *time = 0;
      return true;
   } else {
      return false;
   }
}

static inline int scale_check(const struct device *i2c_dev, uint8_t reg, uint8_t mask, uint8_t value)
{
   uint8_t data = 0;
   int rc = i2c_reg_read_byte(i2c_dev, NAU7802_ADDR, reg, &data);
   if (!rc && ((data & mask) != (value & mask))) {
      rc = -EAGAIN;
   }
   return rc;
}

static int scale_wait(const struct device *i2c_dev, uint8_t reg, uint8_t mask, uint8_t value, const k_timeout_t interval, const k_timeout_t timeout)
{
   int64_t end = k_uptime_get() + k_ticks_to_ms_floor64(timeout.ticks);
   int loops = 0;
   int rc = 0;

   while ((rc = scale_check(i2c_dev, reg, mask, value)) == -EAGAIN) {
      k_sleep(interval);
      if ((k_uptime_get() - end) > 0) {
         break;
      }
      ++loops;
   }
   if (rc == -EAGAIN) {
      rc = scale_check(i2c_dev, reg, mask, value);
   }
   //   LOG_INF("%d wait loops", loops);
   return rc;
}

static inline int scale_start_adc(const struct device *i2c_dev)
{
   return i2c_reg_update_byte(i2c_dev, NAU7802_ADDR, NAU7802_PU_CTRL, BIT(4), BIT(4));
}

static inline int scale_stop_adc(const struct device *i2c_dev)
{
   return i2c_reg_update_byte(i2c_dev, NAU7802_ADDR, NAU7802_PU_CTRL, BIT(4), 0);
}

static inline int scale_wait_adc(const struct device *i2c_dev, const k_timeout_t timeout)
{
   k_sleep(K_MSEC(80));
   return scale_wait(i2c_dev, NAU7802_PU_CTRL, BIT(5), BIT(5), K_MSEC(5), timeout);
}

static int scale_read_regs(const struct scale_config *scale_dev, uint8_t reg, int32_t *val, size_t len)
{
   int32_t v = 0;
   uint8_t value[4] = {0, 0, 0, 0};

   if (len < 1 || len > 4) {
      return -EINVAL;
   }

   int rc = i2c_write_read(scale_dev->i2c_device, NAU7802_ADDR, &reg, sizeof(reg), value, len);
   if (rc) {
      LOG_WRN("ADC %s read %d failure %d (%s).", scale_dev->channel_name, reg, rc, strerror(-rc));
      return rc;
   }

   LOG_HEXDUMP_DBG(value, len, "NAU7802-ADC:");
   switch (len) {
      case 1:
         v = (int32_t)value[0];
         break;
      case 2:
         v = (int32_t)sys_get_be16(value);
         break;
      case 3:
         v = (int32_t)sys_get_be24(value);
         break;
      case 4:
         v = (int32_t)sys_get_be32(value);
         break;
   }
   LOG_DBG("ADC %s reg %d %d", scale_dev->channel_name, reg, v);
   if (val) {
      *val = v;
   }
   return rc;
}

static int scale_read_ocal_value(const struct scale_config *scale_dev, int32_t *val)
{
   int32_t value = 0;
   int rc = scale_read_regs(scale_dev, NAU7802_OCAL1, &value, 3);

   if (!rc) {
      if (value & 0x800000) {
         // Bit 23 is +/-
         value = -(value & 0x7FFFFF);
      }
      if (val) {
         *val = value;
      }
   }
   return rc;
}

static int scale_internal_calibrate(struct scale_config *scale_dev, const char *tag)
{
   int rc = 0;
   int32_t value = 0;

   if (!scale_dev || !scale_dev->ok) {
      return -EINVAL;
   }
   /* Calibrate internal offset */
   rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_CTRL2, BIT(0) | BIT(1), 0);
   if (rc) {
      LOG_WRN("ADC %s I2C NAU7802 int. offset calibration %s, write failure, %d (%s)!",
              scale_dev->channel_name, tag, rc, strerror(-rc));
      return rc;
   }
   scale_wait_uptime(&scale_dev->resume_time);
   /* Start internal calibration */
   rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_CTRL2, BIT(2), BIT(2));
   if (rc) {
      LOG_WRN("ADC %s I2C NAU7802 start int. calibration %s, write failure, %d (%s)!",
              scale_dev->channel_name, tag, rc, strerror(-rc));
      return rc;
   }
   /* wait internal calibration */
   rc = scale_wait(scale_dev->i2c_device, NAU7802_CTRL2, BIT(2), 0, K_MSEC(25), K_MSEC(2000));
   if (rc) {
      LOG_WRN("ADC %s I2C NAU7802 int. calibration %s not ready, %d (%s)!",
              scale_dev->channel_name, tag, rc, strerror(-rc));
      return rc;
   }
   rc = scale_read_ocal_value(scale_dev, &value);
   if (!rc) {
      LOG_INF("ADC %s I2C NAU7802 int. calibration %s offset: %d", scale_dev->channel_name, tag, value);
      scale_dev->internal_offset = value;
   }
   scale_dev->resume_time = k_uptime_get() + NAU7802_MIN_PAUSE_SAMPLE_MS;
   return rc;
}

static inline int scale_suspend(const struct scale_config *scale_dev)
{
   if (!scale_dev || !scale_dev->ok) {
      return -EINVAL;
   }
   LOG_INF("ADC %s suspend", scale_dev->channel_name);
   return i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_PU_CTRL, BIT(7) | BIT(2) | BIT(1), 0);
}

static int scale_reset(const struct scale_config *scale_dev)
{
   /* reset */
   int rc = 0;
   if (!scale_dev || !scale_dev->ok) {
      return -EINVAL;
   }
   LOG_INF("ADC %s reset", scale_dev->channel_name);
   rc = i2c_reg_write_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_PU_CTRL, BIT(0));
   if (rc) {
      LOG_WRN("ADC %s I2C NAU7802 reset, write failure %d (%s)!", scale_dev->channel_name, rc, strerror(-rc));
      return rc;
   }
   k_sleep(K_MSEC(10));
   return rc;
}

static int scale_avdd(struct scale_config *scale_dev, bool internal)
{
   int rc = 0;

   LOG_INF("ADC %s avdd %s", scale_dev->channel_name, internal ? "int" : "ext");

   if (internal) {
      /* internal AVDD, enable AVDD-LDO */
      int ldo_vref = ((4500 - scale_dev->avref) / 300);
      if (ldo_vref < 0) {
         ldo_vref = 0;
      } else if (ldo_vref > 7) {
         ldo_vref = 7;
      }

      /* VLDO 6 => 2.7V, VLDO 7 => 2.4V */
      rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_CTRL1, (7 << 3), (ldo_vref << 3));
      if (rc) {
         LOG_WRN("ADC %s I2C NAU7802 config VLDO, write failure, %d (%s)!", scale_dev->channel_name, rc, strerror(-rc));
         return rc;
      }

      rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_PU_CTRL, BIT(7), BIT(7));
      if (rc) {
         LOG_WRN("ADC %s I2C NAU7802 enable AVDD-LDO, write failure, %d (%s)!", scale_dev->channel_name, rc, strerror(-rc));
      }
      scale_dev->source = AVDD_INTERNAL;
      scale_dev->read_temperature = true;
      return rc;
   } else {
      /* external AVDD, disable AVDD-LDO */
      rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_PU_CTRL, BIT(7), 0);
      if (rc) {
         LOG_WRN("ADC %s I2C NAU7802 disable AVDD-LDO, write failure, %d (%s)!", scale_dev->channel_name, rc, strerror(-rc));
      }
      scale_dev->source = AVDD_EXTERNAL;
      scale_dev->read_temperature = false;
      return rc;
   }
}

static int scale_resume(struct scale_config *scale_dev, bool calibrate)
{
   int rc = 0;

   if (!scale_dev || !scale_dev->ok) {
      return -EINVAL;
   }
   LOG_INF("ADC %s resume", scale_dev->channel_name);
   /* PUD */
   rc = i2c_reg_write_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_PU_CTRL, BIT(1));
   if (rc) {
      LOG_WRN("ADC %s I2C NAU7802 start, write failure %d (%s)!", scale_dev->channel_name, rc, strerror(-rc));
      return rc;
   }
   /* wait PUR */
   rc = scale_wait(scale_dev->i2c_device, NAU7802_PU_CTRL, BIT(3), BIT(3), K_MSEC(25), K_MSEC(2000));
   if (rc) {
      LOG_WRN("ADC %s I2C NAU7802 power up not ready, %d (%s)!", scale_dev->channel_name, rc, strerror(-rc));
      return rc;
   }

   /* PUA */
   rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_PU_CTRL, BIT(2), BIT(2));
   if (rc) {
      LOG_WRN("ADC %s I2C NAU7802 enable PUA, write failure, %d (%s)!", scale_dev->channel_name, rc, strerror(-rc));
      return rc;
   }

   /* Select gain 4 => 16x, gain 7 => 128x */
   rc = i2c_reg_write_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_CTRL1, scale_dev->gain);
   if (rc) {
      LOG_WRN("ADC %s I2C NAU7802 config gain, write failure, %d (%s)!", scale_dev->channel_name, rc, strerror(-rc));
      return rc;
   }

   /* Use low ESR cap, disable PGA bypass, select ADC reg 0x15 */
   rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_PGA_CTRL, BIT(4) | BIT(6) | BIT(7), BIT(6));
   if (rc) {
      LOG_WRN("ADC %s I2C NAU7802 denable low ESR cap., write failure, %d (%s)!", scale_dev->channel_name, rc, strerror(-rc));
      return rc;
   }

   /* Disable ADC Chopper */
   rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_ADC_CTRL, 0x3 << 4, 0x3 << 4);
   if (rc) {
      LOG_WRN("ADC %s I2C NAU7802 disble ADC chopper, write failure, %d (%s)!", scale_dev->channel_name, rc, strerror(-rc));
      return rc;
   }

   /* Enable CAP on Vin2 */
   rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_POWER_CTRL, BIT(7), BIT(7));
   if (rc) {
      LOG_WRN("ADC %s I2C NAU7802 enable Vin 2 cap, write failure, %d (%s)!", scale_dev->channel_name, rc, strerror(-rc));
      return rc;
   }

   if (scale_dev->source != AVDD_UNKNOWN) {
      rc = scale_avdd(scale_dev, scale_dev->source == AVDD_INTERNAL);
      if (rc) {
         return rc;
      }
   }

   if (calibrate) {
      scale_dev->resume_time = k_uptime_get() + NAU7802_MIN_PAUSE_CALIBRATION_MS;
      rc = scale_internal_calibrate(scale_dev, "resume");
   } else {
      scale_dev->resume_time = k_uptime_get() + NAU7802_MIN_PAUSE_SAMPLE_MS;
   }
   return rc;
}

static int scale_read_adc_value(const struct scale_config *scale_dev, int32_t *val, bool log)
{
   int v = 0;
   int rc = scale_wait_adc(scale_dev->i2c_device, K_MSEC(2000));
   if (rc) {
      LOG_WRN("ADC %s wait failure %d (%s).", scale_dev->channel_name, rc, strerror(-rc));
      return rc;
   }
   rc = scale_read_regs(scale_dev, NAU7802_ADC, &v, 3);
   if (rc) {
      LOG_WRN("ADC %s read failure %d (%s).", scale_dev->channel_name, rc, strerror(-rc));
      return rc;
   }
   v <<= 8;
   v >>= 8;
   if (log) {
      LOG_INF("ADC %s raw 0x%06x/%d", scale_dev->channel_name, v & 0xffffff, v);
   }
   if (val) {
      *val = v;
   }

   return rc;
}

static int32_t scale_values_average(int counter, int32_t *values)
{
   int32_t rc = -ENODATA;
   if (counter > 0) {
      int32_t v = values[0];
      if (counter > 1) {
         for (int i = 1; i < counter; ++i) {
            v += values[i];
         }
         rc = (v + (counter / 2)) / counter;
      } else {
         rc = v;
      }
   }
   return rc;
}

static int scale_read_channel_value(struct scale_config *scale_dev, int max_loops, int min_values)
{
   int rc = 0;
   int loops = 0;
   int counter = 0;

   int32_t v = 0;
   int32_t max = 0;
   int32_t min = 0;
   int32_t values[MAX_ADC_HISTORY];

   scale_dev->raw = NAU7802_NONE_ADC_VALUE;

   if (!scale_dev->ok) {
      LOG_INF("ADC %s not available", scale_dev->channel_name);
      return -ESTALE;
   }

   if (scale_wait_uptime(&scale_dev->resume_time)) {
      loops = 1;
   }

   rc = scale_start_adc(scale_dev->i2c_device);
   if (rc) {
      LOG_INF("ADC %s start failed,  %d (%s).", scale_dev->channel_name, rc, strerror(-rc));
      return rc;
   }

   LOG_INF("ADC %s int. calibration offset %d", scale_dev->channel_name, scale_dev->internal_offset);

   if (loops) {
      LOG_INF("ADC %s initial sample", scale_dev->channel_name);
      rc = scale_read_adc_value(scale_dev, &v, false);
      if (rc) {
         return rc;
      }
   } else {
      loops = 1;
   }
   rc = scale_read_adc_value(scale_dev, &v, true);
   if (rc) {
      return rc;
   }
   values[counter++] = v;
   max = v;
   min = v;
   do {
      rc = scale_read_adc_value(scale_dev, &v, true);
      if (rc) break;
      ++loops;
      if (v < min) {
         min = v;
      } else if (v > max) {
         max = v;
      }
      if ((max - min) > MAX_ADC_DITHER) {
         counter = 1;
         values[0] = v;
         if (loops < max_loops) {
            LOG_INF("ADC %s raw 0x%06x, %d, diff: %d, loop: %d, instable => retry", scale_dev->channel_name, v & 0xffffff, v, (max - min), loops);
            max = v;
            min = v;
         }
      } else {
         values[counter++] = v;
      }
   } while (counter < min_values && loops < max_loops);

   scale_stop_adc(scale_dev->i2c_device);

   if (counter) {
      v = scale_values_average(counter, values);
      if (v < -NAU7802_MAX_ADC_VALUE || v > NAU7802_MAX_ADC_VALUE) {
         LOG_INF("ADC %s raw 0x%06x, %d, invalid",
                 scale_dev->channel_name, v & 0xffffff, v);
         return -EINVAL;
      }
      if (scale_dev->internal_offset) {
         int32_t r = v - scale_dev->internal_offset;
         if (r < -NAU7802_MAX_ADC_VALUE || r > NAU7802_MAX_ADC_VALUE) {
            LOG_INF("ADC %s raw 0x%06x, 0x%06x, %d, %d, invalid",
                    scale_dev->channel_name, v & 0xffffff, r & 0xffffff, v, r);
            return -EINVAL;
         } else {
            LOG_INF("ADC %s rev. cal. 0x%06x, %d", scale_dev->channel_name, r & 0xffffff, r);
         }
      }
   } else {
      v = 0;
   }

   if (rc < 0 || counter < min_values) {
      LOG_INF("ADC %s raw 0x%06x, %d, ++/-- %d, %d/%d loops, instable",
              scale_dev->channel_name, v & 0xffffff, v, (max - min), counter, loops);
      rc = -ESTALE;
   } else {
      scale_dev->raw = v;
      LOG_INF("ADC %s raw 0x%06x, %d, +/- %d, %d/%d loops",
              scale_dev->channel_name, v & 0xffffff, v, (max - min), counter, loops);
      rc = 0;
   }
   return rc;
}

// 10 g resolution
#define SCALE_RESOLUTION_G 10

static int scale_values_to_doubles(struct scale_config *scale_dev, double *value, double *temperature)
{
   int rc = -ENODATA;
   int32_t div = scale_dev->divider;

   if (!scale_dev->ok || scale_dev->raw == NAU7802_NONE_ADC_VALUE) {
      LOG_INF("ADC %s => invalid (%s)", scale_dev->channel_name, AVDD_DESCRIPTION[scale_dev->source]);
      return rc;
   }

   if (div > 0) {
      int32_t off = scale_dev->offset;
      int32_t offset_value = scale_dev->weight - off;
      int32_t cal_value = ((offset_value * (1000 / SCALE_RESOLUTION_G)) + (div / 2)) / div;
      double v = ((double)cal_value) / (1000 / SCALE_RESOLUTION_G);
      if (value) {
         *value = v;
      }
      LOG_INF("ADC %s => off: %d, div: %d (%s)", scale_dev->channel_name, off, div, AVDD_DESCRIPTION[scale_dev->source]);
      if (div == 1) {
         LOG_INF("ADC %s => raw: %d, rel: %d", scale_dev->channel_name, scale_dev->weight, offset_value);
      } else {
         LOG_INF("ADC %s => raw: %d, rel: %d := %.2f kg", scale_dev->channel_name, scale_dev->weight, offset_value, v);
      }
      if (scale_dev->read_temperature && temperature) {
         *temperature = TEMPERATURE_DOUBLE(scale_dev->temperature);
      }
      rc = 0;
   }

   return rc;
}

static int scale_read_temperature(struct scale_config *scale_dev, int max_loops, int min_values)
{
   int rc = -ENODATA;
   int64_t val = 0;

   /* Select temperature */
   rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_I2C, BIT(1), BIT(1));
   if (rc) {
      LOG_INF("ADC %s select temperature failed,  %d (%s).", scale_dev->channel_name, rc, strerror(-rc));
      return rc;
   }

   rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_CTRL1, 7, 1);
   if (rc) {
      LOG_INF("ADC %s set gain=2 failed,  %d (%s).", scale_dev->channel_name, rc, strerror(-rc));
      return rc;
   }
   scale_dev->resume_time = k_uptime_get() + NAU7802_MIN_PAUSE_SAMPLE_MS;

   rc = scale_read_channel_value(scale_dev, max_loops, min_values);
   if (rc) {
      LOG_INF("ADC %s read temperature failed,  %d (%s).", scale_dev->channel_name, rc, strerror(-rc));
      return rc;
   }
   /* Unselect temperature */
   rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_I2C, BIT(1), 0);
   if (rc) {
      LOG_INF("ADC %s unselect temperature failed,  %d (%s).", scale_dev->channel_name, rc, strerror(-rc));
   }
   rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_CTRL1, 7, scale_dev->gain);
   if (rc) {
      LOG_INF("ADC %s restore gain failed,  %d (%s).", scale_dev->channel_name, rc, strerror(-rc));
      return rc;
   }
   scale_dev->resume_time = k_uptime_get() + NAU7802_MIN_PAUSE_SAMPLE_MS;

   val = ((int64_t)scale_dev->raw * scale_dev->avref * 1000) >> 24;
   LOG_INF("ADC %s temperature %d V", scale_dev->channel_name, (int32_t)val);
   // datasheet 109mV at 25°C and +390uV/°C
   val = 25000 + (val - 109000) * 1000 / 390;
   LOG_INF("ADC %s temperature %d", scale_dev->channel_name, (int32_t)val);
   scale_dev->temperature = (int32_t)val;

   return rc;
}

static int scale_read_channel(struct scale_config *scale_dev)
{
   int rc = -ENODATA;
   int max_loops = 0;
   int min_values = 0;
   enum calibrate_phase phase = current_calibrate_phase;

   LOG_INF("ADC %s scale start.", scale_dev->channel_name);

   if (scale_dev->divider > 0) {
      if (CALIBRATE_ZERO == phase || CALIBRATE_CHA_10KG == phase || CALIBRATE_CHB_10KG == phase) {
         max_loops = MAX_ADC_LOOPS_CALIBRATION;
         min_values = MIN_ADC_SAMPLES_CALIBRATION;
         rc = 0;
      } else if (CALIBRATE_NONE == phase) {
         max_loops = MAX_ADC_LOOPS;
         min_values = MIN_ADC_SAMPLES;
         rc = 0;
      }
      if (!rc) {
         rc = scale_read_channel_value(scale_dev, max_loops, min_values);
         if (!rc) {
            scale_dev->weight = scale_dev->raw;
         }
      }
   }

   if (!rc && scale_dev->read_temperature) {
      rc = scale_read_temperature(scale_dev, MAX_ADC_LOOPS, MIN_ADC_SAMPLES);
   }

   if (rc) {
      LOG_INF("ADC %s scale channel not ready %d.", scale_dev->channel_name, rc);
   } else if (scale_dev->raw == NAU7802_NONE_ADC_VALUE) {
      LOG_INF("ADC %s => invalid (%s)", scale_dev->channel_name, AVDD_DESCRIPTION[scale_dev->source]);
      rc = -ENODATA;
   }

   return rc;
}

static int scale_init_channel(struct scale_config *scale_dev)
{
   int rc = -ENOTSUP;
   int32_t value = 0;
   uint8_t data = 0;
   uint8_t calibration[CALIBRATE_VALUE_SIZE];
   const struct device *i2c_dev = scale_dev->i2c_device;

   if (!i2c_dev) {
      LOG_INF("ADC %s not configured", scale_dev->channel_name);
      return -ENOTSUP;
   }

   LOG_INF("ADC %s initialize %s", scale_dev->channel_name, i2c_dev->name);

   if (!device_is_ready(i2c_dev)) {
      if (i2c_dev) {
         LOG_WRN("ADC %s missing I2C %s", scale_dev->channel_name, i2c_dev->name);
      } else {
         LOG_WRN("ADC %s missing I2C", scale_dev->channel_name);
      }
      return rc;
   }

   rc = appl_storage_add(scale_dev->storage_config);
   if (rc) {
      LOG_WRN("ADC %s missing calibration EEPROM", scale_dev->channel_name);
      return rc;
   }

   /* reset */
   rc = scale_reset(scale_dev);
   if (rc) {
      return rc;
   }

   /* resume */
   rc = scale_resume(scale_dev, false);
   if (rc) {
      return rc;
   }

   /* AVDD to external */
   rc = scale_avdd(scale_dev, false);
   if (rc) {
      return rc;
   }
   rc = scale_read_ocal_value(scale_dev, &value);
   if (rc) {
      return rc;
   }
   scale_dev->internal_offset = value;

   rc = scale_read_channel_value(scale_dev, 10, 3);
   if (rc && rc != -EINVAL && rc != -ESTALE) {
      return rc;
   }
   if (rc == -EINVAL) {
      LOG_INF("ADC %s I2C NAU7802 ext. AVDD: invalid", scale_dev->channel_name);
   } else if (!rc) {
      LOG_INF("ADC %s I2C NAU7802 ext. AVDD: %d", scale_dev->channel_name, scale_dev->raw);
   }

   if (rc == -EINVAL) {
      /* no external AVDD, enable internal AVDD-LDO */
      rc = scale_avdd(scale_dev, true);
      if (rc) {
         return rc;
      }
      rc = scale_read_channel_value(scale_dev, 10, 3);
      if (rc) {
         return rc;
      }
      LOG_INF("ADC %s I2C NAU7802 int. AVDD: %d", scale_dev->channel_name, scale_dev->raw);
   }

   /* internal calibrate */
   rc = scale_internal_calibrate(scale_dev, "init");
   if (rc) {
      return rc;
   }

   rc = i2c_reg_read_byte(i2c_dev, NAU7802_ADDR, 0x0, &data);
   LOG_INF("ADC %s I2C NAU7802 reg. 00: %02x", scale_dev->channel_name, data);
   rc = i2c_reg_read_byte(i2c_dev, NAU7802_ADDR, 0x1, &data);
   LOG_INF("ADC %s I2C NAU7802 reg. 01: %02x", scale_dev->channel_name, data);
   rc = i2c_reg_read_byte(i2c_dev, NAU7802_ADDR, 0x2, &data);
   LOG_INF("ADC %s I2C NAU7802 reg. 02: %02x", scale_dev->channel_name, data);
   rc = i2c_reg_read_byte(i2c_dev, NAU7802_ADDR, 0x15, &data);
   LOG_INF("ADC %s I2C NAU7802 reg. 15: %02x", scale_dev->channel_name, data);
   rc = i2c_reg_read_byte(i2c_dev, NAU7802_ADDR, 0x1B, &data);
   LOG_INF("ADC %s I2C NAU7802 reg. 1B: %02x", scale_dev->channel_name, data);
   rc = i2c_reg_read_byte(i2c_dev, NAU7802_ADDR, 0x1C, &data);
   LOG_INF("ADC %s I2C NAU7802 reg. 1C: %02x", scale_dev->channel_name, data);

   memset(calibration, 0, sizeof(calibration));
   rc = appl_storage_read_bytes_item(scale_dev->storage_config->id, 0, NULL, calibration, sizeof(calibration));
   if (rc == sizeof(calibration)) {
      uint16_t calibration_avref = sys_get_be16(&calibration[8]);
      scale_dev->divider = NORMALIZE_DIVIDER(sys_get_be16(&calibration[3]));
      if (scale_dev->divider > 0 && scale_dev->avref == calibration_avref) {
         scale_dev->offset = sys_get_be24(calibration) << 8;
         scale_dev->offset >>= 8;
         scale_dev->calibration_temperature = sys_get_be24(&calibration[5]) << 8;
         scale_dev->calibration_temperature >>= 8;
         LOG_INF("ADC %s calibration 0x%06x, %d, %.1f loaded.", scale_dev->channel_name, scale_dev->offset, scale_dev->divider, TEMPERATURE_DOUBLE(scale_dev->calibration_temperature));
      } else {
         LOG_INF("ADC %s disabled.", scale_dev->channel_name);
         scale_dev->offset = 0;
      }
   } else {
#ifdef CONFIG_NAU7802_DUMMY_CALIBRATION
      LOG_INF("ADC %s dummy calibration, loading failed, %d (%s)", scale_dev->channel_name, rc, strerror(-rc));
      scale_dev->divider = 100;
#else  /* CONFIG_NAU7802_DUMMY_CALIBRATION */
      LOG_INF("ADC %s disabled, calibration loading failed, %d (%s)", scale_dev->channel_name, rc, strerror(-rc));
      scale_dev->divider = 0;
#endif /* CONFIG_NAU7802_DUMMY_CALIBRATION */
      scale_dev->offset = 0;
   }

   return 0;
}

static int scale_restart_channel(struct scale_config *scale_dev, bool calibrate)
{
   int rc;

   if (!scale_dev || !device_is_ready(scale_dev->i2c_device)) {
      return -ENOTSUP;
   }

   if (scale_dev->ok) {
      int32_t offset = 0;
      rc = scale_read_ocal_value(scale_dev, &offset);
      if (rc) {
         // reading internal calibration offset failed
         LOG_INF("ADC %s i2c read int. calibration failed, %d (%s).", scale_dev->channel_name, rc, strerror(-rc));
         scale_dev->ok = false;
      } else if (scale_dev->internal_offset != offset) {
         // different internal calibration offset => new sensor
         LOG_INF("ADC %s int. calibration offset changed,  new %d != old %d.",
                 scale_dev->channel_name, offset, scale_dev->internal_offset);
         scale_dev->ok = false;
      }
   }
   if (scale_dev->ok) {
      rc = scale_resume(scale_dev, calibrate);
   } else {
      scale_dev->ok = true;
      rc = scale_init_channel(scale_dev);
   }
   if (rc) {
      scale_suspend(scale_dev);
      scale_dev->ok = false;
      scale_dev->raw = NAU7802_NONE_ADC_VALUE;
   }
   return rc;
}

static int scale_sample_channel(struct scale_config *scale_dev)
{
   int rc = scale_restart_channel(scale_dev, NAU7802_CALIBRATE_ON_RESUME);
   if (!rc) {
      rc = scale_read_channel(scale_dev);
   }
   scale_suspend(scale_dev);
   return rc;
}

#ifdef CONFIG_NAU7802_PARALLEL_READ

static K_SEM_DEFINE(scale_ready, 0, 1);

static void scale_read_channel_B_fn(struct k_work *work)
{
   (void)work;
   scale_read_channel(&configs[1]);
   k_sem_give(&scale_ready);
}

static void scale_sample_channel_B_fn(struct k_work *work)
{
   (void)work;
   scale_sample_channel(&configs[1]);
   k_sem_give(&scale_ready);
}

static K_WORK_DEFINE(scale_read_channel_B_work, scale_read_channel_B_fn);
static K_WORK_DEFINE(scale_sample_channel_B_work, scale_sample_channel_B_fn);

#endif /* CONFIG_NAU7802_PARALLEL_READ */

static inline uint8_t scale_gain_id(uint8_t gain)
{
   switch (gain) {
      case 1:
         return 0;
      case 2:
         return 1;
      case 4:
         return 2;
      case 8:
         return 3;
      case 16:
         return 4;
      case 32:
         return 5;
      case 64:
         return 6;
      case 128:
         return 7;
   }
   return 0;
}

static int scale_init(void)
{
   for (int channel = 0; channel < MAX_ADC_CHANNELS; ++channel) {
      struct scale_config *scale_dev = &configs[channel];
      if (scale_dev) {
         int rc = scale_dev->gain;
         scale_dev->ok = true;
         scale_dev->gain = scale_gain_id(scale_dev->gain);
         LOG_INF("ADC %s setup gain to %d/%d.", scale_dev->channel_name, rc, scale_dev->gain);
         rc = scale_init_channel(scale_dev);
         scale_suspend(scale_dev);
         if (rc) {
            LOG_INF("ADC %s setup failed, %d (%s).", scale_dev->channel_name, rc, strerror(-rc));
            scale_dev->ok = false;
         }
      }
   }

   return 0;
}

/* CONFIG_APPLICATION_INIT_PRIORITY + 1*/
SYS_INIT(scale_init, APPLICATION, 91);

int scale_sample(double *valueA, double *valueB, double *temperatureA, double *temperatureB)
{
   int rc = -EINPROGRESS;
   int64_t time = k_uptime_get();

   k_mutex_lock(&scale_mutex, K_FOREVER);
   if (CALIBRATE_NONE == current_calibrate_phase) {
      rc = 0;
#ifdef CONFIG_NAU7802_PARALLEL_READ
      k_sem_reset(&scale_ready);
      work_submit_to_cmd_queue(&scale_sample_channel_B_work);
#endif /* CONFIG_NAU7802_PARALLEL_READ */
      scale_sample_channel(&configs[0]);
      if (!scale_values_to_doubles(&configs[0], valueA, temperatureA)) {
         rc |= 1;
      }
#ifdef CONFIG_NAU7802_PARALLEL_READ
      if (k_sem_take(&scale_ready, K_MSEC(5000)) == 0) {
         if (!scale_values_to_doubles(&configs[1], valueB, temperatureB)) {
            rc |= 2;
         }
      }
      k_work_cancel(&scale_sample_channel_B_work);
#else  /* CONFIG_NAU7802_PARALLEL_READ */
      scale_sample_channel(&configs[1]);
      if (!scale_values_to_doubles(&configs[1], valueB, temperatureB)) {
         rc |= 2;
      }
#endif /* CONFIG_NAU7802_PARALLEL_READ */
   }
   k_mutex_unlock(&scale_mutex);
   if (-EINPROGRESS == rc) {
      LOG_INF("ADC scale busy.");
   } else {
      time = k_uptime_get() - time;
      if (rc < 0) {
         LOG_INF("ADC scale samples failed with %d in %d ms", rc, (int)time);
      } else {
         LOG_INF("ADC scale samples %c/%c in %d ms", rc & 1 ? 'A' : '-', rc & 2 ? 'B' : '-', (int)time);
      }
   }
   return rc;
}

static void scale_prepare_calibration(struct scale_config *scale_dev)
{
   scale_dev->previous_offset = scale_dev->offset;
   scale_dev->previous_divider = scale_dev->divider;
   scale_dev->divider = 1;
   scale_dev->offset = 0;
}

static void scale_restore_calibration(struct scale_config *scale_dev, bool offset)
{
   if (offset) {
      scale_dev->offset = scale_dev->previous_offset;
   }
   scale_dev->divider = scale_dev->previous_divider;
}

static void scale_save_calibration(struct scale_config *scale_dev, uint8_t *calibration, size_t len)
{
   memset(calibration, 0, len);
   sys_put_be24(scale_dev->offset, calibration);
   sys_put_be16(scale_dev->divider, calibration + 3);
   sys_put_be24(scale_dev->calibration_temperature, calibration + 5);
   sys_put_be16(scale_dev->avref, calibration + 8);
}

int scale_calibrate(enum calibrate_phase phase)
{
   int rc = 0;
   int64_t time = k_uptime_get();
   bool temperature = false;
   bool save = false;
   bool stop = false;
   bool error = false;

   k_mutex_lock(&scale_mutex, K_FOREVER);
   rc = next_calibrate_phase;
   if (CALIBRATE_DONE == phase) {
      if (CALIBRATE_NONE == current_calibrate_phase) {
         rc = CALIBRATE_NONE;
         phase = CALIBRATE_NONE;
      }
   }
   if (current_calibrate_phase != phase) {
      if (CALIBRATE_NONE != phase && CALIBRATE_DONE != phase) {
         if (next_calibrate_phase != phase) {
            phase = CALIBRATE_NONE;
            rc = -ENOENT;
         }
      }
      switch (phase) {
         case CALIBRATE_NONE:
            LOG_INF("ADC Scale canceled calibration.");
            scale_restore_calibration(&configs[0], true);
            scale_restore_calibration(&configs[1], true);
            stop = true;
            break;
         case CALIBRATE_START:
            LOG_INF("ADC Scale start calibration.");
            current_calibrate_phase = phase;
            scale_restart_channel(&configs[0], NAU7802_CALIBRATE_ON_RESUME);
            scale_restart_channel(&configs[1], NAU7802_CALIBRATE_ON_RESUME);
            scale_prepare_calibration(&configs[0]);
            scale_prepare_calibration(&configs[1]);
            rc = CALIBRATE_ZERO;
            next_calibrate_phase = rc;
            current_calibrate_phase = phase;
            break;
         case CALIBRATE_ZERO:
            current_calibrate_phase = phase;
            error = true;
            time = k_uptime_get();
#ifdef CONFIG_NAU7802_PARALLEL_READ
            k_sem_reset(&scale_ready);
            work_submit_to_cmd_queue(&scale_read_channel_B_work);
#endif /* CONFIG_NAU7802_PARALLEL_READ */
            if (scale_read_channel(&configs[0])) {
               configs[0].divider = 0;
               configs[0].calibration_temperature = 0;
            } else {
               configs[0].offset = configs[0].weight;
               configs[0].calibration_temperature = configs[0].temperature;
            }
#ifdef CONFIG_NAU7802_PARALLEL_READ
            k_sem_take(&scale_ready, K_MSEC(5000));
            k_work_cancel(&scale_read_channel_B_work);
            if (!configs[1].ok || configs[1].raw == NAU7802_NONE_ADC_VALUE) {
#else  /* CONFIG_NAU7802_PARALLEL_READ */
            if (scale_read_channel(&configs[1])) {
#endif /* CONFIG_NAU7802_PARALLEL_READ */
               configs[1].divider = 0;
               configs[1].calibration_temperature = 0;
            } else {
               configs[1].offset = configs[1].weight;
               configs[1].calibration_temperature = configs[1].temperature;
            }
            time = k_uptime_get() - time;
            LOG_INF("ADC Scale calibrate 0, CHA 0x%06x/%.1f, CHB 0x%06x/%.1f. (%d ms)",
                    configs[0].offset, TEMPERATURE_DOUBLE(configs[0].calibration_temperature),
                    configs[1].offset, TEMPERATURE_DOUBLE(configs[1].calibration_temperature), (int)time);
            if (configs[0].ok && configs[0].divider > 0) {
               // calibrate channel A
               rc = CALIBRATE_CHA_10KG;
               next_calibrate_phase = rc;
               error = false;
            } else if (configs[1].ok && configs[1].divider > 0) {
               // calibrate channel B
               rc = CALIBRATE_CHB_10KG;
               next_calibrate_phase = rc;
               error = false;
            }
            break;
         case CALIBRATE_CHA_10KG:
            current_calibrate_phase = phase;
            temperature = configs[0].read_temperature;
            configs[0].read_temperature = false;
            time = k_uptime_get();
            rc = scale_read_channel(&configs[0]);
            time = k_uptime_get() - time;
            configs[0].read_temperature = temperature;
            if (!rc) {
               configs[0].weight -= configs[0].offset;
               configs[0].divider = NORMALIZE_DIVIDER((configs[0].weight + 5) / 10); // 10.0kg
               if (configs[0].divider > 0) {
                  LOG_INF("ADC Scale calibrate CHA 10kg, rel: %d, div: %d (%d ms)", configs[0].weight, configs[0].divider, (int)time);
               } else {
                  LOG_INF("ADC Scale disable CHA 10kg, rel: %d (%d ms)", configs[0].weight, (int)time);
               }
            } else {
               LOG_INF("ADC Scale disable CHA, no sample (%d ms)", (int)time);
            }
            if (configs[1].ok && configs[1].divider > 0) {
               rc = CALIBRATE_CHB_10KG;
               next_calibrate_phase = rc;
            } else {
               save = true;
            }
            break;
         case CALIBRATE_CHB_10KG:
            current_calibrate_phase = phase;
            temperature = configs[1].read_temperature;
            configs[1].read_temperature = false;
            time = k_uptime_get();
            rc = scale_read_channel(&configs[1]);
            time = k_uptime_get() - time;
            temperature = configs[1].read_temperature;
            if (!rc) {
               configs[1].weight -= configs[1].offset;
               configs[1].divider = NORMALIZE_DIVIDER((configs[1].weight + 5) / 10); // 10.0kg
               if (configs[1].divider > 0) {
                  LOG_INF("ADC Scale calibrate CHB 10kg, rel: %d, div: %d (%d ms)", configs[1].weight, configs[1].divider, (int)time);
               } else {
                  LOG_INF("ADC Scale disable CHB 10kg, rel: %d (%d ms)", configs[1].weight, (int)time);
               }
            } else {
               LOG_INF("ADC Scale disable CHB, no sample (%d ms)", (int)time);
            }
            save = true;
            break;
         case CALIBRATE_DONE:
            if (current_calibrate_phase == CALIBRATE_ZERO) {
               scale_restore_calibration(&configs[0], false);
               scale_restore_calibration(&configs[1], false);
               save = true;
            } else if (current_calibrate_phase == CALIBRATE_CHA_10KG) {
               save = true;
            } else {
               stop = true;
            }
            break;
      }
      if (error) {
         configs[0].offset = 0;
         configs[1].offset = 0;
         configs[0].calibration_temperature = 0;
         configs[1].calibration_temperature = 0;
         configs[0].divider = 0;
         configs[1].divider = 0;
         configs[0].avref = 0;
         configs[1].avref = 0;
         save = true;
      }
      if (save || stop) {
         current_calibrate_phase = CALIBRATE_NONE;
         next_calibrate_phase = CALIBRATE_START;
         rc = CALIBRATE_NONE;
         scale_suspend(&configs[0]);
         scale_suspend(&configs[1]);
      }
      if (save) {
         uint8_t calibration[CALIBRATE_VALUE_SIZE];

         configs[0].divider = NORMALIZE_DIVIDER(configs[0].divider);
         scale_save_calibration(&configs[0], calibration, sizeof(calibration));
         appl_storage_write_bytes_item(CALIBRATION_A_ID, calibration, sizeof(calibration));

         configs[1].divider = NORMALIZE_DIVIDER(configs[1].divider);
         scale_save_calibration(&configs[1], calibration, sizeof(calibration));
         appl_storage_write_bytes_item(CALIBRATION_B_ID, calibration, sizeof(calibration));

         LOG_INF("ADC Scale calibration saved.");
         LOG_INF("ADC Scale CHA 0x%06x %d", configs[0].offset, configs[0].divider);
         LOG_INF("ADC Scale CHB 0x%06x %d", configs[1].offset, configs[1].divider);
         if (CALIBRATE_DONE != phase) {
            rc = CALIBRATE_DONE;
         }
      }
   }
   k_mutex_unlock(&scale_mutex);
   return rc;
}

bool scale_calibrate_setup(void)
{
   bool request = false;
   int select_mode = CALIBRATE_START;
   int trigger = 0;

   LOG_INF("Start calibration.");
   ui_prio(true);
   select_mode = scale_calibrate(CALIBRATE_START);
   while (CALIBRATE_NONE < select_mode) {
      // waiting
      LOG_INF("Waiting for calibration %d.", select_mode);
      if (select_mode == CALIBRATE_ZERO) {
         ui_led_op_prio(LED_COLOR_GREEN, LED_BLINKING);
      } else if (select_mode == CALIBRATE_CHA_10KG) {
         ui_led_op_prio(LED_COLOR_BLUE, LED_BLINKING);
      } else if (select_mode == CALIBRATE_CHB_10KG) {
         ui_led_op_prio(LED_COLOR_GREEN, LED_BLINKING);
         ui_led_op_prio(LED_COLOR_BLUE, LED_BLINKING);
      }
      trigger = ui_input(K_SECONDS(60));
      if (trigger >= 0) {
         ui_led_op_prio(LED_COLOR_ALL, LED_CLEAR);
         if (trigger) {
            select_mode = CALIBRATE_DONE;
         }
         // calibrate
         if (select_mode == CALIBRATE_ZERO) {
            LOG_INF("Calibrate 0 offsets.");
            ui_led_op_prio(LED_COLOR_GREEN, LED_SET);
            select_mode = scale_calibrate(select_mode);
            ui_led_op_prio(LED_COLOR_GREEN, LED_CLEAR);
         } else if (select_mode == CALIBRATE_CHA_10KG) {
            LOG_INF("Calibrate CHA 10kg.");
            ui_led_op_prio(LED_COLOR_BLUE, LED_SET);
            select_mode = scale_calibrate(select_mode);
            ui_led_op_prio(LED_COLOR_BLUE, LED_CLEAR);
         } else if (select_mode == CALIBRATE_CHB_10KG) {
            LOG_INF("Calibrate CHB 10kg.");
            ui_led_op_prio(LED_COLOR_BLUE, LED_SET);
            ui_led_op_prio(LED_COLOR_GREEN, LED_SET);
            select_mode = scale_calibrate(select_mode);
            ui_led_op_prio(LED_COLOR_BLUE, LED_CLEAR);
            ui_led_op_prio(LED_COLOR_GREEN, LED_CLEAR);
         }
         if (select_mode == CALIBRATE_DONE) {
            LOG_INF("Calibration done.");
            select_mode = scale_calibrate(select_mode);
            request = true;
         }
      } else {
         if (select_mode > CALIBRATE_ZERO) {
            LOG_INF("Calibration 0 done (timeout).");
            select_mode = scale_calibrate(CALIBRATE_DONE);
         } else {
            LOG_INF("Calibration canceled (timeout).");
            select_mode = scale_calibrate(CALIBRATE_NONE);
         }
         break;
      }
   }
   ui_led_op_prio(LED_COLOR_ALL, LED_CLEAR);
   ui_prio(false);

   return request;
}

static int scale_cmd(const char *parameter)
{
   (void)parameter;

   char buf[96];
   double scaleA = 0;
   double scaleB = 0;
   double temperatureA = 0;
   double temperatureB = 0;
   int64_t time = 0;
   int index = 0;
   int res = 0;
   int res2 = 0;
   int err = 0;

   res = scale_sample(&scaleA, &scaleB, &temperatureA, &temperatureB);
   if (0 < res) {
      index += snprintf(buf, sizeof(buf), "Last calibration: ");
      err = appl_storage_read_bytes_item(CALIBRATION_A_ID, 0, &time, NULL, 0);
      if (err > 0) {
         res2 = 1;
         index += snprintf(buf + index, sizeof(buf) - index, "A ");
         index += appl_format_time(time, buf + index, sizeof(buf) - index);
      }
      err = appl_storage_read_bytes_item(CALIBRATION_B_ID, 0, &time, NULL, 0);
      if (err > 0) {
         if (res2) {
            index += snprintf(buf + index, sizeof(buf) - index, ", ");
         } else {
            res2 = 1;
         }
         index += snprintf(buf + index, sizeof(buf) - index, "B ");
         index += appl_format_time(time, buf + index, sizeof(buf) - index);
      }
      if (res2) {
         LOG_INF("%s", buf);
      }
      index = 0;
      if (res & 1) {
         index += snprintf(buf + index, sizeof(buf) - index, "CHA %.2f kg, %.1f°C", scaleA, temperatureA);
         if (res & 2) {
            index += snprintf(buf + index, sizeof(buf) - index, ",");
         }
      }
      if (res & 2) {
         index += snprintf(buf + index, sizeof(buf) - index, "CHB %.2f kg, %.1f°C", scaleB, temperatureB);
      }
      LOG_INF("%s", buf);
   }

   return 0;
}

UART_CMD(scale, NULL, "read scale info.", scale_cmd, NULL, 0);
