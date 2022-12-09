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

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>

#include "io_job_queue.h"
#include "power_manager.h"

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

static K_MUTEX_DEFINE(pm_mutex);

#ifdef CONFIG_SUSPEND_UART
#if defined(CONFIG_UART_CONSOLE)

static const struct device *const uart0_dev = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_console));
static bool uart0_suspended = false;

static void suspend_uart(bool suspend)
{
   int ret = 0;
   if (device_is_ready(uart0_dev) && uart0_suspended != suspend) {
      if (suspend) {
         LOG_INF("Disable UART");
         ret = pm_device_action_run(uart0_dev, PM_DEVICE_ACTION_SUSPEND);
         if (ret < 0) {
            LOG_WRN("Failed to disable UART (%d)", ret);
         } else {
            uart0_suspended = suspend;
         }
      } else {
         uart0_suspended = suspend;
         ret = pm_device_action_run(uart0_dev, PM_DEVICE_ACTION_RESUME);
         if (ret < 0) {
            LOG_WRN("Failed to enable UART (%d)", ret);
            uart0_suspended = !suspend;
         } else {
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

#ifdef CONFIG_ADP536X_POWER_MANAGEMENT

/*
 * first_battery_level is set, when the first complete
 * battery level epoch is detected. The very first change
 * indicates only a partitial epoch.
 */
static int64_t next_forecast_uptime = 0;

static uint8_t first_battery_level = 0xff;
static int64_t first_battery_level_uptime = 0;
static uint8_t last_battery_level = 0xff;
static int64_t last_battery_level_uptime = 0;

static uint8_t current_battery_level = 0xff;
static uint8_t current_battery_changes = 0;
/*
 * last left battery time. -1, if not available
 */
static int64_t last_battery_left_time = -1;

#define MSEC_PER_HOUR (MSEC_PER_SEC * 60 * 60)
#define MSEC_PER_DAY (MSEC_PER_SEC * 60 * 60 * 24)

static int16_t calculate_forecast(int64_t *now, uint8_t battery_level, power_manager_status_t status)
{
   if (battery_level == 0xff) {
      LOG_INF("not ready.");
   } else if (status != FROM_BATTERY) {
      LOG_INF("charging.");
   } else {
      if (((*now) - next_forecast_uptime) >= 0) {
         int64_t passed_time = (*now) - last_battery_level_uptime;
         if (battery_level < current_battery_level) {
            current_battery_level = battery_level;
            ++current_battery_changes;
            if (current_battery_changes == 1) {
               // initial value;
               return -1;
            }
            if (current_battery_changes == 2) {
               // first battery level change
               LOG_INF("first battery level change");
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
               LOG_INF("left battery %u%% time %lld (%lld days, %lld passed)",
                       battery_level, last_battery_left_time,
                       last_battery_left_time / MSEC_PER_DAY, passed_time / MSEC_PER_DAY);
               last_battery_level = battery_level;
               last_battery_level_uptime = *now;
               passed_time = 0;
               diff = first_battery_level - battery_level;
               if (!diff) return -1;
               passed_time = (*now) - first_battery_level_uptime;
               time = (passed_time * battery_level) / diff;
               LOG_INF("left battery time 2 %lld (%lld days, %lld passed)", time,
                       time / MSEC_PER_DAY, passed_time / MSEC_PER_DAY);
               last_battery_left_time = (last_battery_left_time + time) / 2;
               passed_time = 0;
            } else if (current_battery_changes == 3) {
               // first change
               int diff = last_battery_level - battery_level;
               if (!diff) return -1;
               last_battery_left_time = (passed_time * battery_level) / diff;
               LOG_INF("first left battery time %lld (%lld, d:%u%%, %u%%)", last_battery_left_time, passed_time, diff, battery_level);
            }
         }
         if (current_battery_changes >= 3) {
            // after first change
            passed_time += (MSEC_PER_DAY / 2);
            int16_t time = (int16_t)((last_battery_left_time - passed_time) / MSEC_PER_DAY);
            LOG_INF("battery %u%%, %d left days (passed %d days)", battery_level, time, (int)(passed_time / MSEC_PER_DAY));
            return time;
         }
      }
      return -1;
   }
   // fall through, reset
   current_battery_changes = 0;
   first_battery_level = 0xff;
   last_battery_level = 0xff;
   current_battery_level = 0xff;
   last_battery_left_time = -1;
   next_forecast_uptime = *now + MSEC_PER_HOUR;

   return -1;
}

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

static void power_manager_read_level(uint8_t *level)
{
   adp536x_reg_read(ADP536X_I2C_REG_LEVEL, level);
}

static void power_manager_read_voltage(uint16_t *voltage)
{
   uint8_t value1 = 0xff;
   uint8_t value2 = 0xff;

   if (!adp536x_reg_read(ADP536X_I2C_REG_VOLTAGE_HIGH_BYTE, &value1) &&
       !adp536x_reg_read(ADP536X_I2C_REG_VOLTAGE_LOW_BYTE, &value2)) {
      uint16_t value = value1;
      value <<= 5;
      value |= ((value2 >> 3) & 0x1f);
      *voltage = value;
   }
}

static void power_manager_read_status(power_manager_status_t *status)
{
   uint8_t value = 0xff;

   if (!adp536x_reg_read(ADP536X_I2C_REG_STATUS, &value)) {
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
}

#ifdef CONFIG_ADP536X_POWER_MANAGEMENT_LOW_POWER
static void power_management_sleep_mode_work_fn(struct k_work *work)
{
   /*
    * 0x5B: 11%, 10mA, 8 min, sleep, enable
    */
   adp536x_reg_write(ADP536X_I2C_REG_FUEL_GAUGE_MODE, 0x5B);
}

static K_WORK_DELAYABLE_DEFINE(power_management_sleep_mode_work, power_management_sleep_mode_work_fn);
#endif /* CONFIG_ADP536X_POWER_MANAGEMENT_LOW_POWER */

int power_manager_init(void)
{
#if (defined CONFIG_SUSPEND_UART) && (defined CONFIG_UART_CONSOLE)
   if (!device_is_ready(uart0_dev)) {
      LOG_WRN("UART0 console not available.");
   }
#endif
   if (device_is_ready(i2c_dev)) {
      uint8_t value = 0;
      int64_t now = k_uptime_get();

      // init forecast
      calculate_forecast(&now, 0xff, CHARGING_TRICKLE);

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
      work_schedule_for_io_queue(&power_management_sleep_mode_work, K_MINUTES(30));
#endif
      return 0;
   } else {
      LOG_WRN("Failed to initialize battery monitor.");
      return -1;
   }
}

static int power_manager_xvy(uint8_t config_register, bool enable)
{
   if (device_is_ready(i2c_dev)) {
      uint8_t buck_config = 0;

      if (!adp536x_reg_read(config_register, &buck_config)) {
         /*         LOG_INF("buck_conf: %02x %02x", config_register, buck_config); */
         buck_config |= 0xC0; // softstart to 11 => 512ms
         if (enable) {
            buck_config |= 1;
         } else {
            buck_config &= (~1);
         }
         adp536x_reg_write(config_register, buck_config);
         return 0;
      } else {
         LOG_WRN("Failed to read buckbst_cfg.");
         return -1;
      }
   } else {
      LOG_WRN("Failed to write buckbst_cfg.");
      return -1;
   }
}

int power_manager_suspend(bool enable)
{
   k_mutex_lock(&pm_mutex, K_FOREVER);
#ifdef CONFIG_SUSPEND_UART
   suspend_uart(enable);
#endif
#ifdef CONFIG_SUSPEND_3V3
   power_manager_3v3(!enable);
#endif
   k_mutex_unlock(&pm_mutex);
   return 0;
}

int power_manager_3v3(bool enable)
{
   return power_manager_xvy(ADP536X_I2C_REG_BUCK_BOOST_CONFIG, enable);
}

int power_manager_1v8(bool enable)
{
   return power_manager_xvy(ADP536X_I2C_REG_BUCK_CONFIG, enable);
}

int power_manager_voltage(uint16_t *voltage)
{
   if (device_is_ready(i2c_dev)) {
      if (voltage) {
         power_manager_read_voltage(voltage);
      }
      LOG_DBG("%umV", *voltage);
      return 0;
   } else {
      LOG_WRN("Failed to read battery level!");
   }
   return -1;
}

#define SAVE_VALUE(X) ((X) ? *(X) : 0)

int power_manager_status(uint8_t *level, uint16_t *voltage, power_manager_status_t *status, int16_t *forecast)
{
   if (device_is_ready(i2c_dev)) {
      int64_t now = k_uptime_get();
      power_manager_status_t internal_status = POWER_UNKNOWN;
      int16_t days = -1;
      uint8_t internal_level = 0xff;

      LOG_DBG("Read battery monitor status ...");

      power_manager_read_level(&internal_level);
      power_manager_read_status(&internal_status);
      days = calculate_forecast(&now, internal_level, internal_status);
      if (level) {
         *level = internal_level;
      }
      if (status) {
         *status = internal_status;
      }
      if (forecast) {
         *forecast = days;
      }
      if (voltage) {
         power_manager_read_voltage(voltage);
      }
      LOG_DBG("%u%% %umV %d (%d left days)", internal_level, SAVE_VALUE(voltage), internal_status, days);
      return 0;
   } else {
      LOG_WRN("Failed to read battery level!");
   }
   return -1;
}

#else

#include <stdlib.h>

#include "modem.h"

int power_manager_init(void)
{
   return 0;
}

int power_manager_suspend(bool enable)
{
   k_mutex_lock(&pm_mutex, K_FOREVER);
#ifdef CONFIG_SUSPEND_UART
   suspend_uart(enable);
#endif
   k_mutex_unlock(&pm_mutex);
   return 0;
}

int power_manager_3v3(bool enable)
{
   (void)enable;
   return 0;
}

int power_manager_1v8(bool enable)
{
   (void)enable;
   return 0;
}

int power_manager_voltage(uint16_t *voltage)
{
   char buf[32];

   int err = modem_at_cmd("AT%%XVBAT", buf, sizeof(buf), "%XVBAT: ");
   if (err < 0) {
      LOG_WRN("Failed to read battery level from modem!");
      *voltage = 0xffff;
   } else {
      *voltage = atoi(buf);
   }
   return 0;
}

int power_manager_status(uint8_t *level, uint16_t *voltage, power_manager_status_t *status, int16_t *forecast)
{
   if (level) {
      *level = 0xff;
   }
   if (status) {
      *status = POWER_UNKNOWN;
   }
   if (forecast) {
      *forecast = -1;
   }
   return power_manager_voltage(voltage);
}

#endif
