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

#include <stdio.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/pm/device.h>

#include "appl_settings.h"
#include "expansion_port.h"
#include "io_job_queue.h"
#include "modem_at.h"
#include "parse.h"
#include "power_manager.h"
#include "sh_cmd.h"
#include "transform.h"

#ifdef CONFIG_BATTERY_ADC
#include "battery_adc.h"
#endif /* CONFIG_BATTERY_ADC */

LOG_MODULE_REGISTER(POWER_MANAGER, CONFIG_POWER_MANAGER_LOG_LEVEL);

typedef const struct device *t_devptr;

#if DT_NODE_HAS_STATUS(DT_NODELABEL(i2c1), okay) && defined(CONFIG_DISABLE_REALTIME_CLOCK)

#define REALTIME_CLOCK_ADDR 0x51

static int power_manager_suspend_realtime_clock(void)
{
   const struct device *i2c_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c1));
   if (device_is_ready(i2c_dev)) {
      // disable output
      int rc1 = i2c_reg_write_byte(i2c_dev, REALTIME_CLOCK_ADDR, 1, 7);
      // stop
      int rc2 = i2c_reg_write_byte(i2c_dev, REALTIME_CLOCK_ADDR, 0, BIT(5));
      if (!rc1 && !rc2) {
         LOG_INF("Suspended realtime clock.");
      } else {
         LOG_INF("Suspending realtime clock failed. %d %d", rc1, rc2);
      }
   }
   return 0;
}

SYS_INIT(power_manager_suspend_realtime_clock, POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY);

#endif /* DT_NODE_HAS_STATUS(DT_NODELABEL(i2c1), okay) && defined(CONFIG_DISABLE_REALTIME_CLOCK) */

#define PM_INVALID_INTERNAL_LEVEL 0xffff
#define PM_RESET_INTERNAL_LEVEL 0xfffe

#define VOLTAGE_MIN_INTERVAL_MILLIS 10000
#define MAX_PM_DEVICES 10

/**
 * last voltage.
 * suppress new measurement for VOLTAGE_MIN_INTERVAL_MILLIS
 */

static int64_t last_voltage_uptime = 0;
static uint16_t last_voltage = PM_INVALID_VOLTAGE;
static bool last_voltage_charger = false;

static K_MUTEX_DEFINE(pm_mutex);
static k_ticks_t pm_pluse_end = 0;
static bool pm_suspend = false;
static bool pm_suspended = false;

static volatile bool pm_init = false;
static int pm_dev_counter = 0;
static t_devptr pm_dev_table[MAX_PM_DEVICES];

static void suspend_devices(bool suspend)
{
   if (suspend) {
      for (int i = 0; i < pm_dev_counter; ++i) {
         const struct device *dev = pm_dev_table[i];
         int ret = pm_device_action_run(dev, PM_DEVICE_ACTION_SUSPEND);
         if (ret < 0) {
            if (ret != -EALREADY) {
               LOG_WRN("Failed to suspend %s", dev->name);
            }
         } else {
            LOG_INF("Suspended %s", dev->name);
         }
      }
   } else {
      for (int i = pm_dev_counter; i > 0; --i) {
         const struct device *dev = pm_dev_table[i - 1];
         int ret = pm_device_action_run(dev, PM_DEVICE_ACTION_RESUME);
         if (ret < 0) {
            if (ret != -EALREADY) {
               LOG_WRN("Failed to resume %s", dev->name);
            }
         } else {
            LOG_INF("Resumed %s", dev->name);
         }
      }
   }
}

#if defined(CONFIG_SERIAL) && DT_HAS_CHOSEN(zephyr_console) && DT_NODE_HAS_STATUS(DT_CHOSEN(zephyr_console), okay)
static const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
#else  /* CONFIG_SERIAL */
static const struct device *const uart_dev = NULL;
#endif /* CONFIG_SERIAL */

#ifdef CONFIG_SUSPEND_UART
#if defined(CONFIG_UART_CONSOLE) && !defined(CONFIG_CONSOLE_SUBSYS)

static bool uart_suspend = false;
static int uart_counter = 0;

