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
#include <zephyr/random/random.h>
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
#define NAU7802_GCAL1 0x06
#define NAU7802_OCAL2 0x0A
#define NAU7802_GCAL2 0x0D
#define NAU7802_I2C 0x11
#define NAU7802_ADC 0x12
#define NAU7802_ADC_CTRL 0x15
#define NAU7802_PGA_CTRL 0x1B
#define NAU7802_POWER_CTRL 0x1C

#define NAU7802_MAX_ADC_VALUE 0x7ffffd
#define NAU7802_NONE_ADC_VALUE 0xff800000

// 10 g resolution
#define SCALE_RESOLUTION_G 10
// 10 kg calibration
#define SCALE_CALIBRATION_G 10000

// dither & loops for calibration and temperature
#define MAX_INTERNAL_LOOPS 12
#define MIN_INTERNAL_ADC_SAMPLES 4
// enable to calibrate also after samples
// #define POST_INTERNAL_ADC_SAMPLES

#define MAX_TEMPERATURE_DITHER 32
#define MAX_TEMPERATURE_LOOPS 4
#define MIN_TEMPERATURE_ADC_SAMPLES 2

#define MAX_ADC_LOOPS 12
#define MIN_ADC_SAMPLES 4

#define DUMMY_ADC_DIVIDER 1000

#define NORMALIZE_DIVIDER(DIV, MIN) (((DIV) >= (MIN) || (DIV) == DUMMY_ADC_DIVIDER) ? (DIV) : 0)

#define DIV_ROUNDED(N, D) (((N) + (D) / 2) / (D))

#define TEMPERATURE_DOUBLE(T) (((double)T) / 1000.0)

#if DT_NODE_HAS_STATUS(DT_ALIAS(scale_a), okay)
#define HAS_SCALE_A
#if DT_NODE_HAS_STATUS(DT_ALIAS(scale_b), okay)
#define HAS_SCALE_B
#if (DT_REG_ADDR(DT_BUS(DT_ALIAS(scale_a))) == DT_REG_ADDR(DT_BUS(DT_ALIAS(scale_b))))
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

#define CALIBRATION_STORAGE_VERSION 3

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
        .desc = "setup A",
        .is_flash_device = false,
        .id = CALIBRATION_A_ID,
        .magic = 0x03400560,
        .version = CALIBRATION_STORAGE_VERSION,
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
        .desc = "setup B",
        .is_flash_device = false,
        .id = CALIBRATION_B_ID,
        .magic = 0x03400560,
        .version = CALIBRATION_STORAGE_VERSION,
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

enum adc_channel {
   ADC_CHANNEL_1,
   ADC_CHANNEL_TEMPERATURE,
};

struct scale_setup {
   int64_t time;
   int32_t offset;
   int32_t divider;
   int32_t calibration_temperature;
   uint16_t int_avref;
   uint16_t ext_avref;
   uint16_t max_dither;
   uint16_t min_divider;
   uint8_t gain;
   bool int_osc;
   bool pga_cap;
   bool read_temp;
};

struct scale_config {
   const char *channel_name;
   const struct storage_config *storage_config;
   const struct device *i2c_device;
   const struct scale_setup *default_setup;
   struct scale_setup setup;
   bool setup_init;
   bool i2c_ok;
   bool read_temperature;
   enum avdd_source source;
   int32_t raw;
   int32_t internal_offset;
   int32_t weight;
   int32_t temperature;
   int32_t gcal2_marker;
   int64_t last_adc_time;
};

#ifdef HAS_SCALE_A

#define INIT_SCALE_SETUP(N) {                  \
    .int_avref = DT_PROP(N, avref_mv),         \
    .ext_avref = DT_PROP(N, ext_avref_mv),     \
    .gain = DT_PROP(N, gain),                  \
    .int_osc = DT_PROP(N, internal_oscilator), \
    .pga_cap = DT_PROP(N, pga_cap),            \
    .read_temp = DT_PROP(N, read_temperature), \
    .max_dither = DT_PROP(N, max_dither),      \
    .min_divider = DT_PROP(N, min_divider),    \
    .offset = 0,                               \
    .divider = 0,                              \
    .calibration_temperature = 0,              \
}

static const struct scale_setup setup_a = INIT_SCALE_SETUP(DT_ALIAS(scale_a));

static struct scale_config config_a =
    {
        .channel_name = "CHA",
#ifdef HAS_EEPROM_A
        .storage_config = &calibration_storage_config_a,
#else
        .storage_config = NULL,
#endif
        .i2c_device = DEVICE_DT_GET_OR_NULL(DT_BUS(DT_ALIAS(scale_a))),
        .default_setup = &setup_a,
        .setup_init = false,
        .i2c_ok = false,
        .read_temperature = false,
        .source = AVDD_UNKNOWN,
        .internal_offset = NAU7802_NONE_ADC_VALUE,
        .raw = NAU7802_NONE_ADC_VALUE,
        .weight = NAU7802_NONE_ADC_VALUE,
        .temperature = NAU7802_NONE_ADC_VALUE,
};

#endif /* HAS_SCALE_A */

#ifdef HAS_SCALE_B

static const struct scale_setup setup_b = INIT_SCALE_SETUP(DT_ALIAS(scale_b));

