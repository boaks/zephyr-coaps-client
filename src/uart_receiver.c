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
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>

#include <modem/at_monitor.h>
#include <nrf_modem_at.h>

#include "appl_diagnose.h"
#include "modem.h"
#include "parse.h"
#include "ui.h"

LOG_MODULE_DECLARE(MODEM, CONFIG_MODEM_LOG_LEVEL);

#define CONFIG_AT_CMD_THREAD_PRIO 10
#define CONFIG_AT_CMD_MAX_LEN 2048
#define CONFIG_AT_CMD_STACK_SIZE 2048
#define CONFIG_UART_BUFFER_LEN 256

#define CONFIG_UART_RX_CHECK_INTERVAL_MS 50
#define CONFIG_UART_RX_CHECK_INTERVAL_S 60
#define CONFIG_UART_RX_SUSPEND_DELAY_S 5

static void uart_enable_rx_fn(struct k_work *work);
void dtls_trigger(void);

static const struct device *const uart_dev = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_console));

static char at_cmd_buf[CONFIG_AT_CMD_MAX_LEN];
static char uart_buf[CONFIG_UART_BUFFER_LEN];

static struct k_work_q at_cmd_work_q;
static K_WORK_DELAYABLE_DEFINE(uart_enable_rx_work, uart_enable_rx_fn);
static K_THREAD_STACK_DEFINE(at_cmd_stack, CONFIG_AT_CMD_STACK_SIZE);

#if (DT_NODE_HAS_STATUS(DT_NODELABEL(rx0), okay))
static const struct gpio_dt_spec uart_rx = GPIO_DT_SPEC_GET(DT_NODELABEL(rx0), gpios);
static struct gpio_callback uart_rx_cb_data;

static int uart_get_lines(void)
{
   int res = -ENODATA;
   if (device_is_ready(uart_rx.port)) {
      res = gpio_pin_get(uart_rx.port, uart_rx.pin);
   }
   return res;
}

static void uart_rx_line_active(const struct device *dev, struct gpio_callback *cb,
                                uint32_t pins)
{
   if ((BIT(uart_rx.pin) & pins) == 0) {
      return;
   }
   gpio_pin_interrupt_configure_dt(&uart_rx, GPIO_INT_DISABLE);
   k_work_reschedule_for_queue(&at_cmd_work_q, &uart_enable_rx_work, K_MSEC(CONFIG_UART_RX_CHECK_INTERVAL_MS));
}

static int uart_enable_rx_interrupt(void)
{
   gpio_pin_configure_dt(&uart_rx, GPIO_INPUT);
   return gpio_pin_interrupt_configure_dt(&uart_rx, GPIO_INT_LEVEL_HIGH);
}

static int uart_init_lines(void)
{
   gpio_init_callback(&uart_rx_cb_data, uart_rx_line_active, BIT(uart_rx.pin));
   return gpio_add_callback(uart_rx.port, &uart_rx_cb_data);
}

#else
static int uart_get_lines(void)
{
   return -ENODATA;
}

static int uart_enable_rx_interrupt(void)
{
   return -ENOTSUP;
}

static int uart_init_lines(void)
{
   return -ENOTSUP;
}
#endif