static void suspend_uart_fn(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(suspend_uart_work, suspend_uart_fn);

static void suspend_uart_fn(struct k_work *work)
{
   k_mutex_lock(&pm_mutex, K_FOREVER);
   if (uart_suspend) {
      if (++uart_counter < 30) {
         if (!log_data_pending()) {
            // final delay
            uart_counter = 30;
         }
         work_schedule_for_io_queue(&suspend_uart_work, K_MSEC(50));
         k_mutex_unlock(&pm_mutex);
         return;
      }
      int ret = pm_device_action_run(uart_dev, PM_DEVICE_ACTION_SUSPEND);
      if (ret < 0 && ret != -EALREADY) {
         LOG_WRN("Failed to disable UART (%d)", ret);
         uart_suspend = false;
      }
   }
   k_mutex_unlock(&pm_mutex);
}

static void suspend_uart(bool suspend)
{
   int ret = 0;

   if (device_is_ready(uart_dev) && uart_suspend != suspend) {
      uart_suspend = suspend;
      if (suspend) {
         LOG_INF("Disable UART");
         uart_counter = 0;
         work_schedule_for_io_queue(&suspend_uart_work, K_MSEC(50));
      } else {
         ret = pm_device_action_run(uart_dev, PM_DEVICE_ACTION_RESUME);
         if (ret < 0 && ret != -EALREADY) {
            LOG_WRN("Failed to enable UART (%d)", ret);
            uart_suspend = !suspend;
         } else {
            k_sleep(K_MSEC(50));
#ifdef CONFIG_UART_ASYNC_API
            uart_rx_disable(uart_dev);
#endif
            LOG_INF("Enabled UART");
         }
      }
   }
}
#else  /* CONFIG_UART_CONSOLE */
static void suspend_uart(bool suspend)
{
   (void)suspend;
}
#endif /* CONFIG_UART_CONSOLE */
#endif /* CONFIG_SUSPEND_UART */

/** A discharge curve specific to the power source. */

struct battery_profile {
   const char *name;
   const struct transform_curve *curve;
};

#ifdef CONFIG_BATTERY_TYPE_LIPO_1350_MAH
static const struct transform_curve curve_lipo_1350 = {
    /* Thingy:91 */
    .points = 7,
    .curve = {
        {4200, 10000},
        {3950, 8332},
        {3812, 5313},
        {3689, 1592},
        {3626, 1146},
        {3540, 700},
        {3300, 0},
    }};

static const struct battery_profile profile_lipo_1350 = {
    .name = NULL,
    .curve = &curve_lipo_1350};
#endif

#ifdef CONFIG_BATTERY_TYPE_LIPO_2000_MAH
static const struct transform_curve curve_lipo_2000 = {
    /* nRF9160 feather */
    .points = 8,
    .curve = {
        {4180, 10000},
        {4136, 9900},
        {4068, 9500},
        {4022, 9000},
        {4000, 7700},
        {3800, 4500},
        {3420, 500},
        {3350, 0},
    }};

static const struct battery_profile profile_lipo_2000 = {
    .name = "LiPo",
    .curve = &curve_lipo_2000};
#endif

#ifdef CONFIG_BATTERY_TYPE_NIMH_2000_MAH
static const struct transform_curve curve_nimh_2000 = {
    /* nRF9160 feather */
    .points = 8,
    .curve = {
        {4350, 10000}, /* requires external charger */
        {4024, 9265},
        {3886, 7746},
        {3784, 3380},
        {3696, 1830},
        {3540, 845},
        {3430, 422},
        {3300, 0},
    }};
static const struct battery_profile profile_nimh_2000 = {
    .name = "NiMH",
    .curve = &curve_nimh_2000};
#endif

#ifdef CONFIG_BATTERY_TYPE_NIMH_4_2000_MAH
static const struct transform_curve curve_nimh_4_2000 = {
    /* nRF9160 feather */
    .points = 8,
    .curve = {
        {5800, 10000}, /* requires external charger */
        {5365, 9265},
        {5181, 7746},
        {5045, 3380},
        {4928, 1830},
        {4720, 845},
        {4573, 422},
        {4400, 0},
    }};
static const struct battery_profile profile_nimh_4_2000 = {
    .name = "NiMH/4",
    .curve = &curve_nimh_4_2000};
#endif

#ifdef CONFIG_BATTERY_TYPE_SUPER_CAP_LIHY
/* LIB1620Q4R0407, super capacitor, 4V, 400F */
static const struct transform_curve curve_supcap_lihy = {
    /* nRF9160 feather */
    .points = 4,
    .curve = {
        {3950, 10000},
        {3550, 1682},
        {3472, 412},
        {3415, 0},
    }};

static const struct battery_profile profile_supcap_lihy = {
    .name = "LiHy",
    .curve = &curve_supcap_lihy};
#endif

static const struct transform_curve curve_no_bat = {
    /* no battery */
    .points = 1,
    .curve = {
        {0, -1}}};

static const struct battery_profile profile_no_bat = {
    .name = NULL,
    .curve = &curve_no_bat};

static const struct battery_profile *battery_profiles[] = {
    &profile_no_bat,
#ifdef CONFIG_BATTERY_TYPE_LIPO_1350_MAH
    &profile_lipo_1350,
#else
    NULL,
#endif
#ifdef CONFIG_BATTERY_TYPE_LIPO_2000_MAH
    &profile_lipo_2000,
#else
    NULL,
#endif
#ifdef CONFIG_BATTERY_TYPE_NIMH_2000_MAH
    &profile_nimh_2000,
#else
    NULL,
#endif
#ifdef CONFIG_BATTERY_TYPE_NIMH_4_2000_MAH
    &profile_nimh_4_2000,
#else
    NULL,
#endif
#ifdef CONFIG_BATTERY_TYPE_SUPER_CAP_LIHY
    &profile_supcap_lihy,
#else
    NULL,
#endif
};

static const struct battery_profile *pm_get_battery_profile(void)
{
   int profile = appl_settings_get_battery_profile();
   if (profile < 0 ||
       (sizeof(battery_profiles) / sizeof(struct battery_profiles *)) <= profile ||
       !battery_profiles[profile]) {
      profile = 0;
   }
   return battery_profiles[profile];
}

#define LINREG_SIZE 5

static int64_t calc_lr_times[LINREG_SIZE];
static uint16_t calc_lr_values[LINREG_SIZE];
static uint16_t calc_lr_count = 0;
static uint16_t calc_lr_index = 0;

static void reset_linear_regresion(void)
{
   calc_lr_count = 0;
}

static uint16_t calculate_linear_regresion(int64_t *now, uint16_t value)
{
   int64_t seconds = *now / 1000;

   if (!calc_lr_count) {
      calc_lr_index = 0;
   }

   calc_lr_times[calc_lr_index] = seconds;
   calc_lr_values[calc_lr_index] = value;
   ++calc_lr_index;

   if (!calc_lr_count) {
      ++calc_lr_count;
   } else {
      double slope = 0.0;
      double res = 0.0;

      uint32_t sumT = 0;
      uint32_t sumV = 0;
      int64_t sumVT = 0;
      int64_t sumTT = 0;

      if (calc_lr_count < calc_lr_index) {
         calc_lr_count = calc_lr_index;
      }

      if (LINREG_SIZE == calc_lr_index) {
         calc_lr_index = 0;
      }

      for (int index = 0; index < calc_lr_count; ++index) {
         uint32_t rT = (uint32_t)(seconds - calc_lr_times[index]);
         sumT += rT;
         sumV += calc_lr_values[index];
         sumVT += rT * calc_lr_values[index];
         sumTT += rT * rT;
      }

      slope = ((calc_lr_count * sumVT) - ((int64_t)sumV * sumT));
      slope /= ((calc_lr_count * sumTT) - ((int64_t)sumT * sumT));
      res = (sumV / calc_lr_count) - slope * (sumT / calc_lr_count);

      LOG_DBG("=======================\n");
      LOG_DBG("Sum: %d %u %u\n", sumT, sumV, calc_lr_count);
      LOG_DBG("Avg: %d %u\n", sumT / calc_lr_count, sumV / calc_lr_count);
      LOG_DBG("Res: %f => %f\n", slope, res);
      LOG_DBG("=======================\n");
      value = (uint16_t)res;
   }
   return value;
}

static uint16_t current_battery_changes = 0;
static bool forecast_first_day = false;

/*
 * First battery level period.
 */
static uint16_t first_battery_level = PM_INVALID_INTERNAL_LEVEL;
static int64_t first_battery_level_uptime = 0;
/**
 * Last battery level period.
 */
static uint16_t last_battery_level = PM_INVALID_INTERNAL_LEVEL;
static int64_t last_battery_level_uptime = 0;

/**
 * Lowest battery level.
 */
static uint16_t lowest_battery_level = PM_INVALID_INTERNAL_LEVEL;
static int64_t lowest_battery_level_uptime = 0;

/**
 * Threshold to reset forecast. Intended to be used for
 * solar charger without connected charging signal.
 * 0 to disable.
 */
static uint16_t battery_reset_threshold = CONFIG_BATTERY_FORECAST_RESET_THRESHOLD_DEFAULT;

/*
 * Last left battery time.
 */
static int64_t last_battery_left_time = 0;

#define MSEC_PER_MINUTE (MSEC_PER_SEC * 60)
#define MSEC_PER_HOUR (MSEC_PER_SEC * 60 * 60)
#define MSEC_PER_DAY (MSEC_PER_SEC * 60 * 60 * 24)
#define MSEC_PER_WEEK (MSEC_PER_SEC * 60 * 60 * 24 * 7)

/*
 * Minimum battery level delta for new forecast calculation.
 * Value in 2%%
 */
#define MINIMUM_BATTERY_LEVEL_DELTA 20

#define ROUND_DAYS(M) (int16_t)(((M) + ((MSEC_PER_DAY) / 2)) / (MSEC_PER_DAY))
#define ROUND_HOURS(M) (int32_t)(((M) + ((MSEC_PER_HOUR) / 2)) / (MSEC_PER_HOUR))

static int calculate_left_time(const char *tag, int64_t *now, uint16_t battery_level, int64_t *past, uint16_t battery_level_past, int64_t *time)
{
   int diff = battery_level_past - battery_level;
   if (diff == 0) {
      // diff should never be 0, just to ensure, not to fail by div 0
      return -1;
   } else {
      int64_t passed_time = *now - *past;
      int64_t left_time = (passed_time * battery_level) / diff;
      LOG_INF("%s: left battery %u.%02u%% time %lld (%d days, %d passed)",
              tag, battery_level / 100, battery_level % 100, left_time,
              ROUND_DAYS(left_time), ROUND_DAYS(passed_time));
      *time += left_time;
      return 0;
   }
}

static int16_t calculate_forecast(int64_t *now, uint16_t battery_level, power_manager_status_t *status)
{
   int16_t res = -1;
   int64_t passed_time = (*now) - last_battery_level_uptime;
   int32_t delta = ((int32_t)last_battery_level) - battery_level;

   if (PM_INVALID_INTERNAL_LEVEL == battery_level) {
      LOG_INF("forecast: not ready.");
   } else if (PM_RESET_INTERNAL_LEVEL == battery_level) {
      LOG_INF("forecast: reset.");
   } else if (!status || (FROM_BATTERY != *status && POWER_UNKNOWN != *status)) {
      LOG_INF("forecast: charging.");
   } else if (current_battery_changes) {
      if (battery_reset_threshold && delta < -battery_reset_threshold) {
         // unaware "solar" charging
         if (status) {
            *status = CHARGING_S;
         }
         last_battery_level_uptime = *now + MSEC_PER_HOUR;
         last_battery_level = battery_level;
         lowest_battery_level_uptime = *now;
         lowest_battery_level = battery_level;
         current_battery_changes = 1;
         reset_linear_regresion();
         LOG_INF("forecast: charging?");
         return -1;
      } else {
         res = 0;
      }
   } else {
      res = 0;
   }
   if (0 > res) {
      // reset, wait 60min after start running from battery
      last_battery_level_uptime = *now + MSEC_PER_HOUR;
      last_battery_level = battery_level;
      lowest_battery_level_uptime = *now;
      lowest_battery_level = battery_level;
      forecast_first_day = false;
      current_battery_changes = 0;
      reset_linear_regresion();
      return -1;
   }

   if (0 > passed_time) {
      // wait 60min after running from battery
      LOG_INF("forecast: wait 60 minutes, passed %lld minutes", ((MSEC_PER_HOUR + passed_time) / MSEC_PER_MINUTE));
      return -1;
   }

   if (lowest_battery_level > battery_level) {
      lowest_battery_level = battery_level;
      lowest_battery_level_uptime = *now;
      if (2 > current_battery_changes) {
         // start forecast calculation
         LOG_INF("forecast: starting %u.%02u%%", battery_level / 100, battery_level % 100);
         first_battery_level = battery_level;
         first_battery_level_uptime = *now;
         last_battery_level = battery_level;
         last_battery_level_uptime = *now;
         current_battery_changes = 2;
      }
   }
   if (1 < current_battery_changes) {

      if (!forecast_first_day && MSEC_PER_DAY < (*now) - first_battery_level_uptime) {
         forecast_first_day = true;
         LOG_INF("forecast: adjust after 1. day %u.%02u%%", lowest_battery_level / 100, lowest_battery_level % 100);
         first_battery_level = lowest_battery_level;
         first_battery_level_uptime = lowest_battery_level_uptime;
         last_battery_level = lowest_battery_level;
         last_battery_level_uptime = lowest_battery_level_uptime;
      }

      if (last_battery_level_uptime != lowest_battery_level_uptime) {
         bool refresh_calculation = false;
         if (2 == current_battery_changes) {
            // first period
            if (MINIMUM_BATTERY_LEVEL_DELTA <= delta || MSEC_PER_DAY < passed_time) {
               refresh_calculation = true;
            }
         } else {
            if (MINIMUM_BATTERY_LEVEL_DELTA <= delta && MSEC_PER_DAY < passed_time) {
               refresh_calculation = true;
            } else if (MSEC_PER_WEEK < passed_time) {
               refresh_calculation = true;
            }
         }

         if (refresh_calculation) {
            ++current_battery_changes;
            last_battery_left_time = 0;
            if (first_battery_level_uptime == last_battery_level_uptime) {
               calculate_left_time("First period", &lowest_battery_level_uptime, lowest_battery_level, &last_battery_level_uptime, last_battery_level, &last_battery_left_time);
            } else {
               calculate_left_time("All periods", &lowest_battery_level_uptime, lowest_battery_level, &first_battery_level_uptime, first_battery_level, &last_battery_left_time);
               last_battery_left_time *= 2;
               calculate_left_time("Last period", &lowest_battery_level_uptime, lowest_battery_level, &last_battery_level_uptime, last_battery_level, &last_battery_left_time);
               last_battery_left_time /= 3;
            }
            last_battery_level = lowest_battery_level;
            last_battery_level_uptime = lowest_battery_level_uptime;
            passed_time = (*now) - last_battery_level_uptime;
         }
      }
   }
   if (2 < current_battery_changes) {
      // after last change
      int16_t time = ROUND_DAYS((last_battery_left_time - passed_time));
      LOG_INF("battery %u.%02u%%, %d left days (passed %d days, %d changes)", battery_level / 100, battery_level % 100, time, ROUND_DAYS(passed_time), current_battery_changes);
      LOG_INF("%u.%02u%% lowest, %d hours ago", lowest_battery_level / 100, lowest_battery_level % 100, ROUND_HOURS(*now - lowest_battery_level_uptime));
      if (last_battery_level_uptime != lowest_battery_level_uptime) {
         LOG_INF("%u.%02u%% last, %d hours ago", last_battery_level / 100, last_battery_level % 100, ROUND_HOURS(*now - last_battery_level_uptime));
      }
      if (first_battery_level_uptime != lowest_battery_level_uptime &&
          first_battery_level_uptime != last_battery_level_uptime) {
         LOG_INF("%u.%02u%% first, %d hours ago", first_battery_level / 100, first_battery_level % 100, ROUND_HOURS(*now - first_battery_level_uptime));
      }
      return time;
   } else if (delta < MINIMUM_BATTERY_LEVEL_DELTA) {
      LOG_INF("forecast: %u.%02u%%, %d delta, %d changes", battery_level / 100, battery_level % 100, delta, current_battery_changes);
      LOG_INF("%u.%02u%% lowest, %d hours ago", lowest_battery_level / 100, lowest_battery_level % 100, ROUND_HOURS(*now - lowest_battery_level_uptime));
      if (last_battery_level_uptime != lowest_battery_level_uptime) {
         LOG_INF("%u.%02u%% last, %d hours ago", last_battery_level / 100, last_battery_level % 100, ROUND_HOURS(*now - last_battery_level_uptime));
      }
   }
   return -1;
}

#ifdef CONFIG_ADP536X_POWER_MANAGEMENT

#define ADP536X_I2C_REG_CHARGE_TERMINATION 0x3
#define ADP536X_I2C_REG_STATUS 0x8
#define ADP536X_I2C_REG_LEVEL 0x21
#define ADP536X_I2C_REG_VOLTAGE_HIGH_BYTE 0x25
#define ADP536X_I2C_REG_VOLTAGE_LOW_BYTE 0x26
#define ADP536X_I2C_REG_FUEL_GAUGE_MODE 0x27
#define ADP536X_I2C_REG_BUCK_CONFIG 0x29
#define ADP536X_I2C_REG_BUCK_BOOST_CONFIG 0x2B

static const struct i2c_dt_spec i2c_spec = I2C_DT_SPEC_GET(DT_ALIAS(pmic));

static int adp536x_reg_read_bytes(uint8_t reg, uint8_t *buff, size_t len)
{
   return i2c_write_read_dt(&i2c_spec,
                            &reg, sizeof(reg),
                            buff, len);
}

static int adp536x_reg_read(uint8_t reg, uint8_t *buff)
{
   return i2c_reg_read_byte_dt(&i2c_spec, reg, buff);
}

static int adp536x_reg_write(uint8_t reg, uint8_t val)
{
   return i2c_reg_write_byte_dt(&i2c_spec, reg, val);
}

static int adp536x_power_manager_voltage(uint16_t *voltage)
{
   uint8_t buf[2] = {0, 0};
   int rc = adp536x_reg_read_bytes(ADP536X_I2C_REG_VOLTAGE_HIGH_BYTE, buf, sizeof(buf));
   if (!rc) {
      uint16_t value = buf[0];
      value <<= 5;
      value |= ((buf[1] >> 3) & 0x1f);
      *voltage = value;
   }
   return rc;
}

static int adp536x_power_manager_read_status(power_manager_status_t *status)
{
   uint8_t value = 0xff;
   int rc = adp536x_reg_read(ADP536X_I2C_REG_STATUS, &value);

   if (!rc) {
      switch (value & 0x7) {
         case 0:
            *status = FROM_BATTERY;
            break;
         case 1:
            *status = CHARGING_TRICKLE;
            break;
         case 2:
            *status = CHARGING_I;
            break;
         case 3:
            *status = CHARGING_V;
            break;
         case 4:
            *status = CHARGING_COMPLETED;
            break;
         case 5:
         case 6:
         case 7:
         default:
            *status = POWER_UNKNOWN;
            break;
      }
   }
   return rc;
}

static int adp536x_power_manager_xvy(uint8_t config_register, bool enable)
{
   int rc = -ENOTSUP;

   if (device_is_ready(i2c_spec.bus)) {
      uint8_t buck_config = 0;
      rc = adp536x_reg_read(config_register, &buck_config);
      if (!rc) {
         /*         LOG_INF("buck_conf: %02x %02x", config_register, buck_config); */
         buck_config |= 0xC0; // softstart to 11 => 512ms
         if (enable) {
            buck_config |= 1;
         } else {
            buck_config &= (~1);
         }
         adp536x_reg_write(config_register, buck_config);
      } else {
         LOG_WRN("Failed to read buckbst_cfg.");
      }
   }
   return rc;
}

static int adp536x_power_manager_init(void)
{
   int rc = -ENOTSUP;

   if (device_is_ready(i2c_spec.bus)) {
      uint8_t value = 0;

      adp536x_reg_read(ADP536X_I2C_REG_CHARGE_TERMINATION, &value);
      value &= 3;
      value |= 0x78; // 4.16V
      adp536x_reg_write(ADP536X_I2C_REG_CHARGE_TERMINATION, value);
      LOG_INF("Battery monitor initialized.");
      rc = 0;
   } else {
      LOG_WRN("Failed to initialize battery monitor.");
   }
   return rc;
}

#endif

/* NPM1300 nodes */
#define NPM1300_MFD_NODE DT_INST(0, nordic_npm1300)
#define NPM1300_CHARGER_NODE DT_INST(0, nordic_npm1300_charger)

#ifdef CONFIG_REGULATOR_NPM1300

#if (DT_NODE_HAS_STATUS(DT_NODELABEL(npm1300_buck2), okay))

#include "ui.h"
#include <zephyr/drivers/regulator.h>

#define REGULATOR_NODE DT_NODELABEL(npm1300_buck2)

#if defined(CONFIG_MFD_NPM1300_BUCK2_WITH_USB) && DT_PROP_LEN(NPM1300_MFD_NODE, host_int_gpios)
#define MFD_NPM1300_BUCK2_WITH_USB_INT
#endif /* DT_PROP_LEN(NPM1300_MFD_NODE, host_int_gpios) */

static const struct device *npm1300_buck2_dev = DEVICE_DT_GET(REGULATOR_NODE);

static int npm1300_buck2_enabled(void)
{
   return regulator_is_enabled(npm1300_buck2_dev) ? 1 : 0;
}

#if DT_PROP(REGULATOR_NODE, regulator_always_on)

static int npm1300_buck2_enable(bool enable)
{
   return 0;
}

#else /* regulator_always_on */

static int npm1300_buck2_enable(bool enable)
{
   int ret = 0;

   if (!device_is_ready(npm1300_buck2_dev)) {
      LOG_WRN("NPM1300 buck2 not ready!");
      return -ENOTSUP;
   }

   if (enable) {
      if (regulator_is_enabled(npm1300_buck2_dev)) {
         LOG_INF("NPM1300 already enabled buck2.");
      } else {
         while (!ret && !regulator_is_enabled(npm1300_buck2_dev)) {
            ret = regulator_enable(npm1300_buck2_dev);
         }
         if (ret < 0) {
            LOG_WRN("NPM1300 enable buck2 failed, %d (%s)!", ret, strerror(-ret));
         } else {
            LOG_INF("NPM1300 enabled buck2.");
         }
      }
#ifdef CONFIG_MFD_NPM1300_BUCK2_LED
      if (!ret) {
         ui_led_op(LED_BUCK2, LED_SET);
      }
#endif
   } else {
      if (!regulator_is_enabled(npm1300_buck2_dev)) {
         LOG_INF("NPM1300 already disabled buck2.");
      } else {
         while (!ret && regulator_is_enabled(npm1300_buck2_dev)) {
            ret = regulator_disable(npm1300_buck2_dev);
         }
         if (ret < 0) {
            LOG_WRN("NPM1300 disable buck2 failed, %d (%s)!", ret, strerror(-ret));
         } else {
            LOG_INF("NPM1300 disabled buck2.");
         }
      }
#ifdef CONFIG_MFD_NPM1300_BUCK2_LED
      if (!ret) {
         ui_led_op(LED_BUCK2, LED_CLEAR);
      }
#endif
   }

   return ret;
}
#endif /* regulator_always_on */

#else /* DT_NODE_HAS_STATUS(DT_NODELABEL(npm1300_buck2), okay) */
#undef CONFIG_REGULATOR_NPM1300
#undef CONFIG_MFD_NPM1300_BUCK2_WITH_USB
#endif /* DT_NODE_HAS_STATUS(DT_NODELABEL(npm1300_buck2), okay) */
#endif /* CONFIG_REGULATOR_NPM1300 */

#ifdef CONFIG_MFD_NPM1300

#if (DT_NODE_HAS_STATUS(NPM1300_MFD_NODE, okay))

#include <zephyr/drivers/mfd/npm1300.h>

static const struct device *npm1300_mfd_dev = DEVICE_DT_GET(NPM1300_MFD_NODE);

#define NPM1300_SYSREG_BASE 0x2
#define NPM1300_SYSREG_OFFSET_USBCDETECTSTATUS 0x5

#define NPM1300_BUCK_BASE 0x4
#define NPM1300_BUCK_OFFSET_BUCKCTRL0 0x15
#define NPM1300_BUCK2_PULLDOWN_EN BIT(3)

#ifdef CONFIG_MFD_NPM1300_DISABLE_NTC
#define NPM1300_CHGR_BASE 0x3
#define NPM1300_CHGR_OFFSET_DIS_SET 0x06
#define NPM1300_CHGR_OFFSET_DIS_SET_DISABLE_NTC BIT(1)
#endif /* CONFIG_MFD_NPM1300_DISABLE_NTC */

static int npm1300_mfd_detect_usb(uint8_t *usb, bool switch_regulator)
{
#ifndef CONFIG_MFD_NPM1300_BUCK2_WITH_USB
   (void)switch_regulator;
#endif /* !CONFIG_MFD_NPM1300_BUCK2_WITH_USB */

   int ret = 0;
   uint8_t status = 0;

   if (!device_is_ready(npm1300_mfd_dev)) {
      LOG_WRN("NPM1300 mfd not ready!");
      return -ENOTSUP;
   }

   ret = mfd_npm1300_reg_read(npm1300_mfd_dev, NPM1300_SYSREG_BASE, NPM1300_SYSREG_OFFSET_USBCDETECTSTATUS, &status);
   if (ret < 0) {
      LOG_WRN("NPM1300 read usb status failed, %d (%s)!", ret, strerror(-ret));
      return ret;
   } else {
      LOG_INF("NPM1300 USB 0x%x", status);
      if (usb) {
         *usb = status;
      }
#ifdef CONFIG_MFD_NPM1300_BUCK2_WITH_USB
      if (switch_regulator) {
         npm1300_buck2_enable(status);
         status = status ? 0 : NPM1300_BUCK2_PULLDOWN_EN;
         /* Write to MFD to enable/disable pulldown for BUCK2 */
         ret = mfd_npm1300_reg_update(npm1300_mfd_dev, NPM1300_BUCK_BASE, NPM1300_BUCK_OFFSET_BUCKCTRL0, status, NPM1300_BUCK2_PULLDOWN_EN);
      }
#endif /* CONFIG_MFD_NPM1300_BUCK2_WITH_USB */
   }

   return ret;
}

#ifdef MFD_NPM1300_BUCK2_WITH_USB_INT

static void npm1300_event_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
   if (pins & BIT(NPM1300_EVENT_VBUS_DETECTED)) {
      LOG_INF("PM Vbus connected");
      npm1300_buck2_enable(true);
   } else if (pins & BIT(NPM1300_EVENT_VBUS_REMOVED)) {
      LOG_INF("PM Vbus removed");
      npm1300_buck2_enable(false);
   }
}

