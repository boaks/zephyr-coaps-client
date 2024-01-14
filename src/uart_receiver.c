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
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_backend_std.h>
#include <zephyr/logging/log_core.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log_output.h>
#include <zephyr/logging/log_output_dict.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/cbprintf.h>

#include <modem/at_monitor.h>
#include <modem_at.h>

#ifdef CONFIG_UART_UPDATE
#include "appl_update.h"
#include "appl_update_xmodem.h"
#endif

#include "appl_diagnose.h"
#include "dtls_client.h"
#include "io_job_queue.h"
#include "modem.h"
#include "modem_desc.h"
#include "parse.h"
#include "uart_cmd.h"
#include "ui.h"

LOG_MODULE_DECLARE(MODEM, CONFIG_MODEM_LOG_LEVEL);

#define CONFIG_AT_CMD_THREAD_PRIO 10
#define CONFIG_AT_CMD_MAX_LEN 2048
#define CONFIG_AT_CMD_STACK_SIZE 2048
#define CONFIG_UART_THREAD_PRIO 5
#define CONFIG_UART_BUFFER_LEN 256
#define CONFIG_UART_STACK_SIZE 1152

#define CONFIG_UART_RX_CHECK_INTERVAL_MS 50
#define CONFIG_UART_RX_CHECK_INTERVAL_S 60

#define CONFIG_UART_RX_INPUT_TIMEOUT_S 30

#define CONFIG_UART_TX_OUTPUT_TIMEOUT_MS 1500

static void uart_enable_rx_fn(struct k_work *work);
static void uart_pause_tx_fn(struct k_work *work);
static int uart_init(void);
static void at_cmd_send_fn(struct k_work *work);
static void at_cmd_response_fn(struct k_work *work);

static const struct device *const uart_dev = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_console));

static int64_t at_cmd_time = 0;
static size_t at_cmd_max_length = 0;

static char at_cmd_buf[CONFIG_AT_CMD_MAX_LEN];
static int uart_rx_buf_id = 0;
static uint8_t uart_rx_buf[2][CONFIG_UART_BUFFER_LEN];

#define UART_TX_ENABLED 0
#define UART_AT_CMD_EXECUTING 1
#define UART_AT_CMD_PENDING 2
#define UART_SUSPENDED 3
#define UART_UPDATE 4
#define UART_UPDATE_START 5
#define UART_UPDATE_APPLY 6

static atomic_t uart_at_state = ATOMIC_INIT(0);

#ifdef CONFIG_UART_UPDATE

static void uart_xmodem_process_fn(struct k_work *work);
static void uart_xmodem_start_fn(struct k_work *work);

static atomic_t xmodem_retries = ATOMIC_INIT(0);

static K_WORK_DELAYABLE_DEFINE(uart_xmodem_start_work, uart_xmodem_start_fn);
static K_WORK_DELAYABLE_DEFINE(uart_xmodem_nak_work, uart_xmodem_process_fn);
static K_WORK_DELAYABLE_DEFINE(uart_xmodem_ack_work, uart_xmodem_process_fn);
static K_WORK_DELAYABLE_DEFINE(uart_xmodem_timeout_work, uart_xmodem_process_fn);
static K_WORK_DEFINE(uart_xmodem_write_work, uart_xmodem_process_fn);
static K_WORK_DEFINE(uart_xmodem_ready_work, uart_xmodem_process_fn);
#endif

static K_WORK_DELAYABLE_DEFINE(uart_enable_rx_work, uart_enable_rx_fn);
static K_WORK_DEFINE(uart_start_pause_tx_work, uart_pause_tx_fn);
static K_WORK_DEFINE(at_cmd_send_work, at_cmd_send_fn);
static K_WORK_DEFINE(at_cmd_response_work, at_cmd_response_fn);

static struct k_work_q at_cmd_work_q;
static K_THREAD_STACK_DEFINE(at_cmd_stack, CONFIG_AT_CMD_STACK_SIZE);

static struct k_work_q uart_work_q;
static K_THREAD_STACK_DEFINE(uart_stack, CONFIG_UART_STACK_SIZE);

static int uart_reschedule_rx_enable(const k_timeout_t delay)
{
   return k_work_reschedule_for_queue(&uart_work_q, &uart_enable_rx_work, delay);
}

#if (DT_NODE_HAS_STATUS(DT_NODELABEL(rx0), okay))

#ifdef CONFIG_UART_RX_PULLDOWN
#define UART_RX_EXTRA_FLAGS (GPIO_INPUT | GPIO_PULL_DOWN)
#else
#define UART_RX_EXTRA_FLAGS GPIO_INPUT
#endif /* CONFIG_UART_RX_PULLDOWN */

static const struct gpio_dt_spec uart_rx = GPIO_DT_SPEC_GET(DT_NODELABEL(rx0), gpios);
static struct gpio_callback uart_rx_cb_data;


static int uart_get_lines(void)
{
   int res = -ENODATA;
   if (device_is_ready(uart_rx.port)) {
      res = gpio_pin_get_dt(&uart_rx);
   }
   return res;
}

