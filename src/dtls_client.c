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

#include "tinydtls.h"

#include <errno.h>
#include <net/coap.h>
#include <net/socket.h>
#include <stdio.h>
#include <string.h>
#include <sys/reboot.h>

#include "appl_eeprom.h"
#include "coap_client.h"
#include "console_input.h"
#include "dtls.h"
#include "dtls_credentials.h"
#include "dtls_debug.h"
#include "global.h"
#include "io_job_queue.h"
#include "modem.h"
#include "power_manager.h"
#include "ui.h"

#ifdef CONFIG_LOCATION_ENABLE
#include "location.h"
#endif

#ifdef CONFIG_ADXL362_MOTION_DETECTION
#include "accelerometer_sensor.h"
#endif

#include "environment_sensor.h"

#define ERROR_CODE_SOCKET 0x0000
#define ERROR_CODE_OPEN_SOCKET 0x1000
#define ERROR_CODE_INIT_NO_LTE 0x2000
#define ERROR_CODE_INIT_SOCKET 0x3000
#define ERROR_CODE_INIT_NO_DTLS 0x4000
#define ERROR_CODE_TOO_MANY_FAILURES 0x5000
#define ERROR_CODE_MANUAL_TRIGGERED 0xa000

#define ERROR_CODE(BASE, ERR) ((BASE & 0xf000) | (ERR & 0xfff))

#define COAP_ACK_TIMEOUT 3
#define ADD_ACK_TIMEOUT 3

#define LED_APPLICATION LED_LTE_1
#define LED_DTLS LED_LTE_2

LOG_MODULE_REGISTER(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

typedef enum {
   NONE,
   SEND,
   RECEIVE,
   WAIT_RESPONSE,
   SEND_ACK,
   WAIT_SUSPEND,
} request_state_t;

typedef struct dtls_app_data_t {
   int fd;
   bool dtls_pending;
   uint8_t retransmission;
   request_state_t request_state;
   uint16_t timeout;
   int64_t start_time;
} dtls_app_data_t;

static volatile bool network_registered = false;
static volatile bool network_ready = false;
static volatile bool network_sleeping = false;
static volatile bool lte_connected_send = false;
static volatile unsigned int lte_connections = 0;
static volatile unsigned long connected_time = 0;

static dtls_app_data_t app_data;

static unsigned long connect_time = 0;
static unsigned long response_time = 0;

static bool initial_success = false;
static unsigned int current_failures = 0;
static unsigned int handled_failures = 0;

static bool appl_eeprom = false;

static volatile bool ui_led_active = false;

static volatile bool appl_ready = false;
static volatile bool trigger_restart_modem = false;
static volatile bool trigger_reboot = false;
static volatile bool lte_power_off = false;
static bool lte_power_on_off = false;

#ifdef CONFIG_ADXL362_MOTION_DETECTION
static volatile bool moved = false;
#endif

#define MAX_READ_BUF 1600
static uint8_t receive_buffer[MAX_READ_BUF];

#define RTT_SLOTS 9
#define RTT_INTERVAL 2000
static unsigned int rtts[RTT_SLOTS + 2] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 20};

static K_SEM_DEFINE(dtls_trigger_msg, 0, 1);
static K_SEM_DEFINE(dtls_trigger_search, 0, 1);

static K_MUTEX_DEFINE(dtls_pm_mutex);

static void reboot(int error, bool factoryReset)
{
   modem_power_off();
   if (factoryReset) {
      modem_factory_reset();
   }
   ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
   ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
   ui_led_op(LED_COLOR_RED, LED_SET);
   if (appl_eeprom) {
      int64_t now = 0;
      coap_client_get_time(&now);
      appl_eeprom_write_code(now, (int16_t)error);
   }
   k_sleep(K_MSEC(500));
   for (int i = 0; i < 4; ++i) {
      ui_led_op(LED_COLOR_RED, LED_CLEAR);
      ui_led_op(LED_COLOR_BLUE, LED_SET);
      k_sleep(K_MSEC(500));
      ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
      ui_led_op(LED_COLOR_RED, LED_SET);
      k_sleep(K_MSEC(500));
   }
   k_sleep(K_MSEC(500));
   sys_reboot(SYS_REBOOT_COLD);
}

static int get_socket_error(dtls_app_data_t *app)
{
   int error = 0;
   socklen_t len = sizeof(error);
   int result = getsockopt(app->fd, SOL_SOCKET, SO_ERROR, &error, &len);
   if (result) {
      error = errno;
   }
   return error;
}

