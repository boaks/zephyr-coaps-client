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
#include <zephyr/logging/log_output.h>
#include <zephyr/logging/log_output_dict.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/cbprintf.h>

#include <modem/at_monitor.h>
#include <modem_at.h>
#include <nrf_errno.h>

#include "appl_diagnose.h"
#include "coap_client.h"
#include "io_job_queue.h"
#include "modem.h"
#include "modem_cmd.h"
#include "modem_sim.h"
#include "parse.h"
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
#define CONFIG_UART_RX_SUSPEND_DELAY_S 5

#define CONFIG_UART_RX_INPUT_TIMEOUT_S 30

static void uart_enable_rx_fn(struct k_work *work);
static void uart_pause_tx_fn(struct k_work *work);
static int uart_init(void);
static void at_cmd_send_fn(struct k_work *work);
static void at_cmd_response_fn(struct k_work *work);

void dtls_cmd_trigger(bool led, int mode);

static const struct device *const uart_dev = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_console));

static char at_cmd_buf[CONFIG_AT_CMD_MAX_LEN];
static int uart_rx_buf_id = 0;
static uint8_t uart_rx_buf[2][CONFIG_UART_BUFFER_LEN];

#define UART_RX_ENABLED 0
#define UART_AT_CMD_EXECUTING 1
#define UART_AT_CMD_PENDING 2

static atomic_t uart_at_state = ATOMIC_INIT(0);

static K_WORK_DELAYABLE_DEFINE(uart_enable_rx_work, uart_enable_rx_fn);
static K_WORK_DELAYABLE_DEFINE(uart_end_pause_tx_work, uart_pause_tx_fn);
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
   uart_reschedule_rx_enable(K_MSEC(CONFIG_UART_RX_CHECK_INTERVAL_MS));
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

static inline void uart_tx_ready(void)
{
   k_sem_give(&uart_tx_sem);
}