static void uart_rx_line_active(const struct device *dev, struct gpio_callback *cb,
                                uint32_t pins)
{
   gpio_pin_interrupt_configure_dt(&uart_rx, GPIO_INT_DISABLE);
   uart_reschedule_rx_enable(K_MSEC(CONFIG_UART_RX_CHECK_INTERVAL_MS));
}

static int uart_enable_rx_interrupt(void)
{
   gpio_pin_configure_dt(&uart_rx, UART_RX_EXTRA_FLAGS);
   return gpio_pin_interrupt_configure_dt(&uart_rx, GPIO_INT_LEVEL_HIGH);
}

static int uart_init_lines(void)
{
   gpio_pin_configure_dt(&uart_rx, UART_RX_EXTRA_FLAGS);
   gpio_init_callback(&uart_rx_cb_data, uart_rx_line_active, BIT(uart_rx.pin));
   return gpio_add_callback(uart_rx.port, &uart_rx_cb_data);
}

#else
static inline int uart_get_lines(void)
{
   return -ENODATA;
}

static inline int uart_enable_rx_interrupt(void)
{
   return -ENOTSUP;
}

static inline int uart_init_lines(void)
{
   return -ENOTSUP;
}
#endif

static int line_length(const uint8_t *data, size_t length)
{
   int len = length;
   if (len > 0) {
      --len;
      while (len >= 0 && (data[len] == '\n' || data[len] == '\r')) {
         --len;
      }
      ++len;
   }
   return len;
}

#ifdef CONFIG_LOG_BACKEND_UART_RECEIVER

#ifndef CONFIG_LOG_MODE_IMMEDIATE
static K_MUTEX_DEFINE(uart_tx_mutex);
static K_CONDVAR_DEFINE(uart_tx_condvar);
static K_WORK_DELAYABLE_DEFINE(uart_end_pause_tx_work, uart_pause_tx_fn);
static bool uart_tx_in_pause = false;
#endif

static K_SEM_DEFINE(uart_tx_sem, 0, 1);

static uint8_t uart_tx_buf[256];
static atomic_t uart_tx_buf_offset = ATOMIC_INIT(0);
static atomic_t uart_tx_buf_lines = ATOMIC_INIT(0);
static const bool uart_tx_immediate = IS_ENABLED(CONFIG_LOG_MODE_IMMEDIATE);

static void uart_tx_pause(bool pause)
{
#ifdef CONFIG_LOG_MODE_IMMEDIATE
   ARG_UNUSED(pause);
#else
   k_mutex_lock(&uart_tx_mutex, K_FOREVER);
   if (uart_tx_in_pause != pause) {
      uart_tx_in_pause = pause;
      if (pause) {
         work_schedule_for_io_queue(&uart_end_pause_tx_work, K_SECONDS(30));
      } else {
         k_work_cancel_delayable(&uart_end_pause_tx_work);
         k_condvar_broadcast(&uart_tx_condvar);
      }
   }
   k_mutex_unlock(&uart_tx_mutex);
#endif
}

static void uart_tx_off(bool off)
{
#ifdef CONFIG_LOG_MODE_IMMEDIATE
   ARG_UNUSED(off);
#else
   if (off) {
      atomic_clear_bit(&uart_at_state, UART_TX_ENABLED);
   } else {
      atomic_set_bit(&uart_at_state, UART_TX_ENABLED);
   }
#endif
}

static inline void uart_tx_ready(void)
{
   k_sem_give(&uart_tx_sem);
}

static int uart_tx_out(uint8_t *data, size_t length, bool panic)
{
   if (!atomic_test_bit(&uart_at_state, UART_SUSPENDED)) {
      if (panic || length == 1) {
         for (size_t i = 0; i < length; i++) {
            uart_poll_out(uart_dev, data[i]);
         }
      } else {

         k_sem_reset(&uart_tx_sem);

         /* SYS_FOREVER_US disable timeout */
         uart_tx(uart_dev, data, length, SYS_FOREVER_US);

         k_sem_take(&uart_tx_sem, K_MSEC(CONFIG_UART_TX_OUTPUT_TIMEOUT_MS));
      }
   }
   return length;
}

static void uart_tx_out_flush(bool panic);

static int uart_tx_out_buf(int c)
{
   int idx;

   if (uart_tx_immediate) {
      char x = (char)c;
      uart_tx_out((uint8_t *)&x, 1, true);
      return 0;
   }

   if (atomic_get(&uart_tx_buf_offset) == sizeof(uart_tx_buf)) {
      uart_tx_out_flush(false);
   }

   idx = atomic_inc(&uart_tx_buf_offset);
   uart_tx_buf[idx] = (uint8_t)c;

   return 0;
}

static void uart_tx_out_nl(void)
{
   int lines = 0;
   while (!atomic_cas(&uart_tx_buf_lines, lines, 0)) {
      lines = atomic_get(&uart_tx_buf_lines);
   }
   while (lines > 0) {
      uart_tx_out_buf('\r');
      uart_tx_out_buf('\n');
      --lines;
   }
}

static void uart_tx_out_flush(bool panic)
{
   atomic_cas(&uart_tx_buf_lines, 0, 1);
   uart_tx_out_nl();
   uart_tx_out(uart_tx_buf, atomic_get(&uart_tx_buf_offset), panic);
   atomic_set(&uart_tx_buf_offset, 0);
}

