/*
 * Copyright (c) 2025 Achim Kraus CloudCoap.net
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
#include <zephyr/sys/util.h>

#include "io_job_queue.h"
#include "ui.h"

#include "parse.h"
#include "sh_cmd.h"

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

#define GET_BITS(V, P, L) (((V) >> (P)) & ((1 << (L)) - 1))

#define BQ25186_ADDR 0x6A

#define BQ25186_STAT0 0x0
#define BQ25186_STAT1 0x1
#define BQ25186_FLAG0 0x2
#define BQ25186_VBAT_CTRL 0x3
#define BQ25186_ICHG_CTRL 0x4
#define BQ25186_CHARGECTRL0 0x5
#define BQ25186_CHARGECTRL1 0x6
#define BQ25186_IC_CTRL 0x7
#define BQ25186_TMR_ILIM 0x8
#define BQ25186_SHIP_RST 0x9
#define BQ25186_SYS_REG 0xA
#define BQ25186_TS_CONTROL 0xB
#define BQ25186_MASK_ID 0xC

static const struct device *i2c_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(sensor_i2c));

static const char *register_names[] = {
    "STAT0",
    "STAT1",
    "FLAG0",
    "VBAT_CTRL",
    "ICHG_CTRL",
    "CHARGECTRL0",
    "CHARGECTRL1",
    "IC_CTRL",
    "TMR_ILIM",
    "SHIP_RST",
    "SYS_REG",
    "TS_CONTROL",
    "MASK_ID",
};

static const char *stat0_charging[] = {
    "not charging",
    "I charging",
    "V charging",
    "charging done",
};

static const char *stat1_ts[] = {
    "normal",
    "Therm. suspended",
    "Therm. reduced I",
    "Therm. reduced V",
};

static const int chargectrl1_i[] = {
    500, 1000, 1500, 3000};

static const int chargectrl1_u[] = {
    3000, 3000, 3000, 2800, 2600, 2400, 2200, 2000};

static const int tmr_ilim[] = {
    50, 100, 200, 300, 400, 500, 6650, 1050};

static inline int charger_read_reg(const struct device *i2c_dev, uint8_t reg)
{
   uint8_t data = 0;
   int rc = i2c_reg_read_byte(i2c_dev, BQ25186_ADDR, reg, &data);
   if (rc) {
      LOG_INF("Err: read 0x%02x, %d (%s)", reg, rc, strerror(-rc));
   } else {
      LOG_INF("Read %s val 0x%02x", register_names[reg], data);
   }
   return rc;
}

static inline int charger_write_reg(const struct device *i2c_dev, uint8_t reg, uint8_t value)
{
   int rc = i2c_reg_write_byte(i2c_dev, BQ25186_ADDR, reg, value);
   if (rc) {
      LOG_INF("Err: write 0x%02x, %d (%s)", reg, rc, strerror(-rc));
   } else {
      LOG_INF("Write %s val 0x%02x", register_names[reg], value);
   }
   return rc;
}

static int charger_log_status(uint8_t *regs)
{
   int rc = 0;
   char buf[128];
   int size = sizeof(buf);
   int index = 0;

   index += snprintf(&buf[index], size - index, "STAT0: %s", stat0_charging[GET_BITS(regs[0], 5, 2)]);

   if (regs[0] & BIT(7)) {
      index += snprintf(&buf[index], size - index, ", TS open");
   }
   if (regs[0] & BIT(4)) {
      index += snprintf(&buf[index], size - index, ", I lim.");
   }
   if (regs[0] & BIT(3)) {
      index += snprintf(&buf[index], size - index, ", VSYS red.");
   }
   if (regs[0] & BIT(2)) {
      index += snprintf(&buf[index], size - index, ", VIN red.");
   }
   if (regs[0] & BIT(1)) {
      index += snprintf(&buf[index], size - index, ", Therm. reg.");
   }
   if (regs[0] & BIT(0)) {
      index += snprintf(&buf[index], size - index, ", VIN");
   }
   LOG_INF("%s", buf);
   index = 0;
   index += snprintf(&buf[index], size - index, "STAT1: %s", stat1_ts[GET_BITS(regs[1], 3, 2)]);

   if (regs[1] & BIT(7)) {
      index += snprintf(&buf[index], size - index, ", VIN OVP");
   }
   if (regs[1] & BIT(6)) {
      index += snprintf(&buf[index], size - index, ", BAT UVP");
   }
   if (regs[1] & BIT(2)) {
      index += snprintf(&buf[index], size - index, ", safety timer");
   }
   if (regs[1] & BIT(1)) {
      index += snprintf(&buf[index], size - index, ", timer 1");
   }
   if (regs[1] & BIT(0)) {
      index += snprintf(&buf[index], size - index, ", timer 2");
   }
   LOG_INF("%s", buf);
   if (regs[2]) {
      LOG_INF("FLAGS: 0x%02x", regs[2]);
   }
   LOG_INF("VBAT: %d mV", 3500 + GET_BITS(regs[3], 0, 7) * 10);
   if (regs[4] & BIT(7)) {
      LOG_INF("Charging disabled");
   } else {
      int current = GET_BITS(regs[4], 0, 7);
      if (current < 8) {
         current *= 5;
      } else {
         current = 40 + (current - 31) * 10;
      }
      LOG_INF("ICHG: %d mA", current);
   }
   switch (GET_BITS(regs[5], 2, 2)) {
      case 0:
         LOG_INF("VINDPM: VBAT + 300mV");
         break;
      case 1:
         LOG_INF("VINDPM: 4500mV");
         break;
      case 2:
         LOG_INF("VINDPM: 4700mV");
         break;
      case 3:
      default:
         LOG_INF("VINDPM: disabled");
         break;
   }

   LOG_INF("CHARGECTRL1: %d mA, BAT min. %d mV", chargectrl1_i[GET_BITS(regs[6], 6, 2)], chargectrl1_u[GET_BITS(regs[6], 3, 3)]);
   LOG_INF("TMR_ILIM: %d mA", tmr_ilim[GET_BITS(regs[8], 0, 3)]);

   return rc;
}

static int charger_read_status(const struct device *i2c_dev)
{
   int rc = 0;
   static uint8_t prev_regs[0xe] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
   uint8_t regs[0xd];

   if (device_is_ready(i2c_dev)) {
      memset(regs, 0, sizeof(regs));
      rc = i2c_burst_read(i2c_dev, BQ25186_ADDR, 0, regs, sizeof(regs));
      if (!rc) {
         LOG_HEXDUMP_INF(regs, sizeof(regs), "BQ25186 status");
         if (!prev_regs[0x0d]) {
            memcpy(prev_regs, regs, sizeof(regs));
            prev_regs[0x0d] = 1;
         } else {
            char line[sizeof(regs) * 4];
            int pos = 0;
            memset(line, 0, sizeof(line));
            for (int index = 0; index < sizeof(regs); ++index) {
               uint8_t diff = regs[index] ^ prev_regs[index];
               if (diff & 0xf0) {
                  hex2char((prev_regs[index] >> 4), line + pos);
               } else {
                  line[pos] = '.';
               }
               pos++;
               if (diff & 0xf) {
                  hex2char((prev_regs[index] & 0xf), line + pos);
               } else {
                  line[pos] = '.';
               }
               pos++;
               line[pos++] = ' ';
               if ((index % 8) == 7) {
                  line[pos++] = ' ';
               }
               prev_regs[index] = regs[index];
            }
            LOG_INF("%s", line);
         }
         charger_log_status(regs);
      } else {
         LOG_INF("Err: burst read %d (%s)", rc, strerror(-rc));
      }
   }
   return rc;
}

static int charger_init(void)
{
   charger_read_status(i2c_dev);
   return 0;
}

/* CONFIG_APPLICATION_INIT_PRIORITY + 1*/
SYS_INIT(charger_init, APPLICATION, 91);