#endif /* MFD_NPM1300_BUCK2_WITH_USB_INT */

static int npm1300_mfd_init(void)
{
   int ret = 0;

   if (!device_is_ready(npm1300_mfd_dev)) {
      LOG_WRN("NPM1300 mfd not ready!");
      return -ENOTSUP;
   }
#ifdef REGULATOR_NODE
   if (npm1300_buck2_enabled()) {
#ifdef CONFIG_MFD_NPM1300_BUCK2_LED
      ui_led_op(LED_BUCK2, LED_SET);
#endif
   }
#endif /* REGULATOR_NODE */

#ifdef MFD_NPM1300_BUCK2_WITH_USB_INT
   static struct gpio_callback event_cb;

   gpio_init_callback(&event_cb, npm1300_event_callback,
                      BIT(NPM1300_EVENT_VBUS_DETECTED) |
                          BIT(NPM1300_EVENT_VBUS_REMOVED));

   ret = mfd_npm1300_add_callback(npm1300_mfd_dev, &event_cb);
   if (ret) {
      LOG_WRN("NPM1300 mfd set callback failed %d (%s)!", ret, strerror(-ret));
   }
#endif /* MFD_NPM1300_BUCK2_WITH_USB_INT */

#ifdef CONFIG_MFD_NPM1300_BUCK2_WITH_USB
   ret = npm1300_mfd_detect_usb(NULL, true);
#endif /* CONFIG_MFD_NPM1300_BUCK2_WITH_USB */

#ifdef CONFIG_MFD_NPM1300_DISABLE_NTC
   ret = mfd_npm1300_reg_write(npm1300_mfd_dev, NPM1300_CHGR_BASE, NPM1300_CHGR_OFFSET_DIS_SET, 2);
#endif /* CONFIG_MFD_NPM1300_DISABLE_NTC */

   return ret;
}