static int uart_tx_out_func(int c, void *ctx)
{
   ARG_UNUSED(ctx);

   if (c == '\r') {
   } else if (c == '\n') {
      atomic_inc(&uart_tx_buf_lines);
   } else {
      uart_tx_out_nl();
      uart_tx_out_buf(c);
   }

   return 0;
}

#define HEXDUMP_BYTES_IN_LINE 16
#define HEXDUMP_BYTES_IN_BLOCK 8

static void uart_log_spaces(size_t len)
{
   while (len--) {
      uart_tx_out_func(' ', NULL);
   }
}

static void uart_log_dump_hex_line(int prefix, int bytes, const uint8_t *data, size_t data_len)
{
   uart_log_spaces(prefix);

   for (int i = 0; i < bytes; i++) {
      if (i > 0 && !(i % HEXDUMP_BYTES_IN_BLOCK)) {
         uart_tx_out_func(' ', NULL);
      }

      if (i < data_len) {
         cbprintf(uart_tx_out_func, NULL, "%02x ", data[i]);
      } else {
         uart_log_spaces(3);
      }
   }

   uart_tx_out_func('|', NULL);

   for (int i = 0; i < data_len; i++) {
      int c = data[i];

      if (!isprint(c)) {
         c = '.';
      }
      if (!(i % HEXDUMP_BYTES_IN_BLOCK)) {
         uart_tx_out_func(' ', NULL);
      }
      uart_tx_out_func(c, NULL);
   }
   uart_tx_out_func('\n', NULL);
}

static void uart_log_dump_hex(int prefix, const uint8_t *data, size_t data_len)
{
   size_t bytes = data_len <= HEXDUMP_BYTES_IN_BLOCK ? HEXDUMP_BYTES_IN_BLOCK : HEXDUMP_BYTES_IN_LINE;
   while (data_len) {
      size_t length = MIN(data_len, HEXDUMP_BYTES_IN_LINE);
      uart_log_dump_hex_line(prefix, bytes, data, length);
      data += length;
      data_len -= length;
   }
}

static const char *uart_log_get_source_name(struct log_msg *msg)
{
   void *source = (void *)log_msg_get_source(msg);
   if (source != NULL) {
      uint8_t domain_id = log_msg_get_domain(msg);
      int16_t source_id = IS_ENABLED(CONFIG_LOG_RUNTIME_FILTERING) ? log_dynamic_source_id(source) : log_const_source_id(source);
      if (source_id >= 0) {
         return log_source_name_get(domain_id, source_id);
      }
   }
   return NULL;
}

static bool uart_log_filter(struct log_msg *msg)
{
   static int last_level = -1;
   static const char *last_source = NULL;

   const char *source_name = uart_log_get_source_name(msg);

   if (last_level == msg->hdr.desc.level) {
      if (source_name && strcmp("i2c_nrfx_twim", source_name) == 0) {
         if (last_source && strcmp(last_source, source_name) == 0) {
            return true;
         }
      }
   }
   last_level = msg->hdr.desc.level;
   last_source = source_name;

   return false;
}

static void uart_log_process(const struct log_backend *const backend,
                             union log_msg_generic *msg)
{
   if (atomic_test_bit(&uart_at_state, UART_SUSPENDED) ||
       !atomic_test_bit(&uart_at_state, UART_TX_ENABLED)) {
      // silently drop during suspended UART or XMODEM
      return;
   }

   {
      static char level_tab[] = {0, 'E', 'W', 'I', 'D'};

      size_t plen;
      size_t dlen;
      uint8_t *package = log_msg_get_package(&msg->log, &plen);
      uint8_t *data = log_msg_get_data(&msg->log, &dlen);

      if (plen || dlen) {
         int prefix = 0;
         char level = level_tab[msg->log.hdr.desc.level];

         if (uart_log_filter(&msg->log)) {
            // drop filtered msg
            return;
         }

#ifndef CONFIG_LOG_MODE_IMMEDIATE
         k_mutex_lock(&uart_tx_mutex, K_FOREVER);
         if (uart_tx_in_pause) {
            k_condvar_wait(&uart_tx_condvar, &uart_tx_mutex, K_FOREVER);
         }
         k_mutex_unlock(&uart_tx_mutex);
#endif
         if (level) {
            int cycles = sys_clock_hw_cycles_per_sec();
            log_timestamp_t seconds = (msg->log.hdr.timestamp / cycles) % 100;
            log_timestamp_t milliseconds = (msg->log.hdr.timestamp * 1000 / cycles) % 1000;
            if (atomic_test_bit(&uart_at_state, UART_UPDATE)) {
               level = 'u';
            } else if (atomic_test_bit(&uart_at_state, UART_AT_CMD_PENDING) ||
                       atomic_test_bit(&uart_at_state, UART_AT_CMD_EXECUTING)) {
               level = 'b';
            }
            prefix = cbprintf(uart_tx_out_func, NULL, "%c %02d.%03d : ",
                              level, seconds, milliseconds);
         }
         if (plen) {
            cbpprintf(uart_tx_out_func, NULL, package);
         }
         uart_tx_out_flush(false);
         if (dlen) {
            uart_log_dump_hex(prefix, data, dlen);
            uart_tx_out_flush(false);
         }
      }
   }
}