static void restart_modem(dtls_app_data_t *app)
{
   int timeout_seconds = CONFIG_MODEM_SEARCH_TIMEOUT;
   int sleep_minutes = trigger_restart_modem ? 0 : 15;

   dtls_warn("> reconnect modem ...");
   k_sem_reset(&dtls_trigger_search);

   if (trigger_reboot) {
      reboot(ERROR_CODE_MANUAL_TRIGGERED, false);
   }

   while (!network_registered || trigger_restart_modem) {
      dtls_info("> modem offline (%d minutes)", sleep_minutes);
      modem_set_lte_offline();
      k_sleep(K_MSEC(2000));
      ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
      ui_led_op(LED_COLOR_RED, LED_CLEAR);
      ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
      if (sleep_minutes) {
         if (k_sem_take(&dtls_trigger_search, K_MINUTES(sleep_minutes)) == 0) {
            dtls_info("> modem normal (manual)");
            sleep_minutes = 15;
         } else if (sleep_minutes < 61) {
            dtls_info("> modem normal (timeout)");
            sleep_minutes *= 2;
         } else if (app) {
            dtls_info("> modem lost network, reboot");
            reboot(ERROR_CODE(ERROR_CODE_SOCKET, get_socket_error(app)), true);
         } else {
            dtls_info("> modem no network, reboot");
            reboot(ERROR_CODE_INIT_NO_LTE, true);
         }
      } else {
         sleep_minutes = 15;
      }
      trigger_restart_modem = false;
      timeout_seconds *= 2;
      dtls_info("> modem search network (%d minutes)", timeout_seconds / 60);
      if (modem_start(K_SECONDS(timeout_seconds)) == 0) {
         break;
      }
   }
   dtls_info("> connected modem.");
}

static void reopen_socket(dtls_app_data_t *app)
{
   int err;
   bool psm_off = !network_ready;

   dtls_info("> reopen socket ...");
   modem_set_rai_mode(RAI_OFF, app->fd);

   if (psm_off) {
      modem_set_psm(false);
   }
   err = modem_wait_ready(K_SECONDS(CONFIG_MODEM_SEARCH_TIMEOUT));
   if (err) {
      restart_modem(app);
   }
   (void)close(app->fd);
   app->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
   if (app->fd < 0) {
      dtls_warn("> reopen UDP socket failed, %d, errno %d (%s), reboot", app->fd, errno, strerror(errno));
      reboot(ERROR_CODE(ERROR_CODE_OPEN_SOCKET, errno), false);
   }
   if (psm_off) {
      modem_set_psm(true);
   }
   modem_set_rai_mode(RAI_OFF, app->fd);
   dtls_info("> reopend socket.");
}

static int check_socket(dtls_app_data_t *app, bool event)
{
   int error = get_socket_error(app);
   if (error) {
      if (event) {
         k_sleep(K_MSEC(1000));
      }
      reopen_socket(app);
   }
   return -error;
}

static void dtls_trigger(void)
{
   if (app_data.request_state == NONE || app_data.request_state == WAIT_SUSPEND) {
      k_sem_give(&dtls_trigger_msg);
   }
}

static void dtls_manual_trigger(int duration)
{
   if (duration == 1 && appl_ready) {
      trigger_reboot = (ui_config() & 2) == 0;
      trigger_restart_modem = true;
   }
   // LEDs for manual trigger
   ui_enable(true);
   ui_led_op(LED_COLOR_RED, LED_CLEAR);
   k_sem_give(&dtls_trigger_search);
   dtls_trigger();
}

#if CONFIG_COAP_SEND_INTERVAL > 0

