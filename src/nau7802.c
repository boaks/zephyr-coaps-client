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

#include "appl_settings.h"
#include "appl_storage.h"
#include "appl_storage_config.h"
#include "appl_time.h"
#include "expansion_port.h"
#include "io_job_queue.h"
#include "nau7802.h"
#include "parse.h"
#include "ui.h"

#include "sh_cmd.h"

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
#define NAU7802_CALIBRATION_INTERVAL_MS (60 * 60 * 1000) // 1h

// 10 g resolution
#define SCALE_RESOLUTION_G 10
// 10 kg calibration
#define SCALE_CALIBRATION_G 10000

#define MAX_ADC_LOOPS 12
#define MIN_ADC_SAMPLES 4
#define MAX_ADC_LOOPS_CALIBRATION 15
#define MIN_ADC_SAMPLES_CALIBRATION 8
#define MAX_ADC_HISTORY ((MIN_ADC_SAMPLES_CALIBRATION < MIN_ADC_SAMPLES) ? MIN_ADC_SAMPLES : MIN_ADC_SAMPLES_CALIBRATION)

#define MAX_ADC_DITHER (512 * 3)
#define MIN_ADC_DIVIDER 2000
#define DUMMY_ADC_DIVIDER 100

#define NORMALIZE_DIVIDER(DIV) (((DIV) >= MIN_ADC_DIVIDER || (DIV) == DUMMY_ADC_DIVIDER) ? (DIV) : 0)

#define DIV_ROUNDED(N, D) (((N) + (D) / 2) / (D))

#define TEMPERATURE_DOUBLE(T) (((double)T) / 1000.0)

#if DT_NODE_HAS_STATUS(DT_ALIAS(scale_a), okay)
#define HAS_SCALE_A
#if DT_NODE_HAS_STATUS(DT_ALIAS(scale_b), okay)
#define HAS_SCALE_B
#if (DT_BUS(DT_ALIAS(scale_a)) == DT_BUS(DT_ALIAS(scale_b)))
// no parallel reads on the same i2c bus
#undef CONFIG_NAU7802_PARALLEL_READ
#endif
#endif /* DT_NODE_HAS_STATUS(DT_ALIAS(scale_b), okay) */
#else
#error "missing scale definition in devicetree!"
#endif /* DT_NODE_HAS_STATUS(DT_ALIAS(scale_a), okay) */

/* Only the first 256 bytes of the 512 bytes EEPROM are useable! */
/* The second 256 are in I2C address conflict with the feather's clock at 0x51. */
#define CALIBRATION_STORAGE_PAGES 2

enum calibrate_phase {
   CALIBRATE_NONE,
   CALIBRATE_START,
   CALIBRATE_ZERO,
   CALIBRATE_CHA_10KG,
#ifdef HAS_SCALE_B
   CALIBRATE_CHB_10KG,
#endif /* HAS_SCALE_B */
   CALIBRATE_DONE,
   CALIBRATE_CMD,
};

static K_MUTEX_DEFINE(scale_mutex);

#ifdef HAS_SCALE_A
#if DT_NODE_HAS_STATUS(DT_PHANDLE(DT_ALIAS(scale_a), calibration_storage), okay)
#define HAS_EEPROM_A
#endif
#endif /* HAS_SCALE_A */

#ifdef HAS_EEPROM_A
static const struct storage_config calibration_storage_config_a =
    {
        .storage_device = DEVICE_DT_GET_OR_NULL(DT_PHANDLE(DT_ALIAS(scale_a), calibration_storage)),
        .desc = "calibration A",
        .is_flash_device = false,
        .id = CALIBRATION_A_ID,
        .magic = 0x03400560,
        .version = 1,
        .value_size = CALIBRATE_VALUE_SIZE,
        .pages = CALIBRATION_STORAGE_PAGES,
};
#endif /* HAS_EEPROM_A */

#ifdef HAS_SCALE_B

#if DT_NODE_HAS_STATUS(DT_PHANDLE(DT_ALIAS(scale_b), calibration_storage), okay)
#define HAS_EEPROM_B
#endif
#endif /* HAS_SCALE_B */

#ifdef HAS_EEPROM_B
static const struct storage_config calibration_storage_config_b =
    {
        .storage_device = DEVICE_DT_GET_OR_NULL(DT_PHANDLE(DT_ALIAS(scale_b), calibration_storage)),
        .desc = "calibration B",
        .is_flash_device = false,
        .id = CALIBRATION_B_ID,
        .magic = 0x03400560,
        .version = 1,
        .value_size = CALIBRATE_VALUE_SIZE,
        .pages = CALIBRATION_STORAGE_PAGES,
};
#endif /* HAS_EEPROM_B */

enum avdd_source {
   AVDD_UNKNOWN,
   AVDD_EXTERNAL,
   AVDD_INTERNAL,
};

const char *AVDD_DESCRIPTION[] = {"n.a", "ext.", "int."};

struct scale_calibrate {
   int64_t time;
   int32_t offset;
   int32_t divider;
   int32_t calibration_temperature;
   uint16_t avref;
};

struct scale_config {
   const char *channel_name;
   const struct storage_config *storage_config;
   const struct device *i2c_device;
   struct scale_calibrate external_calibration;
   bool i2c_ok;
   bool calibrate;
   bool read_temperature;
   enum avdd_source source;
   uint8_t gain;
   int64_t resume_time;
   int64_t calibration_time;
   int32_t internal_offset;
   int32_t raw;
   int32_t weight;
   int32_t temperature;
};

#ifdef HAS_SCALE_A
static struct scale_config config_a =
    {
        .channel_name = "CHA",
#ifdef HAS_EEPROM_A
        .storage_config = &calibration_storage_config_a,
#else
        .storage_config = NULL,
#endif
        .i2c_device = DEVICE_DT_GET_OR_NULL(DT_BUS(DT_ALIAS(scale_a))),
        .external_calibration = {
            .avref = DT_PROP_OR(DT_ALIAS(scale_a), avref_mv, NAU7802_DEFAULT_AVREF),
            .offset = NAU7802_NONE_ADC_VALUE,
            .divider = 0,
        },
        .i2c_ok = false,
        .read_temperature = false,
        .source = AVDD_UNKNOWN,
        .gain = DT_PROP_OR(DT_ALIAS(scale_a), gain, NAU7802_DEFAULT_GAIN),
        .resume_time = 0,
        .calibration_time = 0,
        .raw = NAU7802_NONE_ADC_VALUE,
        .weight = NAU7802_NONE_ADC_VALUE,
        .temperature = NAU7802_NONE_ADC_VALUE,
};
#endif /* HAS_SCALE_A */