static void uart_log_init(struct log_backend const *const backend)
{
   ARG_UNUSED(backend);
   uart_tx_off(false);
   uart_init();
}

static void uart_log_panic(struct log_backend const *const backend)
{
   ARG_UNUSED(backend);

   uart_tx_out_flush(true);
}

static void uart_log_dropped(const struct log_backend *const backend, uint32_t cnt)
{
   ARG_UNUSED(backend);
   cbprintf(uart_tx_out_func, NULL,
            "--- %u  messages dropped ---", cnt);
   uart_tx_out_flush(false);
}

const struct log_backend_api uart_log_backend_api = {
    .process = uart_log_process,
    .panic = uart_log_panic,
    .init = uart_log_init,
    .dropped = IS_ENABLED(CONFIG_LOG_MODE_IMMEDIATE) ? NULL : uart_log_dropped,
    .format_set = NULL,
};

LOG_BACKEND_DEFINE(uart_log_backend, uart_log_backend_api,
                   IS_ENABLED(CONFIG_LOG_BACKEND_UART_RECEIVER_AUTOSTART));

AT_MONITOR(uart_monitor, ANY, uart_monitor_handler);

static const char *IGNORE_NOTIFY[] = {"%NCELLMEAS:", "%XMODEMSLEEP:", NULL};

static int uart_monitor_ignore_notify(const char *notif)
{
   int index = 0;
   while (IGNORE_NOTIFY[index]) {
      if (strstart(notif, IGNORE_NOTIFY[index], false)) {
         return index;
      }
      ++index;
   }
   return -1;
}

static void uart_monitor_handler(const char *notif)
{
   int index;

   if (appl_reboots()) {
      return;
   }
   index = uart_monitor_ignore_notify(notif);
   if (index < 0) {
      int len;
      printk("%s", notif);
      len = strstart(notif, "+CEREG:", false);
      if (len > 0) {
         const char *cur = parse_next_chars(notif + len, ',', 4);
         if (*cur && strstart(cur, "0,", false)) {
            int code = atoi(cur + 2);
            const char *desc = modem_get_emm_cause_description(code);
            if (desc) {
               LOG_INF("LTE +CEREG: rejected, %s", desc);
            } else {
               LOG_INF("LTE +CEREG: rejected, cause %d", code);
            }
         }
      }
   }
}
#else
static inline void uart_tx_pause(bool pause)
{
   ARG_UNUSED(pause);
   // empty
}

static inline void uart_tx_ready(void)
{
   // empty
}

#endif /* CONFIG_LOG_BACKEND_UART_RECEIVER */

static void uart_pause_tx_fn(struct k_work *work)
{
   uart_tx_pause(&uart_start_pause_tx_work == work);
}

static void uart_enable_rx_fn(struct k_work *work)
{
   int err = uart_get_lines();
   if (err == 1) {
      pm_device_action_run(uart_dev, PM_DEVICE_ACTION_RESUME);
      atomic_clear_bit(&uart_at_state, UART_SUSPENDED);
   } else if (err == 0) {
      if (k_uptime_get() > 10000 && k_sem_count_get(&uart_tx_sem) == 0) {
         // early suspend seems to crash
         atomic_set_bit(&uart_at_state, UART_SUSPENDED);
         pm_device_action_run(uart_dev, PM_DEVICE_ACTION_SUSPEND);
         uart_tx_ready();
      }
   }
   if (err == 1 || err == -ENODATA) {
      err = uart_err_check(uart_dev);
      if (err && err != -ENOSYS) {
         LOG_DBG("UART async rx err %d", err);
         uart_reschedule_rx_enable(K_MSEC(CONFIG_UART_RX_CHECK_INTERVAL_MS));
         return;
      }
      err = uart_rx_enable(uart_dev, uart_rx_buf[uart_rx_buf_id], CONFIG_UART_BUFFER_LEN, 10000);
      if (err == -EBUSY) {
         LOG_DBG("UART async rx already enabled.");
         return;
      } else if (err) {
         LOG_DBG("UART async rx not enabled! %d", err);
      } else {
         LOG_INF("UART async rx enabled.");
         return;
      }
   }
   LOG_DBG("UART not async rx ready.");
   uart_reschedule_rx_enable(K_SECONDS(CONFIG_UART_RX_CHECK_INTERVAL_S));
   uart_enable_rx_interrupt();
}

static void at_cmd_result(int res)
{
   bool finish = atomic_test_and_clear_bit(&uart_at_state, UART_AT_CMD_EXECUTING);
   if (res > 0) {
      // noops
   } else {
      if (res < -1) {
         const char *desc = "";
         switch (res) {
            case -EFAULT:
               desc = "off";
               break;
            case -EBUSY:
               desc = "busy";
               break;
            case -EINVAL:
               desc = "invalid parameter";
               break;
            case -ESHUTDOWN:
               desc = "in shutdown";
               break;
            case -EINPROGRESS:
               desc = "in progress";
               break;
            case -ENOTSUP:
               desc = "not supported";
               break;
            default:
               desc = strerror(-res);
               break;
         }
         LOG_INF("ERROR %d (%s)\n", -res, desc);
      }

      if (finish) {
         if (res < 0) {
            printk("ERROR\n");
         } else {
            printk("OK\n");
         }
      }
   }
}