#else /* DT_NODE_HAS_STATUS(DT_NODELABEL(npm1300_buck2), okay) */
#undef CONFIG_MFD_NPM1300
#endif /* DT_NODE_HAS_STATUS(DT_NODELABEL(npm1300_buck2), okay) */
#endif /* CONFIG_MFD_NPM1300 */

#ifdef CONFIG_NPM1300_CHARGER

#if (DT_NODE_HAS_STATUS(NPM1300_CHARGER_NODE, okay))

#include <zephyr/drivers/sensor/npm1300_charger.h>

static const struct device *npm1300_charger_dev = DEVICE_DT_GET(NPM1300_CHARGER_NODE);

static int npm1300_power_manager_read_temperatures(void)
{
   struct sensor_value value = {0, 0};
   int ret = sensor_sample_fetch_chan(npm1300_charger_dev, SENSOR_CHAN_DIE_TEMP);
   if (ret < 0) {
      LOG_WRN("NPM1300 fetch die temp failed, %d (%s)!", ret, strerror(-ret));
      return ret;
   }

   ret = sensor_channel_get(npm1300_charger_dev, SENSOR_CHAN_DIE_TEMP, &value);
   if (ret < 0) {
      LOG_WRN("NPM1300 get die temp failed, %d (%s)!", ret, strerror(-ret));
      return ret;
   }

   double die_temp = sensor_value_to_double(&value);

   ret = sensor_sample_fetch_chan(npm1300_charger_dev, SENSOR_CHAN_GAUGE_TEMP);
   if (ret < 0) {
      LOG_WRN("NPM1300 fetch gauge temp failed, %d (%s)!", ret, strerror(-ret));
      return ret;
   }

   ret = sensor_channel_get(npm1300_charger_dev, SENSOR_CHAN_GAUGE_TEMP, &value);
   if (ret < 0) {
      LOG_WRN("NPM1300 get gauge temp failed, %d (%s)!", ret, strerror(-ret));
      return ret;
   }

   double gauge_temp = sensor_value_to_double(&value);

   LOG_INF("NPM1300 temperature: die %.2f °C, gauge %.2f °C", die_temp, gauge_temp);

   return ret;
}