static int uart_tx_out(uint8_t *data, size_t length, bool panic)
{
   if (panic || length == 1) {
      for (size_t i = 0; i < length; i++) {
         uart_poll_out(uart_dev, data[i]);
      }
   } else {
      int err;

      k_sem_reset(&uart_tx_sem);

      err = uart_tx(uart_dev, data, length, SYS_FOREVER_US);
      __ASSERT_NO_MSG(err == 0);

      err = k_sem_take(&uart_tx_sem, K_FOREVER);
      __ASSERT_NO_MSG(err == 0);
      (void)err;
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

static void uart_log_process(const struct log_backend *const backend,
                             union log_msg_generic *msg)
{
   static char level_tab[] = {0, 'E', 'W', 'I', 'D'};

   size_t plen;
   uint8_t *package = log_msg_get_package(&msg->log, &plen);
   if (plen) {
      char level = level_tab[msg->log.hdr.desc.level];
#ifndef CONFIG_LOG_MODE_IMMEDIATE
      k_mutex_lock(&uart_tx_mutex, K_FOREVER);
      if (uart_tx_in_pause) {
         k_condvar_wait(&uart_tx_condvar, &uart_tx_mutex, K_FOREVER);
      }
      k_mutex_unlock(&uart_tx_mutex);
#endif
      if (level) {
         int state = atomic_get(&uart_at_state);
         int cycles = sys_clock_hw_cycles_per_sec();
         log_timestamp_t seconds = (msg->log.hdr.timestamp / cycles) % 100;
         log_timestamp_t milliseconds = (msg->log.hdr.timestamp * 1000 / cycles) % 1000;
         cbprintf(uart_tx_out_func, NULL, "%c %d %02d.%03d: ",
                  level, state, seconds, milliseconds);
      }
      cbpprintf(uart_tx_out_func, NULL, package);
      uart_tx_out_flush(false);
   }
}

static void uart_log_init(struct log_backend const *const backend)
{
   ARG_UNUSED(backend);

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

static bool uart_monitor_ignore_notify(const char *notif)
{
   const char **ignore = IGNORE_NOTIFY;
   while (*ignore) {
      if (strstart(notif, *ignore, false)) {
         return true;
      }
      ++ignore;
   }
   return false;
}

static void uart_monitor_handler(const char *notif)
{
   if (!appl_reboots() && !uart_monitor_ignore_notify(notif)) {
      int len;
      printk("%s", notif);
      len = strstart(notif, "+CEREG:", false);
      if (len > 0) {
         const char *cur = parse_next_chars(notif + len, ',', 4);
         if (cur && strstart(cur, "0,", false)) {
            LOG_INF("LTE +CEREG: rejected, cause %d", atoi(cur + 2));
         }
      }
   }
}
#else
static inline void uart_tx_pause(bool pause)
{
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
   } else if (err == 0) {
      pm_device_action_run(uart_dev, PM_DEVICE_ACTION_SUSPEND);
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
         atomic_set_bit(&uart_at_state, UART_RX_ENABLED);
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
         const char *desc;
         switch (res) {
            case -NRF_EFAULT:
               desc = " (off)";
               break;
            case -NRF_EINPROGRESS:
               desc = " (in progress)";
               break;
            default:
               desc = "";
               break;
         }
         LOG_INF("ERROR %d%s\n", -res, desc);
      }
      if (finish) {
         if (res < 0) {
            printk("ERROR\n");
         } else {
            printk("OK\n");
         }
      }
   }
   uart_tx_pause(false);
}

static void at_cmd_response_fn(struct k_work *work)
{
   printk("%s", at_cmd_buf);
   if (strend(at_cmd_buf, "OK", false)) {
      at_cmd_result(1);
   } else if (strend(at_cmd_buf, "ERROR", false)) {
      at_cmd_result(1);
   }
   at_cmd_result(-1);
   atomic_clear_bit(&uart_at_state, UART_AT_CMD_PENDING);
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

static int at_cmd_send()
{
   int res = 0;
   int i = 0;

   if (!stricmp(at_cmd_buf, "reset")) {
      strcpy(at_cmd_buf, "AT%%XFACTORYRESET=0");
   } else if (!stricmp(at_cmd_buf, "off")) {
      strcpy(at_cmd_buf, "AT+CFUN=0");
   } else if (!stricmp(at_cmd_buf, "on")) {
      res = modem_set_normal();
      return RESULT(res);
   } else if (!stricmp(at_cmd_buf, "sim")) {
      res = modem_sim_read_info(NULL, true);
      return RESULT(res);
   } else if (!stricmp(at_cmd_buf, "state")) {
      res = modem_read_network_info(NULL, true);
      return RESULT(res);
   }

   i = strstart(at_cmd_buf, "cfg", true);
   if (i > 0) {
      uart_tx_pause(false);
      res = modem_cmd_config(&at_cmd_buf[i]);
      if (res == 1) {
         LOG_INF(">> (new cfg) send");
         dtls_cmd_trigger(true, 3);
         res = 0;
      }
      return RESULT(res);
   }

   i = strstart(at_cmd_buf, "con", true);
   if (i > 0) {
      uart_tx_pause(false);
      res = modem_cmd_connect(&at_cmd_buf[i]);
      if (res == 1) {
         LOG_INF(">> (new con) send");
         dtls_cmd_trigger(true, 3);
         res = 0;
      }
      return RESULT(res);
   }

#ifdef CONFIG_SMS
   i = strstart(at_cmd_buf, "sms", true);
   if (i > 0) {
      uart_tx_pause(false);
      res = modem_cmd_sms(&at_cmd_buf[i]);
      return RESULT(res);
   }
#endif

   i = strstart(at_cmd_buf, "scan", true);
   if (i == 0) {
      i = strstart(at_cmd_buf, "AT%NCELLMEAS", true);
   }
   if (i > 0) {
      uart_tx_pause(false);
      res = modem_cmd_scan(&at_cmd_buf[i]);
      return RESULT(res);
   }

   if (!strstart(at_cmd_buf, "AT", true)) {
      LOG_INF("ignore > %s", at_cmd_buf);
      return -1;
   }
   LOG_INF(">%s", at_cmd_buf);
   res = modem_at_cmd_async(at_cmd_resp_callback, NULL, at_cmd_buf);
   return PENDING(res);
}

static int at_cmd()
{
   int res = 0;
   int i;

   if (!stricmp(at_cmd_buf, "reboot")) {
      LOG_INF(">> device reboot ...");
      appl_reboot(ERROR_CODE_CMD, K_MSEC(2000));
      return 0;
   } else if (!stricmp(at_cmd_buf, "send")) {
      LOG_INF(">> send");
      dtls_cmd_trigger(true, 3);
      return 0;
   } else if (!stricmp(at_cmd_buf, "env")) {
      res = coap_client_prepare_env_info(at_cmd_buf, sizeof(at_cmd_buf));
      return RESULT(res);
   } else if (!stricmp(at_cmd_buf, "net")) {
      res = coap_client_prepare_net_info(at_cmd_buf, sizeof(at_cmd_buf));
      return RESULT(res);
   } else if (!stricmp(at_cmd_buf, "dev")) {
      res = coap_client_prepare_modem_info(at_cmd_buf, sizeof(at_cmd_buf));
      return RESULT(res);
   } else if (!stricmp(at_cmd_buf, "help")) {
      LOG_INF("> help:");
      LOG_INF("  at???  : modem at-cmd.(*)");
      LOG_INF("  cfg    : configure modem.(*?)");
      LOG_INF("  con    : connect modem.(*?)");
      LOG_INF("  dev    : read device info.");
      LOG_INF("  env    : read environment sensor.");
      LOG_INF("  net    : read network info.");
      LOG_INF("  on     : switch modem on.(*)");
      LOG_INF("  off    : switch modem off.(*)");
      LOG_INF("  reset  : modem factory reset.(*)");
      LOG_INF("  reboot : reboot device.");
      LOG_INF("  scan   : network scan.(*?)");
      LOG_INF("  send   : send message.");
      LOG_INF("  sim    : read SIM-card info.(*)");
#ifdef CONFIG_SMS
      LOG_INF("  sms    : send SMS.(*?)");
#endif
      LOG_INF("  state  : read modem state.(*)");
      LOG_INF("  *      : AT-cmd is used, maybe busy.");
      LOG_INF("  ?      : help <cmd> available.");
      return 0;
   } else {
      i = strstart(at_cmd_buf, "help ", true);
      if (i > 0) {
         if (!stricmp(&at_cmd_buf[i], "cfg")) {
            modem_cmd_config_help();
         } else if (!stricmp(&at_cmd_buf[i], "con")) {
            modem_cmd_connect_help();
         } else if (!stricmp(&at_cmd_buf[i], "scan")) {
            modem_cmd_scan_help();
#ifdef CONFIG_SMS
         } else if (!stricmp(&at_cmd_buf[i], "sms")) {
            modem_cmd_sms_help();
#endif
         } else {
            LOG_INF("> help %s:", &at_cmd_buf[i]);
            LOG_INF("  no details available.");
         }
         return 0;
      }
   }

   if (atomic_test_and_set_bit(&uart_at_state, UART_AT_CMD_PENDING)) {
      LOG_INF("Modem pending ...");
      return 1;
   }
   res = at_cmd_send();
   if (res < 1) {
      atomic_clear_bit(&uart_at_state, UART_AT_CMD_PENDING);
   }
   return res;
}

static void at_cmd_send_fn(struct k_work *work)
{
   ARG_UNUSED(work);
   int res = at_cmd();
   at_cmd_result(res);
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
   // interrupt context!
   //   LOG_INF("UART rx %u bytes", len);
   if (atomic_test_bit(&uart_at_state, UART_AT_CMD_EXECUTING)) {
      LOG_INF("Modem busy ...");
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
      atomic_clear_bit(&uart_at_state, UART_RX_ENABLED);
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

SYS_INIT(uart_receiver_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
// SYS_INIT(uart_receiver_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