static void at_coneval_result(const char *result)
{
   unsigned int status = 0;
   unsigned int rrc = 0;
   unsigned int quality = 0;
   int rsrp = 0;
   int rsrq = 0;
   int snr = 0;
   int err = sscanf(result, "%u,%u,%u,%d,%d,%d,",
                    &status, &rrc, &quality, &rsrp, &rsrq, &snr);
   if (err == 1) {
      const char *desc = NULL;
      lte_network_info_t info;

      switch (status) {
         case 1:
            memset(&info, 0, sizeof(info));
            if (!modem_get_network_info(&info) && info.cell != LTE_LC_CELL_EUTRAN_ID_INVALID) {
               LOG_INF("> eval failed: cell %d/0x%08x not available!", info.cell, info.cell);
               return;
            } else {
               desc = "cell not available";
            }
            break;
         case 2:
            desc = "UICC missing (SIM card)";
            break;
         case 3:
            desc = "only barred cells available";
            break;
         case 4:
            desc = "modem busy";
            break;
         case 5:
            desc = "evaluation aborted";
            break;
         case 6:
            desc = "not registered";
            break;
         case 7:
            desc = "unspecific failure";
            break;
      }
      if (desc) {
         LOG_INF("> eval failed: %s", desc);
      } else {
         LOG_INF("> eval failed: %d", status);
      }
   } else if (err == 6) {
      const char *desc = NULL;
      switch (quality) {
         case 5:
            desc = "bad";
            break;
         case 6:
            desc = "poor";
            break;
         case 7:
            desc = "normal";
            break;
         case 8:
            desc = "good";
            break;
         case 9:
            desc = "excellent";
            break;
      }
      rsrp -= 140;
      rsrq = (rsrq - 39) / 2;
      snr -= 24;
      if (desc) {
         LOG_INF("> eval: quality %s, rsrp %d dBm, rsrq %d dB, snr %d dB",
                 desc, rsrp, rsrq, snr);
      } else {
         LOG_INF("> eval: quality %d, rsrp %d dBm, rsrq %d dB, snr %d dB",
                 quality, rsrp, rsrq, snr);
      }
   } else {
      LOG_INF("> eval parse %d", err);
   }
}

static void at_cmd_finish(void)
{
   if (atomic_test_and_clear_bit(&uart_at_state, UART_AT_CMD_PENDING)) {
      at_cmd_time = k_uptime_get() - at_cmd_time;
      if (at_cmd_time > 5000) {
         LOG_INF("%ld s", (long)((at_cmd_time + 500) / 1000));
      } else if (at_cmd_time > 500) {
         LOG_INF("%ld ms", (long)at_cmd_time);
      }
   }
}

static void at_cmd_response_fn(struct k_work *work)
{
   int index = strstart(at_cmd_buf, "%CONEVAL: ", true);

   printk("%s", at_cmd_buf);
   if (index > 0) {
      at_coneval_result(&at_cmd_buf[index]);
   }

   if (strend(at_cmd_buf, "OK", false)) {
      at_cmd_result(1);
   } else if (strend(at_cmd_buf, "ERROR", false)) {
      at_cmd_result(1);
   } else {
      at_cmd_result(-1);
   }
   at_cmd_finish();
}

static void at_cmd_resp_callback(const char *at_response)
{
   size_t len = strlen(at_response);
   if (len > sizeof(at_cmd_buf) - 1) {
      len = sizeof(at_cmd_buf) - 1;
   }
   len = line_length(at_response, len);
   strncpy(at_cmd_buf, at_response, len);
   at_cmd_buf[len] = 0;
   k_work_submit_to_queue(&at_cmd_work_q, &at_cmd_response_work);
}

#define RESULT(X) ((X < 0) ? (X) : 0)
#define PENDING(X) ((X < 0) ? (X) : 1)

static const struct uart_cmd_entry *at_cmd_get(const char *cmd)
{
   STRUCT_SECTION_FOREACH(uart_cmd_entry, e)
   {
      if (strstartsep(cmd, e->cmd, true, " ") > 0) {
         return e;
      } else if (e->at_cmd && e->at_cmd[0] && strstartsep(cmd, e->at_cmd, true, " =") > 0) {
         return e;
      }
   }
   return NULL;
}