#ifdef HAS_SCALE_B
static struct scale_config config_b =
    {
        .channel_name = "CHB",
#ifdef HAS_EEPROM_B
        .storage_config = &calibration_storage_config_b,
#else
        .storage_config = NULL,
#endif
        .i2c_device = DEVICE_DT_GET_OR_NULL(DT_BUS(DT_ALIAS(scale_b))),
        .external_calibration = {
            .avref = DT_PROP_OR(DT_ALIAS(scale_b), avref_mv, NAU7802_DEFAULT_AVREF),
            .offset = NAU7802_NONE_ADC_VALUE,
            .divider = 0,
        },
        .i2c_ok = false,
        .read_temperature = false,
        .source = AVDD_UNKNOWN,
        .gain = DT_PROP_OR(DT_ALIAS(scale_b), gain, NAU7802_DEFAULT_GAIN),
        .resume_time = 0,
        .calibration_time = 0,
        .raw = NAU7802_NONE_ADC_VALUE,
        .weight = NAU7802_NONE_ADC_VALUE,
        .temperature = NAU7802_NONE_ADC_VALUE,
};
#endif /* HAS_SCALE_B */

static struct scale_config *configs[] =
    {
#ifdef HAS_SCALE_A
        &config_a,
#endif /* HAS_SCALE_A */
#ifdef HAS_SCALE_B
        &config_b,
#endif /* HAS_SCALE_B */
};

static const int max_configs = sizeof(configs) / sizeof(struct scale_config *);

static volatile enum calibrate_phase current_calibrate_phase = CALIBRATE_NONE;
static volatile enum calibrate_phase next_calibrate_phase = CALIBRATE_START;

static inline int32_t expand_sign_24(int32_t value)
{
   return (value << 8) >> 8;
}

static void scale_save_external_calibration(struct scale_config *scale_dev)
{
   int rc = 0;
   uint8_t calibration[CALIBRATE_VALUE_SIZE];

   if (scale_dev->storage_config && !scale_dev->i2c_ok) {
      LOG_INF("ADC %s I2C error.", scale_dev->channel_name);
      return;
   }

   memset(calibration, 0, sizeof(calibration));
   sys_put_be24(scale_dev->external_calibration.offset, calibration);
   sys_put_be24(scale_dev->external_calibration.divider, &calibration[3]);
   sys_put_be24(scale_dev->external_calibration.calibration_temperature, &calibration[6]);
   sys_put_be16(scale_dev->external_calibration.avref, &calibration[9]);
   if (scale_dev->storage_config) {
      rc = appl_storage_write_bytes_item(scale_dev->storage_config->id, calibration, sizeof(calibration));
   } else {
      rc = appl_settings_set_bytes(scale_dev->channel_name, calibration, sizeof(calibration));
   }
   if (rc) {
      LOG_INF("ADC %s saving external calibration failed, %d (%s).", scale_dev->channel_name, rc, strerror(-rc));
   } else {
      appl_get_now(&scale_dev->external_calibration.time);
      LOG_INF("ADC %s external calibration saved.", scale_dev->channel_name);
   }
}

static void scale_read_external_calibration(struct scale_config *scale_dev)
{
   int rc = 0;
   struct scale_calibrate cal;
   uint8_t calibration[CALIBRATE_VALUE_SIZE];

   if (scale_dev->storage_config && !scale_dev->i2c_ok) {
      LOG_INF("ADC %s I2C error.", scale_dev->channel_name);
      return;
   }

   memset(calibration, 0, sizeof(calibration));
   if (scale_dev->storage_config) {
      rc = appl_storage_read_bytes_item(scale_dev->storage_config->id, 0,
                                        &cal.time, calibration, sizeof(calibration));
   } else {
      rc = appl_settings_get_bytes(scale_dev->channel_name, &cal.time, calibration, sizeof(calibration));
   }
   if (rc == sizeof(calibration)) {
      cal.offset = expand_sign_24((int32_t)sys_get_be24(calibration));
      cal.divider = NORMALIZE_DIVIDER(sys_get_be24(&calibration[3]));
      cal.calibration_temperature = expand_sign_24((int32_t)sys_get_be24(&calibration[6]));
      cal.avref = sys_get_be16(&calibration[9]);
      if (cal.divider > 0 &&
          (cal.divider == DUMMY_ADC_DIVIDER || cal.avref == scale_dev->external_calibration.avref)) {
         scale_dev->external_calibration = cal;
         LOG_INF("ADC %s calibration 0x%06x, %d, %.1f loaded.", scale_dev->channel_name,
                 cal.offset, cal.divider, TEMPERATURE_DOUBLE(cal.calibration_temperature));
      } else {
         LOG_INF("ADC %s disabled.", scale_dev->channel_name);
         scale_dev->external_calibration.divider = 0;
         scale_dev->external_calibration.offset = 0;
      }
   } else {
      if (rc) {
         LOG_INF("ADC %s disabled, calibration loading failed, %d (%s)", scale_dev->channel_name, rc, strerror(-rc));
      } else {
         LOG_INF("ADC %s disabled, calibration not available.", scale_dev->channel_name);
      }
      scale_dev->external_calibration.divider = 0;
      scale_dev->external_calibration.offset = 0;
   }
}

static void scales_read_external_calibration(void)
{
   for (int channel = 0; channel < max_configs; ++channel) {
      scale_read_external_calibration(configs[channel]);
   }
}


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

static int scale_write_regs(const struct scale_config *scale_dev, uint8_t reg, int32_t val, size_t len)
{
   int rc = 0;
   uint8_t value[4] = {0, 0, 0, 0};

   if (len < 1 || len > 4) {
      return -EINVAL;
   }

   switch (len) {
      case 1:
         value[0] = (uint8_t)val;
         break;
      case 2:
         sys_put_be16(val, value);
         break;
      case 3:
         sys_put_be24(val, value);
         break;
      case 4:
         sys_put_be32(val, value);
         break;
   }

   rc = i2c_burst_write(scale_dev->i2c_device, NAU7802_ADDR, reg, value, len);
   if (rc) {
      LOG_WRN("ADC %s write %d failure %d (%s).", scale_dev->channel_name, reg, rc, strerror(-rc));
   }
   return rc;
}

