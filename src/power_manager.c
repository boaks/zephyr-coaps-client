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

#include <kernel.h>
#include <logging/log.h>

#include "power_manager.h"

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

#ifdef CONFIG_ADP536X_POWER_MANAGEMENT

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>

#define ADP536X_I2C_ADDR 0x46

static const struct device *i2c_dev;

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
   adp536x_reg_read(0x21, level);
}

static void power_manager_read_voltage(uint16_t *voltage)
{
   uint8_t value1 = 0xff;
   uint8_t value2 = 0xff;

   if (!adp536x_reg_read(0x25, &value1) && !adp536x_reg_read(0x26, &value2)) {
      uint16_t value = value1;
      value <<= 5;
      value |= ((value2 >> 3) & 0x1f);
      *voltage = value;
   }
}

static void power_manager_read_status(power_manager_status_t *status)
{
   uint8_t value = 0xff;

   if (!adp536x_reg_read(0x8, &value)) {
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

int power_manager_init(void)
{
   i2c_dev = device_get_binding(CONFIG_ADP536X_BUS_NAME);
   if (i2c_dev) {
      /*
       * 11%, 10mA, 8 min, enable
       */
      adp536x_reg_write(0x27, 0x59);
      return 0;
   } else {
      LOG_WRN("Failed to initialize battery monitor.");
      return -1;
   }
}

int power_manager_status(uint8_t *level, uint16_t *voltage, power_manager_status_t *status)
{
   if (i2c_dev) {
      if (level) {
         power_manager_read_level(level);
      }
      if (voltage) {
         power_manager_read_voltage(voltage);
      }
      if (status) {
         power_manager_read_status(status);
      }
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
   modem_init(NULL);
   return 0;
}

int power_manager_status(uint8_t *level, uint16_t *voltage, power_manager_status_t *status)
{
   char buf[32];

   int err = modem_at_cmd("AT%%XVBAT", buf, sizeof(buf), "%XVBAT: ");
   if (err < 0) {
      LOG_WRN("Failed to read battery level from modem!");
      *voltage = 0xffff;
   } else {
      *voltage = atoi(buf);
   }
   if (level) {
      *level = 0xff;
   }
   if (status) {
      *status = POWER_UNKNOWN;
   }
   return 0;
}

#endif
