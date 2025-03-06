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

#ifdef CONFIG_UART_UPDATE
#include "appl_update.h"
#include "appl_update_xmodem.h"
#endif

#include "appl_diagnose.h"
#include "dtls_client.h"
#include "io_job_queue.h"
#include "sh_cmd.h"
#include "ui.h"

LOG_MODULE_REGISTER(UART_MANAGER, CONFIG_UART_MANAGER_LOG_LEVEL);

#define CONFIG_UART_CMD_MAX_LEN 2048
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

static const struct device *const uart_dev = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_console));

static char uart_cmd_buf[CONFIG_UART_CMD_MAX_LEN];
static int uart_rx_buf_id = 0;
static uint8_t uart_rx_buf[2][CONFIG_UART_BUFFER_LEN];

static atomic_t uart_state = ATOMIC_INIT(0);

#define UART_SUSPENDED 0

#ifdef CONFIG_LOG_BACKEND_UART_MANAGER
#define UART_TX_ENABLED 1
#define UART_PANIC 2
#define UART_PENDING 3
#endif

#ifdef CONFIG_UART_UPDATE
#define UART_UPDATE 4
#define UART_UPDATE_START 5
#define UART_UPDATE_APPLY 6
#define UART_UPDATE_FLAGS (BIT(UART_UPDATE) | BIT(UART_UPDATE_START) | BIT(UART_UPDATE_APPLY))

static void uart_xmodem_process_fn(struct k_work *work);
static void uart_xmodem_start_fn(struct k_work *work);

static atomic_t xmodem_retries = ATOMIC_INIT(0);

static K_WORK_DELAYABLE_DEFINE(uart_xmodem_start_work, uart_xmodem_start_fn);
static K_WORK_DELAYABLE_DEFINE(uart_xmodem_nak_work, uart_xmodem_process_fn);
static K_WORK_DELAYABLE_DEFINE(uart_xmodem_ack_work, uart_xmodem_process_fn);
static K_WORK_DELAYABLE_DEFINE(uart_xmodem_timeout_work, uart_xmodem_process_fn);
static K_WORK_DEFINE(uart_xmodem_write_work, uart_xmodem_process_fn);
static K_WORK_DEFINE(uart_xmodem_ready_work, uart_xmodem_process_fn);

static inline bool uart_update_pending(void)
{
   return atomic_test_bit(&uart_state, UART_UPDATE);
}

#else  /* CONFIG_UART_UPDATE */
static inline bool uart_update_pending(void)
{
   return false;
}

static inline void uart_xmodem_handler(const char *buffer, size_t len)
{
   // empty
}
#endif /* CONFIG_UART_UPDATE */

static K_WORK_DELAYABLE_DEFINE(uart_enable_rx_work, uart_enable_rx_fn);
static K_WORK_DEFINE(uart_start_pause_tx_work, uart_pause_tx_fn);
static K_WORK_DEFINE(uart_stop_pause_tx_work, uart_pause_tx_fn);

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

#ifdef CONFIG_LOG_BACKEND_UART_MANAGER

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
   atomic_set_bit_to(&uart_state, UART_TX_ENABLED, !off);
}

static inline void uart_tx_ready(void)
{
   k_sem_give(&uart_tx_sem);
}

static inline bool uart_tx_pending(void)
{
   return atomic_test_bit(&uart_state, UART_PENDING);
}

