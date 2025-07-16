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

#include <ctype.h>
#include <modem/nrf_modem_lib_trace.h>
#include <stdio.h>
#include <stdlib.h>
#include <zephyr/logging/log.h>

#include "parse.h"
#include "sh_cmd.h"

LOG_MODULE_DECLARE(MODEM, CONFIG_MODEM_LOG_LEVEL);

#ifdef CONFIG_NRF_MODEM_LIB_TRACE_BACKEND_FLASH

#include <zephyr/drivers/uart.h>

#define READ_BUF_SIZE CONFIG_NRF_MODEM_LIB_TRACE_BACKEND_FLASH_BUF_SIZE

#define UART1_DT_NODE DT_NODELABEL(uart1)

static const struct device *const uart_dev = DEVICE_DT_GET(UART1_DT_NODE);

static void print_uart1(char *buf, int len)
{
   if (!device_is_ready(uart_dev)) {
      LOG_ERR("uart1 device not found/ready!");
      return;
   }
   for (int i = 0; i < len; i++) {
      uart_poll_out(uart_dev, buf[i]);
   }
}

static uint8_t read_buf[READ_BUF_SIZE];

static int modem_trace_cmd_print_traces(const char *config)
{
   (void)config;
   int ret = 0;
   size_t read_offset = 0;

   ret = nrf_modem_lib_trace_data_size();
   LOG_INF("Reading out %d bytes of trace data", ret);

   /* Read out the trace data from flash */
   while (ret > 0) {
      ret = nrf_modem_lib_trace_read(read_buf, READ_BUF_SIZE);
      if (ret < 0) {
         if (ret == -ENODATA) {
            break;
         }
         LOG_ERR("Error reading modem traces: %d", ret);
         break;
      }
      if (ret == 0) {
         LOG_DBG("No more traces to read from flash");
         break;
      }

      read_offset += ret;
      print_uart1(read_buf, ret);
   }
   LOG_INF("Total trace bytes read from flash: %d", read_offset);
   return 0;
}

SH_CMD(trout, NULL, "read modem trace", modem_trace_cmd_print_traces, NULL, 0);

#endif

typedef struct trace_level_definition {
   const char *name;
   const char *desc;
   const enum nrf_modem_lib_trace_level level;
} trace_level_definition_t;

static trace_level_definition_t trace_level_definitions[] = {
    {.name = "off", .desc = "switch modem trace off", .level = NRF_MODEM_LIB_TRACE_LEVEL_OFF},
    {.name = "core", .desc = "modem trace core dumps only", .level = NRF_MODEM_LIB_TRACE_LEVEL_COREDUMP_ONLY},
    {.name = "full", .desc = "modem trace full", .level = NRF_MODEM_LIB_TRACE_LEVEL_FULL},
    {.name = "ip", .desc = "modem trace ip", .level = NRF_MODEM_LIB_TRACE_LEVEL_IP_ONLY},
    {.name = "iplte", .desc = "modem trace ip & lte", .level = NRF_MODEM_LIB_TRACE_LEVEL_LTE_AND_IP},
    {.name = NULL, .desc = NULL, .level = NRF_MODEM_LIB_TRACE_LEVEL_OFF},
};

static enum nrf_modem_lib_trace_level modem_trace_current_level = CONFIG_NRF_MODEM_LIB_TRACE_LEVEL;
static bool modem_trace_current_level_set = false;

static int modem_trace_get_level(const char *value)
{
   for (int index = 0; trace_level_definitions[index].name; ++index) {
      if (!stricmp(value, trace_level_definitions[index].name)) {
         return trace_level_definitions[index].level;
      }
   }
   return -EINVAL;
}

static int modem_trace_get_level_idx(int level)
{
   for (int index = 0; trace_level_definitions[index].name; ++index) {
      if (level == trace_level_definitions[index].level) {
         return index;
      }
   }
   return -EINVAL;
}

