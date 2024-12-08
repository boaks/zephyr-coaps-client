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
#include "io_job_queue.h"
#include "modem_at.h"
#include "power_manager.h"
#include "sh_cmd.h"
#include "transform.h"

#ifdef CONFIG_BATTERY_ADC
#include "battery_adc.h"
#endif /* CONFIG_BATTERY_ADC */

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

typedef const struct device *t_devptr;

#if DT_NODE_HAS_STATUS(DT_NODELABEL(i2c1), okay) && defined(CONFIG_DISABLE_REALTIME_CLOCK)

#define REALTIME_CLOCK_ADDR 0x51

static inline void power_manager_suspend_realtime_clock()
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
}
#else
static inline void power_manager_suspend_realtime_clock()
{
   // empty
}
#endif

#define PM_INVALID_INTERNAL_LEVEL 0xffff

#define VOLTAGE_MIN_INTERVAL_MILLIS 10000
#define MAX_PM_DEVICES 10

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

static const struct device *const uart_dev = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_console));

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
    .points = 9,
    .curve = {
        {4200, 10000},
        {4056, 8750},
        {3951, 7500},
        {3810, 5440},
        {3762, 4110},
        {3738, 2650},
        {3637, 1470},
        {3474, 440},
        {3200, 0},
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

/*
 * mimimum uptime to calculate forecast after running from battery.
 */
static int64_t min_forecast_uptime = 0;

/*
 * first_battery_level is set, when the first complete
 * battery level epoch is detected. The very first change
 * indicates only a partitial epoch.
 */
static uint16_t first_battery_level = PM_INVALID_INTERNAL_LEVEL;
static int64_t first_battery_level_uptime = 0;
/**
 * Last battery level period.
 */
static uint16_t last_battery_level = PM_INVALID_INTERNAL_LEVEL;
static int64_t last_battery_level_uptime = 0;
/*
 * filter battery levels and record changes.
 */
static uint16_t current_battery_level = PM_INVALID_INTERNAL_LEVEL;
static uint16_t current_battery_changes = 0;

/*
 * last left battery time.
 */
static int64_t last_battery_left_time = 0;

#define MSEC_PER_HOUR (MSEC_PER_SEC * 60 * 60)
#define MSEC_PER_DAY (MSEC_PER_SEC * 60 * 60 * 24)

/*
 * Minimum battery level delta for new forecast calculation.
 * Value in 0.01%
 */
#define MINIMUM_BATTERY_LEVEL_DELTA 20

static int16_t calculate_left_time(const char *tag, int64_t *now, uint16_t battery_level, int64_t *past, uint16_t battery_level_past, int64_t *time)
{
   int diff = battery_level_past - battery_level;
   if (diff == 0) {
      // diff should never be 0, just to ensure, not to fail by div 0
      return -1;
   } else {
      int64_t passed_time = *now - *past;
      int64_t left_time = (passed_time * battery_level) / diff;
      LOG_DBG("%s: left battery %u%% time %lld (%lld days, %lld passed)",
              tag, battery_level, left_time,
              left_time / MSEC_PER_DAY, passed_time / MSEC_PER_DAY);
      *time += left_time;
      return 0;
   }
}

static int16_t calculate_forecast(int64_t *now, uint16_t battery_level, power_manager_status_t status)
{
   int16_t res = -1;

   if (battery_level == PM_INVALID_INTERNAL_LEVEL) {
      LOG_DBG("forecast: not ready.");
   } else if (status != FROM_BATTERY) {
      LOG_DBG("forecast: charging.");
   } else {
      res = 0;
   }
   if (res < 0) {
      // fall through, reset
      current_battery_changes = 0;
      first_battery_level = PM_INVALID_INTERNAL_LEVEL;
      last_battery_level = PM_INVALID_INTERNAL_LEVEL;
      current_battery_level = PM_INVALID_INTERNAL_LEVEL;
      // wait 1h after running from battery
      min_forecast_uptime = *now + MSEC_PER_HOUR;
      return res;
   }

   if (((*now) - min_forecast_uptime) < 0) {
      // wait at least 1h after running from battery
      return -1;
   } else {
      int64_t passed_time = (*now) - last_battery_level_uptime;

      if ((current_battery_level - battery_level) > MINIMUM_BATTERY_LEVEL_DELTA) {
         // battery level changed since last forecast

         current_battery_level = battery_level;
         ++current_battery_changes;
         if (current_battery_changes == 1) {
            // initial value;
            return -1;
         }

         if (current_battery_changes == 2) {
            // first battery level change since start
            LOG_DBG("first battery level change");
            first_battery_level = battery_level;
            first_battery_level_uptime = *now;
            last_battery_level = battery_level;
            last_battery_level_uptime = *now;
            return -1;
         }

         if (passed_time >= MSEC_PER_DAY) {
            // change after a minium of 24h
            last_battery_left_time = 0;
            calculate_left_time("All periods", now, battery_level, &first_battery_level_uptime, first_battery_level, &last_battery_left_time);
            last_battery_left_time *= 2;
            calculate_left_time("Last period", now, battery_level, &last_battery_level_uptime, last_battery_level, &last_battery_left_time);
            last_battery_left_time /= 3;
            passed_time = 0;
         } else if (current_battery_changes == 3) {
            // first full battery-level period
            last_battery_left_time = 0;
            calculate_left_time("First period", now, battery_level, &last_battery_level_uptime, last_battery_level, &last_battery_left_time);
            passed_time = 0;
         }
         if (passed_time == 0) {
            last_battery_level = battery_level;
            last_battery_level_uptime = *now;
         }
      }
      if (current_battery_changes >= 3) {
         // after last change
         passed_time += (MSEC_PER_DAY / 2);
         int16_t time = (int16_t)((last_battery_left_time - passed_time + MSEC_PER_DAY) / MSEC_PER_DAY);
         LOG_DBG("battery %u%%, %d left days (passed %d days)", battery_level, time, (int)(passed_time / MSEC_PER_DAY));
         return time;
      }
      return -1;
   }
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

#ifdef CONFIG_BATTERY_VOLTAGE_SOURCE_ADP536X
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
#endif

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

#ifdef CONFIG_ADP536X_POWER_MANAGEMENT_LOW_POWER
static void adp536x_power_management_sleep_mode_work_fn(struct k_work *work)
{
   /*
    * 0x5B: 11%, 10mA, 8 min, sleep, enable
    */
   adp536x_reg_write(ADP536X_I2C_REG_FUEL_GAUGE_MODE, 0x5B);
}

static K_WORK_DELAYABLE_DEFINE(adp536x_power_management_sleep_mode_work, adp536x_power_management_sleep_mode_work_fn);
#endif /* CONFIG_ADP536X_POWER_MANAGEMENT_LOW_POWER */

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

int adp536x_power_manager_init(void)
{
   int rc = -ENOTSUP;

   if (device_is_ready(i2c_spec.bus)) {
      uint8_t value = 0;

      adp536x_reg_read(ADP536X_I2C_REG_CHARGE_TERMINATION, &value);
      value &= 3;
      value |= 0x80; // 4.2V
      adp536x_reg_write(ADP536X_I2C_REG_CHARGE_TERMINATION, value);
      /*
       * 0x51: 11%, 10mA, enable
       */
      adp536x_reg_write(ADP536X_I2C_REG_FUEL_GAUGE_MODE, 0x51);
      LOG_INF("Battery monitor initialized.");
#ifdef CONFIG_ADP536X_POWER_MANAGEMENT_LOW_POWER
      work_schedule_for_io_queue(&adp536x_power_management_sleep_mode_work, K_MINUTES(30));
#endif
      rc = 0;
   } else {
      LOG_WRN("Failed to initialize battery monitor.");
   }
   return rc;
}

#endif

#ifdef CONFIG_REGULATOR_NPM1300
#if (DT_NODE_HAS_STATUS(DT_NODELABEL(npm1300_buck2), okay))

#include "ui.h"
#include <zephyr/drivers/regulator.h>

static const struct device *npm1300_buck2_dev = DEVICE_DT_GET(DT_NODELABEL(npm1300_buck2));

static int npm1300_buck2_suspend(bool suspend)
{
   int ret = 0;

   if (!device_is_ready(npm1300_buck2_dev)) {
      LOG_WRN("NPM1300 buck2 not ready!");
      return -ENOTSUP;
   }
   if (suspend) {
      ret = regulator_disable(npm1300_buck2_dev);
      if (ret < 0) {
         LOG_WRN("NPM1300 disable buck2 failed, %d (%s)!", ret, strerror(-ret));
#ifdef CONFIG_MFD_NPM1300_BUCK2_LED
      } else {
         ui_led_op(LED_BUCK2, LED_CLEAR);
#endif
      }
   } else {
      ret = regulator_enable(npm1300_buck2_dev);
      if (ret < 0) {
         LOG_WRN("NPM1300 enable buck2 failed, %d (%s)!", ret, strerror(-ret));
#ifdef CONFIG_MFD_NPM1300_BUCK2_LED
      } else {
         ui_led_op(LED_BUCK2, LED_SET);
#endif
      }
   }

   return ret;
}
#else /* DT_NODE_HAS_STATUS(DT_NODELABEL(npm1300_buck2), okay) */
#undef CONFIG_REGULATOR_NPM1300
#undef CONFIG_MFD_NPM1300_BUCK2_WITH_USB
#endif /* DT_NODE_HAS_STATUS(DT_NODELABEL(npm1300_buck2), okay) */
#endif /* CONFIG_REGULATOR_NPM1300 */

#ifdef CONFIG_MFD_NPM1300
#if (DT_NODE_HAS_STATUS(DT_INST(0, nordic_npm1300), okay))

#include <zephyr/drivers/mfd/npm1300.h>

static const struct device *npm1300_mfd_dev = DEVICE_DT_GET(DT_INST(0, nordic_npm1300));

#define NPM1300_SYSREG_BASE 0x2
#define NPM1300_USBCDETECTSTATUS_OFFSET 0x5

#define NPM1300_CHGR_BASE 0x3
#define NPM1300_CHGR_OFFSET_DIS_SET 0x06

static int npm1300_mfd_detect_usb(uint8_t *usb, bool switch_regulator)
{
#ifndef CONFIG_REGULATOR_NPM1300
   (void)switch_regulator;
#endif
   int ret = 0;
   uint8_t status = 0;

   if (!device_is_ready(npm1300_mfd_dev)) {
      LOG_WRN("NPM1300 mfd not ready!");
      return -ENOTSUP;
   }

   ret = mfd_npm1300_reg_read(npm1300_mfd_dev, NPM1300_SYSREG_BASE, NPM1300_USBCDETECTSTATUS_OFFSET, &status);
   if (ret < 0) {
      LOG_WRN("NPM1300 read usb status failed, %d (%s)!", ret, strerror(-ret));
      return ret;
   } else {
      LOG_INF("NPM1300 USB 0x%x", status);
      if (usb) {
         *usb = status;
      }
#ifdef CONFIG_REGULATOR_NPM1300
      if (switch_regulator) {
         npm1300_buck2_suspend(!status);
      }
#endif
   }

   return ret;
}

static int npm1300_mfd_init(void)
{
   int ret = 0;

   if (!device_is_ready(npm1300_mfd_dev)) {
      LOG_WRN("NPM1300 mfd not ready!");
      return -ENOTSUP;
   }

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

#if (DT_NODE_HAS_STATUS(DT_INST(0, nordic_npm1300_charger), okay))

#include <zephyr/drivers/sensor/npm1300_charger.h>

/* nPM1300_PS_v1.1.pdf, 6.2.14.31 BCHGCHARGESTATUS, page 45 */
#define NPM1300_CHG_STATUS_BATTERY_DETECTED BIT(0)
#define NPM1300_CHG_STATUS_COMPLETED BIT(1)
#define NPM1300_CHG_STATUS_TRICKLE BIT(2)
#define NPM1300_CHG_STATUS_CURRENT BIT(3)
#define NPM1300_CHG_STATUS_VOLTAGE BIT(4)
#define NPM1300_CHG_STATUS_RECHARGE BIT(5)
#define NPM1300_CHG_STATUS_HIGH_TEMPERATURE BIT(6)
#define NPM1300_CHG_STATUS_SUPLEMENT BIT(7)

static const struct device *npm1300_charger_dev = DEVICE_DT_GET(DT_INST(0, nordic_npm1300_charger));

static int npm1300_power_manager_read_status(power_manager_status_t *status, char *buf, size_t len)
{
   struct sensor_value value;
   int ret = 0;
   int index = 0;

   if (!device_is_ready(npm1300_charger_dev)) {
      LOG_WRN("NPM1300 charger not ready!");
      return -ENOTSUP;
   }

   ret = sensor_sample_fetch_chan(npm1300_charger_dev, SENSOR_CHAN_NPM1300_CHARGER_STATUS);
   if (ret < 0) {
      LOG_WRN("NPM1300 fetch channel failed, %d (%s)!", ret, strerror(-ret));
      return ret;
   }

   ret = sensor_channel_get(npm1300_charger_dev, SENSOR_CHAN_NPM1300_CHARGER_STATUS, &value);
   if (ret < 0) {
      LOG_WRN("NPM1300 get channel failed, %d (%s)!", ret, strerror(-ret));
      return ret;
   }
   LOG_DBG("NPM1300 status 0x%02x", value.val1);
   if (buf && len && value.val1) {
      index += snprintf(&buf[index], len - index, " 0x%02x", value.val1);
      ret = index;
   } else {
      ret = 0;
   }
   if (value.val1 & NPM1300_CHG_STATUS_BATTERY_DETECTED) {
      LOG_DBG("NPM1300 status battery");
      if (value.val1 & NPM1300_CHG_STATUS_COMPLETED) {
         LOG_DBG("NPM1300 status battery full");
         *status = CHARGING_COMPLETED;
      } else if (value.val1 & NPM1300_CHG_STATUS_TRICKLE) {
         LOG_DBG("NPM1300 status battery trickle");
         *status = CHARGING_TRICKLE;
      } else if (value.val1 & NPM1300_CHG_STATUS_CURRENT) {
         LOG_DBG("NPM1300 status battery current");
         *status = CHARGING_I;
      } else if (value.val1 & NPM1300_CHG_STATUS_VOLTAGE) {
         LOG_DBG("NPM1300 status battery voltage");
         *status = CHARGING_V;
      } else {
         LOG_DBG("NPM1300 status from battery");
         *status = FROM_BATTERY;
      }
   } else {
      power_manager_status_t temp = FROM_BATTERY;
#ifdef CONFIG_MFD_NPM1300
      uint8_t usb_status = 0;
      if (!npm1300_mfd_detect_usb(&usb_status, false)) {
         if (usb_status) {
            temp = FROM_EXTERNAL;
            if (buf && len) {
               index += snprintf(&buf[index], len - index, " usb 0x%02x", usb_status);
               ret = index;
            }
         }
      }
#endif /* CONFIG_MFD_NPM1300 */
      *status = temp;
      LOG_DBG("NPM1300 status not charging, USB %sconnected", temp == FROM_EXTERNAL ? "" : "not ");
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

int power_manager_read_ina219(uint16_t *voltage, uint16_t *current)
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
      if (ina219_0) {
         LOG_WRN("Device %s is not ready.", ina219_0->name);
      } else if (ina219_1) {
         LOG_WRN("Device %s is not ready.", ina219_1->name);
      } else {
         LOG_WRN("Device INA219 is not available.");
      }
      return -EINVAL;
   }

   rc = sensor_sample_fetch(ina219);
   if (rc) {
      LOG_WRN("Device %s could not fetch sensor data.\n", ina219->name);
      return rc;
   }

   rc = sensor_channel_get(ina219, SENSOR_CHAN_VOLTAGE, &value);
   if (rc) {
      LOG_WRN("Device %s could not get voltage.\n", ina219->name);
   } else if (voltage) {
      *voltage = sensor_value_to_double(&value) * 1000;
   }
   rc = sensor_channel_get(ina219, SENSOR_CHAN_CURRENT, &value);
   if (rc) {
      LOG_WRN("Device %s could not get current.\n", ina219->name);
   } else if (current) {
      *current = sensor_value_to_double(&value) * 1000;
   }
   return rc;
}
#endif /* CONFIG_INA219 */

int power_manager_init(void)
{
   int rc = -ENOTSUP;
   int64_t now = k_uptime_get();

   calculate_forecast(&now, PM_INVALID_INTERNAL_LEVEL, CHARGING_TRICKLE);

   if (device_is_ready(uart_dev)) {
#if defined(CONFIG_UART_ASYNC_API) && !defined(CONFIG_UART_RECEIVER)
      uart_rx_disable(uart_dev);
#endif
   } else {
#if defined(CONFIG_SUSPEND_UART) && defined(CONFIG_UART_CONSOLE) && !defined(CONFIG_CONSOLE_SUBSYS)
      LOG_WRN("UART0 console not available.");
#endif
   }
   power_manager_suspend_realtime_clock();
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
   static int64_t last_time = 0;
   static uint16_t last_voltage = 0;

   int rc = -ENOTSUP;

   if (pm_init) {
      uint16_t internal_voltage;
      int64_t time;

      k_mutex_lock(&pm_mutex, K_FOREVER);
      time = last_time ? k_uptime_get() - last_time : VOLTAGE_MIN_INTERVAL_MILLIS;
      internal_voltage = last_voltage;
      k_mutex_unlock(&pm_mutex);

      if (time < VOLTAGE_MIN_INTERVAL_MILLIS) {
         rc = 0;
         LOG_DBG("Last %u mV", internal_voltage);
      } else {
         internal_voltage = PM_INVALID_VOLTAGE;

#ifdef CONFIG_BATTERY_VOLTAGE_SOURCE_ADP536X
         rc = adp536x_power_manager_voltage(&internal_voltage);
         LOG_DBG("ADP536X %u mV", internal_voltage);
#elif defined(CONFIG_BATTERY_VOLTAGE_SOURCE_ADC)
         rc = battery_sample(&internal_voltage);
         LOG_DBG("ADC %u mV", internal_voltage);
#elif defined(CONFIG_INA219_MODE_POWER_MANAGER)
         rc = power_manager_read_ina219(&internal_voltage, NULL);
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
            LOG_DBG("Modem %u mV", internal_voltage);
            rc = 0;
         }
#endif
         if (!rc) {
            k_mutex_lock(&pm_mutex, K_FOREVER);
            last_time = k_uptime_get();
            last_voltage = internal_voltage;
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
   rc = power_manager_read_ina219(voltage, NULL);
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
         power_manager_status_t internal_status_forecast = FROM_BATTERY;
         int16_t days = -1;
         uint16_t internal_level = PM_INVALID_INTERNAL_LEVEL;

#ifdef CONFIG_ADP536X_POWER_MANAGEMENT
         adp536x_power_manager_read_status(&internal_status);
         internal_status_forecast = internal_status;
#endif
#ifdef CONFIG_NPM1300_CHARGER
         npm1300_power_manager_read_status(&internal_status, NULL, 0);
         internal_status_forecast = internal_status;
#endif
         internal_level = transform_curve(internal_voltage, pm_get_battery_profile()->curve);
         days = calculate_forecast(&now, internal_level, internal_status_forecast);
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
      index += npm1300_power_manager_read_status(&battery_status, buf + index, len - index);
#endif
   }
   return index;
}

#ifdef CONFIG_SH_CMD

static int sh_cmd_battery(const char *parameter)
{
   (void)parameter;
   char buf[128];
   uint16_t battery_voltage = PM_INVALID_VOLTAGE;

   if (power_manager_status_desc(buf, sizeof(buf))) {
      LOG_INF("%s", buf);
   }
   if (!power_manager_voltage_ext(&battery_voltage)) {
      LOG_INF("Ext.Bat.: %u mV", battery_voltage);
   }
   return 0;
}

#ifdef CONFIG_BATTERY_VOLTAGE_SOURCE_MODEM
SH_CMD(bat, "", "read battery status.", sh_cmd_battery, NULL, 0);
#else
SH_CMD(bat, NULL, "read battery status.", sh_cmd_battery, NULL, 0);
#endif
#endif /* CONFIG_SH_CMD */