static struct scale_config config_b =
    {
        .channel_name = "CHB",
#ifdef HAS_EEPROM_B
        .storage_config = &calibration_storage_config_b,
#else
        .storage_config = NULL,
#endif
        .i2c_device = DEVICE_DT_GET_OR_NULL(DT_BUS(DT_ALIAS(scale_b))),
        .default_setup = &setup_b,
        .setup_init = false,
        .i2c_ok = false,
        .read_temperature = false,
        .source = AVDD_UNKNOWN,
        .internal_offset = NAU7802_NONE_ADC_VALUE,
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

static inline bool scale_use_pga_capacitor(const struct scale_config *scale_dev)
{
   return scale_dev->setup.pga_cap;
}

static inline int32_t expand_sign_24(int32_t value)
{
   return (value << 8) >> 8;
}

static inline uint8_t scale_gain_reg(uint8_t gain)
{
   uint8_t reg = 0;
   uint8_t value = 1;

   while (gain > value) {
      ++reg;
      value <<= 1;
   }
   return reg;
}

static inline uint8_t scale_gain_factor(uint8_t id)
{
   uint8_t value = 1;

   while (id > 0) {
      --id;
      value <<= 1;
   }
   return value;
}

// 1.5.2 Noise performance, 3.3V
// 1   :   19.09
// 2   :   18.96
// 4   :   19.00
// 8   :   18.64
// 16  :   18.19
// 32  :   17.85
// 64  :   17.23
// 128 :   16.53
//                              0  1  2   4   8  16  32  64  128
const uint16_t gain_dither[] = {8, 8, 8, 12, 16, 32, 48, 64, 256};

static inline uint16_t scale_gain_dither(struct scale_config *scale_dev)
{
   uint8_t gain = scale_dev->setup.gain;
   if (gain > 0) {
      gain = scale_gain_reg(gain) + 1;
   }
   return gain_dither[gain];
}

#ifdef CONFIG_NAU7802_SCALE_ON_EXPANSION_BOARD
static int scale_expansion_port_power(bool enable)
{
   int rc = 0;
   if (!enable) {
      k_sleep(K_MSEC(10));
   }
   rc = expansion_port_power(enable);
   if (enable) {
      k_sleep(K_MSEC(100));
   }
   return rc;
}
#else /* CONFIG_NAU7802_SCALE_ON_EXPANSION_BOARD */

static inline int scale_expansion_port_power(bool enable)
{
   (void)enable;
   return 0;
}

#endif /* CONFIG_NAU7802_SCALE_ON_EXPANSION_BOARD */

static void scale_dump_calibration(struct scale_config *scale_dev)
{
   int32_t divider = 0;
   struct scale_setup *setup = NULL;

   if (!scale_dev) {
      return;
   }
   if (CALIBRATE_CMD == current_calibrate_phase) {
      LOG_INF("ADC %s manual calibration pending.", scale_dev->channel_name);
   } else if (CALIBRATE_NONE != current_calibrate_phase) {
      LOG_INF("ADC %s calibration pending.", scale_dev->channel_name);
   }

   setup = &scale_dev->setup;
   divider = setup->divider;

   if (!setup->time) {
      LOG_INF("ADC %s calibration missing.", scale_dev->channel_name);
   } else {
      char buf[32];
      appl_format_time(setup->time, buf, sizeof(buf));
      LOG_INF("ADC %s calibration %s", scale_dev->channel_name, buf);
   }
   LOG_INF("ADC %s offset:      %7d", scale_dev->channel_name, setup->offset);
   LOG_INF("ADC %s divider:     %7d%s", scale_dev->channel_name, divider, divider == DUMMY_ADC_DIVIDER ? " (dummy)" : "");
   LOG_INF("ADC %s temperature: %7.1f C", scale_dev->channel_name, TEMPERATURE_DOUBLE(setup->calibration_temperature));
   LOG_INF("ADC %s gain         %7u x", scale_dev->channel_name, setup->gain);
   LOG_INF("ADC %s int. avref:  %7.1f V", scale_dev->channel_name, ((double)setup->int_avref) / 1000);
   LOG_INF("ADC %s ext. avref:  %7.1f V", scale_dev->channel_name, ((double)setup->ext_avref) / 1000);
   LOG_INF("ADC %s avref:       %7s", scale_dev->channel_name, AVDD_DESCRIPTION[scale_dev->source]);
   LOG_INF("ADC %s max. dither  %7u", scale_dev->channel_name, setup->max_dither);
   LOG_INF("ADC %s min. divider %7u", scale_dev->channel_name, setup->min_divider);
   LOG_INF("ADC %s int. osc.:   %7s", scale_dev->channel_name, setup->int_osc ? "yes" : "no");
   LOG_INF("ADC %s int. temp.:  %7s", scale_dev->channel_name, setup->read_temp ? "yes" : "no");
   LOG_INF("ADC %s PGA cap.:    %7s", scale_dev->channel_name, setup->pga_cap ? "yes" : "no");
   LOG_INF("ADC %s int. off.:   %7d", scale_dev->channel_name, scale_dev->internal_offset);
}

static void scale_save_setup(struct scale_config *scale_dev)
{
   int rc = 0;
   uint8_t calibration[CALIBRATE_VALUE_SIZE];
   uint8_t *cal = calibration;

   memset(calibration, 0, sizeof(calibration));
   if (!scale_dev->storage_config) {
      *cal++ = CALIBRATION_STORAGE_VERSION;
   }
   sys_put_be24(scale_dev->setup.offset, cal);
   cal += 3;
   sys_put_be24(scale_dev->setup.divider, cal);
   cal += 3;
   sys_put_be24(scale_dev->setup.calibration_temperature, cal);
   cal += 3;
   sys_put_be16(scale_dev->setup.int_avref, cal);
   cal += 2;
   sys_put_be16(scale_dev->setup.ext_avref, cal);
   cal += 2;
   sys_put_be16(scale_dev->setup.max_dither, cal);
   cal += 2;
   sys_put_be16(scale_dev->setup.min_divider, cal);
   cal += 2;
   *cal++ = scale_dev->setup.gain;
   *cal++ = scale_dev->setup.int_osc ? 1 : 0;
   *cal++ = scale_dev->setup.read_temp ? 1 : 0;
   *cal++ = scale_dev->setup.pga_cap ? 1 : 0;
   if (scale_dev->storage_config) {
      rc = appl_storage_write_bytes_item(scale_dev->storage_config->id, calibration, sizeof(calibration));
   } else {
      rc = appl_settings_set_bytes(scale_dev->channel_name, calibration, sizeof(calibration));
   }
   if (rc) {
      LOG_INF("ADC %s saving setup failed, %d (%s).", scale_dev->channel_name, rc, strerror(-rc));
   } else {
      appl_get_now(&scale_dev->setup.time);
      LOG_INF("ADC %s setup saved.", scale_dev->channel_name);
      scale_dump_calibration(scale_dev);
   }
}

static void scale_load_setup(struct scale_config *scale_dev)
{
   int rc = 0;
   int64_t time = 0;
   uint8_t calibration[CALIBRATE_VALUE_SIZE];
   uint8_t *cal = calibration;

   memset(calibration, 0, sizeof(calibration));
   if (scale_dev->storage_config) {
      rc = appl_storage_read_bytes_item(scale_dev->storage_config->id, 0,
                                        &time, calibration, sizeof(calibration));
   } else {
      rc = appl_settings_get_bytes(scale_dev->channel_name, &time, calibration, sizeof(calibration));
      if (*cal++ != CALIBRATION_STORAGE_VERSION) {
         rc = 1;
      }
   }
   if (rc == sizeof(calibration)) {
      struct scale_setup *setup = &scale_dev->setup;
      setup->time = time;
      setup->offset = expand_sign_24((int32_t)sys_get_be24(cal));
      cal += 3;
      setup->divider = sys_get_be24(cal);
      cal += 3;
      setup->calibration_temperature = expand_sign_24((int32_t)sys_get_be24(cal));
      cal += 3;
      setup->int_avref = sys_get_be16(cal);
      cal += 2;
      setup->ext_avref = sys_get_be16(cal);
      cal += 2;
      setup->max_dither = sys_get_be16(cal);
      cal += 2;
      setup->min_divider = sys_get_be16(cal);
      cal += 2;
      setup->gain = *cal++;
      setup->int_osc = *cal++ ? true : false;
      setup->read_temp = *cal++ ? true : false;
      setup->pga_cap = *cal++ ? true : false;
      setup->divider = NORMALIZE_DIVIDER(setup->divider, setup->min_divider);
      if (setup->divider > 0) {
         LOG_INF("ADC %s setup 0x%06x, %d, %.1f loaded.", scale_dev->channel_name,
                 setup->offset, setup->divider, TEMPERATURE_DOUBLE(setup->calibration_temperature));
      } else {
         LOG_INF("ADC %s setup disabled.", scale_dev->channel_name);
      }
   } else {
      if (rc < 0) {
         LOG_INF("ADC %s disabled, loading setup failed, %d (%s)", scale_dev->channel_name, rc, strerror(-rc));
      } else if (rc == 1) {
         LOG_INF("ADC %s disabled, setup version %d doesn't match expected %d!", scale_dev->channel_name, *--cal, CALIBRATION_STORAGE_VERSION);
      } else {
         LOG_INF("ADC %s disabled, setup not available.", scale_dev->channel_name);
      }
      scale_dev->setup = *scale_dev->default_setup;
   }
   scale_dev->setup_init = true;
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

static int scale_write_regs(const struct scale_config *scale_dev, uint8_t reg, int32_t val, size_t len)
{
   int rc = 0;
   uint8_t value[4] = {0, 0, 0, 0};

   if (!scale_dev->i2c_ok) {
      return -EINVAL;
   }

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

   // no i2c_ok check, required to check mark for auto detection!

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

static int scale_suspend(const struct scale_config *scale_dev)
{
   if (!scale_dev->i2c_ok) {
      return -EINVAL;
   }
   LOG_INF("ADC %s suspend", scale_dev->channel_name);
   return i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_PU_CTRL, BIT(7) | BIT(2) | BIT(1), 0);
}

static int scale_reset(const struct scale_config *scale_dev)
{
   /* reset */
   int rc = 0;
   if (!scale_dev->i2c_ok) {
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

static int scale_mark(struct scale_config *scale_dev)
{
   if (!scale_dev->i2c_ok) {
      return -EINVAL;
   }
   sys_csrand_get(&scale_dev->gcal2_marker, sizeof(scale_dev->gcal2_marker));
   if (scale_dev->gcal2_marker == 0x800000) {
      // prevent default value
      scale_dev->gcal2_marker--;
   }
   return scale_write_regs(scale_dev, NAU7802_GCAL2, scale_dev->gcal2_marker, sizeof(scale_dev->gcal2_marker));
}

static int scale_check_mark(struct scale_config *scale_dev)
{
   int32_t mark = 0;
   int rc = 0;

   if (!device_is_ready(scale_dev->i2c_device)) {
      scale_dev->i2c_ok = false;
      return -EINVAL;
   }
   rc = scale_read_regs(scale_dev, NAU7802_GCAL2, &mark, sizeof(mark));
   if (rc) {
      // reading internal calibration offset failed
      LOG_INF("ADC %s i2c read marker failed, %d (%s).", scale_dev->channel_name, rc, strerror(-rc));
   } else if (mark == 0x800000) {
      // default value => new sensor
      LOG_INF("ADC %s new sensor.", scale_dev->channel_name);
      rc = 1;
   } else if (scale_dev->gcal2_marker != mark) {
      // different mark => changed sensor
      LOG_INF("ADC %s changed sensor.", scale_dev->channel_name);
      rc = 2;
   } else {
      LOG_INF("ADC %s same sensor.", scale_dev->channel_name);
   }
   scale_dev->i2c_ok = 0 <= rc;
   return rc;
}

static int scale_set_avdd(struct scale_config *scale_dev, bool internal)
{
   int rc = 0;

   if (!scale_dev->i2c_ok) {
      return -EINVAL;
   }

   LOG_INF("ADC %s avdd %s", scale_dev->channel_name, internal ? "int" : "ext");

   if (internal) {
      /* internal AVDD, enable AVDD-LDO */
      int ldo_vref = ((4500 - scale_dev->setup.int_avref) / 300);
      if (ldo_vref < 0) {
         ldo_vref = 0;
      } else if (ldo_vref > 7) {
         ldo_vref = 7;
      }

      /* VLDO 6 => 2.7V, VLDO 7 => 2.4V */
      rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_CTRL1, BIT(3) | BIT(4) | BIT(5), (ldo_vref << 3));
      if (rc) {
         LOG_WRN("ADC %s I2C NAU7802 config VLDO, write failure, %d (%s)!", scale_dev->channel_name, rc, strerror(-rc));
         return rc;
      }

      rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_PU_CTRL, BIT(7), BIT(7));
      if (rc) {
         LOG_WRN("ADC %s I2C NAU7802 enable AVDD-LDO, write failure, %d (%s)!", scale_dev->channel_name, rc, strerror(-rc));
      }
      scale_dev->source = AVDD_INTERNAL;
      return rc;
   } else {
      /* external AVDD, disable AVDD-LDO */
      rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_PU_CTRL, BIT(7), 0);
      if (rc) {
         LOG_WRN("ADC %s I2C NAU7802 disable AVDD-LDO, write failure, %d (%s)!", scale_dev->channel_name, rc, strerror(-rc));
      }
      scale_dev->source = AVDD_EXTERNAL;
      return rc;
   }
}

static int scale_set_gain(struct scale_config *scale_dev, uint8_t gain)
{
   int rc = 0;

   if (!scale_dev->i2c_ok) {
      return -EINVAL;
   }

   rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_POWER_CTRL, BIT(7), scale_use_pga_capacitor(scale_dev) ? BIT(7) : 0);
   if (rc) {
      LOG_WRN("ADC %s I2C NAU7802 set PGA cap., write failure, %d (%s)!", scale_dev->channel_name, rc, strerror(-rc));
      return rc;
   }

   if (gain > 0) {
      uint8_t gain_reg = scale_gain_reg(gain);
      rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_PGA_CTRL, BIT(4) | BIT(5), 0);
      if (rc) {
         LOG_INF("ADC %s enable PGA failed,  %d (%s).", scale_dev->channel_name, rc, strerror(-rc));
         return rc;
      }
      rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_CTRL1, BIT(0) | BIT(1) | BIT(2), gain_reg);
      if (rc) {
         LOG_INF("ADC %s set gain=%d failed,  %d (%s).", scale_dev->channel_name, gain, rc, strerror(-rc));
      }
   } else {
      rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_PGA_CTRL, BIT(4) | BIT(5), BIT(4));
      if (rc) {
         LOG_INF("ADC %s bypass PGA failed,  %d (%s).", scale_dev->channel_name, rc, strerror(-rc));
      }
      rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_CTRL1, BIT(0) | BIT(1) | BIT(2), 0);
      if (rc) {
         LOG_INF("ADC %s set gain=%d failed,  %d (%s).", scale_dev->channel_name, gain, rc, strerror(-rc));
      }
      /* Enable CAP on Vin2 */
   }
   if (!rc) {
      LOG_INF("ADC %s set gain %u", scale_dev->channel_name, gain);
   }

   return rc;
}