/* nPM1300_PS_v1.1.pdf, 6.2.14.31 BCHGCHARGESTATUS, page 45 */
#define NPM1300_CHG_STATUS_BATTERY_DETECTED BIT(0)
#define NPM1300_CHG_STATUS_COMPLETED BIT(1)
#define NPM1300_CHG_STATUS_TRICKLE BIT(2)
#define NPM1300_CHG_STATUS_CURRENT BIT(3)
#define NPM1300_CHG_STATUS_VOLTAGE BIT(4)
#define NPM1300_CHG_STATUS_RECHARGE BIT(5)
#define NPM1300_CHG_STATUS_HIGH_TEMPERATURE BIT(6)
#define NPM1300_CHG_STATUS_SUPLEMENT BIT(7)

static int npm1300_power_manager_read_status(power_manager_status_t *status, uint16_t *voltage, char *buf, size_t len)
{
   power_manager_status_t current_status = POWER_UNKNOWN;
   struct sensor_value value = {0, 0};
   int ret = 0;
   int index = 0;

   if (!device_is_ready(npm1300_charger_dev)) {
      LOG_WRN("NPM1300 charger not ready!");
      return -ENOTSUP;
   }

   ret = sensor_sample_fetch_chan(npm1300_charger_dev, SENSOR_CHAN_NPM1300_CHARGER_STATUS);
   if (ret < 0) {
      LOG_WRN("NPM1300 fetch status failed, %d (%s)!", ret, strerror(-ret));
      return ret;
   }

   ret = sensor_channel_get(npm1300_charger_dev, SENSOR_CHAN_NPM1300_CHARGER_STATUS, &value);
   if (ret < 0) {
      LOG_WRN("NPM1300 get status failed, %d (%s)!", ret, strerror(-ret));
      return ret;
   }
   LOG_DBG("NPM1300 status 0x%02x", value.val1);
   if (buf && len && value.val1) {
      index += snprintf(&buf[index], len - index, " 0x%02x", value.val1);
      ret = index;
   } else {
      ret = 0;
   }
   if (value.val1 & NPM1300_CHG_STATUS_HIGH_TEMPERATURE) {
      LOG_WRN("NPM1300 status high temperature");
   }
   if (value.val1 & NPM1300_CHG_STATUS_BATTERY_DETECTED) {
      LOG_DBG("NPM1300 status battery");
      if (value.val1 & NPM1300_CHG_STATUS_COMPLETED) {
         LOG_DBG("NPM1300 status battery full");
         current_status = CHARGING_COMPLETED;
      } else if (value.val1 & NPM1300_CHG_STATUS_TRICKLE) {
         LOG_DBG("NPM1300 status battery trickle");
         current_status = CHARGING_TRICKLE;
      } else if (value.val1 & NPM1300_CHG_STATUS_CURRENT) {
         LOG_DBG("NPM1300 status battery current");
         current_status = CHARGING_I;
      } else if (value.val1 & NPM1300_CHG_STATUS_VOLTAGE) {
         LOG_DBG("NPM1300 status battery voltage");
         current_status = CHARGING_V;
      } else {
         LOG_DBG("NPM1300 status from battery");
         current_status = FROM_BATTERY;
      }
   } else {
      current_status = FROM_BATTERY;
#ifdef CONFIG_MFD_NPM1300
      uint8_t usb_status = 0;
      if (!npm1300_mfd_detect_usb(&usb_status, false)) {
         if (usb_status) {
            current_status = FROM_EXTERNAL;
            if (buf && len) {
               index += snprintf(&buf[index], len - index, " usb 0x%02x", usb_status);
               ret = index;
            }
         }
      }
#endif /* CONFIG_MFD_NPM1300 */
      LOG_DBG("NPM1300 status not charging, USB %sconnected", current_status == FROM_EXTERNAL ? "" : "not ");
   }
   if (status) {
      *status = current_status;
   }
   if (voltage) {
      int ret2 = sensor_sample_fetch_chan(npm1300_charger_dev, SENSOR_CHAN_GAUGE_VOLTAGE);
      if (ret2 < 0) {
         LOG_WRN("NPM1300 fetch gauge voltage failed, %d (%s)!", ret2, strerror(-ret2));
      } else {
         ret2 = sensor_channel_get(npm1300_charger_dev, SENSOR_CHAN_GAUGE_VOLTAGE, &value);
         if (ret2 < 0) {
            LOG_WRN("NPM1300 get gauge voltage failed, %d (%s)!", ret2, strerror(-ret2));
         } else {
            int milliVolt = value.val1 * 1000 + value.val2 / 1000;
            LOG_DBG("NPM1300 gauge voltage %d mV", milliVolt);
            if (voltage) {
               *voltage = (uint16_t)milliVolt;
            }
         }
      }
      if (ret2 < 0) {
         ret = ret2;
      }
      npm1300_power_manager_read_temperatures();
   }
   return ret;
}
#else /* DT_NODE_HAS_STATUS(DT_NODELABEL(npm1300_charger), okay) */
#undef CONFIG_NPM1300_CHARGER
#endif /* DT_NODE_HAS_STATUS(DT_NODELABEL(npm1300_charger), okay) */
#endif /* CONFIG_NPM1300_CHARGER */