static int scale_read_regs(const struct scale_config *scale_dev, uint8_t reg, int32_t *val, size_t len)
{
   int rc = 0;
   int32_t v = 0;
   uint8_t value[4] = {0, 0, 0, 0};

   if (len < 1 || len > 4) {
      return -EINVAL;
   }

   rc = i2c_burst_read(scale_dev->i2c_device, NAU7802_ADDR, reg, value, len);
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

static int scale_write_ocal_value(const struct scale_config *scale_dev, int32_t val)
{
   bool neg = val < 0;

   if (neg) {
      val = -val;
   }
   if (val > 0x7FFFFF) {
      val &= 0x7FFFFF;
   }
   if (neg) {
      val |= 0x800000;
   }

   return scale_write_regs(scale_dev, NAU7802_OCAL1, val, 3);
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
   int64_t now = 0;

   if (!scale_dev || !scale_dev->i2c_ok || !scale_dev->i2c_device) {
      return -EINVAL;
   }
   /* Calibrate internal offset */
   rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_CTRL2, BIT(0) | BIT(1), 0);
   if (rc) {
      LOG_WRN("ADC %s I2C NAU7802 config int. calibration %s, write failure, %d (%s)!",
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
   now = k_uptime_get();
   rc = scale_read_ocal_value(scale_dev, &value);
   if (!rc) {
      if (value == 0) {
         /* Unlikely, the 0 mostly indicates a new device */
         value++;
         scale_write_ocal_value(scale_dev, value);
         LOG_INF("ADC %s I2C NAU7802 %s, int. calibration ready: offset %d*", scale_dev->channel_name, tag, value);
      } else {
         LOG_INF("ADC %s I2C NAU7802 %s, int. calibration ready: offset %d", scale_dev->channel_name, tag, value);
      }
      scale_dev->internal_offset = value;
      scale_dev->calibration_time = now + NAU7802_CALIBRATION_INTERVAL_MS;
      scale_dev->calibrate = false;
   }
   scale_dev->resume_time = now + NAU7802_MIN_PAUSE_SAMPLE_MS;
   return rc;
}

static inline int scale_suspend(const struct scale_config *scale_dev)
{
   if (!scale_dev || !scale_dev->i2c_ok || !scale_dev->i2c_device) {
      return -EINVAL;
   }
   LOG_INF("ADC %s suspend", scale_dev->channel_name);
   return i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_PU_CTRL, BIT(7) | BIT(2) | BIT(1), 0);
}

static int scale_reset(const struct scale_config *scale_dev)
{
   /* reset */
   int rc = 0;
   if (!scale_dev || !scale_dev->i2c_ok || !scale_dev->i2c_device) {
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
      int ldo_vref = ((4500 - scale_dev->external_calibration.avref) / 300);
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

static int scale_resume(struct scale_config *scale_dev)
{
   int rc = 0;
   int64_t now = 0;

   if (!scale_dev || !scale_dev->i2c_ok || !scale_dev->i2c_device) {
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

   now = k_uptime_get();
   if (scale_dev->calibrate && (now - scale_dev->calibration_time) > 0) {
      scale_dev->resume_time = now + NAU7802_MIN_PAUSE_CALIBRATION_MS;
      rc = scale_internal_calibrate(scale_dev, "resume");
   } else {
      scale_dev->resume_time = now + NAU7802_MIN_PAUSE_SAMPLE_MS;
   }
   return rc;
}

static int scale_read_adc_value(const struct scale_config *scale_dev, int32_t *val, bool log)
{
   int32_t v = 0;
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
   v = expand_sign_24(v);
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
   int maxCounter = 1;

   int32_t v = 0;
   int32_t max = 0;
   int32_t min = 0;
   int32_t values[MAX_ADC_HISTORY];

   scale_dev->raw = NAU7802_NONE_ADC_VALUE;

   if (!scale_dev->i2c_ok) {
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
      LOG_INF("ADC %s read failed, %d (%s)", scale_dev->channel_name, rc, strerror(-rc));
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
         if (counter > maxCounter) {
            maxCounter = counter;
         }
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
            LOG_INF("ADC %s no. cal. 0x%06x, %d", scale_dev->channel_name, r & 0xffffff, r);
         }
      }
   } else {
      v = 0;
   }

   if (rc < 0) {
      LOG_INF("ADC %s read failed, %d (%s)", scale_dev->channel_name, rc, strerror(-rc));
   } else if (counter < min_values) {
      LOG_INF("ADC %s raw 0x%06x, %d, ++/-- %d, %d/%d loops, instable",
              scale_dev->channel_name, v & 0xffffff, v, (max - min), maxCounter, loops);
      rc = -ESTALE;
   } else {
      scale_dev->raw = v;
      LOG_INF("ADC %s raw 0x%06x, %d, +/- %d, %d/%d loops",
              scale_dev->channel_name, v & 0xffffff, v, (max - min), counter, loops);
      rc = 0;
   }
   return rc;
}

static int scale_values_to_doubles(struct scale_config *scale_dev, double *value, double *temperature)
{
   int rc = -ENODATA;
   int32_t div = scale_dev->external_calibration.divider;

   if (!scale_dev->i2c_ok) {
      LOG_INF("ADC %s => missing.", scale_dev->channel_name);
      return rc;
   }
   if (scale_dev->external_calibration.divider == 0) {
      LOG_INF("ADC %s => not calibrated.", scale_dev->channel_name);
      return rc;
   }
   if (scale_dev->raw == NAU7802_NONE_ADC_VALUE) {
      LOG_INF("ADC %s => invalid (%s)", scale_dev->channel_name, AVDD_DESCRIPTION[scale_dev->source]);
      return rc;
   }

   if (div > 0) {
      int32_t off = scale_dev->external_calibration.offset;
      int32_t offset_value = scale_dev->weight - off;
      int32_t cal_value = DIV_ROUNDED((offset_value * (1000 / SCALE_RESOLUTION_G)), div);
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

   // avref [mV] * 1000 => [uV]
   val = ((int64_t)scale_dev->raw * scale_dev->external_calibration.avref * 1000) >> 24;
   LOG_INF("ADC %s temperature %d uV", scale_dev->channel_name, (int32_t)val);
   // datasheet 109mV at 25°C and +390uV/°C
   val = 25000 + (val - 109000) * 1000 / 390;
   LOG_INF("ADC %s temperature %d", scale_dev->channel_name, (int32_t)val);
   scale_dev->temperature = (int32_t)val;

   return rc;
}

static int scale_init_channel(struct scale_config *scale_dev)
{
   int rc = -ENOTSUP;
   int32_t value = 0;
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

   if (scale_dev->storage_config) {
      rc = appl_storage_add(scale_dev->storage_config);
      if (rc) {
         LOG_WRN("ADC %s missing calibration EEPROM", scale_dev->channel_name);
         return rc;
      }
   }

   /* reset */
   rc = scale_reset(scale_dev);
   if (rc) {
      return rc;
   }

   /* resume */
   scale_dev->calibrate = false;
   rc = scale_resume(scale_dev);
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
   /*
   {
      uint8_t data = 0;
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
   }
   */
   if (CALIBRATE_NONE == current_calibrate_phase) {
      scale_read_external_calibration(scale_dev);
   }
   return 0;
}

static int scale_restart_channel(struct scale_config *scale_dev)
{
   int rc;

   if (!scale_dev || !device_is_ready(scale_dev->i2c_device)) {
      return -ENOTSUP;
   }
   scale_dev->weight = NAU7802_NONE_ADC_VALUE;
   scale_dev->temperature = NAU7802_NONE_ADC_VALUE;
   if (scale_dev->i2c_ok) {
      int32_t offset = 0;
      rc = scale_read_ocal_value(scale_dev, &offset);
      if (rc) {
         // reading internal calibration offset failed
         LOG_INF("ADC %s i2c read int. calibration failed, %d (%s).", scale_dev->channel_name, rc, strerror(-rc));
         scale_dev->i2c_ok = false;
      } else if (offset == 0) {
         // different internal calibration offset => new sensor
         LOG_INF("ADC %s new sensor.", scale_dev->channel_name);
         scale_dev->i2c_ok = false;
      } else if (scale_dev->internal_offset != offset) {
         // different internal calibration offset => new sensor
         LOG_INF("ADC %s changed sensor, int. calibration offset changed,  new %d != old %d.",
                 scale_dev->channel_name, offset, scale_dev->internal_offset);
         scale_dev->i2c_ok = false;
      } else {
         LOG_INF("ADC %s same sensor.", scale_dev->channel_name);
      }
   }
   if (scale_dev->i2c_ok) {
      if (scale_dev->external_calibration.divider == 0) {
         LOG_INF("ADC %s disabled, divider 0.", scale_dev->channel_name);
         scale_suspend(scale_dev);
         return -ENODATA;
      } else {
         rc = scale_resume(scale_dev);
      }
   } else {
      scale_dev->i2c_ok = true;
      rc = scale_init_channel(scale_dev);
   }
   if (rc) {
      scale_suspend(scale_dev);
      scale_dev->i2c_ok = false;
      scale_dev->raw = NAU7802_NONE_ADC_VALUE;
   }
   return rc;
}

static int scale_check_channel(struct scale_config *scale_dev)
{
   int rc = scale_restart_channel(scale_dev);
   if (!rc) {
      scale_suspend(scale_dev);
   }
   return rc;
}

static int scale_sample_channel(struct scale_config *scale_dev)
{
   enum calibrate_phase phase = current_calibrate_phase;
   int max_loops = 0;
   int min_values = 0;
   int rc = scale_restart_channel(scale_dev);
   if (!rc) {
      rc = -ENODATA;
      LOG_INF("ADC %s scale start.", scale_dev->channel_name);
#ifdef HAS_SCALE_B
      if (CALIBRATE_ZERO == phase || CALIBRATE_CHA_10KG == phase || CALIBRATE_CHB_10KG == phase) {
#else  /* HAS_SCALE_B */
      if (CALIBRATE_ZERO == phase || CALIBRATE_CHA_10KG == phase) {
#endif /* HAS_SCALE_B */
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
      if (!rc && scale_dev->read_temperature) {
         rc = scale_read_temperature(scale_dev, MAX_ADC_LOOPS, MIN_ADC_SAMPLES);
      }

      if (rc) {
         LOG_INF("ADC %s scale channel not ready %d.", scale_dev->channel_name, rc);
      } else if (scale_dev->raw == NAU7802_NONE_ADC_VALUE) {
         LOG_INF("ADC %s => invalid (%s)", scale_dev->channel_name, AVDD_DESCRIPTION[scale_dev->source]);
         rc = -ENODATA;
      }
      scale_suspend(scale_dev);
   } else if (scale_dev->i2c_ok && scale_dev->external_calibration.divider == 0) {
      LOG_INF("ADC %s scale channel not calibrated.", scale_dev->channel_name);
   } else {
      LOG_INF("ADC %s scale channel not available.", scale_dev->channel_name);
   }
   return rc;
}

#ifdef HAS_SCALE_B
#ifdef CONFIG_NAU7802_PARALLEL_READ

static K_SEM_DEFINE(scale_ready, 0, 1);

static void scale_check_channel_B_fn(struct k_work *work)
{
   (void)work;
   scale_check_channel(configs[1]);
   k_sem_give(&scale_ready);
}

static void scale_sample_channel_B_fn(struct k_work *work)
{
   (void)work;
   scale_sample_channel(configs[1]);
   k_sem_give(&scale_ready);
}

static K_WORK_DEFINE(scale_check_channel_B_work, scale_check_channel_B_fn);
static K_WORK_DEFINE(scale_sample_channel_B_work, scale_sample_channel_B_fn);

#endif /* CONFIG_NAU7802_PARALLEL_READ */
#endif /* HAS_SCALE_B */

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
   expansion_port_power(true);
   for (int channel = 0; channel < max_configs; ++channel) {
      struct scale_config *scale_dev = configs[channel];
      int rc;
      scale_dev->i2c_ok = true;
      scale_dev->gain = scale_gain_id(scale_dev->gain);
      rc = scale_init_channel(scale_dev);
      scale_suspend(scale_dev);
      if (rc) {
         LOG_INF("ADC %s setup failed, %d (%s).", scale_dev->channel_name, rc, strerror(-rc));
         scale_dev->i2c_ok = false;
      }
   }
   expansion_port_power(false);
   return 0;
}

/* CONFIG_APPLICATION_INIT_PRIORITY + 1 */
SYS_INIT(scale_init, APPLICATION, CONFIG_NAU7802_INIT_PRIORITY);

static void scales_set_i2c(bool i2c_ok)
{
   for (int channel = 0; channel < max_configs; ++channel) {
      configs[channel]->i2c_ok = i2c_ok;
   }
}

static void scales_set_calibrate(bool calibate)
{
   for (int channel = 0; channel < max_configs; ++channel) {
      configs[channel]->calibrate = calibate;
   }
}

int scale_sample(double *valueA, double *valueB, double *temperatureA, double *temperatureB)
{
   int rc = -EINPROGRESS;
   int64_t time = k_uptime_get();

   k_mutex_lock(&scale_mutex, K_FOREVER);
   if (CALIBRATE_NONE == current_calibrate_phase) {
      int exp_rc = expansion_port_power(true);
      if (!exp_rc) {
         scales_set_i2c(false);
      }
      rc = 0;
      scales_set_calibrate(NAU7802_CALIBRATE_ON_RESUME);

#ifdef HAS_SCALE_B
#ifdef CONFIG_NAU7802_PARALLEL_READ
      k_sem_reset(&scale_ready);
      work_submit_to_cmd_queue(&scale_sample_channel_B_work);
#endif /* CONFIG_NAU7802_PARALLEL_READ */
#endif /* HAS_SCALE_B */
      scale_sample_channel(configs[0]);
      if (!scale_values_to_doubles(configs[0], valueA, temperatureA)) {
         rc |= 1;
      }
#ifdef HAS_SCALE_B
#ifdef CONFIG_NAU7802_PARALLEL_READ
      if (k_sem_take(&scale_ready, K_MSEC(5000)) == 0) {
         if (!scale_values_to_doubles(configs[1], valueB, temperatureB)) {
            rc |= 2;
         }
      }
      k_work_cancel(&scale_sample_channel_B_work);
#else  /* CONFIG_NAU7802_PARALLEL_READ */
      scale_sample_channel(configs[1]);
      if (!scale_values_to_doubles(configs[1], valueB, temperatureB)) {
         rc |= 2;
      }
#endif /* CONFIG_NAU7802_PARALLEL_READ */
#endif /* HAS_SCALE_B */
      expansion_port_power(false);
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
   if (!scale_dev->external_calibration.divider) {
      scale_dev->external_calibration.divider = 1;
   }
   scale_dev->calibrate = true;
   scale_dev->read_temperature = true;
}

static void scale_calc_calibration(struct scale_config *scale_dev, int reference, int time)
{
   int64_t weight = scale_dev->weight - scale_dev->external_calibration.offset;
   weight = DIV_ROUNDED((weight * 1000 - 1), reference); // ref/10.0kg
   scale_dev->external_calibration.divider = NORMALIZE_DIVIDER((int32_t)weight);
   if (scale_dev->external_calibration.divider > 0) {
      LOG_INF("ADC Scale calibrate %s 10kg, rel: %d, div: %d (%d ms)", scale_dev->channel_name, (int32_t)weight, scale_dev->external_calibration.divider, time);
   } else {
      LOG_INF("ADC Scale disable %s 10kg, rel: %d (%d ms)", scale_dev->channel_name, (int32_t)weight, time);
   }
}

static void scales_prepare_calibration(void)
{
   for (int channel = 0; channel < max_configs; ++channel) {
      scale_prepare_calibration(configs[channel]);
   }
}

static void scales_set_calibration_error(void)
{
   for (int channel = 0; channel < max_configs; ++channel) {
      struct scale_config *scale_dev = configs[channel];
      scale_dev->external_calibration.offset = 0;
      scale_dev->external_calibration.calibration_temperature = 0;
      scale_dev->external_calibration.divider = 0;
   }
}

static void scales_suspend(void)
{
   for (int channel = 0; channel < max_configs; ++channel) {
      scale_suspend(configs[channel]);
   }
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
   if (CALIBRATE_CMD == current_calibrate_phase) {
      phase = CALIBRATE_CMD;
      rc = CALIBRATE_NONE;
   } else {
      rc = next_calibrate_phase;
      if (CALIBRATE_DONE == phase) {
         if (CALIBRATE_NONE == current_calibrate_phase) {
            rc = CALIBRATE_NONE;
            phase = CALIBRATE_NONE;
         }
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
            scales_read_external_calibration();
            stop = true;
            break;
         case CALIBRATE_START:
            expansion_port_power(true);
            LOG_INF("ADC Scale start calibration.");
            current_calibrate_phase = phase;
#ifdef HAS_SCALE_B
#ifdef CONFIG_NAU7802_PARALLEL_READ
            k_sem_reset(&scale_ready);
            work_submit_to_cmd_queue(&scale_check_channel_B_work);
#endif /* CONFIG_NAU7802_PARALLEL_READ */
#endif /* HAS_SCALE_B */
            scale_check_channel(configs[0]);
#ifdef HAS_SCALE_B
#ifdef CONFIG_NAU7802_PARALLEL_READ
            k_sem_take(&scale_ready, K_MSEC(5000));
            k_work_cancel(&scale_sample_channel_B_work);
#else  /* CONFIG_NAU7802_PARALLEL_READ */
            scale_check_channel(configs[1]);
#endif /* CONFIG_NAU7802_PARALLEL_READ */
#endif /* HAS_SCALE_B */
            scales_prepare_calibration();
            rc = CALIBRATE_ZERO;
            next_calibrate_phase = rc;
            current_calibrate_phase = phase;
            break;
         case CALIBRATE_ZERO:
            current_calibrate_phase = phase;
            error = true;
            time = k_uptime_get();
#ifdef HAS_SCALE_B
#ifdef CONFIG_NAU7802_PARALLEL_READ
            k_sem_reset(&scale_ready);
            if (configs[1]->i2c_ok) {
               work_submit_to_cmd_queue(&scale_sample_channel_B_work);
            }
#endif /* CONFIG_NAU7802_PARALLEL_READ */
#endif /* HAS_SCALE_B */
            if (!configs[0]->i2c_ok || scale_sample_channel(configs[0])) {
               // failure
               configs[0]->external_calibration.divider = 0;
               configs[0]->external_calibration.calibration_temperature = 0;
            } else {
               configs[0]->external_calibration.offset = configs[0]->weight;
               configs[0]->external_calibration.calibration_temperature = configs[0]->temperature;
            }
#ifdef HAS_SCALE_B
#ifdef CONFIG_NAU7802_PARALLEL_READ
            if (configs[1]->i2c_ok) {
               k_sem_take(&scale_ready, K_MSEC(5000));
               k_work_cancel(&scale_sample_channel_B_work);
            }
            if (!configs[1]->i2c_ok || configs[1]->raw == NAU7802_NONE_ADC_VALUE) {
#else  /* CONFIG_NAU7802_PARALLEL_READ */
            if (scale_sample_channel(configs[1])) {
#endif /* CONFIG_NAU7802_PARALLEL_READ */
               // failure
               configs[1]->external_calibration.divider = 0;
               configs[1]->external_calibration.calibration_temperature = 0;
            } else {
               configs[1]->external_calibration.offset = configs[1]->weight;
               configs[1]->external_calibration.calibration_temperature = configs[1]->temperature;
            }
#endif /* HAS_SCALE_B */
            time = k_uptime_get() - time;

#ifdef HAS_SCALE_B
            LOG_INF("ADC Scale calibrate 0, CHA 0x%06x/%.1f, CHB 0x%06x/%.1f. (%d ms)",
                    configs[0]->external_calibration.offset, TEMPERATURE_DOUBLE(configs[0]->external_calibration.calibration_temperature),
                    configs[1]->external_calibration.offset, TEMPERATURE_DOUBLE(configs[1]->external_calibration.calibration_temperature), (int)time);
#else  /* HAS_SCALE_B */
            LOG_INF("ADC Scale calibrate 0, CHA 0x%06x/%.1f (%d ms)",
                    configs[0]->external_calibration.offset, TEMPERATURE_DOUBLE(configs[0]->external_calibration.calibration_temperature), (int)time);
#endif /* HAS_SCALE_B */

            if (configs[0]->i2c_ok && configs[0]->external_calibration.divider > 0) {
               // calibrate channel A
               rc = CALIBRATE_CHA_10KG;
               next_calibrate_phase = rc;
               error = false;
#ifdef HAS_SCALE_B
            } else if (configs[1]->i2c_ok && configs[1]->external_calibration.divider > 0) {
               // calibrate channel B
               rc = CALIBRATE_CHB_10KG;
               next_calibrate_phase = rc;
               error = false;
#endif /* HAS_SCALE_B */
            }
            break;
         case CALIBRATE_CHA_10KG:
            current_calibrate_phase = phase;
            temperature = configs[0]->read_temperature;
            configs[0]->read_temperature = false;
            time = k_uptime_get();
            rc = scale_sample_channel(configs[0]);
            time = k_uptime_get() - time;
            configs[0]->read_temperature = temperature;
            if (!rc) {
               scale_calc_calibration(configs[0], SCALE_CALIBRATION_G, (int)time);
            } else {
               LOG_INF("ADC Scale disable CHA, no sample (%d ms)", (int)time);
            }
#ifdef HAS_SCALE_B
            if (configs[1]->i2c_ok && configs[1]->external_calibration.divider > 0) {
               rc = CALIBRATE_CHB_10KG;
               next_calibrate_phase = rc;
            } else {
               save = true;
            }
            break;
         case CALIBRATE_CHB_10KG:
            current_calibrate_phase = phase;
            temperature = configs[1]->read_temperature;
            configs[1]->read_temperature = false;
            time = k_uptime_get();
            rc = scale_sample_channel(configs[1]);
            time = k_uptime_get() - time;
            temperature = configs[1]->read_temperature;
            if (!rc) {
               scale_calc_calibration(configs[1], SCALE_CALIBRATION_G, (int)time);
            } else {
               LOG_INF("ADC Scale disable CHB, no sample (%d ms)", (int)time);
            }
#endif /* HAS_SCALE_B */
            save = true;
            break;
         case CALIBRATE_DONE:
            if (current_calibrate_phase == CALIBRATE_ZERO) {
               save = true;
            } else if (current_calibrate_phase == CALIBRATE_CHA_10KG) {
               save = true;
            } else {
               stop = true;
            }
            break;
         case CALIBRATE_CMD:
            // prevent warning
            break;
      }
      if (error) {
         scales_set_calibration_error();
         save = true;
      }
      if (save || stop) {
         current_calibrate_phase = CALIBRATE_NONE;
         next_calibrate_phase = CALIBRATE_START;
         rc = CALIBRATE_NONE;
         scales_suspend();
         expansion_port_power(false);
      }
      if (save) {
         for (int channel = 0; channel < max_configs; ++channel) {
            struct scale_config *scale_dev = configs[channel];
            scale_dev->external_calibration.divider = NORMALIZE_DIVIDER(scale_dev->external_calibration.divider);
            scale_save_external_calibration(scale_dev);
            LOG_INF("ADC Scale %s 0x%06x %d", scale_dev->channel_name, scale_dev->external_calibration.offset, scale_dev->external_calibration.divider);
         }

         LOG_INF("ADC Scale calibration saved.");
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
#ifdef HAS_SCALE_B
      } else if (select_mode == CALIBRATE_CHB_10KG) {
         ui_led_op_prio(LED_COLOR_GREEN, LED_BLINKING);
         ui_led_op_prio(LED_COLOR_BLUE, LED_BLINKING);
#endif /* HAS_SCALE_B */
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
#ifdef HAS_SCALE_B
         } else if (select_mode == CALIBRATE_CHB_10KG) {
            LOG_INF("Calibrate CHB 10kg.");
            ui_led_op_prio(LED_COLOR_BLUE, LED_SET);
            ui_led_op_prio(LED_COLOR_GREEN, LED_SET);
            select_mode = scale_calibrate(select_mode);
            ui_led_op_prio(LED_COLOR_BLUE, LED_CLEAR);
            ui_led_op_prio(LED_COLOR_GREEN, LED_CLEAR);
#endif /* HAS_SCALE_B */
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

int scale_sample_desc(char *buf, size_t len)
{
   double scaleA = 0;
   double scaleB = 0;
   double temperatureA = 0;
   double temperatureB = 0;
   int64_t time = 0;
   int index = 0;
   int start = 0;
   int res = 0;
   int res2 = 0;

   res = scale_sample(&scaleA, &scaleB, &temperatureA, &temperatureB);
   if (0 < res) {
      index += snprintf(buf, len, "Last calibration: ");
      time = configs[0]->external_calibration.time;
      if (time) {
         res2 = 1;
         index += snprintf(buf + index, len - index, "A ");
         index += appl_format_time(time, buf + index, len - index);
         if (configs[0]->external_calibration.divider == 0) {
            index += snprintf(buf + index, len - index, " (disabled)");
         } else if (configs[0]->external_calibration.divider == 100 && configs[0]->external_calibration.offset == 0) {
            index += snprintf(buf + index, len - index, " (dummy)");
         }
      }
#ifdef HAS_SCALE_B
      time = configs[1]->external_calibration.time;
      if (time) {
         if (res2) {
            index += snprintf(buf + index, len - index, ", ");
         } else {
            res2 = 1;
         }
         index += snprintf(buf + index, len - index, "B ");
         index += appl_format_time(time, buf + index, len - index);
         if (configs[1]->external_calibration.divider == 0) {
            index += snprintf(buf + index, len - index, " (disabled)");
         } else if (configs[1]->external_calibration.divider == 100 && configs[1]->external_calibration.offset == 0) {
            index += snprintf(buf + index, len - index, " (dummy)");
         }
      }
#endif /* HAS_SCALE_B */
      if (res2) {
         LOG_INF("%s", buf);
         buf[index++] = '\n';
         start = index;
      } else {
         index = 0;
      }

      if (res & 1) {
         index += snprintf(buf + index, len - index, "CHA %.2f kg, %.1f°C", scaleA, temperatureA);
      }
#ifdef HAS_SCALE_B
      if (index > start) {
         index += snprintf(buf + index, len - index, ", ");
      }
      if (res & 2) {
         index += snprintf(buf + index, len - index, "CHB %.2f kg, %.1f°C", scaleB, temperatureB);
      }
#endif /* HAS_SCALE_B */
      LOG_INF("%s", &buf[start]);
#ifdef CONFIG_NAU7802_DUMMY_CALIBRATION
#ifdef HAS_SCALE_B
      if (configs[0]->weight != NAU7802_NONE_ADC_VALUE || configs[1]->weight != NAU7802_NONE_ADC_VALUE) {
#else  /* HAS_SCALE_B */
      if (configs[0]->weight != NAU7802_NONE_ADC_VALUE) {
#endif /* HAS_SCALE_B */
         buf[index++] = '\n';
         start = index;
         if (configs[0]->weight != NAU7802_NONE_ADC_VALUE) {
            index += snprintf(buf + index, len - index, "CHA %d/%d/%d raw, %.1f°C",
                              configs[0]->weight, configs[0]->internal_offset, configs[0]->external_calibration.divider, temperatureA);
         }
#ifdef HAS_SCALE_B
         if (index > start) {
            index += snprintf(buf + index, len - index, ", ");
         }
         if (configs[1]->weight != NAU7802_NONE_ADC_VALUE) {
            index += snprintf(buf + index, len - index, "CHB %d/%d/%d raw, %.1f°C",
                              configs[1]->weight, configs[1]->internal_offset, configs[1]->external_calibration.divider, temperatureB);
         }
#endif /* HAS_SCALE_B */
         LOG_INF("%s", &buf[start]);
      }
#endif /* CONFIG_NAU7802_DUMMY_CALIBRATION */
   }
   return index;
}

#ifdef CONFIG_SH_CMD

static int sh_cmd_scale(const char *parameter)
{
   (void)parameter;

   char buf[300];
   scale_sample_desc(buf, sizeof(buf));
   return 0;
}

static void scale_dump_calibration(struct scale_config *scale_dev)
{
   if (!scale_dev) {
      return;
   }
   if (CALIBRATE_CMD == current_calibrate_phase) {
      LOG_INF("ADC %s manual calibration pending.", scale_dev->channel_name);
   } else if (CALIBRATE_NONE != current_calibrate_phase) {
      LOG_INF("ADC %s calibration pending.", scale_dev->channel_name);
   }
   if (!scale_dev->external_calibration.time) {
      LOG_INF("ADC %s calibration missing.", scale_dev->channel_name);
      return;
   } else {
      int32_t offset = scale_dev->external_calibration.offset;
      int32_t divider = scale_dev->external_calibration.divider;
      int32_t temperature = scale_dev->external_calibration.calibration_temperature;
      uint16_t avref = scale_dev->external_calibration.avref;
      char buf[32];
      appl_format_time(scale_dev->external_calibration.time, buf, sizeof(buf));
      LOG_INF("ADC %s calibration  %s", scale_dev->channel_name, buf);
      if (scale_dev->external_calibration.avref == avref) {
         LOG_INF("ADC %s aref         %7.1f V", scale_dev->channel_name, ((double)avref) / 1000);
      } else {
         LOG_INF("ADC %s aref         %7.1f V (system %.1f V)", scale_dev->channel_name,
                 ((double)avref) / 1000, ((double)scale_dev->external_calibration.avref) / 1000);
      }
      LOG_INF("ADC %s offset:      %7d", scale_dev->channel_name, offset);
      LOG_INF("ADC %s divider:     %7d%s", scale_dev->channel_name, divider, divider == DUMMY_ADC_DIVIDER ? " (dummy)" : "");
      LOG_INF("ADC %s temperature: %7.1f", scale_dev->channel_name, TEMPERATURE_DOUBLE(temperature));
   }
}

static int scale_start_calibration(struct scale_config *scale_dev)
{
   int res = -EBUSY;
   k_mutex_lock(&scale_mutex, K_FOREVER);
   if (CALIBRATE_NONE == current_calibrate_phase) {
      current_calibrate_phase = CALIBRATE_CMD;
      scale_prepare_calibration(scale_dev);
      expansion_port_power(true);
   }
   if (CALIBRATE_CMD == current_calibrate_phase) {
      res = 0;
   } else {
      LOG_INF("ADC %s busy.", scale_dev->channel_name);
   }
   k_mutex_unlock(&scale_mutex);
   return res;
}

static int scale_finish_calibration(struct scale_config *scale_dev, bool save)
{
   int res = -EINVAL;
   k_mutex_lock(&scale_mutex, K_FOREVER);
   if (CALIBRATE_CMD == current_calibrate_phase) {
      if (save) {
         scale_save_external_calibration(scale_dev);
         LOG_INF("ADC %s calibration saved.", scale_dev->channel_name);
      } else {
         scale_read_external_calibration(scale_dev);
         LOG_INF("ADC %s calibration canceled.", scale_dev->channel_name);
      }
      current_calibrate_phase = CALIBRATE_NONE;
      expansion_port_power(false);
      res = 0;
   } else {
      LOG_INF("ADC %s no calibration pending.", scale_dev->channel_name);
   }
   k_mutex_unlock(&scale_mutex);
   return res;
}

static int scale_sample_calibration(struct scale_config *scale_dev,
                                    enum calibrate_phase phase, int reference, const char *msg)
{
   int res = 0;
   int64_t time;

   k_mutex_lock(&scale_mutex, K_FOREVER);
   if (CALIBRATE_CMD != current_calibrate_phase) {
      LOG_INF("ADC %s busy.", scale_dev->channel_name);
   } else {
      current_calibrate_phase = phase;
      scale_check_channel(scale_dev);
      if (scale_dev->i2c_ok) {
         LOG_INF("ADC %s calibrate %s.", scale_dev->channel_name, msg);
         if (CALIBRATE_CHA_10KG == phase) {
            scale_dev->external_calibration.divider = 1;
            scale_dev->read_temperature = false;
         }
         time = k_uptime_get();
         res = scale_sample_channel(scale_dev);
         time = k_uptime_get() - time;
         if (CALIBRATE_CHA_10KG == phase) {
            scale_dev->read_temperature = true;
         }
         if (res) {
            // failure
            res = 0;
            LOG_INF("ADC %s calibrate %s failed.", scale_dev->channel_name, msg);
         } else {
            if (CALIBRATE_CHA_10KG == phase) {
               scale_calc_calibration(scale_dev, reference, (int)time);
            } else {
               scale_dev->external_calibration.offset = scale_dev->weight;
               scale_dev->external_calibration.calibration_temperature = scale_dev->temperature;
            }
            res = 1;
         }
      } else {
         LOG_INF("ADC %s missing.", scale_dev->channel_name);
      }
      current_calibrate_phase = CALIBRATE_CMD;
   }
   k_mutex_unlock(&scale_mutex);

   return res;
}

static int scale_set_calibration_value(struct scale_config *scale_dev, int32_t *value, bool has_value, int32_t new_value, const char *name)
{
   if (!has_value) {
      LOG_INF("ADC %s: missing %s value", scale_dev->channel_name, name);
      return -EINVAL;
   } else {
      int res = scale_start_calibration(scale_dev);
      if (!res) {
         *value = new_value;
         LOG_INF("ADC %s calibration %s: %7d", scale_dev->channel_name, name, new_value);
      }
      return res;
   }
}

static int scale_set_calibration(struct scale_config *scale_dev, const char *parameter)
{
   int res = 0;
   const char *cur = parameter;
   char name[10];

   memset(name, 0, sizeof(name));
   cur = parse_next_text(cur, ' ', name, sizeof(name));
   if (!name[0]) {
      scale_dump_calibration(scale_dev);
   } else {
      char value[10];
      char *t = value;
      int32_t num_value = 0;
      bool has_value = false;
      memset(value, 0, sizeof(value));
      cur = parse_next_text(cur, ' ', value, sizeof(value));
      if (value[0]) {
         num_value = (int32_t)strtol(value, &t, 10);
         has_value = !*t;
      }
      if (!stricmp("off", name)) {
         res = scale_set_calibration_value(scale_dev, &scale_dev->external_calibration.offset, has_value, num_value, "offset");
      } else if (!stricmp("div", name)) {
         res = scale_set_calibration_value(scale_dev, &scale_dev->external_calibration.divider, has_value, num_value, "divider");
      } else if (!stricmp("temp", name)) {
         res = scale_set_calibration_value(scale_dev, &scale_dev->external_calibration.calibration_temperature, has_value, num_value, "temperature");
         if (!res) {
            LOG_INF("ADC %s calibration temperature %7.1f", scale_dev->channel_name,
                    TEMPERATURE_DOUBLE(num_value));
         }
#ifdef CONFIG_NAU7802_DUMMY_CALIBRATION
      } else if (!stricmp("dummy", name)) {
         res = scale_start_calibration(scale_dev);
         if (!res) {
            scale_dev->external_calibration.offset = 0;
            scale_dev->external_calibration.calibration_temperature = 0;
            scale_dev->external_calibration.divider = DUMMY_ADC_DIVIDER;
            LOG_INF("ADC %s dummy calibration.", scale_dev->channel_name);
            res = scale_finish_calibration(scale_dev, true);
         }
#endif /* CONFIG_NAU7802_DUMMY_CALIBRATION */
      } else if (!stricmp("zero", name)) {
         res = scale_start_calibration(scale_dev);
         if (!res && scale_sample_calibration(scale_dev, CALIBRATE_ZERO, 0, "zero")) {
            num_value = scale_dev->external_calibration.offset;
            LOG_INF("ADC %s calibration offset: %7d", scale_dev->channel_name, num_value);
         }
      } else if (!stricmp("done", name)) {
         res = scale_finish_calibration(scale_dev, true);
      } else if (!stricmp("cancel", name)) {
         res = scale_finish_calibration(scale_dev, false);
      } else {
         if (!stricmp("ref", name)) {
            num_value = SCALE_CALIBRATION_G;
            t = name + 3;
         } else {
            num_value = (int32_t)strtol(name, &t, 10);
         }
         if (!*t) {
            res = scale_start_calibration(scale_dev);
            if (!res && scale_sample_calibration(scale_dev, CALIBRATE_CHA_10KG, num_value, "divider")) {
               num_value = scale_dev->external_calibration.divider;
               LOG_INF("ADC %s calibration divider: %7d%s", scale_dev->channel_name,
                       num_value, num_value == DUMMY_ADC_DIVIDER ? " (dummy)" : "");
            }
         } else {
            LOG_INF("ADC %s: missing reference value", scale_dev->channel_name);
            res = -EINVAL;
         }
      }
   }
   return res;
}

static int sh_cmd_scale_calibration(const char *parameter)
{
   const char *cur = parameter;
   char value[10];

   memset(value, 0, sizeof(value));
   cur = parse_next_text(cur, ' ', value, sizeof(value));
   if (!value[0]) {
      for (int channel = 0; channel < max_configs; ++channel) {
         scale_dump_calibration(configs[channel]);
      }
   } else {
      for (int channel = 0; channel < max_configs; ++channel) {
         if (!stricmp(configs[channel]->channel_name, value)) {
            return scale_set_calibration(configs[channel], cur);
         }
      }
      if (max_configs == 1) {
         return scale_set_calibration(configs[0], parameter);
      } else {
         LOG_INF("Channel %s not available!", value);
      }
   }
   return 0;
}

static void sh_cmd_scale_calibration_help(void)
{
   LOG_INF("> help scalecal:");
   LOG_INF("  scalecal              : show calibration data of scales.");
   LOG_INF("  scalecal [CHA|CHB]    : show calibration data of scale A or B.");
   LOG_INF("  scalecal [CHA|CHB] [off|div|temp] <value>:");
   LOG_INF("           off <value>  : set calibration offset.");
   LOG_INF("           div <value>  : set calibration divider.");
   LOG_INF("           temp <value> : set calibration temperature.");
   LOG_INF("  scalecal [CHA|CHB] [zero|<reference>|done|cancel]:");
   LOG_INF("           zero         : measure 0 to calibrate offset.");
   LOG_INF("           <reference>  : measure <reference> [g] to calibrate divider.");
   LOG_INF("           done         : save calibration.");
   LOG_INF("           cancel       : cancel calibration.");
   LOG_INF("           [CHA|CHB] : may be omitted, if only one scale is defined.");
#ifdef CONFIG_NAU7802_DUMMY_CALIBRATION
   LOG_INF("  scalecal [CHA|CHB] dummy : set dummy calibration and save it.");
#endif /* CONFIG_NAU7802_DUMMY_CALIBRATION */
}

SH_CMD(scale, NULL, "read scale info.", sh_cmd_scale, NULL, 0);
SH_CMD(scalecal, NULL, "scale calibration.", sh_cmd_scale_calibration, sh_cmd_scale_calibration_help, 0);
#endif /* CONFIG_SH_CMD */