static void dtls_timer_trigger_fn(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(dtls_timer_trigger_work, dtls_timer_trigger_fn);

static void dtls_timer_trigger_fn(struct k_work *work)
{
   if (app_data.request_state == NONE) {
      // no LEDs for time trigger
      ui_enable(false);
      dtls_trigger();
   } else {
      work_schedule_for_io_queue(&dtls_timer_trigger_work, K_SECONDS(CONFIG_COAP_SEND_INTERVAL));
   }
}
#endif

static void dtls_power_management(void)
{
   static bool power_manager_suspended = false;
   bool suspend;
   bool changed;

   k_mutex_lock(&dtls_pm_mutex, K_FOREVER);
   suspend = network_sleeping && !ui_led_active && app_data.request_state == NONE;
   changed = power_manager_suspended != suspend;
   if (changed) {
      power_manager_suspended = suspend;
   }
   k_mutex_unlock(&dtls_pm_mutex);

   if (changed) {
      if (suspend) {
         ui_led_op(LED_COLOR_RED, LED_CLEAR);
         ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
         ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
      }
      power_manager_suspend(suspend);
   }
}

static void dtls_power_management_suspend_fn(struct k_work *work)
{
   ui_led_active = false;
   ui_led_op(LED_COLOR_RED, LED_CLEAR);
   dtls_power_management();
}

static K_WORK_DELAYABLE_DEFINE(dtls_power_management_suspend_work, dtls_power_management_suspend_fn);

static void dtls_coap_next(void)
{
   char buf[64];

   ui_led_op(LED_APPLICATION, LED_CLEAR);
   if (lte_power_on_off) {
      dtls_info("> modem switching off ...");
      lte_power_off = true;
      modem_power_off();
      dtls_info("modem off");
   }
#if CONFIG_COAP_SEND_INTERVAL > 0
#if CONFIG_COAP_FAILURE_SEND_INTERVAL > 0
   if (current_failures > 0) {
      work_reschedule_for_io_queue(&dtls_timer_trigger_work, K_SECONDS(CONFIG_COAP_FAILURE_SEND_INTERVAL));
   } else {
      work_schedule_for_io_queue(&dtls_timer_trigger_work, K_SECONDS(CONFIG_COAP_SEND_INTERVAL));
   }
#else  /*CONFIG_COAP_FAILURE_SEND_INTERVAL*/
   work_schedule_for_io_queue(&dtls_timer_trigger_work, K_SECONDS(CONFIG_COAP_SEND_INTERVAL));
#endif /*CONFIG_COAP_FAILURE_SEND_INTERVAL*/
#endif /*CONFIG_COAP_SEND_INTERVAL*/

   if (coap_client_time(buf, sizeof(buf))) {
      dtls_info("%s", buf);
   }
}

static void dtls_coap_success(dtls_app_data_t *app)
{
   long time1 = connected_time - connect_time;
   long time2 = response_time - connect_time;
   if (time1 < 0) {
      time1 = -1;
   }
   if (time2 < 0) {
      time2 = -1;
   }
   k_sem_reset(&dtls_trigger_msg);
   app->request_state = WAIT_SUSPEND;
   ui_led_op(LED_COLOR_RED, LED_CLEAR);
   ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
   ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
   dtls_info("%u/%ldms/%ldms: success", lte_connections, time1, time2);
   if (app->retransmission <= COAP_MAX_RETRANSMISSION) {
      transmissions[app->retransmission]++;
   }
   time2 /= RTT_INTERVAL;
   if (time2 < RTT_SLOTS) {
      rtts[time2]++;
   } else {
      rtts[RTT_SLOTS]++;
      time2 = (response_time - connect_time) / 1000;
      if (time2 > rtts[RTT_SLOTS + 1]) {
         rtts[RTT_SLOTS + 1] = time2;
      }
   }
   if (time1 < 2000) {
      int index = 0;
      unsigned int rtt = 0;
      dtls_info("retrans: 0*%u, 1*%u, 2*%u, 3*%u, failures %u", transmissions[0], transmissions[1], transmissions[2], transmissions[3], transmissions[4]);
      dtls_info("rtt: 0-2s: %u, 2-4s: %u, 4-6s: %u, 6-8s: %u, 8-10s: %u", rtts[0], rtts[1], rtts[2], rtts[3], rtts[4]);
      dtls_info("rtt: 10-12s: %u, 12-14s: %u, 14-16s: %u, 16-18s: %u, 18-%u: %u", rtts[5], rtts[6], rtts[7], rtts[8], rtts[10], rtts[9]);
      time1 = 0;
      time2 = 0;
      for (index = 0; index <= RTT_SLOTS; ++index) {
         rtt = rtts[index];
         if (rtt > 0) {
            time1 += rtt;
            time2 += rtt * (index * 2 + 1);
         }
      }
      if (time1 > 0) {
         dtls_info("rtt: avg. %lds (%ld#)", time2 / time1, time1);
      }
      dtls_info("vbat: %u, %u, %u, %u, %u", bat_level[0], bat_level[1], bat_level[2], bat_level[3], bat_level[4]);
      dtls_info("      %u, %u, %u, %u, %u", bat_level[5], bat_level[6], bat_level[7], bat_level[8], bat_level[9]);
   }
   // reset failures on success
   current_failures = 0;
   handled_failures = 0;
   initial_success = true;
   dtls_coap_next();
}

static void dtls_coap_failure(dtls_app_data_t *app)
{
   long time1 = connected_time - connect_time;
   long time2 = response_time - connect_time;
   if (time1 < 0) {
      time1 = -1;
   }
   if (time2 < 0) {
      time2 = -1;
   }
   k_sem_reset(&dtls_trigger_msg);
   app->request_state = WAIT_SUSPEND;
   if (!ui_led_op(LED_COLOR_RED, LED_SET)) {
      ui_led_active = true;
      work_reschedule_for_io_queue(&dtls_power_management_suspend_work, K_SECONDS(10));
   }
   ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
   ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
   dtls_info("%u/%ldms/%ldms: failure", lte_connections, time1, time2);
   transmissions[COAP_MAX_RETRANSMISSION + 1]++;
   if (initial_success) {
      current_failures++;
      dtls_info("current failures %d.", current_failures);
   }
   dtls_coap_next();
}

static int
read_from_peer(dtls_context_t *ctx, session_t *session, uint8 *data, size_t len)
{
   (void)ctx;
   (void)session;
   dtls_app_data_t *app = dtls_get_app_data(ctx);

   int err = coap_client_parse_data(data, len);
   if (err < 0) {
      return err;
   }

   switch (err) {
      case PARSE_IGN:
         break;
      case PARSE_RST:
         if (NONE != app->request_state) {
            response_time = (unsigned long)k_uptime_get();
            dtls_coap_failure(app);
         }
         break;
      case PARSE_ACK:
         if (NONE != app->request_state) {
            app->request_state = WAIT_RESPONSE;
         }
         break;
      case PARSE_RESPONSE:
         if (NONE != app->request_state) {
            response_time = (unsigned long)k_uptime_get();
            dtls_coap_success(app);
         }
         break;
      case PARSE_CON_RESPONSE:
         if (NONE != app->request_state) {
            response_time = (unsigned long)k_uptime_get();
            app->request_state = SEND_ACK;
         }
         break;
   }

   return 0;
}

static void prepare_socket(dtls_app_data_t *app)
{
   lte_connected_send = false;
   if (!app->dtls_pending && !lte_power_on_off) {
#ifdef CONFIG_COAP_NO_RESPONSE_ENABLE
      modem_set_rai_mode(RAI_LAST, app->fd);
#else
      modem_set_rai_mode(RAI_ONE_RESPONSE, app->fd);
#endif
   } else {
      modem_set_rai_mode(RAI_OFF, app->fd);
   }
}

static int
send_to_peer(dtls_app_data_t *app, session_t *session, const uint8_t *data, size_t len)
{
   int result = 0;
   const char *tag = app->dtls_pending ? (app->retransmission ? "hs_re" : "hs_") : (app->retransmission ? "re" : "");

   if (!lte_power_on_off) {
      connect_time = (unsigned long)k_uptime_get();
      prepare_socket(app);
   }
   result = sendto(app->fd, data, len, MSG_DONTWAIT, &session->addr.sa, session->size);
   if (result < 0) {
      dtls_warn("%ssend_to_peer failed: %d, errno %d (%s)", tag, result, errno, strerror(errno));
      return result;
   }
   if (network_ready) {
      modem_set_transmission_time();
   }
   if (app->retransmission == 0) {
      app->timeout = COAP_ACK_TIMEOUT;
   }
#ifndef NDEBUG
   /* logging */
   if (SEND == app->request_state) {
      if (network_ready) {
         dtls_info("%ssent_to_peer %d", tag, result);
      } else {
         dtls_info("%ssend_to_peer %d", tag, result);
      }
   } else if (RECEIVE == app->request_state) {
      if (network_ready) {
         dtls_info("%sunintended resent_to_peer %d", tag, result);
      } else {
         dtls_info("%sunintended resend_to_peer %d", tag, result);
      }
   }
#endif
   return result;
}

static int
dtls_send_to_peer(dtls_context_t *ctx,
                  session_t *session, uint8 *data, size_t len)
{
   int result;
   dtls_app_data_t *app = dtls_get_app_data(ctx);
   if (app->dtls_pending &&
       (app->request_state == RECEIVE || app->request_state == NONE)) {
      app->request_state = SEND;
   }
   result = send_to_peer(app, session, data, len);
   if (app->dtls_pending && app->request_state == SEND) {
      app->request_state = RECEIVE;
   }
   return result;
}

static int
dtls_handle_event(dtls_context_t *ctx, session_t *session,
                  dtls_alert_level_t level, unsigned short code)
{

   if (DTLS_EVENT_CONNECTED == code) {
      dtls_info("dtls connected.");
      app_data.dtls_pending = false;
      app_data.request_state = NONE;
      ui_led_op(LED_COLOR_RED, LED_CLEAR);
      ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
      ui_led_op(LED_DTLS, LED_SET);
   } else if (DTLS_EVENT_CONNECT == code) {
      dtls_info("dtls connect ...");
      app_data.dtls_pending = true;
      ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
      ui_led_op(LED_COLOR_RED, LED_SET);
      ui_led_op(LED_COLOR_GREEN, LED_SET);
      ui_led_op(LED_DTLS, LED_CLEAR);
   }
   return 0;
}

static int
recvfrom_peer(dtls_app_data_t *app, dtls_context_t *ctx)
{
   int result;
   session_t session;

   memset(&session, 0, sizeof(session_t));
   session.size = sizeof(session.addr);
   result = recvfrom(app->fd, receive_buffer, MAX_READ_BUF, 0,
                     &session.addr.sa, &session.size);
   if (result < 0) {
      dtls_warn("recv_from_peer failed: errno %d (%s)", result, strerror(errno));
      return result;
   } else {
      dtls_dsrv_log_addr(DTLS_LOG_DEBUG, "peer", &session);
      dtls_debug_dump("bytes from peer", receive_buffer, result);
      modem_set_transmission_time();
   }
   dtls_info("received_from_peer %d bytes", result);
   if (ctx) {
      app->retransmission = 0;
      return dtls_handle_message(ctx, &session, receive_buffer, result);
   } else {
      return read_from_peer(NULL, &session, receive_buffer, result);
   }
}

static int
sendto_peer(dtls_app_data_t *app, session_t *dst, struct dtls_context_t *ctx)
{
   int result = 0;
   size_t coap_message_len = 0;
   const uint8_t *coap_message_buf = NULL;

   if (app->request_state == NONE) {
      app->request_state = SEND;
   }
   coap_message_len = coap_client_message(&coap_message_buf);
   if (ctx) {
      if (coap_message_len > 0) {
         result = dtls_write(ctx, dst, (uint8_t *)coap_message_buf, coap_message_len);
         if (result < 0) {
            dtls_warn("Failed to send CoAP request via DTLS, %d", errno);
         }
      }
   } else {
      result = send_to_peer(app, dst, coap_message_buf, coap_message_len);
      if (result < 0) {
         dtls_warn("Failed to send CoAP request via UDP, %d (%s)", errno, strerror(errno));
      }
   }
   if (result > 0) {
      if (!lte_power_off) {
         ui_led_op(LED_COLOR_GREEN, LED_SET);
      }
      if (network_ready) {
         ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
         app->request_state = RECEIVE;
#ifdef CONFIG_COAP_NO_RESPONSE_ENABLE
         if (!app->dtls_pending) {
            dtls_coap_success(app);
         }
#endif
      }
   } else {
      dtls_coap_failure(app);
   }
   return result;
}

static int
dtls_start_connect(struct dtls_context_t *ctx, session_t *dst)
{
   dtls_app_data_t *app = dtls_get_app_data(ctx);

   app->retransmission = 0;
   app->request_state = SEND;
   return dtls_connect(ctx, dst);
}
/*---------------------------------------------------------------------------*/

static dtls_handler_t cb = {
    .write = dtls_send_to_peer,
    .read = read_from_peer,
    .event = dtls_handle_event,
};

static void dtls_lte_state_handler(enum lte_state_type type, bool active)
{
   if (type == LTE_STATE_REGISTRATION) {
      network_registered = active;
      if (active) {
         if (!network_ready) {
            dtls_info("LTE online, no network");
            ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
            ui_led_op(LED_COLOR_RED, LED_CLEAR);
         }
      } else {
         dtls_info("LTE offline");
         network_ready = false;
         led_op_t op = LED_SET;
         if (lte_power_off) {
            op = LED_CLEAR;
         }
         ui_led_op(LED_COLOR_BLUE, op);
         ui_led_op(LED_COLOR_RED, op);
         ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
      }
   } else if (type == LTE_STATE_READY) {
      if (!app_data.dtls_pending) {
         if (active) {
            ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
            ui_led_op(LED_COLOR_GREEN, LED_SET);
         } else if (NONE == app_data.request_state || WAIT_SUSPEND == app_data.request_state) {
            dtls_info("LTE not ready");
            ui_led_op(LED_COLOR_RED, LED_CLEAR);
            ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
         }
      }
      if (network_ready != active) {
         network_ready = active;
         if (active) {
            connected_time = (unsigned long)k_uptime_get();
            ++lte_connections;
            lte_connected_send = true;
         }
      }
   }
   if (type == LTE_STATE_SLEEPING) {
      network_sleeping = active;
      dtls_power_management();
   }
}

#ifdef CONFIG_ADXL362_MOTION_DETECTION
static void accelerometer_handler(const struct accelerometer_evt *const evt)
{
   moved = true;
   dtls_info("accelerometer trigger, x %.02f, y %.02f, z %.02f", evt->values[0], evt->values[1], evt->values[2]);
#ifdef CONFIG_ADXL362_MOTION_DETECTION_LED
   ui_led_op(LED_COLOR_GREEN, LED_BLINK);
   ui_led_op(LED_COLOR_RED, LED_BLINK);
#endif /* CONFIG_ADXL362_MOTION_DETECTION_LED */
}
#endif

#define USE_POLL

int dtls_loop(session_t *dst, int flags)
{
#ifdef USE_POLL
   struct pollfd udp_poll;
#else
   fd_set rfds, efds;
   struct timeval io_timeout;
#endif
   dtls_context_t *dtls_context = NULL;
   int result;
   int loops = 0;
   long time;
   bool send_request = false;

#ifdef CONFIG_COAP_WAIT_ON_POWERMANAGER
   uint16_t battery_voltage = 0xffff;
#endif

#ifdef CONFIG_LOCATION_ENABLE
   bool location_init = true;
#endif

   coap_client_init();
   app_data.fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

   if (app_data.fd < 0) {
      dtls_warn("Failed to create UDP socket: %d (%s)", errno, strerror(errno));
      reboot(ERROR_CODE_INIT_SOCKET, false);
   }
   modem_set_rai_mode(RAI_OFF, app_data.fd);

   if (flags & FLAG_TLS) {
      dtls_init();
      dtls_context = dtls_new_context(&app_data);
      if (!dtls_context) {
         dtls_emerg("cannot create context");
         reboot(ERROR_CODE_INIT_NO_DTLS, false);
      }
      dtls_credentials_init_handler(&cb);
      dtls_set_handler(dtls_context, &cb);
      app_data.dtls_pending = true;
   }
#if defined(CONFIG_UDP_AS_RAI_ENABLE) && defined(USE_SO_RAI_NO_DATA)
   // using SO_RAI_NO_DATA requires a destination, for what ever
   connect(fd, (struct sockaddr *)&dst.addr.sin, sizeof(struct sockaddr_in));
#endif

   app_data.timeout = COAP_ACK_TIMEOUT;
   if ((flags & FLAG_KEEP_CONNECTION) && (flags & FLAG_TLS)) {
      dtls_start_connect(dtls_context, dst);
   } else {
      app_data.request_state = NONE;
   }

   while (1) {

#ifdef CONFIG_LOCATION_ENABLE
      uint8_t battery_level = 0xff;
      bool force = false;
#ifdef CONFIG_ADXL362_MOTION_DETECTION
      force = moved;
      moved = false;
#endif
      power_manager_status(&battery_level, NULL, NULL, NULL);
      if (location_enabled()) {
         if (battery_level < 20) {
            dtls_info("Low battery, switch off GNSS");
            location_stop();
         } else if (force) {
            dtls_info("Motion detected, force GNSS");
            location_start(force);
         }
      } else if (!dtls_pending) {
         if (battery_level > 80 && battery_level < 0xff) {
            dtls_info("High battery, switch on GNSS");
            location_start(false);
         } else if (location_init && (battery_level == 0xff || battery_level >= 20)) {
            location_init = false;
            dtls_info("Starting, switch on GNSS");
            location_start(false);
         }
      }
#endif
      if (current_failures > handled_failures) {
         handled_failures = current_failures;
         dtls_info("handle failure %d.", current_failures);
         if (current_failures == 4) {
            // reboot
            reboot(ERROR_CODE_TOO_MANY_FAILURES, false);
         } else if (current_failures == 3) {
            // restart dtls
            app_data.dtls_pending = true;
            dtls_trigger();
         } else if (current_failures == 2) {
            // restart modem
            trigger_restart_modem = true;
            dtls_trigger();
         }
      }
      if (trigger_restart_modem) {
         restart_modem(&app_data);
         reopen_socket(&app_data);
      }
#ifdef USE_POLL
      udp_poll.fd = app_data.fd;
      udp_poll.events = POLLIN;
      udp_poll.revents = 0;
#else
      FD_ZERO(&rfds);
      FD_ZERO(&efds);
      FD_SET(dtls_add_data.fd, &rfds);
      FD_SET(dtls_add_data.fd, &efds);
#endif

      if (app_data.request_state != NONE) {
#ifdef USE_POLL
         result = poll(&udp_poll, 1, 1000);
#else
         io_timeout.tv_sec = 1;
         io_timeout.tv_usec = 0;
         result = select(dtls_add_data.fd + 1, &rfds, NULL, &efds, &io_timeout);
#endif
      } else {
#ifdef CONFIG_COAP_WAIT_ON_POWERMANAGER
         if (0xffff == battery_voltage || 0 == battery_voltage) {
            /* wait until the power manager starts to report the battery voltage */
            if (!power_manager_voltage(&battery_voltage)) {
               if (0 == battery_voltage || 0xffff == battery_voltage) {
                  k_sleep(K_MSEC(200));
                  continue;
               }
               dtls_info("Power-manager ready: %umV", battery_voltage);
            }
         }
#endif
         result = 0;
         dtls_power_management();
         if (k_sem_take(&dtls_trigger_msg, K_SECONDS(60)) == 0) {
            app_data.request_state = SEND;
            dtls_power_management();
            ui_led_op(LED_APPLICATION, LED_SET);
#if CONFIG_COAP_SEND_INTERVAL > 0
            work_reschedule_for_io_queue(&dtls_timer_trigger_work, K_SECONDS(CONFIG_COAP_SEND_INTERVAL));
#endif
            if (lte_power_off) {
               dtls_info("modem on");
               lte_power_off = false;
               trigger_restart_modem = false;
               connect_time = (unsigned long)k_uptime_get();
               modem_start(K_SECONDS(CONFIG_MODEM_SEARCH_TIMEOUT));
               reopen_socket(&app_data);
            } else if (trigger_restart_modem) {
               restart_modem(&app_data);
               reopen_socket(&app_data);
            } else {
               check_socket(&app_data, false);
            }
            loops = 0;
            app_data.retransmission = 0;
            if (coap_client_prepare_post() < 0) {
               dtls_coap_failure(&app_data);
            } else {
               if (app_data.dtls_pending) {
                  dtls_peer_t *peer = dtls_get_peer(dtls_context, dst);
                  if (peer) {
                     dtls_reset_peer(dtls_context, peer);
                  }
                  ui_led_op(LED_COLOR_GREEN, LED_SET);
                  dtls_start_connect(dtls_context, dst);
                  send_request = true;
               } else {
                  sendto_peer(&app_data, dst, dtls_context);
               }
            }
         }
      }
      if (result < 0) { /* error */
         if (errno != EINTR) {
            dtls_warn("select failed: errno %d (%s)", result, strerror(errno));
         }
      } else if (result == 0) { /* timeout */
         if (app_data.request_state == SEND) {
            if (lte_connected_send) {
               loops = 0;
               time = connected_time - connect_time;
               if (time < 0)
                  time = -1;
               if (app_data.request_state == SEND) {
                  dtls_info("%d/%d/%u-%ld ms: connected => sent", network_ready, lte_connected_send, lte_connections, time);
               } else {
                  dtls_info("%d/%d/%u-%ld ms: connected => resent", network_ready, lte_connected_send, lte_connections, time);
               }
               app_data.request_state = RECEIVE;
#ifdef CONFIG_COAP_NO_RESPONSE_ENABLE
               if (!app_data.dtls_pending) {
                  dtls_coap_success(&app_data);
               }
#endif
            } else if (loops < 60) {
               ++loops;
               if (loops % 5 == 4) {
                  check_socket(&app_data, false);
               }
            } else {
               if (app_data.dtls_pending) {
                  dtls_info("DTLS send timeout");
               } else {
                  dtls_info("CoAP send timeout");
               }
               dtls_coap_failure(&app_data);
               reopen_socket(&app_data);
            }
         } else if (app_data.request_state == RECEIVE) {
            int temp = app_data.timeout;
            if (!network_ready) {
               if (app_data.retransmission >= COAP_MAX_RETRANSMISSION) {
                  // stop waiting ...
                  temp = loops;
               } else {
                  temp += ADD_ACK_TIMEOUT;
               }
            }
            ++loops;
            if (app_data.retransmission > 0) {
               dtls_info("CoAP wait %d/%d/%d, retrans. %d, network %d", loops, app_data.timeout, temp, app_data.retransmission, network_ready);
            } else {
               dtls_info("CoAP wait %d/%d/%d", loops, app_data.timeout, temp);
            }
            if (loops > temp) {
               result = -1;
               if (app_data.retransmission < COAP_MAX_RETRANSMISSION) {
                  ++app_data.retransmission;
                  loops = 0;
                  app_data.timeout <<= 1;
                  app_data.request_state = SEND;

                  if (app_data.dtls_pending) {
                     dtls_info("hs resend, timeout %d", app_data.timeout);
                     dtls_check_retransmit(dtls_context, NULL);
                  } else {
                     dtls_info("CoAP request resend, timeout %d", app_data.timeout);
                     sendto_peer(&app_data, dst, dtls_context);
                  }
               } else {
                  // maximum retransmissions reached
                  if (app_data.dtls_pending) {
                     dtls_info("DTLS hs timeout %d", app_data.timeout);
                  } else {
                     dtls_info("CoAP request timeout %d", app_data.timeout);
                  }
                  dtls_coap_failure(&app_data);
               }
            }
         } else if (app_data.request_state == WAIT_RESPONSE) {
            if (loops < 60) {
               ++loops;
            } else {
               dtls_coap_failure(&app_data);
            }
         } else if (app_data.request_state == WAIT_SUSPEND) {
            // wait for late received data
            if (network_sleeping) {
               // modem enters sleep, no more data
               app_data.request_state = NONE;
               dtls_info("CoAP suspend after %d", loops);
            } else {
               if (k_sem_count_get(&dtls_trigger_msg)) {
                  // send button pressed
                  app_data.request_state = NONE;
               }
               ++loops;
            }
         } else if (app_data.request_state != NONE) {
            ++loops;
            dtls_info("CoAP wait state %d, %d", app_data.request_state, loops);
         }
      } else { /* ok */
#ifdef USE_POLL
         if (udp_poll.revents & POLLIN) {
#else
         if (FD_ISSET(dtls_add_data.fd, &rfds)) {
#endif
            recvfrom_peer(&app_data, dtls_context);
            if (app_data.request_state == SEND_ACK) {
               unsigned long temp_time = connect_time;
               sendto_peer(&app_data, dst, dtls_context);
               connect_time = temp_time;
               dtls_coap_success(&app_data);
               dtls_info("CoAP ACK sent.");
            } else if (!app_data.dtls_pending && send_request) {
               dtls_info("DTLS finished, send coap request.");
               send_request = false;
               loops = 0;
               app_data.retransmission = 0;
               sendto_peer(&app_data, dst, dtls_context);
            }
            if (app_data.request_state == NONE && !lte_power_on_off) {
               modem_set_rai_mode(RAI_NOW, app_data.fd);
            }
            if (app_data.request_state == NONE &&
                (flags & FLAG_TLS) &&
                !(flags & FLAG_KEEP_CONNECTION) &&
                !app_data.dtls_pending) {
               app_data.dtls_pending = true;
               ui_led_op(LED_DTLS, LED_CLEAR);
            }
#ifdef USE_POLL
         } else if (udp_poll.revents & (POLLERR | POLLNVAL)) {
#else
         } else if (FD_ISSET(dtls_add_data.fd, &efds)) {
#endif
            result = check_socket(&app_data, true);
            if (result) {
               if (app_data.request_state == SEND ||
                   app_data.request_state == RECEIVE) {
                  loops = 0;
                  app_data.retransmission = 0;
                  app_data.request_state = SEND;
                  if (app_data.dtls_pending) {
                     dtls_info("hs send again");
                     dtls_check_retransmit(dtls_context, NULL);
                  } else {
                     dtls_info("CoAP request send again");
                     sendto_peer(&app_data, dst, dtls_context);
                  }
               }
            }
         }
      }
   }

   dtls_free_context(dtls_context);
   return 0;
}

static int init_destination(int protocol, session_t *destination)
{
   const char *host = NULL;
   char ipv4_addr[NET_IPV4_ADDR_LEN];
   int err = 0;
   int rc = -ENOENT;

#ifdef CONFIG_COAP_SERVER_HOSTNAME

   if (strlen(CONFIG_COAP_SERVER_HOSTNAME) > 0) {
      int count = 0;
      struct addrinfo *result = NULL;
      struct addrinfo hints = {
          .ai_family = AF_INET,
          .ai_socktype = SOCK_DGRAM};

      host = CONFIG_COAP_SERVER_HOSTNAME;
      err = getaddrinfo(CONFIG_COAP_SERVER_HOSTNAME, NULL, &hints, &result);
      while (-err == EAGAIN && count < 10) {
         k_sleep(K_MSEC(1000));
         ++count;
         err = getaddrinfo(CONFIG_COAP_SERVER_HOSTNAME, NULL, &hints, &result);
      }
      if (err != 0) {
         dtls_warn("ERROR: getaddrinfo failed %d", err);
         rc = -EIO;
      } else if (result == NULL) {
         dtls_warn("ERROR: Address not found");
         rc = -ENOENT;
      }
      if (result) {
         /* Free the address. */
         rc = 0;
         destination->addr.sin = *((struct sockaddr_in *)result->ai_addr);
         freeaddrinfo(result);
      }
   }
#endif
#ifdef CONFIG_COAP_SERVER_ADDRESS_STATIC
   if (rc) {
      if (strlen(CONFIG_COAP_SERVER_ADDRESS_STATIC) > 0) {
         err = inet_pton(AF_INET, CONFIG_COAP_SERVER_ADDRESS_STATIC,
                         &(destination->addr.sin.sin_addr));
         if (err == 1) {
            destination->addr.sin.sin_family = AF_INET;
            rc = 0;
         } else {
            dtls_warn("ERROR: inet_pton failed %d", err);
            rc = -EIO;
         }
      }
   }
#endif
   if (rc) {
      return rc;
   }
   if (protocol & 1) {
      // non-secure
      destination->addr.sin.sin_port = htons(CONFIG_COAP_SERVER_PORT);
   } else {
      // secure
      destination->addr.sin.sin_port = htons(CONFIG_COAPS_SERVER_PORT);
   }
   destination->size = sizeof(struct sockaddr_in);

   inet_ntop(AF_INET, &destination->addr.sin.sin_addr.s_addr, ipv4_addr,
             sizeof(ipv4_addr));
   if (host) {
      dtls_info("Destination: %s", host);
      dtls_info("IPv4 Address found %s", ipv4_addr);
   } else {
      dtls_info("Destination: %s", ipv4_addr);
   }
   return 0;
}

void main(void)
{
   int config = 0;
   int protocol = -1;
   int flags = 0;
   char imei[MODEM_ID_SIZE];
   session_t dst;

   memset(&app_data, 0, sizeof(app_data));

   LOG_INF("CoAP/DTLS 1.2 CID sample " CLIENT_VERSION " has started");

   dtls_set_log_level(DTLS_LOG_INFO);

   if (!appl_eeprom_init()) {
      appl_eeprom = true;
   }

   io_job_queue_init();

   ui_init(dtls_manual_trigger);
   config = ui_config();

#ifdef CONFIG_LTE_POWER_ON_OFF_ENABLE
   dtls_info("LTE power on/off");
   lte_power_on_off = true;
#elif CONFIG_LTE_POWER_ON_OFF_CONFIG_SWITCH
   if (config >= 0) {
      lte_power_on_off = config & 4 ? true : false;
      dtls_info("LTE power on/off %s.", lte_power_on_off ? "enabled" : "disabled");
      if (config & 8) {
         protocol = 1;
      }
   }
#elif CONFIG_PROTOCOL_CONFIG_SWITCH
   if (config >= 0) {
      protocol = config >> 3;
      switch (protocol) {
         case 0:
            dtls_info("CoAP/DTLS 1.2 CID");
            flags |= FLAG_KEEP_CONNECTION;
            break;
         case 1:
            dtls_info("CoAP/UDP");
            break;
      }
   }
#endif
   if (protocol < 0) {
#ifdef CONFIG_PROTOCOL_MODE_UDP
      protocol = 1;
      dtls_info("CoAP/UDP");
#elif CONFIG_PROTOCOL_MODE_DTLS
      protocol = 0;
      flags |= FLAG_KEEP_CONNECTION;
      dtls_info("CoAP/DTLS 1.2 CID");
#else
      protocol = 0;
      flags |= FLAG_KEEP_CONNECTION;
      dtls_info("CoAP/DTLS 1.2 CID");
#endif
   }
   if (!(protocol & 1)) {
      flags |= FLAG_TLS;
   }

#if (defined CONFIG_DTLS_ALWAYS_HANDSHAKE)
   dtls_always_handshake = true;
#endif
   if (config < 0) {
      config = 0;
   }

   modem_init(config, dtls_lte_state_handler);
   dtls_trigger();

   power_manager_init();

#ifdef CONFIG_LOCATION_ENABLE
#ifdef CONFIG_LOCATION_ENABLE_TRIGGER_MESSAGE
   dtls_info("location with trigger");
   location_init(dtls_trigger);
#else  /* CONFIG_LOCATION_ENABLE_TRIGGER_MESSAGE */
   dtls_info("location without trigger");
   location_init(NULL);
#endif /* CONFIG_LOCATION_ENABLE_TRIGGER_MESSAGE */
#else  /* CONFIG_LOCATION_ENABLE */
   dtls_warn("no location");
#endif

#ifdef CONFIG_ADXL362_MOTION_DETECTION
   accelerometer_init(accelerometer_handler);
   accelerometer_enable(true);
#endif

#ifdef ENVIRONMENT_SENSOR
   environment_init();
#endif

#ifdef CONFIG_CONSOLE_SUBSYS
   console_init_input();
#endif

   if (modem_start(K_SECONDS(CONFIG_MODEM_SEARCH_TIMEOUT)) != 0) {
      appl_ready = true;
      restart_modem(NULL);
   }
   appl_ready = true;

   modem_get_imei(imei, sizeof(imei));
   dtls_credentials_init_psk(imei);
   coap_client_set_id(dtls_credentials_get_psk_identity());

   memset(&dst, 0, sizeof(session_t));
   init_destination(protocol, &dst);

   dtls_loop(&dst, flags);
}

void main_(void)
{
   modem_power_off();
   power_manager_init();
   power_manager_suspend(true);
   // power_manager_3v3(false);
   // power_manager_1v8(false);
   k_sleep(K_MSEC(1000));
   NRF_REGULATORS->SYSTEMOFF = 1;
}
