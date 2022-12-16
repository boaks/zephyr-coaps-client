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
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "environment_sensor.h"

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

static const struct device *const sht21_i2c = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c2));

#define SHT21_I2C_ADDR 0x40

#define SHT21_TEMPERATURE_OFFSET -46.85f
#define SHT21_TEMPERATURE_RANGE 175.72f

#define SHT21_CMD_RESET 0xFE
#define SHT21_CMD_READ_TEMPERATURE_HOLD 0xE3
#define SHT21_CMD_READ_TEMPERATURE_NO_HOLD 0xF3

// P(x)=x^8+x^5+x^4+1 = 100110001
#define CRC8_POLYNOMIAL ((uint16_t)0x131)

static uint8_t calc_crc(uint8_t *data, size_t len)
{
   size_t index;
   uint8_t crc = 0;
   // calculates 8-Bit checksum with given polynomial
   for (index = 0; index < len; ++index) {
      crc ^= (data[index]);
      for (uint8_t bit = 8; bit > 0; --bit) {
         if (crc & 0x80) {
            crc <<= 1;
            crc ^= CRC8_POLYNOMIAL;
         } else {
            crc <<= 1;
         }
      }
   }
   return crc;
}

static double calc_temperature(uint8_t *data)
{
   uint16_t value = data[0] << 8 | (data[1] & ~0x3); // clear bits [1..0] (status bits)

   //-- calculate temperature [°C] --
   // T= -46.85 + 175.72 * ST/2^16
   return SHT21_TEMPERATURE_OFFSET + (SHT21_TEMPERATURE_RANGE * value) / 65536;
}

static int read_memory(const struct device *i2c_dev, uint16_t addr, uint16_t mem_addr,
                       uint8_t *data, uint32_t num_bytes)
{
   uint8_t cmd[2];
   struct i2c_msg msgs[2];

   /* Setup I2C messages */
   cmd[0] = (mem_addr >> 8) & 0xff;
   cmd[1] = mem_addr & 0xff;

   /* Data to be written, and STOP after this. */
   msgs[0].buf = cmd;
   msgs[0].len = sizeof(cmd);
   msgs[0].flags = I2C_MSG_WRITE;

   /* Data to be written, and STOP after this. */
   msgs[1].buf = data;
   msgs[1].len = num_bytes;
   msgs[1].flags = I2C_MSG_RESTART | I2C_MSG_READ | I2C_MSG_STOP;

   return i2c_transfer(i2c_dev, &msgs[0], 2, addr);
}

static int read_reg(const struct device *i2c_dev, uint16_t addr, uint8_t cmd, uint8_t *data,
                    uint32_t num_bytes)
{
   struct i2c_msg msgs[2];

   /* Setup I2C messages */

   /* Data to be written, and STOP after this. */
   msgs[0].buf = &cmd;
   msgs[0].len = sizeof(cmd);
   msgs[0].flags = I2C_MSG_WRITE;

   /* Data to be written, and STOP after this. */
   msgs[1].buf = data;
   msgs[1].len = num_bytes;
   msgs[1].flags = I2C_MSG_RESTART | I2C_MSG_READ | I2C_MSG_STOP;

   return i2c_transfer(i2c_dev, &msgs[0], 2, addr);
}

static int write_cmd(const struct device *i2c_dev, uint16_t addr, uint8_t cmd)
{
   return i2c_write(i2c_dev, &cmd, sizeof(cmd), addr);
}

static int read_temperature(const struct device *i2c_dev, const uint16_t addr, const bool hold, double *value)
{
   int err = -ENODATA;
   uint8_t temperature[3];

   LOG_INF("SHT21 reading temparature ...");

   for (int count = 0; count < 3; ++count) {
      if (hold) {
         err = read_reg(i2c_dev, addr, SHT21_CMD_READ_TEMPERATURE_HOLD, temperature, sizeof(temperature));
      } else {
         int waits = 0;
         err = write_cmd(i2c_dev, addr, SHT21_CMD_READ_TEMPERATURE_NO_HOLD);
         if (!err) {
            while (i2c_read(i2c_dev, temperature, sizeof(temperature), addr)) {
               ++waits;
               LOG_INF("SHT21 i2c error/nack %d. => waiting for temperature", waits);
               if (waits > 4) {
                  err = -ENODATA;
                  break;
               }
               k_sleep(K_MSEC(35));
            }
            if (!err) {
               LOG_INF("SHT21 i2c ack => temperature available");
            }
         }
      }
      if (err) {
         LOG_INF("SHT21 cmd failure");
      } else if ((temperature[1] & 0x2) != 0) {
         LOG_INF("SHT21 status bits %02x", temperature[1] & 0x3);
         err = -ENODATA;
      } else {
         uint8_t crc = calc_crc(temperature, sizeof(uint16_t));
         if (crc == temperature[2]) {
            break;
         } else {
            LOG_INF("SHT21 crc failure %02x %02x", crc, temperature[2]);
            err = -ENODATA;
         }
      }
   }
   if (err) {
      LOG_WRN("SHT21 read failure");
   } else if (value) {
      *value = calc_temperature(temperature);
      LOG_INF("SHT21 temparature %0.2f", *value);
   }
   return err;
}

int environment_init(void)
{
   LOG_INF("SHT21 initialize");

   if (sht21_i2c == NULL) {
      LOG_INF("Could not get I2C_2 device\n");
      return -ENOTSUP;
   }
   if (!device_is_ready(sht21_i2c)) {
      LOG_ERR("%s device is not ready", sht21_i2c->name);
      return -ENOTSUP;
   }
   if (write_cmd(sht21_i2c, SHT21_I2C_ADDR, SHT21_CMD_RESET)) {
      LOG_ERR("SHT21 reset failed!");
   }
   environment_init_history();

   return 0;
}

int environment_sensor_fetch(bool force)
{
   (void)force;
   return 0;
}

int environment_get_temperature(double *value)
{
   return read_temperature(sht21_i2c, SHT21_I2C_ADDR, false, value);
}

int environment_get_humidity(double *value)
{
   return -ENODATA;
}

int environment_get_pressure(double *value)
{
   return -ENODATA;
}

int environment_get_gas(int32_t *value)
{
   return -ENODATA;
}

int environment_get_iaq(int32_t *value)
{
   return -ENODATA;
}