static int uart_tx_out(uint8_t *data, size_t length)
{
#ifdef CONFIG_LOG_MODE_IMMEDIATE
   for (size_t i = 0; i < length; i++) {
      uart_poll_out(uart_dev, data[i]);
   }
#else
   bool panic = atomic_test_bit(&uart_state, UART_PANIC);
   if (panic) {
      for (size_t i = 0; i < length; i++) {
         uart_poll_out(uart_dev, data[i]);
      }
   } else if (!atomic_test_bit(&uart_state, UART_SUSPENDED)) {
      if (length == 1) {
         uart_poll_out(uart_dev, data[0]);
      } else {
         atomic_set_bit(&uart_state, UART_PENDING);
         k_sem_reset(&uart_tx_sem);

         /* SYS_FOREVER_US disable timeout */
         uart_tx(uart_dev, data, length, SYS_FOREVER_US);

         k_sem_take(&uart_tx_sem, K_MSEC(CONFIG_UART_TX_OUTPUT_TIMEOUT_MS));
         atomic_clear_bit(&uart_state, UART_PENDING);
      }
   }
#endif
   return length;
}

static int uart_tx_out_buf(int c)
{
#ifdef CONFIG_LOG_MODE_IMMEDIATE
   uart_poll_out(uart_dev, c);
#else
   if (atomic_test_bit(&uart_state, UART_PANIC)) {
      uart_poll_out(uart_dev, c);
   } else {
      int idx = atomic_inc(&uart_tx_buf_offset);
      if (idx == (sizeof(uart_tx_buf))) {
         uart_tx_out(uart_tx_buf, idx);
         atomic_set(&uart_tx_buf_offset, 1);
         idx = 0;
      }
      uart_tx_buf[idx] = (uint8_t)c;
   }
#endif

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

static void uart_tx_out_flush(void)
{
   atomic_cas(&uart_tx_buf_lines, 0, 1);
   uart_tx_out_nl();
   uart_tx_out(uart_tx_buf, atomic_get(&uart_tx_buf_offset));
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

static void uart_log_process(const struct log_backend *const backend,
                             union log_msg_generic *msg)
{
   bool panic = atomic_test_bit(&uart_state, UART_PANIC);

   if (!panic &&
       (atomic_test_bit(&uart_state, UART_SUSPENDED) ||
        !atomic_test_bit(&uart_state, UART_TX_ENABLED))) {
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

#ifndef CONFIG_LOG_MODE_IMMEDIATE
         if (!panic) {
            k_mutex_lock(&uart_tx_mutex, K_FOREVER);
            if (uart_tx_in_pause) {
               k_condvar_wait(&uart_tx_condvar, &uart_tx_mutex, K_FOREVER);
            }
            k_mutex_unlock(&uart_tx_mutex);
         }
#endif
         if (level) {
            int cycles = sys_clock_hw_cycles_per_sec();
            log_timestamp_t seconds = (msg->log.hdr.timestamp / cycles) % 100;
            log_timestamp_t milliseconds = (msg->log.hdr.timestamp * 1000 / cycles) % 1000;
            if (uart_update_pending()) {
               level = 'u';
            } else if (sh_busy()) {
               level = 'b';
            }
            prefix = cbprintf(uart_tx_out_func, NULL, "%c %02d.%03d : ",
                              level, seconds, milliseconds);
         }
         if (plen) {
            cbpprintf(uart_tx_out_func, NULL, package);
         }
         uart_tx_out_flush();
#ifdef CONFIG_LOG_BACKEND_UART_THROTTLE
         k_sleep(K_MSEC(4));
#endif /* CONFIG_LOG_BACKEND_UART_THROTTLE */
         if (dlen) {
            uart_log_dump_hex(prefix, data, dlen);
            uart_tx_out_flush();
#ifdef CONFIG_LOG_BACKEND_UART_THROTTLE
            k_sleep(K_MSEC(4));
#endif /* CONFIG_LOG_BACKEND_UART_THROTTLE */
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
   atomic_set_bit(&uart_state, UART_PANIC);
   uart_tx_out_flush();
}

static void uart_log_dropped(const struct log_backend *const backend, uint32_t cnt)
{
   ARG_UNUSED(backend);
   cbprintf(uart_tx_out_func, NULL,
            "--- %u  messages dropped ---", cnt);
   uart_tx_out_flush();
}

const struct log_backend_api uart_log_backend_api = {
    .process = uart_log_process,
    .panic = uart_log_panic,
    .init = uart_log_init,
    .dropped = IS_ENABLED(CONFIG_LOG_MODE_IMMEDIATE) ? NULL : uart_log_dropped,
    .format_set = NULL,
};

LOG_BACKEND_DEFINE(uart_log_backend, uart_log_backend_api,
                   IS_ENABLED(CONFIG_LOG_BACKEND_UART_MANAGER_AUTOSTART));

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

static inline bool uart_tx_pending(void)
{
   return false;
}

#endif /* CONFIG_LOG_BACKEND_UART_MANAGER */

static void uart_pause_tx_fn(struct k_work *work)
{
   uart_tx_pause(&uart_start_pause_tx_work == work);
}

static void uart_enable_rx_fn(struct k_work *work)
{
   int err = uart_get_lines();

   if (err == 1) {
      pm_device_action_run(uart_dev, PM_DEVICE_ACTION_RESUME);
      atomic_clear_bit(&uart_state, UART_SUSPENDED);
#ifdef CONFIG_UART_LED
      ui_led_op(LED_UART, LED_SET);
#endif /* CONFIG_UART_LED */
   } else if (err == 0) {
      if (k_uptime_get() > 10000 && !uart_tx_pending()) {
         // early suspend seems to crash
         atomic_set_bit(&uart_state, UART_SUSPENDED);
#ifdef CONFIG_UART_LED
         ui_led_op(LED_UART, LED_CLEAR);
#endif /* CONFIG_UART_LED */
         pm_device_action_run(uart_dev, PM_DEVICE_ACTION_SUSPEND);
         uart_tx_ready();
      }
   } else {
#ifdef CONFIG_UART_LED
      ui_led_op(LED_UART, LED_BLINKING);
#endif
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

static bool uart_receiver_handler(uint8_t character)
{
   // interrupt context!
   static bool inside_quotes = false;
   static size_t at_cmd_len = 0;
   static int64_t last = 0;

   int64_t now = k_uptime_get();
   if ((now - last) > (MSEC_PER_SEC * CONFIG_UART_RX_INPUT_TIMEOUT_S)) {
      if (at_cmd_len) {
         uart_cmd_buf[at_cmd_len] = '\0';
         LOG_INF("timeout %s", uart_cmd_buf);
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
            if (uart_cmd_buf[at_cmd_len--] == '"') {
               inside_quotes = !inside_quotes;
            }
         }
         return false;
      case '\r':
      case '\n':
         if (!inside_quotes) {
            uart_cmd_buf[at_cmd_len] = '\0';
            at_cmd_len = 0;
            /* Check for the presence of one printable non-whitespace character */
            for (const char *c = uart_cmd_buf; *c; c++) {
               if (*c > ' ') {
                  int rc = 0;
                  work_submit_to_io_queue(&uart_stop_pause_tx_work);
                  rc = sh_cmd_execute(uart_cmd_buf);
                  if (rc == -EBUSY) {
                     LOG_INF("sh busy \?\?\?");
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
   if (at_cmd_len > sizeof(uart_cmd_buf) - 2) {
      LOG_ERR("Buffer overflow, dropping '%c'", character);
      return false;
   }

   /* Write character to AT buffer */
   uart_cmd_buf[at_cmd_len++] = character;

   /* Handle special written character */
   if (character == '"') {
      inside_quotes = !inside_quotes;
   }
   return false;
}

#if defined(CONFIG_UART_UPDATE)

static void uart_xmodem_start_fn(struct k_work *work)
{
   ARG_UNUSED(work);
   int res = 0;

   int retry = atomic_inc(&xmodem_retries);

   if (retry == 0) {
      atomic_set_bit(&uart_state, UART_UPDATE_START);
      LOG_INF("Please start xmodem, update begins in about 10s!");
      k_sleep(K_MSEC(500));
      uart_tx_off(true);
      res = appl_update_erase();
      if (res) {
         appl_update_cancel();
         atomic_and(&uart_state, ~UART_UPDATE_FLAGS);
         uart_tx_off(false);
         LOG_INF("Failed erase update area! %d", res);
         return;
      }
      ui_led_op(LED_COLOR_ALL, LED_BLINK);
   }

   if (retry < 3) {
      // CRC
      appl_update_xmodem_start(uart_cmd_buf, sizeof(uart_cmd_buf), true);
      uart_poll_out(uart_dev, XMODEM_CRC);
      work_reschedule_for_cmd_queue(&uart_xmodem_start_work, K_MSEC(2000));
   } else if (retry < 6) {
      // CHECKSUM
      appl_update_xmodem_start(uart_cmd_buf, sizeof(uart_cmd_buf), false);
      uart_poll_out(uart_dev, XMODEM_NAK);
      work_reschedule_for_cmd_queue(&uart_xmodem_start_work, K_MSEC(2000));
   } else {
      appl_update_cancel();
      atomic_and(&uart_state, ~UART_UPDATE_FLAGS);
      uart_tx_off(false);
      LOG_INF("Failed to start XMODEM transfer!");
   }
}

static void uart_xmodem_process_fn(struct k_work *work)
{
   bool start = atomic_test_bit(&uart_state, UART_UPDATE_START);
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
         if (start && atomic_test_and_clear_bit(&uart_state, UART_UPDATE_START)) {
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
      bool apply = atomic_test_bit(&uart_state, UART_UPDATE_APPLY);
      k_work_cancel_delayable(&uart_xmodem_nak_work);
      k_work_cancel_delayable(&uart_xmodem_ack_work);
      k_work_cancel_delayable(&uart_xmodem_timeout_work);
      rc = appl_update_finish();
      atomic_and(&uart_state, ~UART_UPDATE_FLAGS);
      uart_poll_out(uart_dev, XMODEM_ACK);
      k_sleep(K_MSEC(100));
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
         if (apply) {
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
      atomic_and(&uart_state, ~UART_UPDATE_FLAGS);
      uart_poll_out(uart_dev, XMODEM_NAK);
      uart_tx_off(false);
   }
}

static void uart_xmodem_handler(const char *buffer, size_t len)
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
}

static int sh_cmd_update(const char *parameter)
{
   int res = appl_update_cmd(parameter);
   if (res > 0) {
      // download
      int i = res;
      res = appl_update_start();
      if (!res) {
         atomic_set_bit(&uart_state, UART_UPDATE);
         if (i == 2) {
            // apply
            atomic_set_bit(&uart_state, UART_UPDATE_APPLY);
         }
         atomic_set(&xmodem_retries, 0);
         res = work_reschedule_for_cmd_queue(&uart_xmodem_start_work, K_MSEC(500));
      }
   }

   return 0;
}

SH_CMD(update, NULL, "start application firmware update. Requires XMODEM client.", sh_cmd_update, appl_update_cmd_help, 0);
#endif /* CONFIG_UART_UPDATE */

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
   if (sh_busy() & SH_CMD_EXECUTING) {
      LOG_INF("Cmd busy ...");
   } else if (uart_update_pending()) {
      uart_xmodem_handler(buffer, len);
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

static int uart_manager_init(void)
{
   int err;
   struct k_work_queue_config uart_cfg = {
       .name = "uart_workq",
   };

   err = uart_init();

   uart_init_lines();

   k_work_queue_start(&uart_work_q, uart_stack,
                      K_THREAD_STACK_SIZEOF(uart_stack),
                      CONFIG_UART_THREAD_PRIO, &uart_cfg);

   uart_reschedule_rx_enable(K_MSEC(CONFIG_UART_RX_CHECK_INTERVAL_MS));

   return err;
}

SYS_INIT(uart_manager_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
