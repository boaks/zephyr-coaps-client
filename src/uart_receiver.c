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

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include <modem/at_monitor.h>
#include <modem/lte_lc.h>
#include <nrf_modem_at.h>

#include "modem.h"
#include "parse.h"

LOG_MODULE_DECLARE(MODEM, CONFIG_MODEM_LOG_LEVEL);

#define CONFIG_AT_CMD_THREAD_PRIO 10
#define CONFIG_AT_CMD_MAX_LEN 2048
#define CONFIG_AT_CMD_STACK_SIZE 2048
#define CONFIG_UART_BUFFER_LEN 256

K_THREAD_STACK_DEFINE(at_cmd_stack, CONFIG_AT_CMD_STACK_SIZE);

static const struct device *const uart_dev = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_console));

static char at_cmd_buf[CONFIG_AT_CMD_MAX_LEN];
static char uart_buf[CONFIG_UART_BUFFER_LEN];

static struct k_work_q at_cmd_work_q;

static void at_cmd_send_fn(struct k_work *work)
{
   ARG_UNUSED(work);
   static struct lte_lc_ncellmeas_params params = {LTE_LC_NEIGHBOR_SEARCH_TYPE_GCI_EXTENDED_COMPLETE, 6};
   int i;

   if (strcmp(at_cmd_buf, "reset") == 0) {
      modem_factory_reset();
      LOG_INF(">> modem reseted.");
      return;
   } else if (strcmp(at_cmd_buf, "reboot") == 0) {
      LOG_INF(">> modem reboot ...");
      k_sleep(K_SECONDS(2));
      sys_reboot(SYS_REBOOT_COLD);
      return;
   } else if (strcmp(at_cmd_buf, "off") == 0) {
      modem_set_offline();
      LOG_INF(">> modem offline.");
      return;
   } else if (strcmp(at_cmd_buf, "on") == 0) {
      modem_set_normal();
      LOG_INF(">> modem online.");
      return;
   } else if (strcmp(at_cmd_buf, "sim") == 0) {
      modem_read_sim_info(NULL);
      return;
   }

   i = strstart(at_cmd_buf, "scan", true);
   if (i == 0) {
      i = strstart(at_cmd_buf, "AT%NCELLMEAS", true);
   }
   if (i > 0) {
      if (at_cmd_buf[i]) {
         if (at_cmd_buf[i++] == '=') {
            char *t = NULL;
            int type = (int)strtol(&at_cmd_buf[i], &t, 10);
            if (&at_cmd_buf[i] != t) {
               params.search_type = type + 1;
               if (*t == ',') {
                  params.gci_count = (int)strtol(++t, NULL, 10);
               }
            }
         } else {
            LOG_INF("ignore > %s", at_cmd_buf);
            return;
         }
      }
      if (params.search_type < LTE_LC_NEIGHBOR_SEARCH_TYPE_GCI_DEFAULT) {
         LOG_INF(">AT%%NCELLMEAS=%d", params.search_type - 1);
      } else {
         LOG_INF(">AT%%NCELLMEAS=%d,%d", params.search_type - 1, params.gci_count);
      }
      i = lte_lc_neighbor_cell_measurement(&params);
      if (i) {
         LOG_INF("ERROR %d", i);
      } else {
         LOG_INF("OK");
      }
      return;
   }

   if (!strstart(at_cmd_buf, "AT", true)) {
      LOG_INF("ignore > %s", at_cmd_buf);
      return;
   }

   LOG_INF(">%s", at_cmd_buf);
   i = nrf_modem_at_cmd(at_cmd_buf, sizeof(at_cmd_buf), "%s", at_cmd_buf);
   if (i < 0) {
      LOG_ERR("Error while processing AT command: %d", i);
   }

   i = strlen(at_cmd_buf) - 1;
   while (i >= 0 && (at_cmd_buf[i] == '\n' || at_cmd_buf[i] == '\r')) {
      at_cmd_buf[i--] = 0;
   }

   LOG_INF("%s", at_cmd_buf);
}

static K_WORK_DEFINE(at_cmd_send_work, at_cmd_send_fn);