static int scale_set_osc(struct scale_config *scale_dev)
{
   /* ext. osc. */
   bool enable = scale_dev->setup.int_osc;
   int rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_PU_CTRL, BIT(6), enable ? 0 : BIT(6));
   if (rc) {
      LOG_WRN("ADC %s I2C NAU7802 %s osc., write failure, %d (%s)!", scale_dev->channel_name, enable ? "int." : "ext.", rc, strerror(-rc));
      return rc;
   }
   LOG_INF("ADC %s I2C NAU7802 %s osc.", scale_dev->channel_name, enable ? "int." : "ext.");
   return rc;
}

static int scale_select_channel(struct scale_config *scale_dev, enum adc_channel channel)
{
   int rc = 0;
   bool temperature = channel == ADC_CHANNEL_TEMPERATURE;
   const char *desc = temperature ? "temperature" : "vin";

   if (!scale_dev->i2c_ok) {
      return -EINVAL;
   }

   rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_I2C, BIT(1), temperature ? BIT(1) : 0);
   if (rc) {
      LOG_WRN("ADC %s I2C NAU7802 select %s failed, %d (%s)!", scale_dev->channel_name, desc, rc, strerror(-rc));
   }

   return rc;
}

static int scale_set_short_inputs(struct scale_config *scale_dev, bool enable)
{
   int rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_I2C, BIT(3), enable ? BIT(3) : 0);
   if (rc) {
      LOG_WRN("ADC %s I2C NAU7802 %s inputs, write failure, %d (%s)!", scale_dev->channel_name,
              enable ? "short" : "open", rc, strerror(-rc));
   }
   return rc;
}

static int scale_set_bandgap_chopper(struct scale_config *scale_dev, bool enable)
{
   /* Enable/disable bandgap Chopper */
   int rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_I2C, BIT(0), enable ? 0 : BIT(0));
   if (rc) {
      LOG_WRN("ADC %s I2C NAU7802 %s bandgap chopper, write failure, %d (%s)!", scale_dev->channel_name, enable ? "enabled" : "disable", rc, strerror(-rc));
      return rc;
   }

   return rc;
}