static int at_cmd_help(const char *parameter)
{
   if (*parameter) {
      const struct uart_cmd_entry *cmd = at_cmd_get(parameter);
      const char *msg = cmd ? "no details available." : "cmd unknown.";
      if (cmd && cmd->help && cmd->help_handler) {
         cmd->help_handler();
      } else {
         LOG_INF("> help %s:", parameter);
         LOG_INF("  %s", msg);
      }
   } else {
      LOG_INF("> help:");
      LOG_INF("  %-*s: generic modem at-cmd.(*)", at_cmd_max_length, "at\?\?\?");

      STRUCT_SECTION_FOREACH(uart_cmd_entry, e)
      {
         if (e->help) {
            const char *details = "";
            if (e->at_cmd) {
               if (e->help_handler) {
                  details = "(*?)";
               } else {
                  details = "(*)";
               }
            } else if (e->help_handler) {
               details = "(?)";
            }
            LOG_INF("  %-*s: %s%s", at_cmd_max_length, e->cmd, e->help, details);
         }
      }
      LOG_INF("  %-*s: AT-cmd is used, maybe busy.", at_cmd_max_length, "*");
      LOG_INF("  %-*s: help <cmd> available.", at_cmd_max_length, "?");
   }
   return 0;
}

static void at_cmd_help_help(void)
{
   /* empty by intention */
}

UART_CMD(help, NULL, NULL, at_cmd_help, at_cmd_help_help, 0);

static int at_cmd()
{
   int res = 0;
   int i;
   const struct uart_cmd_entry *cmd = at_cmd_get(at_cmd_buf);
   const char *at_cmd = at_cmd_buf;

   if (cmd) {
      i = strstartsep(at_cmd_buf, cmd->cmd, true, " ");
      if (!i && cmd->at_cmd && cmd->at_cmd[0]) {
         i = strstartsep(at_cmd_buf, cmd->at_cmd, true, " =");
      }
      if (at_cmd_buf[i] && !cmd->help_handler) {
         LOG_INF("%s doesn't support parameter '%s'!", cmd->cmd, &at_cmd_buf[i]);
         return 1;
      }
      if (cmd->at_cmd) {
         if (cmd->at_cmd[0] && !cmd->handler) {
            /* simple AT cmd*/
            at_cmd = cmd->at_cmd;
            goto at_cmd_modem;
         } else {
            /* handler AT cmd*/
            if (atomic_test_and_set_bit(&uart_at_state, UART_AT_CMD_PENDING)) {
               LOG_INF("Modem pending ...");
               return 1;
            }
            at_cmd_time = k_uptime_get();
            res = cmd->handler(&at_cmd_buf[i]);
            if (res == 1) {
               if (cmd->send) {
                  LOG_INF(">> (new %s) send", cmd->cmd);
                  dtls_cmd_trigger(true, cmd->send, NULL, 0);
               }
               res = 0;
            } else {
               res = RESULT(res);
            }
            at_cmd_finish();
         }
      } else {
         res = cmd->handler(&at_cmd_buf[i]);
         res = RESULT(res);
      }
      if (res == -EINVAL && cmd->help_handler) {
         cmd->help_handler();
      }
      return res;
   }
at_cmd_modem:
   if (!strstart(at_cmd, "AT", true)) {
      LOG_INF("ignore > %s", at_cmd);
      LOG_INF("> 'help' for available commands.");
      return -1;
   }
   if (atomic_test_and_set_bit(&uart_at_state, UART_AT_CMD_PENDING)) {
      LOG_INF("Modem pending ...");
      return 1;
   }
   LOG_INF(">%s", at_cmd);
   at_cmd_time = k_uptime_get();
   res = modem_at_cmd_async(at_cmd_resp_callback, NULL, at_cmd);
   if (res < 0) {
      at_cmd_finish();
   } else {
      res = 1;
   }
   return res;
}

static void at_cmd_send_fn(struct k_work *work)
{
   ARG_UNUSED(work);
   if (!atomic_test_bit(&uart_at_state, UART_UPDATE)) {
      int res = 0;
      uart_tx_pause(false);
      res = at_cmd();
      at_cmd_result(res);
   }
}