static void uart_enable_rx_fn(struct k_work *work)
{
   int err = uart_get_lines();
   if (err == 1) {
      pm_device_action_run(uart_dev, PM_DEVICE_ACTION_RESUME);
   } else if (err == 0) {
      pm_device_action_run(uart_dev, PM_DEVICE_ACTION_SUSPEND);
   }
   if (err == 1 || err == -ENODATA) {
      err = uart_err_check(uart_dev);
      if (err && err != -ENOSYS) {
         k_work_reschedule_for_queue(&at_cmd_work_q, &uart_enable_rx_work, K_MSEC(CONFIG_UART_RX_CHECK_INTERVAL_MS));
         return;
      }
      err = uart_rx_enable(uart_dev, uart_buf, CONFIG_UART_BUFFER_LEN, 10000);
      if (err == -EBUSY) {
         LOG_INF("UART async rx already enabled.");
         return;
      } else if (err) {
         LOG_ERR("UART async rx not enabled! %d", err);
      } else {
         LOG_INF("UART async rx enabled.");
         return;
      }
   }
   LOG_INF("UART not async rx ready.");
   k_work_reschedule_for_queue(&at_cmd_work_q, &uart_enable_rx_work, K_SECONDS(CONFIG_UART_RX_CHECK_INTERVAL_S));
   uart_enable_rx_interrupt();
}

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
      LOG_INF(">> device reboot ...");
      appl_reboot(ERROR_CODE_CMD, 2000);
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
   } else if (strcmp(at_cmd_buf, "send") == 0) {
      dtls_trigger();
      return;
   } else if (strcmp(at_cmd_buf, "help") == 0) {
      LOG_INF("> help: 0.1");
      LOG_INF("  at???  : modem at-cmd.");
      LOG_INF("  on     : switch modem on.");
      LOG_INF("  off    : switch modem off.");
      LOG_INF("  reset  : modem factory reset.");
      LOG_INF("  reboot : reboot device.");
      LOG_INF("  scan   : network scan.");
      LOG_INF("  send   : send message.");
      LOG_INF("  sim    : read SIM-card info.");
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

static bool uart_receiver_handler(uint8_t character)
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
         return false;
      case '\r':
      case '\n':
         if (!inside_quotes) {
            at_cmd_buf[at_cmd_len] = '\0';
            at_cmd_len = 0;
            /* Check for the presence of one printable non-whitespace character */
            for (const char *c = at_cmd_buf; *c; c++) {
               if (*c > ' ') {
                  k_work_submit_to_queue(&at_cmd_work_q, &at_cmd_send_work);
                  return true;
               }
            }
            return false;
         }
   }

   /* Detect AT command buffer overflow, leaving space for null */
   if (at_cmd_len > sizeof(at_cmd_buf) - 2) {
      LOG_ERR("Buffer overflow, dropping '%c'\n", character);
      return false;
   }

   /* Write character to AT buffer */
   at_cmd_buf[at_cmd_len++] = character;

   /* Handle special written character */
   if (character == '"') {
      inside_quotes = !inside_quotes;
   }
   return false;
}

static void uart_receiver_loop(const char *buffer, size_t len)
{
   //   LOG_INF("UART rx %u bytes", len);
   if (!k_work_busy_get(&at_cmd_send_work)) {
      for (int index = 0; index < len; ++index) {
         if (uart_receiver_handler(buffer[index])) {
            break;
         }
      }
   } else {
      LOG_INF("Modem busy ...");
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
         k_work_reschedule_for_queue(&at_cmd_work_q, &uart_enable_rx_work, K_SECONDS(CONFIG_UART_RX_SUSPEND_DELAY_S));
         uart_enable_rx_interrupt();
         break;
   }
}

static int uart_receiver_init(void)
{
   int err;

   /* Initialize the UART module */
   if (!device_is_ready(uart_dev)) {
      LOG_ERR("UART device not ready");
      return -EFAULT;
   }
   err = uart_callback_set(uart_dev, uart_receiver_callback, (void *)uart_dev);
   if (err) {
      LOG_ERR("UART callback not set!");
      err = 0;
   }
   uart_init_lines();

   k_work_queue_start(&at_cmd_work_q, at_cmd_stack,
                      K_THREAD_STACK_SIZEOF(at_cmd_stack),
                      CONFIG_AT_CMD_THREAD_PRIO, NULL);

   k_work_reschedule_for_queue(&at_cmd_work_q, &uart_enable_rx_work, K_MSEC(CONFIG_UART_RX_CHECK_INTERVAL_MS));

   return err;
}

SYS_INIT(uart_receiver_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