static int scale_read_adc_value(struct scale_config *scale_dev, int32_t *val, bool log)
{
   int32_t v = 0;
   int rc = 0;

   if (!scale_dev->i2c_ok) {
      return -EINVAL;
   }

   if (scale_dev->last_adc_time) {
      int64_t time = k_uptime_get() - scale_dev->last_adc_time;
      if (time < 100) {
         k_sleep(K_MSEC(100 - time));
      }
   } else {
      k_sleep(K_MSEC(100));
   }
   rc = scale_wait(scale_dev->i2c_device, NAU7802_PU_CTRL, BIT(5), BIT(5), K_MSEC(5), K_MSEC(2000));
   if (rc) {
      LOG_WRN("ADC %s wait failure %d (%s).", scale_dev->channel_name, rc, strerror(-rc));
      scale_dev->last_adc_time = 0;
      return rc;
   }
   scale_dev->last_adc_time = k_uptime_get();
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

static int32_t scale_values_sum(int counter, int32_t *values)
{
   int32_t v = values[0];
   for (int i = 1; i < counter; ++i) {
      v += values[i];
   }
   return v;
}

static int scale_reduce_values(int32_t values[], int count, int32_t max_dither)
{
   int index = count;
   int index2 = 0;
   int32_t v = values[index];
   int32_t max = v;
   int32_t min = v;

   while (index > 0) {
      v = values[--index];
      if (v < min) {
         min = v;
      } else if (v > max) {
         max = v;
      }
      if ((max - min) > max_dither) {
         while (index < count) {
            values[index2++] = values[++index];
         }
         break;
      }
   }

   return index2;
}

static int scale_read_channel_value(struct scale_config *scale_dev, int max_loops, int min_values, int32_t max_dither)
{
   int rc = 0;
   int loops = 1;
   int counter = 0;
   int maxCounter = 1;

   int32_t v = 0;
   int32_t avg = 0;
   int32_t max = 0;
   int32_t min = 0;
   int32_t values[MIN_ADC_SAMPLES];

   scale_dev->raw = NAU7802_NONE_ADC_VALUE;
   scale_dev->last_adc_time = 0;

   if (!scale_dev->i2c_ok) {
      LOG_INF("ADC %s not available", scale_dev->channel_name);
      return -ESTALE;
   }

   rc = scale_start_adc(scale_dev->i2c_device);
   if (rc) {
      LOG_INF("ADC %s start failed,  %d (%s).", scale_dev->channel_name, rc, strerror(-rc));
      return rc;
   }
   LOG_INF("ADC %s started.", scale_dev->channel_name);
   k_sleep(K_MSEC(300)); // +100 ms in scale_read_adc_value

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
      if ((max - min) > max_dither) {
         if (loops < max_loops) {
            LOG_INF("ADC %s raw 0x%06x, %d, diff: %d > %d, loop: %d, instable => retry", scale_dev->channel_name, v & 0xffffff, v, (max - min), max_dither, loops);
            values[counter] = v;
            counter = scale_reduce_values(values, counter, max_dither);
            v = values[0];
            max = v;
            min = v;
            LOG_INF("ADC %s => [0] raw 0x%06x, %d", scale_dev->channel_name, v & 0xffffff, v);
            for (int index = 1; index < counter; ++index) {
               v = values[index];
               LOG_INF("ADC %s => [%d] raw 0x%06x, %d", scale_dev->channel_name, index, v & 0xffffff, v);
               if (v < min) {
                  min = v;
               } else if (v > max) {
                  max = v;
               }
            }
         } else {
            counter = 1;
            values[0] = v;
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
      v = scale_values_sum(counter, values);
      avg = v / counter;
      if (avg < -NAU7802_MAX_ADC_VALUE || avg > NAU7802_MAX_ADC_VALUE) {
         LOG_INF("ADC %s raw 0x%06x, %d, invalid",
                 scale_dev->channel_name, avg & 0xffffff, avg);
         return -EINVAL;
      }
   } else {
      v = 0;
   }

   if (rc < 0) {
      LOG_INF("ADC %s read failed, %d (%s)", scale_dev->channel_name, rc, strerror(-rc));
   } else if (counter < min_values) {
      LOG_INF("ADC %s raw 0x%06x, %d, ++/-- %d (max %d), %d<%d/%d loops, instable",
              scale_dev->channel_name, avg & 0xffffff, avg, (max - min), max_dither, maxCounter, min_values, loops);
      rc = -ESTALE;
   } else {
      scale_dev->raw = v;
      LOG_INF("ADC %s raw 0x%06x, %d, +/- %d <= %d, %d/%d loops",
              scale_dev->channel_name, avg & 0xffffff, avg, (max - min), max_dither, counter, loops);
      rc = 0;
   }
   return rc;
}

static int scale_values_to_doubles(struct scale_config *scale_dev, double *value, double *temperature)
{
   int rc = -ENODATA;
   int32_t div = scale_dev->setup.divider;

   if (!scale_dev->i2c_ok) {
      return rc;
   }
   if (div == 0) {
      LOG_INF("ADC %s => not calibrated.", scale_dev->channel_name);
      return rc;
   }
   if (scale_dev->raw == NAU7802_NONE_ADC_VALUE) {
      LOG_INF("ADC %s => invalid (%s)", scale_dev->channel_name, AVDD_DESCRIPTION[scale_dev->source]);
      return rc;
   }

   if (div > 0) {
      int32_t off = scale_dev->setup.offset;
      int32_t offset_value = scale_dev->weight - off;
      double v = (offset_value * 10000.0) / div; // [g]
      v /= SCALE_RESOLUTION_G;
      v = round(v);
      if (-0.5 < v && v < 0.5) {
         // eliminate "-0.0"
         v = 0.0;
      }
      v *= SCALE_RESOLUTION_G;
      v /= 1000; // [kg]
      if (value) {
         *value = v;
      }
      LOG_INF("ADC %s => off: %d, div: %d (avdd %s)", scale_dev->channel_name, off, div, AVDD_DESCRIPTION[scale_dev->source]);
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

static int scale_read_temperature(struct scale_config *scale_dev, int max_loops, int min_values, int32_t max_dither)
{
   int rc = -ENODATA;
   int64_t val = 0;

   if (!scale_dev->i2c_ok) {
      return rc;
   }

   /* Select temperature */
   rc = scale_select_channel(scale_dev, ADC_CHANNEL_TEMPERATURE);
   if (rc) {
      goto exit_restore;
   }

   /* Select gain 2x */
   rc = scale_set_gain(scale_dev, 2);
   if (rc) {
      goto exit_restore;
   }

   rc = scale_read_channel_value(scale_dev, max_loops, min_values, max_dither);
   if (rc) {
      LOG_INF("ADC %s read temperature failed,  %d (%s).", scale_dev->channel_name, rc, strerror(-rc));
      goto exit_restore;
   }

   // avref [mV] * 1000 => [uV]
   if (AVDD_INTERNAL == scale_dev->source) {
      val = scale_dev->setup.int_avref;
   } else {
      val = scale_dev->setup.ext_avref;
   }
   val = ((scale_dev->raw / min_values) * val * 1000) >> 24;
   LOG_INF("ADC %s temperature %d uV", scale_dev->channel_name, (int32_t)val);
   // datasheet 109mV at 25°C and +390uV/°C
   val = 25000 + (val - 109000) * 1000 / 390;
   LOG_INF("ADC %s temperature %d", scale_dev->channel_name, (int32_t)val);
   scale_dev->temperature = (int32_t)val;

exit_restore:
   /* Unselect temperature */
   scale_select_channel(scale_dev, ADC_CHANNEL_1);
   scale_set_gain(scale_dev, scale_dev->setup.gain);
   return rc;
}

static int scale_read_internal_offset(struct scale_config *scale_dev, int max_loops, int min_values, int32_t max_dither)
{
   int rc = -ENODATA;

   if (!scale_dev->i2c_ok) {
      return rc;
   }

   rc = scale_set_short_inputs(scale_dev, true);
   if (rc) {
      return rc;
   }

   rc = scale_read_channel_value(scale_dev, max_loops, min_values, max_dither);
   if (rc) {
      LOG_INF("ADC %s read internal offset failed,  %d (%s).", scale_dev->channel_name, rc, strerror(-rc));
   } else {
      scale_dev->internal_offset += scale_dev->raw;
      LOG_INF("ADC %s internal offset %d", scale_dev->channel_name, scale_dev->raw / min_values);
   }

   return rc;
}

static void scale_prepare_calibration(struct scale_config *scale_dev)
{
   if (!scale_dev->setup.divider) {
      scale_dev->setup.divider = 1;
   }
   scale_dev->read_temperature = true;
}

static int scale_resume(struct scale_config *scale_dev)
{
   int rc = 0;

   if (!scale_dev->i2c_ok) {
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
   /* short inputs for calibration */
   rc = scale_set_short_inputs(scale_dev, true);
   if (rc) {
      return rc;
   }

   /* PUA */
   rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_PU_CTRL, BIT(2), BIT(2));
   if (rc) {
      LOG_WRN("ADC %s I2C NAU7802 enable PUA, write failure, %d (%s)!", scale_dev->channel_name, rc, strerror(-rc));
      return rc;
   }

   /* oscilator */
   rc = scale_set_osc(scale_dev);
   if (rc) {
      return rc;
   }

   /* gain */
   rc = scale_set_gain(scale_dev, scale_dev->setup.gain);
   if (rc) {
      return rc;
   }

   /* channel */
   rc = scale_select_channel(scale_dev, ADC_CHANNEL_1);
   if (rc) {
      return rc;
   }

   /* Use 5 Ohm ESR cap, select ADC reg 0x15 */
   rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_PGA_CTRL, BIT(6) | BIT(7), 0);
   if (rc) {
      LOG_WRN("ADC %s I2C NAU7802 enable ESR cap., write failure, %d (%s)!", scale_dev->channel_name, rc, strerror(-rc));
      return rc;
   }

   /* Disable ADC Chopper */
   rc = i2c_reg_update_byte(scale_dev->i2c_device, NAU7802_ADDR, NAU7802_ADC_CTRL, 0x3 << 4, 0x3 << 4);
   if (rc) {
      LOG_WRN("ADC %s I2C NAU7802 disable ADC chopper, write failure, %d (%s)!", scale_dev->channel_name, rc, strerror(-rc));
      return rc;
   }

   if (scale_dev->source != AVDD_UNKNOWN) {
      rc = scale_set_avdd(scale_dev, scale_dev->source == AVDD_INTERNAL);
      if (rc) {
         return rc;
      }
   }

   /* Disable bandgap Chopper */
   rc = scale_set_bandgap_chopper(scale_dev, false);
   if (rc) {
      return rc;
   }

   scale_dev->internal_offset = 0;
   rc = scale_read_internal_offset(scale_dev, MAX_INTERNAL_LOOPS, MIN_INTERNAL_ADC_SAMPLES, scale_gain_dither(scale_dev));

   return rc;
}

static int scale_init_channel(struct scale_config *scale_dev)
{
   int rc = -ENOTSUP;
   const struct device *i2c_dev = scale_dev->i2c_device;

   if (!i2c_dev) {
      LOG_INF("ADC %s not configured", scale_dev->channel_name);
      return -rc;
   }

   if (!device_is_ready(i2c_dev)) {
      LOG_WRN("ADC %s I2C %s not ready", scale_dev->channel_name, i2c_dev->name);
      return rc;
   }

   LOG_INF("ADC %s initialize", scale_dev->channel_name);

   if (scale_dev->storage_config) {
      rc = appl_storage_add(scale_dev->storage_config);
      if (rc) {
         LOG_WRN("ADC %s missing setup EEPROM", scale_dev->channel_name);
         return rc;
      }
   }

   scale_load_setup(scale_dev);
   if (CALIBRATE_NONE != current_calibrate_phase) {
      scale_prepare_calibration(scale_dev);
   }

   /* reset */
   rc = scale_reset(scale_dev);
   if (rc) {
      return rc;
   }

   if (scale_dev->setup.ext_avref > 0) {
      scale_dev->source = AVDD_EXTERNAL;
   } else {
      scale_dev->source = AVDD_INTERNAL;
   }

   rc = scale_resume(scale_dev);

   if (rc == -EAGAIN && !scale_dev->setup.int_osc) {
      scale_dev->setup.int_osc = true;
      scale_set_osc(scale_dev);
      scale_dev->internal_offset = 0;
      rc = scale_read_internal_offset(scale_dev, MAX_INTERNAL_LOOPS, MIN_INTERNAL_ADC_SAMPLES, scale_gain_dither(scale_dev));
   }

   if (rc && rc != -EINVAL && rc != -ESTALE) {
      return rc;
   }

   if (scale_dev->setup.ext_avref > 0) {
      if (rc == -EINVAL) {
         LOG_INF("ADC %s I2C NAU7802 ext. AVDD: invalid", scale_dev->channel_name);
      } else if (!rc) {
         LOG_INF("ADC %s I2C NAU7802 ext. AVDD: %d", scale_dev->channel_name, scale_dev->raw);
      }

      if (rc == -EINVAL) {
         /* no external AVDD, enable internal AVDD-LDO */
         rc = scale_set_avdd(scale_dev, true);
         if (rc) {
            return rc;
         }
         scale_dev->internal_offset = 0;
         rc = scale_read_internal_offset(scale_dev, MAX_INTERNAL_LOOPS, MIN_INTERNAL_ADC_SAMPLES, scale_gain_dither(scale_dev));
         if (rc) {
            return rc;
         }
         LOG_INF("ADC %s I2C NAU7802 int. AVDD: %d", scale_dev->channel_name, scale_dev->raw);
      }
   }
   scale_mark(scale_dev);

   return 0;
}

static int scale_restart_channel(struct scale_config *scale_dev)
{
   int rc;

   if (!device_is_ready(scale_dev->i2c_device)) {
      return -ENOTSUP;
   }

   scale_dev->internal_offset = NAU7802_NONE_ADC_VALUE;
   scale_dev->weight = NAU7802_NONE_ADC_VALUE;
   scale_dev->temperature = NAU7802_NONE_ADC_VALUE;

   rc = scale_check_mark(scale_dev);
   if (rc < 0) {
      // no i2c device
      return rc;
   }

   if (rc > 0) {
      // new sensor
      rc = scale_init_channel(scale_dev);
   } else {
      if (scale_dev->setup.divider == 0) {
         LOG_INF("ADC %s disabled, divider 0.", scale_dev->channel_name);
         scale_suspend(scale_dev);
         return -ENODATA;
      } else {
         rc = scale_resume(scale_dev);
      }
   }
   if (rc) {
      // error
      scale_suspend(scale_dev);
      scale_dev->i2c_ok = false;
      scale_dev->raw = NAU7802_NONE_ADC_VALUE;
   }
   return rc;
}

static int scale_sample_channel(struct scale_config *scale_dev)
{
   enum calibrate_phase phase = current_calibrate_phase;
   int rc = scale_restart_channel(scale_dev);
   if (!rc) {
      LOG_INF("ADC %s scale start.", scale_dev->channel_name);
#ifdef HAS_SCALE_B
      if (CALIBRATE_ZERO == phase || CALIBRATE_CHA_10KG == phase || CALIBRATE_CHB_10KG == phase || CALIBRATE_NONE == phase) {
#else  /* HAS_SCALE_B */
      if (CALIBRATE_ZERO == phase || CALIBRATE_CHA_10KG == phase || CALIBRATE_NONE == phase) {
#endif /* HAS_SCALE_B */
         /* open inputs */
         scale_set_short_inputs(scale_dev, false);
         rc = scale_read_channel_value(scale_dev, MAX_ADC_LOOPS, MIN_ADC_SAMPLES, scale_dev->setup.max_dither);
         if (!rc) {
            scale_dev->weight = scale_dev->raw;
         }

         if (!rc) {
#if ((MIN_ADC_SAMPLES - MIN_INTERNAL_ADC_SAMPLES) > 0)
            rc = scale_read_internal_offset(scale_dev, MAX_INTERNAL_LOOPS, MIN_ADC_SAMPLES - MIN_INTERNAL_ADC_SAMPLES, scale_gain_dither(scale_dev));
#elif defined(POST_INTERNAL_ADC_SAMPLES)
            rc = scale_read_internal_offset(scale_dev, MAX_INTERNAL_LOOPS, MIN_INTERNAL_ADC_SAMPLES, scale_gain_dither(scale_dev));
            if (!rc) {
               scale_dev->internal_offset /= (MIN_INTERNAL_ADC_SAMPLES * 2) / MIN_ADC_SAMPLES;
            }
#endif
            if (!rc) {
               scale_dev->weight -= scale_dev->internal_offset;
               scale_dev->internal_offset /= MIN_ADC_SAMPLES;
            }
            scale_set_short_inputs(scale_dev, false);
            scale_dev->weight /= MIN_ADC_SAMPLES;
         }

         if (!rc && scale_dev->read_temperature) {
            rc = scale_read_temperature(scale_dev, MAX_TEMPERATURE_LOOPS, MIN_TEMPERATURE_ADC_SAMPLES, MAX_TEMPERATURE_DITHER);
         }

         if (rc) {
            LOG_INF("ADC %s scale channel not ready %d.", scale_dev->channel_name, rc);
         } else if (scale_dev->raw == NAU7802_NONE_ADC_VALUE) {
            LOG_INF("ADC %s => invalid (%s)", scale_dev->channel_name, AVDD_DESCRIPTION[scale_dev->source]);
            rc = -ENODATA;
         }
      }
      scale_suspend(scale_dev);
   } else if (scale_dev->i2c_ok && scale_dev->setup.divider == 0) {
      LOG_INF("ADC %s scale channel not setup.", scale_dev->channel_name);
   } else {
      LOG_INF("ADC %s scale channel not available.", scale_dev->channel_name);
   }
   return rc;
}

#ifdef HAS_SCALE_B
#ifdef CONFIG_NAU7802_PARALLEL_READ

static K_SEM_DEFINE(scale_ready, 0, 1);

static void scale_check_mark_B_fn(struct k_work *work)
{
   (void)work;
   scale_check_mark(configs[1]);
   k_sem_give(&scale_ready);
}

static void scale_sample_channel_B_fn(struct k_work *work)
{
   (void)work;
   scale_sample_channel(configs[1]);
   k_sem_give(&scale_ready);
}

static K_WORK_DEFINE(scale_check_mark_B_work, scale_check_mark_B_fn);
static K_WORK_DEFINE(scale_sample_channel_B_work, scale_sample_channel_B_fn);

#endif /* CONFIG_NAU7802_PARALLEL_READ */
#endif /* HAS_SCALE_B */

static int scale_init(void)
{
   scale_expansion_port_power(true);
   for (int channel = 0; channel < max_configs; ++channel) {
      struct scale_config *scale_dev = configs[channel];
      int rc = scale_check_mark(scale_dev);
      if (rc < 0) {
         // no i2c device
         continue;
      }
      rc = scale_init_channel(scale_dev);
      scale_suspend(scale_dev);
      if (rc) {
         LOG_INF("ADC %s setup failed, %d (%s).", scale_dev->channel_name, rc, strerror(-rc));
         scale_dev->i2c_ok = false;
      }
   }
   scale_expansion_port_power(false);
   return 0;
}

/* CONFIG_APPLICATION_INIT_PRIORITY + 1 */
SYS_INIT(scale_init, APPLICATION, CONFIG_NAU7802_INIT_PRIORITY);

static void scales_set_read_temperature(void)
{
   for (int channel = 0; channel < max_configs; ++channel) {
      configs[channel]->read_temperature = configs[channel]->setup.read_temp;
   }
}

int scale_sample(double *valueA, double *valueB, double *temperatureA, double *temperatureB)
{
   int rc = -EINPROGRESS;
   int64_t time = k_uptime_get();

   k_mutex_lock(&scale_mutex, K_FOREVER);
   if (CALIBRATE_NONE == current_calibrate_phase) {
      scale_expansion_port_power(true);
      rc = 0;
      scales_set_read_temperature();

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
      scale_expansion_port_power(false);
   }
   k_mutex_unlock(&scale_mutex);
   if (-EINPROGRESS == rc) {
      LOG_INF("ADC scale busy.");
   } else {
      time = k_uptime_get() - time;
      if (rc < 0) {
         LOG_INF("ADC scale samples failed with %d (%s) in %d ms", rc, strerror(-rc), (int)time);
      } else {
         LOG_INF("ADC scale samples %c/%c in %d ms", rc & 1 ? 'A' : '-', rc & 2 ? 'B' : '-', (int)time);
      }
   }
   return rc;
}

static void scale_calc_calibration(struct scale_config *scale_dev, int reference, int time)
{
   int32_t min_adc_divider = scale_dev->setup.min_divider;
   int64_t weight = scale_dev->weight - scale_dev->setup.offset;
   scale_dev->setup.divider = (int32_t)DIV_ROUNDED((weight * 10000 - 1), reference); // ref/10.0kg
   if (scale_dev->setup.divider < min_adc_divider) {
      LOG_INF("ADC %s scale disable %dkg, rel: %d, div: %d < %d (%d ms)", scale_dev->channel_name, reference / 1000, (int32_t)weight, scale_dev->setup.divider, min_adc_divider, time);
      scale_dev->setup.divider = 0;
   } else {
      LOG_INF("ADC %s scale setup %dkg, rel: %d, div: %d (%d ms)", scale_dev->channel_name, reference / 1000, (int32_t)weight, scale_dev->setup.divider, time);
   }
}

#ifdef CONFIG_ADC_SCALE_SETUP

static void scales_load_setup(void)
{
   for (int channel = 0; channel < max_configs; ++channel) {
      scale_load_setup(configs[channel]);
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
      scale_dev->setup.offset = 0;
      scale_dev->setup.calibration_temperature = 0;
      scale_dev->setup.divider = 0;
   }
}

static void scales_suspend(void)
{
   for (int channel = 0; channel < max_configs; ++channel) {
      scale_suspend(configs[channel]);
   }
}

static int scale_calibrate(enum calibrate_phase phase)
{
   int rc = 0;
   int64_t time = k_uptime_get();
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
            scales_load_setup();
            stop = true;
            break;
         case CALIBRATE_START:
            scale_expansion_port_power(true);
            LOG_INF("ADC Scale start calibration.");
            current_calibrate_phase = phase;
#ifdef HAS_SCALE_B
#ifdef CONFIG_NAU7802_PARALLEL_READ
            k_sem_reset(&scale_ready);
            work_submit_to_cmd_queue(&scale_check_mark_B_work);
#endif /* CONFIG_NAU7802_PARALLEL_READ */
#endif /* HAS_SCALE_B */
            scale_check_mark(configs[0]);
#ifdef HAS_SCALE_B
#ifdef CONFIG_NAU7802_PARALLEL_READ
            k_sem_take(&scale_ready, K_MSEC(5000));
            k_work_cancel(&scale_sample_channel_B_work);
#else  /* CONFIG_NAU7802_PARALLEL_READ */
            scale_check_mark(configs[1]);
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
               configs[0]->setup.divider = 0;
               configs[0]->setup.calibration_temperature = 0;
            } else {
               configs[0]->setup.offset = configs[0]->weight;
               configs[0]->setup.calibration_temperature = configs[0]->temperature;
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
               configs[1]->setup.divider = 0;
               configs[1]->setup.calibration_temperature = 0;
            } else {
               configs[1]->setup.offset = configs[1]->weight;
               configs[1]->setup.calibration_temperature = configs[1]->temperature;
            }
#endif /* HAS_SCALE_B */
            time = k_uptime_get() - time;

#ifdef HAS_SCALE_B
            LOG_INF("ADC Scale calibrate 0, CHA 0x%06x/%.1f, CHB 0x%06x/%.1f. (%d ms)",
                    configs[0]->setup.offset, TEMPERATURE_DOUBLE(configs[0]->setup.calibration_temperature),
                    configs[1]->setup.offset, TEMPERATURE_DOUBLE(configs[1]->setup.calibration_temperature), (int)time);
#else  /* HAS_SCALE_B */
            LOG_INF("ADC Scale calibrate 0, CHA 0x%06x/%.1f (%d ms)",
                    configs[0]->setup.offset, TEMPERATURE_DOUBLE(configs[0]->setup.calibration_temperature), (int)time);
#endif /* HAS_SCALE_B */

            if (configs[0]->i2c_ok && configs[0]->setup.divider > 0) {
               // calibrate channel A
               rc = CALIBRATE_CHA_10KG;
               next_calibrate_phase = rc;
               error = false;
#ifdef HAS_SCALE_B
            } else if (configs[1]->i2c_ok && configs[1]->setup.divider > 0) {
               // calibrate channel B
               rc = CALIBRATE_CHB_10KG;
               next_calibrate_phase = rc;
               error = false;
#endif /* HAS_SCALE_B */
            }
            break;
         case CALIBRATE_CHA_10KG:
            current_calibrate_phase = phase;
            time = k_uptime_get();
            rc = scale_sample_channel(configs[0]);
            time = k_uptime_get() - time;
            if (!rc) {
               scale_calc_calibration(configs[0], SCALE_CALIBRATION_G, (int)time);
            } else {
               LOG_INF("ADC Scale disable CHA, no sample (%d ms)", (int)time);
            }
#ifdef HAS_SCALE_B
            if (configs[1]->i2c_ok && configs[1]->setup.divider > 0) {
               rc = CALIBRATE_CHB_10KG;
               next_calibrate_phase = rc;
            } else {
               save = true;
            }
            break;
         case CALIBRATE_CHB_10KG:
            current_calibrate_phase = phase;
            time = k_uptime_get();
            rc = scale_sample_channel(configs[1]);
            time = k_uptime_get() - time;
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
         scale_expansion_port_power(false);
      }
      if (save) {
         for (int channel = 0; channel < max_configs; ++channel) {
            struct scale_config *scale_dev = configs[channel];
            scale_save_setup(scale_dev);
            LOG_INF("ADC Scale %s 0x%06x %d", scale_dev->channel_name, scale_dev->setup.offset, scale_dev->setup.divider);
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
#endif /* CONFIG_ADC_SCALE_SETUP */

bool scale_calibrate_setup(void)
{
   bool request = false;
#ifdef CONFIG_ADC_SCALE_SETUP
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
#endif /* CONFIG_ADC_SCALE_SETUP */

   return request;
}

#define SCALE_INVALID_VALUE -1000000.0

int scale_sample_desc(char *buf, size_t len)
{
   double scaleA = SCALE_INVALID_VALUE;
   double scaleB = SCALE_INVALID_VALUE;
   double temperatureA = SCALE_INVALID_VALUE;
   double temperatureB = SCALE_INVALID_VALUE;
   int64_t time = 0;
   int index = 0;
   int start = 0;
   int res = 0;
   int res2 = 0;

   res = scale_sample(&scaleA, &scaleB, &temperatureA, &temperatureB);
   if (0 < res) {
      index += snprintf(buf, len, "Last calibration: ");
      time = configs[0]->setup.time;
      if (time) {
         res2 = 1;
         index += snprintf(buf + index, len - index, "A ");
         index += appl_format_time(time, buf + index, len - index);
         if (configs[0]->setup.divider == 0) {
            index += snprintf(buf + index, len - index, " (disabled)");
         } else if (configs[0]->setup.divider == 100 && configs[0]->setup.offset == 0) {
            index += snprintf(buf + index, len - index, " (dummy)");
         }
      }
#ifdef HAS_SCALE_B
      time = configs[1]->setup.time;
      if (time) {
         if (res2) {
            index += snprintf(buf + index, len - index, ", ");
         } else {
            res2 = 1;
         }
         index += snprintf(buf + index, len - index, "B ");
         index += appl_format_time(time, buf + index, len - index);
         if (configs[1]->setup.divider == 0) {
            index += snprintf(buf + index, len - index, " (disabled)");
         } else if (configs[1]->setup.divider == 100 && configs[1]->setup.offset == 0) {
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
         index += snprintf(buf + index, len - index, "CHA %.2f kg", scaleA);
         if (SCALE_INVALID_VALUE != temperatureA) {
            index += snprintf(buf + index, len - index, ", %.1f°C", temperatureA);
         }
      }
#ifdef HAS_SCALE_B
      if (index > start) {
         index += snprintf(buf + index, len - index, ", ");
      }
      if (res & 2) {
         index += snprintf(buf + index, len - index, "CHB %.2f kg", scaleB);
         if (SCALE_INVALID_VALUE != temperatureB) {
            index += snprintf(buf + index, len - index, ", %.1f°C", temperatureB);
         }
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
            index += snprintf(buf + index, len - index, "CHA %d/%d/%d/%d raw",
                              configs[0]->weight, configs[0]->internal_offset,
                              configs[0]->setup.offset, configs[0]->setup.divider);
            if (temperatureA != SCALE_INVALID_VALUE) {
               index += snprintf(buf + index, len - index, ", %.1f°C", temperatureA);
            }
         }
#ifdef HAS_SCALE_B
         if (index > start) {
            index += snprintf(buf + index, len - index, ", ");
         }
         if (configs[1]->weight != NAU7802_NONE_ADC_VALUE) {
            index += snprintf(buf + index, len - index, "CHB %d/%d/%d/%d raw",
                              configs[1]->weight, configs[1]->internal_offset,
                              configs[1]->setup.offset, configs[1]->setup.divider);
            if (SCALE_INVALID_VALUE != temperatureB) {
               index += snprintf(buf + index, len - index, ", %.1f°C", temperatureB);
            }
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

static int scale_start_calibration(struct scale_config *scale_dev)
{
   int res = -EBUSY;
   k_mutex_lock(&scale_mutex, K_FOREVER);
   if (CALIBRATE_NONE == current_calibrate_phase) {
      current_calibrate_phase = CALIBRATE_CMD;
      if (!scale_dev->setup_init) {
         scale_load_setup(scale_dev);
      }
      scale_prepare_calibration(scale_dev);
      scale_expansion_port_power(true);
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
         scale_save_setup(scale_dev);
         LOG_INF("ADC %s calibration saved.", scale_dev->channel_name);
      } else {
         scale_load_setup(scale_dev);
         LOG_INF("ADC %s calibration canceled.", scale_dev->channel_name);
      }
      current_calibrate_phase = CALIBRATE_NONE;
      scale_suspend(scale_dev);
      scale_expansion_port_power(false);
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
      LOG_INF("ADC %s calibrate %s.", scale_dev->channel_name, msg);
      time = k_uptime_get();
      res = scale_sample_channel(scale_dev);
      time = k_uptime_get() - time;
      if (res) {
         // failure
         res = 0;
         LOG_INF("ADC %s calibrate %s failed.", scale_dev->channel_name, msg);
      } else {
         if (CALIBRATE_CHA_10KG == phase) {
            scale_calc_calibration(scale_dev, reference, (int)time);
         } else {
            scale_dev->setup.offset = scale_dev->weight;
         }
         scale_dev->setup.calibration_temperature = scale_dev->temperature;
         res = 1;
      }
      current_calibrate_phase = CALIBRATE_CMD;
   }
   k_mutex_unlock(&scale_mutex);

   return res;
}

static int scale_set_calibration_value(struct scale_config *scale_dev, void *value, size_t value_size, bool has_value, int32_t new_value, const char *name)
{
   if (!has_value) {
      LOG_INF("ADC %s: missing %s value", scale_dev->channel_name, name);
      return -EINVAL;
   } else {
      int res = scale_start_calibration(scale_dev);
      if (!res) {
         switch (value_size) {
            case sizeof(uint8_t):
               *((uint8_t *)value) = (uint8_t)new_value;
               break;
            case sizeof(uint16_t):
               *((uint16_t *)value) = (uint16_t)new_value;
               break;
            case sizeof(int32_t):
               *((int32_t *)value) = (int32_t)new_value;
               break;
         }
         LOG_INF("ADC %s calibration %s: %7d", scale_dev->channel_name, name, new_value);
         res = scale_finish_calibration(scale_dev, true);
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
      struct scale_setup *cal = &scale_dev->setup;

      memset(value, 0, sizeof(value));
      cur = parse_next_text(cur, ' ', value, sizeof(value));
      if (value[0]) {
         num_value = (int32_t)strtol(value, &t, 10);
         has_value = !*t;
      }

      if (!stricmp("off", name)) {
         res = scale_set_calibration_value(scale_dev, &cal->offset, sizeof(cal->offset), has_value, num_value, "offset");
      } else if (!stricmp("div", name)) {
         res = scale_set_calibration_value(scale_dev, &cal->divider, sizeof(cal->divider), has_value, num_value, "divider");
      } else if (!stricmp("av", name)) {
         res = scale_set_calibration_value(scale_dev, &cal->int_avref, sizeof(cal->int_avref), has_value, num_value, "int. avref");
      } else if (!stricmp("extav", name)) {
         res = scale_set_calibration_value(scale_dev, &cal->ext_avref, sizeof(cal->ext_avref), has_value, num_value, "ext. avref");
      } else if (!stricmp("maxdit", name)) {
         res = scale_set_calibration_value(scale_dev, &cal->max_dither, sizeof(cal->max_dither), has_value, num_value, "max. dither");
      } else if (!stricmp("mindiv", name)) {
         res = scale_set_calibration_value(scale_dev, &cal->min_divider, sizeof(cal->min_divider), has_value, num_value, "min. divider");
      } else if (!stricmp("intosc", name)) {
         num_value = num_value ? true : false;
         res = scale_set_calibration_value(scale_dev, &cal->int_osc, sizeof(cal->int_osc), true, num_value, "int. osc.");
      } else if (!stricmp("pgacap", name)) {
         num_value = num_value ? true : false;
         res = scale_set_calibration_value(scale_dev, &cal->pga_cap, sizeof(cal->pga_cap), true, num_value, "PGA cap.");
      } else if (!stricmp("readtemp", name)) {
         num_value = num_value ? true : false;
         res = scale_set_calibration_value(scale_dev, &cal->read_temp, sizeof(cal->read_temp), true, num_value, "read temp.");
      } else if (!stricmp("gain", name)) {
         res = scale_set_calibration_value(scale_dev, &cal->gain, sizeof(cal->gain), has_value, num_value, "gain");
      } else if (!stricmp("temp", name)) {
         res = scale_set_calibration_value(scale_dev, &cal->calibration_temperature, sizeof(cal->calibration_temperature), has_value, num_value, "temperature");
         if (!res) {
            LOG_INF("ADC %s calibration temperature %7.1f", scale_dev->channel_name,
                    TEMPERATURE_DOUBLE(num_value));
         }
      } else if (!stricmp("def", name)) {
         res = scale_start_calibration(scale_dev);
         if (!res) {
            *cal = *scale_dev->default_setup;
            LOG_INF("ADC %s calibration reset to defaults", scale_dev->channel_name);
            res = scale_finish_calibration(scale_dev, true);
         }
      } else if (!stricmp("load", name)) {
         scale_load_setup(scale_dev);
         scale_dump_calibration(scale_dev);
         res = 0;
#ifdef CONFIG_NAU7802_DUMMY_CALIBRATION
      } else if (!stricmp("dummy", name)) {
         res = scale_start_calibration(scale_dev);
         if (!res) {
            scale_dev->setup.offset = 0;
            scale_dev->setup.calibration_temperature = 0;
            scale_dev->setup.divider = DUMMY_ADC_DIVIDER;
            LOG_INF("ADC %s dummy calibration.", scale_dev->channel_name);
            res = scale_finish_calibration(scale_dev, true);
         }
#endif /* CONFIG_NAU7802_DUMMY_CALIBRATION */
      } else {
         if (!stricmp("ref", name)) {
            num_value = SCALE_CALIBRATION_G;
            t += 3;
         } else if (!stricmp("zero", name)) {
            num_value = 0;
            t += 4;
         } else {
            num_value = (int32_t)strtol(name, &t, 10);
         }
         if (!*t) {
            bool success = false;
            res = scale_start_calibration(scale_dev);
            if (!res) {
               if (num_value == 0) {
                  if (scale_sample_calibration(scale_dev, CALIBRATE_ZERO, num_value, "zero")) {
                     num_value = scale_dev->setup.offset;
                     LOG_INF("ADC %s calibration offset: %7d", scale_dev->channel_name, num_value);
                     success = true;
                  }
               } else {
                  if (scale_sample_calibration(scale_dev, CALIBRATE_CHA_10KG, num_value, "divider")) {
                     num_value = scale_dev->setup.divider;
                     LOG_INF("ADC %s calibration divider: %7d%s", scale_dev->channel_name,
                             num_value, num_value == DUMMY_ADC_DIVIDER ? " (dummy)" : "");
                     success = true;
                  }
               }
            }
            res = scale_finish_calibration(scale_dev, success);
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
   LOG_INF("  scalecal                : show calibration data of scales.");
   LOG_INF("  scalecal [CHA|CHB]      : show calibration data of scale A or B.");
   LOG_INF("  scalecal [CHA|CHB] [av|extav|gain|mindiv|maxdit|off|div|temp] <value>:");
   LOG_INF("           av <value>     : set calibration int. avref.");
   LOG_INF("           extav <value>  : set calibration ext. avref.");
   LOG_INF("           gain <value>   : set calibration gain. 1, 2, 4, 8, 16, 32, 64, 128");
   LOG_INF("           mindiv <value> : set calibration minimum divider.");
   LOG_INF("           maxdit <value> : set calibration maximum dither.");
   LOG_INF("           off <value>    : set calibration offset.");
   LOG_INF("           div <value>    : set calibration divider (10kg).");
   LOG_INF("           temp <value>   : set calibration temperature.");
   LOG_INF("  scalecal [CHA|CHB] [intosc|pgacap|readtemp] [0|1]:");
   LOG_INF("           intosc         : set internal oscilator.");
   LOG_INF("           pgacap         : set PGA capacitor.");
   LOG_INF("           readtemp       : set read internal temperature.");
   LOG_INF("  scalecal [CHA|CHB] [zero|<reference>]:");
   LOG_INF("           zero           : measure 0 to calibrate offset.");
   LOG_INF("           <reference>    : measure <reference> [g] to calibrate divider.");
   LOG_INF("           [CHA|CHB]      : may be omitted, if only one scale is defined.");
   LOG_INF("  scalecal [CHA|CHB] def  : set calibration to default values.");
   LOG_INF("  scalecal [CHA|CHB] load : (re-)oad calibration.");
#ifdef CONFIG_NAU7802_DUMMY_CALIBRATION
   LOG_INF("  scalecal [CHA|CHB] dummy : set dummy calibration and save it.");
#endif /* CONFIG_NAU7802_DUMMY_CALIBRATION */
}

SH_CMD(scale, NULL, "read scale info.", sh_cmd_scale, NULL, 0);
SH_CMD(scalecal, NULL, "scale calibration.", sh_cmd_scale_calibration, sh_cmd_scale_calibration_help, 0);
#endif /* CONFIG_SH_CMD */