static bool uart_receiver_handler(uint8_t character)
{
   // interrupt context!
   static bool inside_quotes = false;
   static size_t at_cmd_len = 0;
   static int64_t last = 0;

   int64_t now = k_uptime_get();
   if ((now - last) > (MSEC_PER_SEC * CONFIG_UART_RX_INPUT_TIMEOUT_S)) {
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
                  if (!atomic_test_and_set_bit(&uart_at_state, UART_AT_CMD_EXECUTING)) {
                     k_work_submit_to_queue(&at_cmd_work_q, &at_cmd_send_work);
                  } else {
                     LOG_INF("Modem busy ???");
                  }
                  return true;
               }
            }
            return false;
         }
      default:
         break;
   }

   /* Detect AT command buffer overflow, leaving space for null */
   if (at_cmd_len > sizeof(at_cmd_buf) - 2) {
      LOG_ERR("Buffer overflow, dropping '%c'", character);
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

#ifdef CONFIG_UART_UPDATE

static void uart_xmodem_start_fn(struct k_work *work)
{
   ARG_UNUSED(work);
   int res = 0;

   int retry = atomic_inc(&xmodem_retries);

   if (retry == 0) {
      atomic_set_bit(&uart_at_state, UART_UPDATE_START);
      LOG_INF("Please start xmodem, update begins in about 10s!");
      k_sleep(K_MSEC(500));
      uart_tx_off(true);
      res = appl_update_erase();
      if (res) {
         appl_update_cancel();
         atomic_clear_bit(&uart_at_state, UART_UPDATE);
         atomic_clear_bit(&uart_at_state, UART_UPDATE_START);
         atomic_clear_bit(&uart_at_state, UART_UPDATE_APPLY);
         uart_tx_off(false);
         LOG_INF("Failed erase update area! %d", res);
         return;
      }
      ui_led_op(LED_COLOR_ALL, LED_BLINK);
   }

   if (retry < 3) {
      // CRC
      appl_update_xmodem_start(at_cmd_buf, sizeof(at_cmd_buf), true);
      uart_poll_out(uart_dev, XMODEM_CRC);
      work_reschedule_for_cmd_queue(&uart_xmodem_start_work, K_MSEC(2000));
   } else if (retry < 6) {
      // CHECKSUM
      appl_update_xmodem_start(at_cmd_buf, sizeof(at_cmd_buf), false);
      uart_poll_out(uart_dev, XMODEM_NAK);
      work_reschedule_for_cmd_queue(&uart_xmodem_start_work, K_MSEC(2000));
   } else {
      appl_update_cancel();
      atomic_clear_bit(&uart_at_state, UART_UPDATE);
      atomic_clear_bit(&uart_at_state, UART_UPDATE_START);
      atomic_clear_bit(&uart_at_state, UART_UPDATE_APPLY);
      uart_tx_off(false);
      LOG_INF("Failed to start XMODEM transfer!");
   }
}

static void uart_xmodem_process_fn(struct k_work *work)
{
   bool start = atomic_test_bit(&uart_at_state, UART_UPDATE_START);
   bool retry = false;
   bool cancel = false;
   int rc = 9;

   if (&uart_xmodem_write_work == work) {
      k_work_cancel_delayable(&uart_xmodem_nak_work);
      rc = appl_update_xmodem_write_block();
      if (rc < 0) {
         retry = true;
      } else if (rc == XMODEM_DUPLICATE) {
         if (!start) {
            // small delay, maybe the next block is already in flight
            work_reschedule_for_cmd_queue(&uart_xmodem_ack_work, K_MSEC(500));
         }
         return;
      } else {
         if (start && atomic_test_and_clear_bit(&uart_at_state, UART_UPDATE_START)) {
            k_work_cancel_delayable(&uart_xmodem_start_work);
         }
         atomic_set(&xmodem_retries, 0);
         k_work_cancel_delayable(&uart_xmodem_ack_work);
         work_reschedule_for_cmd_queue(&uart_xmodem_timeout_work, K_SECONDS(15));
         uart_poll_out(uart_dev, XMODEM_ACK);
         return;
      }
   } else if (&uart_xmodem_nak_work.work == work) {
      retry = true;
      appl_update_xmodem_retry();
   } else if (&uart_xmodem_timeout_work.work == work) {
      cancel = true;
      LOG_INF("XMODEM transfer timeout.");
   } else if (&uart_xmodem_ack_work.work == work) {
      uart_poll_out(uart_dev, XMODEM_ACK);
      return;
   } else {
      k_work_cancel_delayable(&uart_xmodem_nak_work);
      k_work_cancel_delayable(&uart_xmodem_ack_work);
      k_work_cancel_delayable(&uart_xmodem_timeout_work);
      rc = appl_update_finish();
      atomic_clear_bit(&uart_at_state, UART_UPDATE);
      atomic_clear_bit(&uart_at_state, UART_UPDATE_START);
      uart_poll_out(uart_dev, XMODEM_ACK);
      uart_tx_off(false);
      if (!rc) {
         rc = appl_update_dump_pending_image();
      }
      if (!rc) {
         rc = appl_update_request_upgrade();
      }
      if (rc) {
         LOG_INF("XMODEM transfer failed. %d", rc);
      } else {
         LOG_INF("XMODEM transfer succeeded.");
         if (atomic_test_and_clear_bit(&uart_at_state, UART_UPDATE_APPLY)) {
            appl_update_reboot();
         } else {
            LOG_INF("Reboot required to apply update.");
         }
      }
      return;
   }
   if (start) {
      return;
   }
   if (retry) {
      if (atomic_inc(&xmodem_retries) < 10) {
         uart_poll_out(uart_dev, XMODEM_NAK);
      } else {
         cancel = true;
         LOG_INF("XMODEM transfer failed by multiple errors.");
      }
   }
   if (cancel) {
      appl_update_cancel();
      atomic_clear_bit(&uart_at_state, UART_UPDATE);
      atomic_clear_bit(&uart_at_state, UART_UPDATE_START);
      atomic_clear_bit(&uart_at_state, UART_UPDATE_APPLY);
      uart_poll_out(uart_dev, XMODEM_NAK);
      uart_tx_off(false);
   }
}

static bool uart_xmodem_handler(const char *buffer, size_t len)
{
   int rc = appl_update_xmodem_append(buffer, len);

   switch (rc) {
      case XMODEM_NOT_OK:
         work_reschedule_for_cmd_queue(&uart_xmodem_nak_work, K_MSEC(2000));
         break;
      case XMODEM_BLOCK_READY:
         work_submit_to_cmd_queue(&uart_xmodem_write_work);
         break;
      case XMODEM_READY:
         work_submit_to_cmd_queue(&uart_xmodem_ready_work);
         break;
   }
   return false;
}

static int at_cmd_update(const char *parameter)
{
   int res = appl_update_cmd(parameter);
   if (res > 0) {
      // download
      int i = res;
      res = appl_update_start();
      if (!res) {
         atomic_set_bit(&uart_at_state, UART_UPDATE);
         if (i == 2) {
            // apply
            atomic_set_bit(&uart_at_state, UART_UPDATE_APPLY);
         }
         atomic_set(&xmodem_retries, 0);
         res = work_reschedule_for_cmd_queue(&uart_xmodem_start_work, K_MSEC(500));
      }
   }

   return 0;
}

UART_CMD(update, NULL, "start application firmware update. Requires XMODEM.", at_cmd_update, appl_update_cmd_help, 0);

#endif

static void uart_receiver_loop(const char *buffer, size_t len)
{
   // interrupt context!
#if 0   
   if (len == 1 && isprint(*buffer)) {
      LOG_INF("UART rx '%c'", *buffer);
   } else if (len == 1 && *buffer == '\n') {
      LOG_INF("UART rx '\\n'");
   } else if (len == 1 && *buffer == '\r') {
      LOG_INF("UART rx '\\r'");
   } else {
      LOG_INF("UART rx %u bytes", len);
   }
#endif
   if (atomic_test_bit(&uart_at_state, UART_AT_CMD_EXECUTING)) {
      LOG_INF("Cmd busy ...");
   } else if (atomic_test_bit(&uart_at_state, UART_UPDATE)) {
#ifdef CONFIG_UART_UPDATE
      uart_xmodem_handler(buffer, len);
#endif
   } else {
      work_submit_to_io_queue(&uart_start_pause_tx_work);
      for (int index = 0; index < len; ++index) {
         if (uart_receiver_handler(buffer[index])) {
            break;
         }
      }
   }
}

static void uart_receiver_callback(const struct device *dev,
                                   struct uart_event *evt,
                                   void *user_data)
{
   static int buf_requests = 0;

   bool enable = false;

   // interrupt context!
   switch (evt->type) {
      case UART_TX_DONE:
         uart_tx_ready();
         break;

      case UART_TX_ABORTED:
         uart_tx_ready();
         break;

      case UART_RX_RDY:
         uart_receiver_loop(&(evt->data.rx.buf[evt->data.rx.offset]), evt->data.rx.len);
         break;

      case UART_RX_BUF_REQUEST:
         ++buf_requests;
         uart_rx_buf_id ^= 1;
         LOG_DBG("UART async rx buf request %d/%d", uart_rx_buf_id, buf_requests);
         uart_rx_buf_rsp(uart_dev, uart_rx_buf[uart_rx_buf_id], CONFIG_UART_BUFFER_LEN);
         break;

      case UART_RX_BUF_RELEASED:
         {
            int id = -1;
            --buf_requests;
            if (evt->data.rx_buf.buf == uart_rx_buf[0]) {
               id = 0;
            } else if (evt->data.rx_buf.buf == uart_rx_buf[1]) {
               id = 1;
            }
            LOG_DBG("UART async rx buf released %d", id);
         }
         break;

      case UART_RX_DISABLED:
         enable = true;
         LOG_INF("UART async rx disabled");
         break;

      case UART_RX_STOPPED:
         enable = true;
         LOG_INF("UART async rx stopped (%d)", evt->data.rx_stop.reason);
         break;
   }
   if (enable) {
      uart_reschedule_rx_enable(K_MSEC(CONFIG_UART_RX_CHECK_INTERVAL_MS));
   }
}

static int uart_init(void)
{
   static bool init = true;
   static int err = 0;

   if (init) {
      init = false;
      /* Initialize the UART module */
      if (!device_is_ready(uart_dev)) {
         LOG_ERR("UART device not ready");
         err = -EFAULT;
      } else {
         err = uart_callback_set(uart_dev, uart_receiver_callback, (void *)uart_dev);
         if (err) {
            LOG_ERR("UART callback not set!");
         }
      }
   }
   return err;
}

static int uart_receiver_init(void)
{
   int err;

   STRUCT_SECTION_FOREACH(uart_cmd_entry, e)
   {
      if (e->cmd) {
         at_cmd_max_length = MAX(at_cmd_max_length, strlen(e->cmd));
      }
   }
   at_cmd_max_length++;

   err = uart_init();

   uart_init_lines();

   k_work_queue_start(&at_cmd_work_q, at_cmd_stack,
                      K_THREAD_STACK_SIZEOF(at_cmd_stack),
                      CONFIG_AT_CMD_THREAD_PRIO, NULL);

   k_work_queue_start(&uart_work_q, uart_stack,
                      K_THREAD_STACK_SIZEOF(uart_stack),
                      CONFIG_UART_THREAD_PRIO, NULL);

   uart_reschedule_rx_enable(K_MSEC(CONFIG_UART_RX_CHECK_INTERVAL_MS));

   return err;
}

SYS_INIT(uart_receiver_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