#ifdef CONFIG_INA219

#include <zephyr/drivers/sensor.h>

#define PM_NODE_0 DT_NODELABEL(ina219_0)
#define PM_NODE_1 DT_NODELABEL(ina219_1)

static const struct device *const ina219_0 = DEVICE_DT_GET_OR_NULL(PM_NODE_0);
static const struct device *const ina219_1 = DEVICE_DT_GET_OR_NULL(PM_NODE_1);

static int power_manager_read_ina219(uint16_t *voltage, int16_t *current, uint16_t *power)
{
   int rc;
   struct sensor_value value;
   const struct device *ina219 = NULL;

   if (device_is_ready(ina219_0)) {
      ina219 = ina219_0;
   } else if (device_is_ready(ina219_1)) {
      ina219 = ina219_1;
   }

   if (!ina219) {
      if (ina219_0 == NULL && ina219_1 == NULL) {
         LOG_WRN("No INA219 device available.");
      } else {
         if (ina219_0) {
            LOG_WRN("%s device is not ready.", ina219_0->name);
         }
         if (ina219_1) {
            LOG_WRN("%s device is not ready.", ina219_1->name);
         }
      }
      return -EINVAL;
   }

   rc = sensor_sample_fetch(ina219);
   if (rc) {
      LOG_WRN("Device %s could not fetch sensor data.", ina219->name);
   } else {
      uint16_t vol = PM_INVALID_VOLTAGE;
      uint16_t pow = PM_INVALID_POWER;
      int16_t cur = PM_INVALID_CURRENT;
      rc = sensor_channel_get(ina219, SENSOR_CHAN_VOLTAGE, &value);
      if (rc) {
         LOG_WRN("Device %s could not get voltage.", ina219->name);
      } else {
         vol = sensor_value_to_double(&value) * 1000;
         LOG_DBG("Ext. voltage %u mV.", vol);
      }
      if (voltage) {
         *voltage = vol;
      }
      rc = sensor_channel_get(ina219, SENSOR_CHAN_CURRENT, &value);
      if (rc) {
         LOG_WRN("Device %s could not get current.\n", ina219->name);
      } else {
         cur = sensor_value_to_double(&value) * 1000;
         LOG_DBG("Ext. current %d mA.", cur);
      }
      if (current) {
         *current = cur;
      }
      rc = sensor_channel_get(ina219, SENSOR_CHAN_POWER, &value);
      if (rc) {
         LOG_WRN("Device %s could not get power.\n", ina219->name);
      } else {
         pow = sensor_value_to_double(&value) * 1000;
         LOG_DBG("Ext. power %u mW.", pow);
      }
      if (power) {
         *power = pow;
      }
   }

   pm_device_action_run(ina219, PM_DEVICE_ACTION_SUSPEND);
   expansion_port_power(false);

   return rc;
}
#endif /* CONFIG_INA219 */

int power_manager_init(void)
{
   int rc = -ENOTSUP;
   int64_t now = k_uptime_get();

   calculate_forecast(&now, PM_RESET_INTERNAL_LEVEL, NULL);

   if (device_is_ready(uart_dev)) {
#ifndef CONFIG_UART_CONSOLE
      power_manager_suspend_device(uart_dev);
#elif defined(CONFIG_UART_ASYNC_API) && !defined(CONFIG_UART_RECEIVER)
      uart_rx_disable(uart_dev);
#endif /* CONFIG_UART_ASYNC_API  && !CONFIG_UART_RECEIVER */
   } else {
#if defined(CONFIG_SUSPEND_UART) && defined(CONFIG_UART_CONSOLE) && !defined(CONFIG_CONSOLE_SUBSYS)
      LOG_WRN("UART0 console not available.");
#endif
   }

#if defined(CONFIG_SERIAL) && !defined(CONFIG_NRF_MODEM_LIB_TRACE) && DT_HAS_CHOSEN(nordic_modem_trace_uart)
#if (DT_NODE_HAS_STATUS(DT_CHOSEN(nordic_modem_trace_uart), okay))
   power_manager_suspend_device(DEVICE_DT_GET(DT_CHOSEN(nordic_modem_trace_uart)));
#endif /* DT_NODE_HAS_STATUS(DT_CHOSEN(nordic_modem_trace_uart), okay) */
#endif /* !CONFIG_SERIAL && !CONFIG_NRF_MODEM_LIB_TRACE && DT_HAS_CHOSEN(nordic_modem_trace_uart) */

#ifdef CONFIG_INA219
   if (device_is_ready(ina219_0)) {
      power_manager_add_device(ina219_0);
   } else if (device_is_ready(ina219_1)) {
      power_manager_add_device(ina219_1);
   }
#endif

#ifdef CONFIG_ADP536X_POWER_MANAGEMENT
   rc = adp536x_power_manager_init();
#endif
#ifdef CONFIG_MFD_NPM1300
   rc = npm1300_mfd_init();
#endif

   pm_init = true;
   rc = power_manager_voltage(NULL);
   if (rc) {
      LOG_WRN("Read battery voltage failed %d (%s).", rc, strerror(-rc));
      pm_init = rc == -ESTALE;
   }

   return rc;
}

int power_manager_3v3(bool enable)
{
#ifdef CONFIG_ADP536X_POWER_MANAGEMENT
   return adp536x_power_manager_xvy(ADP536X_I2C_REG_BUCK_BOOST_CONFIG, enable);
#else
   (void)enable;
   return 0;
#endif
}