#ifdef CONFIG_SH_CMD

static int sh_cmd_read_charger_status(const char *parameter)
{
   long reg = -1;
   const char *cur = parse_next_long(parameter, 0, &reg);

   if (parameter == cur) {
      return charger_read_status(i2c_dev);
   } else if (0 > reg || 12 < reg) {
      LOG_INF("chrg <reg> %ld out of range [0-12]!", reg);
      return -EINVAL;
   }
   return charger_read_reg(i2c_dev, (uint8_t)reg);
}

static void sh_cmd_read_charger_status_help(void)
{
   LOG_INF("> help chrg:");
   LOG_INF("  chrg       : read all charger status register.");
   LOG_INF("  chrg <reg> : read charger status register <reg>.");
   LOG_INF("       <reg> : [0-12].");
}

static int sh_cmd_write_charger_status(const char *parameter)
{
   long reg = -1;
   long value = -1;
   const char *cur = parameter;
   const char *t = parse_next_long(cur, 0, &reg);

   if (t == cur) {
      LOG_INF("chrgw <reg> and <val> missing!");
      return -EINVAL;
   } else if (0 > reg || 12 < reg) {
      LOG_INF("chrgw <reg> %ld out of range [0-12]!", reg);
      return -EINVAL;
   }

   cur = t;
   t = parse_next_long(cur, 0, &value);

   if (t == cur) {
      LOG_INF("chrgw %s <val> missing!", parameter);
      return -EINVAL;
   } else if (0 > value || 255 < value) {
      LOG_INF("chrgw <val> %ld out of range [0-255]!", value);
      return -EINVAL;
   }

   return charger_write_reg(i2c_dev, (uint8_t)reg, (uint8_t)value);
}

static void sh_cmd_write_charger_status_help(void)
{
   LOG_INF("> help chrgw:");
   LOG_INF("  chrgw <reg> <val> : write <val> to <reg>.");
   LOG_INF("        <reg>       : [0-12].");
   LOG_INF("        <val>       : [0-255].");
}

SH_CMD(chrg, NULL, "read charger status.", sh_cmd_read_charger_status, sh_cmd_read_charger_status_help, 0);
SH_CMD(chrgw, NULL, "write charger status.", sh_cmd_write_charger_status, sh_cmd_write_charger_status_help, 0);
#endif /* CONFIG_SH_CMD */