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
#include <zephyr/drivers/eeprom.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <stdio.h>

#include "appl_eeprom.h"

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

#ifdef CONFIG_EEPROM

#define CODETIME_SIZE 6
#define CODEVALUE_SIZE 2
#define CODEINFO_SIZE (CODETIME_SIZE + CODEVALUE_SIZE)

#define BOOTINFO_VER 0x0000001

#define APPL_EEPROM_CODES_HEADER 0x200
#define APPL_EEPROM_CODES_OFFSET (APPL_EEPROM_CODES_HEADER + CODEINFO_SIZE)
#define APPL_EEPROM_CODES_END 0x300

enum eeprom_init_state {
   EEPROM_NOT_INITIALIZED,
   EEPROM_INITIALIZE_ERROR,
   EEPROM_INITIALIZED,
};

static volatile enum eeprom_init_state eeprom_init_state = EEPROM_NOT_INITIALIZED;

static int eeprom_current_code_offset = APPL_EEPROM_CODES_OFFSET;

static K_MUTEX_DEFINE(appl_eeprom_mutex);

#define CONFIG_EEPROM_I2C_LOW_LEVELx

#ifdef CONFIG_EEPROM_I2C_LOW_LEVEL

static const struct device *eeprom_i2c_24cw160 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c2));

#define EEPROM_I2C_ADDR 0x50