static int modem_trace_cmd_level(const char *config)
{
   int err = 0;
   int lvl = 0;
   char level[8];
   const char *cur = config;

   memset(level, 0, sizeof(level));
   cur = parse_next_text(cur, ' ', level, sizeof(level));
   if (level[0]) {
      err = modem_trace_get_level(level);
      if (err >= 0) {
         lvl = err;
         err = nrf_modem_lib_trace_level_set(lvl);
         if (!err) {
            modem_trace_current_level = lvl;
            modem_trace_current_level_set = true;
         }
      }
   }
   if (!err && modem_trace_current_level_set) {
      lvl = modem_trace_get_level_idx(modem_trace_current_level);
      if (lvl >= 0) {
         LOG_INF("Modem trace level %s (%s)", trace_level_definitions[lvl].name, trace_level_definitions[lvl].desc);
      }
   }
   return err;
}

static void modem_trace_cmd_level_help(void)
{
   LOG_INF("> help trlvl:");
   LOG_INF("  trlvl       : show modem trace level. (only if set before!)");
   LOG_INF("  trlvl off   : switch modem trace off.");
   LOG_INF("  trlvl core  : set modem trace level to core dumps only.");
   LOG_INF("  trlvl full  : set modem trace level to full.");
   LOG_INF("  trlvl ip    : set modem trace level to ip only.");
   LOG_INF("  trlvl iplte : set modem trace level to ip and lte.");
}

static int modem_trace_cmd_clear(const char *config)
{
   (void)config;
   LOG_INF("start clear modem trace ...");
   int res = nrf_modem_lib_trace_clear();
   if (!res) {
      LOG_INF("cleared modem trace.");
   }
   return res;
}

static int modem_trace_cmd_info(const char *config)
{
   (void)config;
   int res = nrf_modem_lib_trace_data_size();
   int lvl = -1;

   if (modem_trace_current_level_set) {
      lvl = modem_trace_get_level_idx(modem_trace_current_level);
   }

#ifdef CONFIG_NRF_MODEM_LIB_TRACE_BACKEND_UART
#define NRF_MODEM_LIB_TRACE_BACKEND_NAME "UART"
#elif defined(CONFIG_NRF_MODEM_LIB_TRACE_BACKEND_FLASH)
#define NRF_MODEM_LIB_TRACE_BACKEND_NAME "FLASH"
#elif defined(CONFIG_NRF_MODEM_LIB_TRACE_BACKEND_RAM)
#define NRF_MODEM_LIB_TRACE_BACKEND_NAME "RAM"
#elif defined(CONFIG_NRF_MODEM_LIB_TRACE_BACKEND_RTT)
#define NRF_MODEM_LIB_TRACE_BACKEND_NAME "RTT"
#endif

   if (res == -ENOTSUP) {
      if (lvl >= 0) {
         LOG_INF("Mode %s, trace level %s (%s)", NRF_MODEM_LIB_TRACE_BACKEND_NAME, trace_level_definitions[lvl].name, trace_level_definitions[lvl].desc);
      } else {
         LOG_INF("Mode %s", NRF_MODEM_LIB_TRACE_BACKEND_NAME);
      }
   } else {
      if (lvl >= 0) {
         LOG_INF("Mode %s, trace level %s (%s), %d bytes of trace data", NRF_MODEM_LIB_TRACE_BACKEND_NAME, trace_level_definitions[lvl].name, trace_level_definitions[lvl].desc, res);
      } else {
         LOG_INF("Mode %s, %d bytes of trace data", NRF_MODEM_LIB_TRACE_BACKEND_NAME, res);
      }
   }
   return 0;
}

SH_CMD(trlvl, "", "modem trace level.", modem_trace_cmd_level, modem_trace_cmd_level_help, 0);
SH_CMD(trclr, NULL, "modem trace clear.", modem_trace_cmd_clear, NULL, 0);
SH_CMD(trinfo, NULL, "modem trace info", modem_trace_cmd_info, NULL, 0);
