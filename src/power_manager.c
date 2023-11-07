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
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/pm/device.h>

#include "io_job_queue.h"
#include "modem_at.h"
#include "power_manager.h"
#include "transform.h"
#include "uart_cmd.h"

#ifdef CONFIG_BATTERY_VOLTAGE_SOURCE_ADC
#include "battery_adc.h"
#endif /* CONFIG_BATTERY_VOLTAGE_SOURCE_ADC */

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

static K_MUTEX_DEFINE(pm_mutex);
typedef const struct device *t_devptr;

#define MAX_PM_DEVICES 10

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
static const struct transform_curve curve = {
#ifdef CONFIG_BATTERY_TYPE_LIPO_2000_MAH
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
    }
#elif defined(CONFIG_BATTERY_TYPE_ENELOOP_2000_MAH)
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
    }
#elif defined(CONFIG_BATTERY_TYPE_LIPO_1350_MAH)
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
    }
#else
    /* no battery */
    .points = 1,
    .curve = {
        {0, -1}}
#endif
};

/*
 * first_battery_level is set, when the first complete
 * battery level epoch is detected. The very first change
 * indicates only a partitial epoch.
 */
static int64_t min_forecast_uptime = 0;

static uint16_t first_battery_level = 0xffff;
static int64_t first_battery_level_uptime = 0;
static uint16_t last_battery_level = 0xffff;
static int64_t last_battery_level_uptime = 0;

static uint16_t current_battery_level = 0xffff;
static uint16_t current_battery_changes = 0;
/*
 * last left battery time. -1, if not available
 */
static int64_t last_battery_left_time = -1;

#define MSEC_PER_HOUR (MSEC_PER_SEC * 60 * 60)
#define MSEC_PER_DAY (MSEC_PER_SEC * 60 * 60 * 24)

static int16_t calculate_forecast(int64_t *now, uint16_t battery_level, power_manager_status_t status)
{
   if (battery_level == 0xffff) {
      LOG_DBG("forecast: not ready.");
   } else if (status != FROM_BATTERY) {
      LOG_DBG("forecast: charging.");
   } else {
      if (((*now) - min_forecast_uptime) >= 0) {
         int64_t passed_time = (*now) - last_battery_level_uptime;
         if (current_battery_level - battery_level > 20) {
            current_battery_level = battery_level;
            ++current_battery_changes;
            if (current_battery_changes == 1) {
               // initial value;
               return -1;
            }
            if (current_battery_changes == 2) {
               // first battery level change
               LOG_DBG("first battery level change");
               first_battery_level = battery_level;
               first_battery_level_uptime = *now;
               last_battery_level = battery_level;
               last_battery_level_uptime = *now;
               return -1;
            }
            if (passed_time >= MSEC_PER_DAY) {
               // change after a minium of 24h
               int64_t time;
               int diff = last_battery_level - battery_level;
               if (!diff) return -1;
               last_battery_left_time = (passed_time * battery_level) / diff;
               LOG_DBG("left battery %u%% time %lld (%lld days, %lld passed)",
                       battery_level, last_battery_left_time,
                       last_battery_left_time / MSEC_PER_DAY, passed_time / MSEC_PER_DAY);
               last_battery_level = battery_level;
               last_battery_level_uptime = *now;
               passed_time = 0;
               diff = first_battery_level - battery_level;
               if (!diff) return -1;
               passed_time = (*now) - first_battery_level_uptime;
               time = (passed_time * battery_level) / diff;
               LOG_DBG("left battery time 2 %lld (%lld days, %lld passed)", time,
                       time / MSEC_PER_DAY, passed_time / MSEC_PER_DAY);
               last_battery_left_time = (last_battery_left_time + time) / 2;
               passed_time = 0;
            } else if (current_battery_changes == 3) {
               // first change
               int diff = last_battery_level - battery_level;
               if (!diff) return -1;
               last_battery_left_time = (passed_time * battery_level) / diff;
               LOG_DBG("first left battery time %lld (%lld, d:%u%%, %u%%)", last_battery_left_time, passed_time, diff, battery_level);
            }
         }
         if (current_battery_changes >= 3) {
            // after first change
            passed_time += (MSEC_PER_DAY / 2);
            int16_t time = (int16_t)((last_battery_left_time - passed_time + MSEC_PER_DAY) / MSEC_PER_DAY);
            LOG_DBG("battery %u%%, %d left days (passed %d days)", battery_level, time, (int)(passed_time / MSEC_PER_DAY));
            return time;
         }
      }
      return -1;
   }
   // fall through, reset
   current_battery_changes = 0;
   first_battery_level = 0xffff;
   last_battery_level = 0xffff;
   current_battery_level = 0xffff;
   last_battery_left_time = -1;
   // first forecast 1h after start
   min_forecast_uptime = *now + MSEC_PER_HOUR;

   return -1;
}