int power_manager_1v8(bool enable)
{
#ifdef CONFIG_ADP536X_POWER_MANAGEMENT
   return adp536x_power_manager_xvy(ADP536X_I2C_REG_BUCK_CONFIG, enable);
#else
   (void)enable;
   return 0;
#endif
}

int power_manager_add_device(const struct device *dev)
{
   if (dev) {
      enum pm_device_state state = PM_DEVICE_STATE_OFF;
      int rc = pm_device_state_get(dev, &state);
      if (rc) {
         return rc;
      }
      if (pm_dev_counter >= MAX_PM_DEVICES) {
         return -ENOMEM;
      }
      pm_dev_table[pm_dev_counter] = dev;
      pm_dev_counter++;
      LOG_INF("PM add %s", dev->name);
   }
   return 0;
}

int power_manager_suspend_device(const struct device *dev)
{
   if (dev) {
      int rc = pm_device_action_run(dev, PM_DEVICE_ACTION_SUSPEND);
      if (rc) {
         return rc;
      }
      LOG_INF("PM suspended %s", dev->name);
   }
   return 0;
}

static int power_manager_apply(void)
{
   bool suspend = pm_suspend && (pm_pluse_end - sys_clock_tick_get()) < 0;

   if (pm_suspended != suspend) {
      pm_suspended = suspend;
#ifdef CONFIG_ADP536X_POWER_MANAGEMENT
#ifdef CONFIG_SUSPEND_3V3
#ifndef CONFIG_SUSPEND_UART
      if (suspend) {
         LOG_INF("Suspend 3.3V");
      } else {
         LOG_INF("Resume 3.3V");
      }
#endif
#endif
#endif
#ifdef CONFIG_BATTERY_VOLTAGE_SOURCE_ADC
      if (suspend) {
         battery_measure_enable(false);
      }
#endif
      suspend_devices(suspend);
#ifdef CONFIG_SUSPEND_UART
      suspend_uart(suspend);
#endif
#ifdef CONFIG_SUSPEND_3V3
      power_manager_3v3(!suspend);
#endif
#ifndef MFD_NPM1300_BUCK2_WITH_USB_INT
#ifdef CONFIG_MFD_NPM1300_BUCK2_WITH_USB
      npm1300_mfd_detect_usb(NULL, true);
#endif /* CONFIG_MFD_NPM1300_BUCK2_WITH_USB */
#endif /* MFD_NPM1300_BUCK2_WITH_USB_INT */
   }
   return 0;
}

int power_manager_suspend(bool enable)
{
   int res = 0;

   k_mutex_lock(&pm_mutex, K_FOREVER);
   pm_suspend = enable;
   res = power_manager_apply();
   k_mutex_unlock(&pm_mutex);

   return res;
}

static void power_management_suspend_fn(struct k_work *work)
{
   k_mutex_lock(&pm_mutex, K_FOREVER);
   power_manager_apply();
   k_mutex_unlock(&pm_mutex);
}

static K_WORK_DELAYABLE_DEFINE(power_management_suspend_work, power_management_suspend_fn);

int power_manager_pulse(k_timeout_t time)
{
   int res = 0;
   k_ticks_t end = time.ticks + sys_clock_tick_get() - K_MSEC(50).ticks;
   k_mutex_lock(&pm_mutex, K_FOREVER);
   if ((end - pm_pluse_end) > 0) {
      pm_pluse_end = end;
      work_reschedule_for_io_queue(&power_management_suspend_work, time);
   }
   res = power_manager_apply();
   k_mutex_unlock(&pm_mutex);
   return res;
}

int power_manager_voltage(uint16_t *voltage)
{
   int rc = -ENOTSUP;

   if (pm_init) {
      bool charger = false;
      uint16_t internal_voltage = PM_INVALID_VOLTAGE;
      uint16_t charger_voltage = PM_INVALID_VOLTAGE;
      int64_t now = k_uptime_get();
      int64_t time = VOLTAGE_MIN_INTERVAL_MILLIS;

#ifdef CONFIG_ADP536X_POWER_MANAGEMENT
      {
         power_manager_status_t status = POWER_UNKNOWN;
         adp536x_power_manager_read_status(&status);
         if (FROM_BATTERY != status) {
            rc = adp536x_power_manager_voltage(&charger_voltage);
            if (!rc) {
               LOG_INF("ADP536X %u mV", charger_voltage);
               charger = true;
            }
         }
      }
#elif CONFIG_NPM1300_CHARGER
      {
         power_manager_status_t status = POWER_UNKNOWN;
         rc = npm1300_power_manager_read_status(&status, &charger_voltage, NULL, 0);
         if (rc >= 0 && FROM_BATTERY != status && FROM_EXTERNAL != status) {
            LOG_INF("NPM1300 %u mV", charger_voltage);
            charger = true;
         }
      }
#endif

      k_mutex_lock(&pm_mutex, K_FOREVER);
      if (!charger && !last_voltage_charger && last_voltage_uptime) {
         time = now - last_voltage_uptime;
      }
      internal_voltage = last_voltage;
      k_mutex_unlock(&pm_mutex);

      if (time < VOLTAGE_MIN_INTERVAL_MILLIS) {
         rc = 0;
         LOG_DBG("Last %u mV", internal_voltage);
      } else {
         if (charger) {
            internal_voltage = charger_voltage;
            rc = 0;
         } else {
            internal_voltage = PM_INVALID_VOLTAGE;
#if defined(CONFIG_BATTERY_VOLTAGE_SOURCE_ADC)
            rc = battery_sample(&internal_voltage);
            LOG_DBG("ADC %u mV", internal_voltage);
#elif defined(CONFIG_BATTERY_VOLTAGE_SOURCE_INA219)
            rc = power_manager_read_ina219(&internal_voltage, NULL, NULL);
            LOG_DBG("INA219 %u mV", internal_voltage);
#else
            char buf[32];

            rc = modem_at_lock_no_warn(K_NO_WAIT);
            if (!rc) {
               rc = modem_at_cmd(buf, sizeof(buf), "%XVBAT: ", "AT%XVBAT");
               modem_at_unlock();
            }
            if (rc < 0) {
               if (rc == -EBUSY) {
                  LOG_DBG("Failed to read battery level from modem, modem is busy!");
               } else {
                  LOG_DBG("Failed to read battery level from modem! %d (%s)", rc, strerror(-rc));
               }
            } else {
               internal_voltage = atoi(buf);
               LOG_INF("Modem %u mV", internal_voltage);
               rc = 0;
            }
#endif
         }
         if (!rc) {
            k_mutex_lock(&pm_mutex, K_FOREVER);
            if (1000 < internal_voltage) {
               internal_voltage = calculate_linear_regresion(&now, internal_voltage);
               last_voltage_uptime = k_uptime_get();
               last_voltage = internal_voltage;
               last_voltage_charger = charger;
            } else {
               // drop too low voltages
               internal_voltage = last_voltage;
            }
            k_mutex_unlock(&pm_mutex);
         }
      }
      if (!rc && voltage) {
         *voltage = internal_voltage;
      }
   }
   return rc;
}

int power_manager_voltage_ext(uint16_t *voltage)
{
   int rc = -ENODEV;
#ifdef CONFIG_BATTERY_ADC
   rc = battery2_sample(voltage);
#elif defined(CONFIG_INA219) && !defined(CONFIG_INA219_MODE_POWER_MANAGER)
   rc = power_manager_read_ina219(voltage, NULL, NULL);
#endif
   return rc;
}

int power_manager_ext(uint16_t *voltage, int16_t *current, uint16_t *power)
{
   int rc = -ENODEV;
#ifdef CONFIG_BATTERY_ADC
   rc = battery2_sample(voltage);
#elif defined(CONFIG_INA219) && !defined(CONFIG_INA219_MODE_POWER_MANAGER)
   rc = power_manager_read_ina219(voltage, current, power);
#endif
   return rc;
}