static void uart_receiver_handler(uint8_t character)
{
   static bool inside_quotes = false;
   static size_t at_cmd_len = 0;
   static int64_t last = 0;

   int64_t now = k_uptime_get();
   if ((now - last) > (MSEC_PER_SEC * 30)) {
      if (at_cmd_len) {
         at_cmd_buf[at_cmd_len] = '\0';
         LOG_INF("timeout %s", at_cmd_buf);
         at_cmd_len = 0;
      }
      inside_quotes = false;
   }
   last = now;
   /* Handle control characters */
   switch (character) {
      case 0x08:
      case 0x7F:
         if (at_cmd_len > 0) {
            if (at_cmd_buf[at_cmd_len--] == '"') {
               inside_quotes = !inside_quotes;
            }
         }
         return;
      case '\r':
         if (!inside_quotes) {
            at_cmd_buf[at_cmd_len] = '\0';
            at_cmd_len = 0;
            /* Check for the presence of one printable non-whitespace character */
            for (const char *c = at_cmd_buf; *c; c++) {
               if (*c > ' ') {
                  k_work_submit_to_queue(&at_cmd_work_q, &at_cmd_send_work);
                  break;
               }
            }
            return;
         }
   }

   /* Detect AT command buffer overflow, leaving space for null */
   if (at_cmd_len > sizeof(at_cmd_buf) - 2) {
      LOG_ERR("Buffer overflow, dropping '%c'\n", character);
      return;
   }

   /* Write character to AT buffer */
   at_cmd_buf[at_cmd_len++] = character;

   /* Handle special written character */
   if (character == '"') {
      inside_quotes = !inside_quotes;
   }
}

static void uart_receiver_loop(const char *buffer, size_t len)
{
   //   LOG_INF("UART rx %u bytes", len);
   for (int index = 0; index < len; ++index) {
      if (!k_work_busy_get(&at_cmd_send_work)) {
         uart_receiver_handler(buffer[index]);
      }
   }
}

static void uart_irq_handler(const struct device *dev, void *context)
{
   if (uart_irq_rx_ready(dev)) {
      uint8_t buf[10];
      int len = uart_fifo_read(dev, buf, sizeof(buf));
      uart_receiver_loop(buf, len);
   }
}

static void uart_receiver_callback(const struct device *dev,
                                   struct uart_event *evt,
                                   void *user_data)
{
   switch (evt->type) {
      case UART_TX_DONE:
         break;

      case UART_TX_ABORTED:
         break;

      case UART_RX_RDY:
         uart_receiver_loop(&(evt->data.rx.buf[evt->data.rx.offset]), evt->data.rx.len);
         break;

      case UART_RX_BUF_REQUEST:
         break;

      case UART_RX_BUF_RELEASED:
         break;

      case UART_RX_DISABLED:
         break;

      case UART_RX_STOPPED:
         break;
   }
}

static int uart_receiver_init(const struct device *arg)
{
   int err;
   ARG_UNUSED(arg);

   /* Initialize the UART module */
   if (!device_is_ready(uart_dev)) {
      LOG_ERR("UART device not ready");
      return -EFAULT;
   }
   if (IS_ENABLED(CONFIG_NRF_SW_LPUART_INT_DRIVEN)) {
      uart_irq_callback_set(uart_dev, uart_irq_handler);
      uart_irq_rx_enable(uart_dev);
      LOG_INF("UART irq receiver initialized.");
   } else {
      err = uart_callback_set(uart_dev, uart_receiver_callback, (void *)uart_dev);
      if (err) {
         LOG_ERR("UART callback not set!");
      }
      err = uart_rx_enable(uart_dev, uart_buf, CONFIG_UART_BUFFER_LEN, 10000);
      if (err) {
         LOG_ERR("UART rx not enabled!");
      }
      LOG_INF("UART async receiver initialized.");
      err = 0;
   }
   k_work_queue_start(&at_cmd_work_q, at_cmd_stack,
                      K_THREAD_STACK_SIZEOF(at_cmd_stack),
                      CONFIG_AT_CMD_THREAD_PRIO, NULL);

   return err;
}

SYS_INIT(uart_receiver_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