static int eeprom_i2c_read_memory(const struct device *i2c_dev, uint16_t addr, uint16_t mem_addr,
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

static int eeprom_i2c_write_memory(const struct device *i2c_dev, uint16_t addr, uint16_t mem_addr,
                                   uint8_t *data, uint32_t num_bytes)
{
   uint8_t cmd[34];
   struct i2c_msg msgs[1];

   /* Setup I2C messages */
   cmd[0] = (mem_addr >> 8) & 0xff;
   cmd[1] = mem_addr & 0xff;
   memcpy(&cmd[2], data, num_bytes);

   msgs[0].buf = cmd;
   msgs[0].len = num_bytes + 2;
   msgs[0].flags = I2C_MSG_WRITE | I2C_MSG_STOP;

   return i2c_transfer(i2c_dev, &msgs[0], 1, addr);
}

int appl_eeprom_read_memory(uint16_t mem_addr, uint8_t *data, uint32_t num_bytes)
{
   if (eeprom_init_state != EEPROM_INITIALIZE_ERROR) {
      return eeprom_i2c_read_memory(eeprom_i2c_24cw160, EEPROM_I2C_ADDR, mem_addr, data, num_bytes);
   } else {
      return -ENOTSUP;
   }
}

int appl_eeprom_write_memory(uint16_t mem_addr, const uint8_t *data, uint32_t num_bytes)
{
   if (eeprom_init_state != EEPROM_INITIALIZE_ERROR) {
      return eeprom_i2c_write_memory(eeprom_i2c_24cw160, EEPROM_I2C_ADDR, mem_addr, data, num_bytes);
   } else {
      return -ENOTSUP;
   }
}

#else /* CONFIG_EEPROM_I2C_LOW_LEVEL */

static const struct device *eeprom_i2c_24cw160 = DEVICE_DT_GET_OR_NULL(DT_ALIAS(eeprom_appl));

int appl_eeprom_read_memory(uint16_t mem_addr, uint8_t *data, uint32_t num_bytes)
{
   if (eeprom_init_state != EEPROM_INITIALIZE_ERROR) {
      return eeprom_read(eeprom_i2c_24cw160, mem_addr, data, num_bytes);
   } else {
      return -ENOTSUP;
   }
}

int appl_eeprom_write_memory(uint16_t mem_addr, const uint8_t *data, uint32_t num_bytes)
{
   if (eeprom_init_state != EEPROM_INITIALIZE_ERROR) {
      return eeprom_write(eeprom_i2c_24cw160, mem_addr, data, num_bytes);
   } else {
      return -ENOTSUP;
   }
}

#endif /* CONFIG_EEPROM_I2C_LOW_LEVEL */

static bool only_ff(const uint8_t *data, size_t len)
{
   for (int index = 0; index < len; ++index) {
      if (data[index] != 0xff) {
         return false;
      }
   }
   return true;
}

int appl_eeprom_write(uint16_t mem_addr, const uint8_t *data, uint32_t num_bytes)
{
   int rc = appl_eeprom_write_memory(mem_addr, data, num_bytes);
   if (rc) {
      LOG_INF("eeprom: write 0x%04x failed, %d", mem_addr, rc);
      k_sleep(K_MSEC(2000));
   } else {
      LOG_INF("eeprom: written 0x%04x", mem_addr);
      k_sleep(K_MSEC(5));
   }
   return rc;
}

static void appl_eeprom_init_code_offset(void)
{
   uint8_t data[32];

   k_mutex_lock(&appl_eeprom_mutex, K_FOREVER);
   if (!appl_eeprom_read_memory(APPL_EEPROM_CODES_HEADER, data, sizeof(data))) {
      if (sys_get_be32(data) != BOOTINFO_VER) {
         LOG_INF("eeprom: format bootcodes.");
         memset(data, 0xff, sizeof(data));
         for (int addr = APPL_EEPROM_CODES_HEADER + sizeof(data); addr < APPL_EEPROM_CODES_END; addr += sizeof(data)) {
            appl_eeprom_write(addr, data, sizeof(data));
         }
         sys_put_be32(BOOTINFO_VER, data);
         appl_eeprom_write(APPL_EEPROM_CODES_HEADER, data, sizeof(data));
         eeprom_current_code_offset = APPL_EEPROM_CODES_OFFSET;
         LOG_INF("eeprom: format bootcodes ready.");
      } else {
         for (int addr = APPL_EEPROM_CODES_OFFSET; addr < APPL_EEPROM_CODES_END; addr += sizeof(data)) {
            if (!appl_eeprom_read_memory(addr, data, sizeof(data))) {
               for (int index = 0;
                    index < sizeof(data) && (addr + index) < APPL_EEPROM_CODES_END;
                    index += CODEINFO_SIZE) {
                  if (only_ff(&data[index], CODEVALUE_SIZE)) {
                     eeprom_current_code_offset = addr + index;
                     k_mutex_unlock(&appl_eeprom_mutex);
                     return;
                  }
               }
            }
         }
      }
   }
   k_mutex_unlock(&appl_eeprom_mutex);
}

int appl_eeprom_init(void)
{
   uint8_t data[16];
   int rc;

   if (eeprom_init_state == EEPROM_NOT_INITIALIZED) {
      if (eeprom_i2c_24cw160 == NULL) {
         LOG_INF("Could not get I2C driver\n");
         goto init_error;
      }
      if (!device_is_ready(eeprom_i2c_24cw160)) {
         LOG_ERR("%s device is not ready", eeprom_i2c_24cw160->name);
         goto init_error;
      }

      for (int addr = 0x0; addr < 0x800; addr += sizeof(data)) {
         rc = appl_eeprom_read_memory(addr, data, sizeof(data));
         if (rc) {
            LOG_WRN("Error: Couldn't read eeprom 0x%03x: err: %d.\n", addr, rc);
            goto init_error;
         } else if (!only_ff(data, sizeof(data))) {
            char label[16];
            snprintf(label, sizeof(label), "eeprom 0x%03x", addr);
            LOG_HEXDUMP_INF(data, sizeof(data), label);
         }
      }
      eeprom_init_state = EEPROM_INITIALIZED;
      appl_eeprom_init_code_offset();
   }
   if (eeprom_init_state == EEPROM_INITIALIZED) {
      return 0;
   }
init_error:
   eeprom_init_state = EEPROM_INITIALIZE_ERROR;
   return -ENOTSUP;
}

int appl_eeprom_write_code(int64_t time, uint16_t code)
{
   int rc;
   int next;
   uint8_t data[CODEINFO_SIZE + CODEVALUE_SIZE];

   if (code == 0xffff) {
      return -EOVERFLOW;
   }
   k_mutex_lock(&appl_eeprom_mutex, K_FOREVER);
   if (eeprom_current_code_offset + sizeof(data) <= APPL_EEPROM_CODES_END) {
      rc = appl_eeprom_read_memory(eeprom_current_code_offset, data, sizeof(data));
      next = eeprom_current_code_offset + CODEINFO_SIZE;
   } else {
      rc = appl_eeprom_read_memory(eeprom_current_code_offset, data, CODEINFO_SIZE);
      next = APPL_EEPROM_CODES_OFFSET;
      rc = rc || appl_eeprom_read_memory(next, &data[CODEINFO_SIZE], CODEVALUE_SIZE);
   }
   if (!rc) {
      sys_put_be16(code, data);
      sys_put_be48((time / MSEC_PER_SEC), &data[CODEVALUE_SIZE]);
      if (!only_ff(&data[CODEINFO_SIZE], CODEVALUE_SIZE)) {
         memset(&data[CODEINFO_SIZE], 0xff, CODEVALUE_SIZE);
         if (next == APPL_EEPROM_CODES_OFFSET) {
            rc = appl_eeprom_write_memory(next, &data[CODEINFO_SIZE], CODEVALUE_SIZE);
            rc = rc || appl_eeprom_write_memory(eeprom_current_code_offset, data, CODEINFO_SIZE);
         } else {
            rc = appl_eeprom_write_memory(eeprom_current_code_offset, data, sizeof(data));
         }
      } else {
         rc = appl_eeprom_write_memory(eeprom_current_code_offset, data, CODEINFO_SIZE);
      }
      if (!rc) {
         eeprom_current_code_offset = next;
      }
   }
   k_mutex_unlock(&appl_eeprom_mutex);
   if (!rc) {
      k_sleep(K_MSEC(5));
   }

   return rc;
}

static int appl_eeprom_read_code(int *current, int64_t *times, uint16_t *codes)
{
   int rc = -ENOTSUP;
   uint8_t data[CODEINFO_SIZE];

   if (*current == APPL_EEPROM_CODES_OFFSET) {
      *current = APPL_EEPROM_CODES_END;
   }
   *current -= CODEINFO_SIZE;
   rc = appl_eeprom_read_memory(*current, data, sizeof(data));
   if (!rc) {
      if (only_ff(data, CODEVALUE_SIZE)) {
         rc = 0;
      } else {
         rc = 1;
         if (codes) {
            *codes = sys_get_be16(data);
         }
         if (times) {
            *times = sys_get_be48(&data[CODEVALUE_SIZE]) * MSEC_PER_SEC;
         }
      }
   }
   return rc;
}

int appl_eeprom_read_codes(int64_t *times, uint16_t *codes, size_t count)
{
   int rc = -ENOTSUP;
   size_t read = 0;
   int current;

   k_mutex_lock(&appl_eeprom_mutex, K_FOREVER);
   current = eeprom_current_code_offset;
   k_mutex_unlock(&appl_eeprom_mutex);
   while (read < count) {
      rc = appl_eeprom_read_code(&current, times, codes);
      if (rc != 1) {
         break;
      }
      if (times) {
         ++times;
      }
      if (codes) {
         ++codes;
      }
      ++read;
   }
   if (read) {
      return read;
   }
   return rc;
}
#else  /* CONFIG_EEPROM */

int appl_eeprom_init(void)
{
   return -ENOTSUP;
}

int appl_eeprom_read_memory(uint16_t mem_addr, uint8_t *data, uint32_t num_bytes)
{
   (void)mem_addr;
   (void)data;
   (void)num_bytes;
   return -ENOTSUP;
}

int appl_eeprom_write_memory(uint16_t mem_addr, const uint8_t *data, uint32_t num_bytes)
{
   (void)mem_addr;
   (void)data;
   (void)num_bytes;
   return -ENOTSUP;
}

int appl_eeprom_write_code(int64_t time, uint16_t code)
{
   (void)time;
   (void)code;
   return -ENOTSUP;
}

int appl_eeprom_read_codes(int64_t *times, uint16_t *codes, size_t count)
{
   (void)times;
   (void)codes;
   (void)count;
   return -ENOTSUP;
}
#endif /* CONFIG_EEPROM */