#ifdef CONFIG_ADP536X_POWER_MANAGEMENT

#include <zephyr/drivers/i2c.h>

#define ADP536X_I2C_ADDR 0x46

#define ADP536X_I2C_REG_CHARGE_TERMINATION 0x3
#define ADP536X_I2C_REG_STATUS 0x8
#define ADP536X_I2C_REG_LEVEL 0x21
#define ADP536X_I2C_REG_VOLTAGE_HIGH_BYTE 0x25
#define ADP536X_I2C_REG_VOLTAGE_LOW_BYTE 0x26
#define ADP536X_I2C_REG_FUEL_GAUGE_MODE 0x27
#define ADP536X_I2C_REG_BUCK_CONFIG 0x29
#define ADP536X_I2C_REG_BUCK_BOOST_CONFIG 0x2B

static const struct device *const i2c_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c2));

static int adp536x_reg_read(uint8_t reg, uint8_t *buff)
{
   return i2c_reg_read_byte(i2c_dev, ADP536X_I2C_ADDR, reg, buff);
}

static int adp536x_reg_write(uint8_t reg, uint8_t val)
{
   return i2c_reg_write_byte(i2c_dev, ADP536X_I2C_ADDR, reg, val);
}

#ifdef CONFIG_BATTERY_VOLTAGE_SOURCE_ADP536X
static int adp536x_power_manager_voltage(uint16_t *voltage)
{
   int rc = -EIO;
   uint8_t value1 = 0xff;
   uint8_t value2 = 0xff;

   if (!adp536x_reg_read(ADP536X_I2C_REG_VOLTAGE_HIGH_BYTE, &value1) &&
       !adp536x_reg_read(ADP536X_I2C_REG_VOLTAGE_LOW_BYTE, &value2)) {
      uint16_t value = value1;
      value <<= 5;
      value |= ((value2 >> 3) & 0x1f);
      *voltage = value;
      rc = 0;
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

   if (device_is_ready(i2c_dev)) {
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

   if (device_is_ready(i2c_dev)) {
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

int power_manager_init(void)
{
   int rc = -ENOTSUP;
   int64_t now = k_uptime_get();

   calculate_forecast(&now, 0xffff, CHARGING_TRICKLE);

   if (device_is_ready(uart_dev)) {
#if defined(CONFIG_UART_ASYNC_API) && !defined(CONFIG_UART_RECEIVER)
      uart_rx_disable(uart_dev);
#endif
   } else {
#if defined(CONFIG_SUSPEND_UART) && defined(CONFIG_UART_CONSOLE) && !defined(CONFIG_CONSOLE_SUBSYS)
      LOG_WRN("UART0 console not available.");
#endif
   }

#ifdef CONFIG_ADP536X_POWER_MANAGEMENT
   rc = adp536x_power_manager_init();
#endif

   pm_init = true;
   rc = power_manager_voltage(NULL);

   pm_init = !rc;

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

int power_manager_suspend(bool enable)
{
#ifdef CONFIG_ADP536X_POWER_MANAGEMENT
#ifdef CONFIG_SUSPEND_3V3
#ifndef CONFIG_SUSPEND_UART
   if (enable) {
      LOG_INF("Suspend 3.3V");
   } else {
      LOG_INF("Resume 3.3V");
   }
#endif
#endif
#endif
   k_mutex_lock(&pm_mutex, K_FOREVER);
#ifdef CONFIG_BATTERY_VOLTAGE_SOURCE_ADC
   if (enable) {
      battery_measure_enable(false);
   }
#endif
   suspend_devices(enable);
#ifdef CONFIG_SUSPEND_UART
   suspend_uart(enable);
#endif
#ifdef CONFIG_SUSPEND_3V3
   power_manager_3v3(!enable);
#endif
   k_mutex_unlock(&pm_mutex);
   return 0;
}

int power_manager_voltage(uint16_t *voltage)
{
   int rc = -ENOTSUP;

   if (pm_init) {
      uint16_t internal_voltage = 0xffff;

#ifdef CONFIG_BATTERY_VOLTAGE_SOURCE_ADP536X
      rc = adp536x_power_manager_voltage(&internal_voltage);
      LOG_DBG("ADP536X %u mV", internal_voltage);
#elif defined(CONFIG_BATTERY_VOLTAGE_SOURCE_ADC)
      rc = battery_sample(&internal_voltage);
      LOG_DBG("ADC %u mV", internal_voltage);
#else
      char buf[32];
      rc = modem_at_cmd(buf, sizeof(buf), "%XVBAT: ", "AT%XVBAT");
      if (rc < 0) {
         if (rc == -EBUSY) {
            LOG_WRN("Failed to read battery level from modem, modem is busy!");
         } else {
            LOG_WRN("Failed to read battery level from modem! %d (%s)", rc, strerror(-rc));
         }
      } else {
         internal_voltage = atoi(buf);
         LOG_DBG("Modem %u mV", internal_voltage);
         rc = 0;
      }
#endif
      if (!rc && voltage) {
         *voltage = internal_voltage;
      }
   }
   return rc;
}

int power_manager_status(uint8_t *level, uint16_t *voltage, power_manager_status_t *status, int16_t *forecast)
{
   int rc = -ENOTSUP;

   if (pm_init) {
      uint16_t internal_voltage = 0xffff;

      LOG_DBG("Read battery monitor status ...");

      rc = power_manager_voltage(&internal_voltage);
      if (!rc) {
         int64_t now = k_uptime_get();
         power_manager_status_t internal_status = POWER_UNKNOWN;
         power_manager_status_t internal_status_forecast = FROM_BATTERY;
         int16_t days = -1;
         uint16_t internal_level = 0xffff;

#ifdef CONFIG_ADP536X_POWER_MANAGEMENT
         adp536x_power_manager_read_status(&internal_status);
         internal_status_forecast = internal_status;
#endif
         internal_level = transform_curve(internal_voltage, &curve);
         days = calculate_forecast(&now, internal_level, internal_status_forecast);
         if (internal_level < 25500) {
            internal_level /= 100;
         } else {
            internal_level = 255;
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
      }
   } else {
      LOG_WRN("Failed to read battery status!");
   }
   return rc;
}

int power_manager_status_desc(char *buf, size_t len)
{
   power_manager_status_t battery_status = POWER_UNKNOWN;
   int index = 0;
   uint16_t battery_voltage = 0xffff;
   int16_t battery_forecast = -1;
   uint8_t battery_level = 0xff;

   power_manager_status(&battery_level, &battery_voltage, &battery_status, &battery_forecast);
   if (battery_voltage < 0xffff) {
      index += snprintf(buf, len, "%u mV", battery_voltage);
      if (battery_level < 0xff) {
         index += snprintf(buf + index, len - index, " %u%%", battery_level);
      }
      if (battery_forecast > 1 || battery_forecast == 0) {
         index += snprintf(buf + index, len - index, " (%u days left)", battery_forecast);
      } else if (battery_forecast == 1) {
         index += snprintf(buf + index, len - index, " (1 day left)");
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
         default:
            break;
      }
      if (msg[0]) {
         index += snprintf(buf + index, len - index, " %s", msg);
      }
   }
   return index;
}

static int battery_cmd(const char *parameter)
{
   (void)parameter;
   char buf[128];
   if (power_manager_status_desc(buf, sizeof(buf))) {
      LOG_INF("%s", buf);
   }
   return 0;
}

#ifdef CONFIG_BATTERY_VOLTAGE_SOURCE_MODEM
UART_CMD(bat, "", "read battery status.", battery_cmd, NULL, 0);
#else
UART_CMD(bat, NULL, "read battery status.", battery_cmd, NULL, 0);
#endif