int power_manager_status(uint8_t *level, uint16_t *voltage, power_manager_status_t *status, int16_t *forecast)
{
   int rc = -ENOTSUP;

   if (pm_init) {
      uint16_t internal_voltage = PM_INVALID_VOLTAGE;

      LOG_DBG("Read battery monitor status ...");

      rc = power_manager_voltage(&internal_voltage);
      if (!rc) {
         int64_t now = k_uptime_get();
         power_manager_status_t internal_status = POWER_UNKNOWN;
         int16_t days = -1;
         uint16_t internal_level = PM_INVALID_INTERNAL_LEVEL;

#ifdef CONFIG_ADP536X_POWER_MANAGEMENT
         adp536x_power_manager_read_status(&internal_status);
#endif
#ifdef CONFIG_NPM1300_CHARGER
         npm1300_power_manager_read_status(&internal_status, NULL, NULL, 0);
#endif
         internal_level = transform_curve(internal_voltage, pm_get_battery_profile()->curve);

         days = calculate_forecast(&now, internal_level, &internal_status);

         if (internal_level < 25500) {
            internal_level /= 100;
         } else {
            internal_level = PM_INVALID_LEVEL;
         }
         if (level) {
            *level = (uint8_t)internal_level;
         }
         if (voltage) {
            *voltage = internal_voltage;
         }
         if (status) {
            *status = internal_status;
         }
         if (forecast) {
            *forecast = days;
         }
         LOG_DBG("%u%% %umV %d (%d left days)", internal_level, internal_voltage, internal_status, days);
      } else {
         LOG_WRN("Read battery status failed %d (%s).", rc, strerror(-rc));
      }
   } else {
      LOG_WRN("Failed to read initial battery status!");
   }
   return rc;
}

int power_manager_status_desc(char *buf, size_t len)
{
   power_manager_status_t battery_status = POWER_UNKNOWN;
   int index = 0;
   uint16_t battery_voltage = PM_INVALID_VOLTAGE;
   int16_t battery_forecast = -1;
   uint8_t battery_level = PM_INVALID_LEVEL;

   power_manager_status(&battery_level, &battery_voltage, &battery_status, &battery_forecast);
   if (battery_voltage < PM_INVALID_VOLTAGE) {
      index += snprintf(buf, len, "%u mV", battery_voltage);
      if (battery_level < PM_INVALID_LEVEL) {
         index += snprintf(buf + index, len - index, " %u%%", battery_level);
      }
      const struct battery_profile *profile = pm_get_battery_profile();
      if (profile->name || battery_forecast >= 0) {
         index += snprintf(buf + index, len - index, " (");
         if (profile->name) {
            index += snprintf(buf + index, len - index, "%s", profile->name);
            if (battery_forecast >= 0) {
               index += snprintf(buf + index, len - index, ", ");
            }
         }
         if (battery_forecast > 1 || battery_forecast == 0) {
            index += snprintf(buf + index, len - index, "%u days left", battery_forecast);
         } else if (battery_forecast == 1) {
            index += snprintf(buf + index, len - index, "1 day left");
         }
         index += snprintf(buf + index, len - index, ")");
      }
      const char *msg = "";
      switch (battery_status) {
         case FROM_BATTERY:
            msg = "battery";
            break;
         case CHARGING_TRICKLE:
            msg = "charging (trickle)";
            break;
         case CHARGING_I:
            msg = "charging (I)";
            break;
         case CHARGING_V:
            msg = "charging (V)";
            break;
         case CHARGING_S:
            msg = "charging";
            break;
         case CHARGING_COMPLETED:
            msg = "full";
            break;
         case FROM_EXTERNAL:
            msg = "external";
            break;
         default:
            break;
      }
      if (msg[0]) {
         index += snprintf(buf + index, len - index, " %s", msg);
      }
#ifdef CONFIG_NPM1300_CHARGER
      int rc = npm1300_power_manager_read_status(&battery_status, NULL, buf + index, len - index);
      if (rc > 0) {
         index += rc;
      }
#endif
   }
   return index;
}

#ifdef CONFIG_SH_CMD

static int sh_cmd_battery(const char *parameter)
{
   (void)parameter;
   char buf[128];
   size_t length = sizeof(buf);
   int index = 0;
   uint16_t battery_voltage = PM_INVALID_VOLTAGE;
   int16_t battery_current = PM_INVALID_CURRENT;
   uint16_t battery_power = PM_INVALID_POWER;

   if (power_manager_status_desc(buf, sizeof(buf))) {
      LOG_INF("%s", buf);
   }
   if (!power_manager_ext(&battery_voltage, &battery_current, &battery_power)) {
      index = snprintf(buf, length, "Ext.Bat.: ");
      if (battery_voltage != PM_INVALID_VOLTAGE) {
         index += snprintf(buf + index, length - index, "%u mV ", battery_voltage);
      }
      if (battery_current != PM_INVALID_CURRENT) {
         index += snprintf(buf + index, length - index, "%d mA ", battery_current);
      }
      if (battery_power != PM_INVALID_POWER) {
         index += snprintf(buf + index, length - index, "%u mW", battery_power);
      }
      LOG_INF("%s", buf);
   }
   return 0;
}

static int sh_cmd_battery_forecast_reset(const char *parameter)
{
   (void)parameter;
   int64_t now = 0;

   k_mutex_lock(&pm_mutex, K_FOREVER);
   last_voltage_uptime = 0;
   last_voltage = PM_INVALID_VOLTAGE;
   now = k_uptime_get();
   calculate_forecast(&now, PM_RESET_INTERNAL_LEVEL, NULL);
   k_mutex_unlock(&pm_mutex);

   return 0;
}

static int sh_cmd_battery_forecast_reset_threshold(const char *parameter)
{
   int res = 0;
   const char *cur = parameter;
   char value[10];

   memset(value, 0, sizeof(value));
   cur = parse_next_text(cur, ' ', value, sizeof(value));

   if (value[0]) {
      uint32_t threshold = 0;
      res = sscanf(value, "%u", &threshold);
      if (res == 1) {
         if (threshold > 9999) {
            LOG_INF("%u invalid value for battery forecast reset threshold [0...9999].", threshold);
            res = -EINVAL;
         }
         battery_reset_threshold = threshold;
         res = 0;
         cur = "set ";
      } else {
         res = -EINVAL;
      }
   } else {
      cur = "";
   }
   if (!res) {
      if (!battery_reset_threshold) {
         LOG_INF("%sno battery forecast reset threshold.", cur);
      } else {
         LOG_INF("%sbattery forecast reset threshold %us [1-9999]", cur, battery_reset_threshold);
      }
   }
   return res;
}

static void sh_cmd_battery_forecast_reset_threshold_help(void)
{
   LOG_INF("> help batrstth:");
   LOG_INF("  batrstth         : read battery forecast reset threshold.");
   LOG_INF("  batrstth <level> : set battery forecast reset threshold. 0 disabled.");
   LOG_INF("                   : 1-9999 threshold for level up to reset the forecast.");
}

#ifdef CONFIG_BATTERY_VOLTAGE_SOURCE_MODEM
SH_CMD(bat, "", "read battery status.", sh_cmd_battery, NULL, 0);
#else
SH_CMD(bat, NULL, "read battery status.", sh_cmd_battery, NULL, 0);
#endif
SH_CMD(batreset, NULL, "reset battery forecast.", sh_cmd_battery_forecast_reset, NULL, 0);
SH_CMD(batrstth, NULL, "set battery forecast reset threshold.", sh_cmd_battery_forecast_reset_threshold, sh_cmd_battery_forecast_reset_threshold_help, 0);
#endif /* CONFIG_SH_CMD */
