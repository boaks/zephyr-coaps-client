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
#include <stdio.h>
#include <string.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/socket.h>

#include "appl_diagnose.h"
#include "appl_storage.h"
#include "appl_storage_config.h"
#include "appl_time.h"
#ifdef CONFIG_UART_UPDATE
#include "appl_update.h"
#endif
#include "coap_client.h"
#include "dtls.h"
#include "dtls_credentials.h"
#include "dtls_debug.h"
#include "global.h"
#include "io_job_queue.h"
#include "modem.h"
#include "modem_sim.h"
#include "power_manager.h"
#include "ui.h"

#ifdef CONFIG_LOCATION_ENABLE
#include "location.h"
#endif

#ifdef CONFIG_MOTION_SENSOR
#include "accelerometer_sensor.h"
#endif

#include "environment_sensor.h"

#define COAP_ACK_TIMEOUT 3
#define ADD_ACK_TIMEOUT 3

#define LED_APPLICATION LED_LTE_1
#define LED_DTLS LED_NONE

#define MSEC_PER_HOUR (MSEC_PER_SEC * 60 * 60)
#define MSEC_PER_DAY (MSEC_PER_SEC * 60 * 60 * 24)

LOG_MODULE_REGISTER(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

typedef enum {
   NONE,
   SEND,
   RECEIVE,
   WAIT_RESPONSE,
   SEND_ACK,
   WAIT_SUSPEND,
} request_state_t;

typedef enum {
   NO_SEARCH,
   MANUAL_SEARCH,
   CMD_SEARCH,
   EVENT_SEARCH,
   READY_SEARCH
} search_trigger_t;

typedef struct dtls_app_data_t {
   session_t *destination;
   int fd;
   bool dtls_pending;
   uint8_t retransmission;
   request_state_t request_state;
   uint16_t timeout;
   int64_t start_time;
} dtls_app_data_t;

#define LTE_REGISTERED 0
#define LTE_READY 1
#define LTE_CONNECTED 2
#define LTE_SLEEPING 3
#define LTE_CONNECTED_SEND 4
#define LTE_SOCKET_ERROR 5
#define PM_PREVENT_SUSPEND 6
#define PM_SUSPENDED 7

static atomic_t general_states = ATOMIC_INIT(0);

static atomic_t lte_connections = ATOMIC_INIT(0);
static atomic_t not_ready_time = ATOMIC_INIT(0);
static atomic_t connected_time = ATOMIC_INIT(0);
static long connect_time = 0;
static long response_time = 0;

static dtls_app_data_t app_data;

static bool initial_success = false;
static unsigned int current_failures = 0;
static unsigned int handled_failures = 0;

static volatile bool appl_ready = false;
static volatile int trigger_duration = 0;
static volatile search_trigger_t trigger_search = NO_SEARCH;

static volatile bool lte_power_off = false;
static bool lte_power_on_off = false;

#ifdef CONFIG_MOTION_DETECTION
static volatile bool moved = false;
#endif

#define MAX_READ_BUF 1600
static uint8_t receive_buffer[MAX_READ_BUF];

#define RTT_SLOTS 9
#define RTT_INTERVAL (2 * MSEC_PER_SEC)
static unsigned int rtts[RTT_SLOTS + 2] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 20};

static K_SEM_DEFINE(dtls_trigger_msg, 0, 1);
static K_SEM_DEFINE(dtls_trigger_search, 0, 1);

static K_MUTEX_DEFINE(dtls_pm_mutex);

static void dtls_power_management(void);

static void dtls_power_management_fn(struct k_work *work)
{
   dtls_power_management();
}

static K_WORK_DEFINE(dtls_power_management_work, dtls_power_management_fn);

static void dtls_power_management_suspend_fn(struct k_work *work)
{
   atomic_clear_bit(&general_states, PM_PREVENT_SUSPEND);
   ui_led_op(LED_COLOR_ALL, LED_CLEAR);
   dtls_power_management();
}

static K_WORK_DELAYABLE_DEFINE(dtls_power_management_suspend_work, dtls_power_management_suspend_fn);

static void dtls_log_state(void)
{
   char buf[128];
   int index = 0;
   if (atomic_test_bit(&general_states, LTE_CONNECTED_SEND)) {
      index = snprintf(buf, sizeof(buf), "connected send");
   } else if (atomic_test_bit(&general_states, LTE_CONNECTED)) {
      index = snprintf(buf, sizeof(buf), "connected");
   } else if (atomic_test_bit(&general_states, LTE_READY)) {
      index = snprintf(buf, sizeof(buf), "ready");
   } else if (atomic_test_bit(&general_states, LTE_REGISTERED)) {
      index = snprintf(buf, sizeof(buf), "registered");
   } else {
      index = snprintf(buf, sizeof(buf), "not registered");
   }
   if (atomic_test_bit(&general_states, LTE_SLEEPING)) {
      index += snprintf(&buf[index], sizeof(buf) - index, ", modem sleeping");
   }
   if (atomic_test_bit(&general_states, LTE_SOCKET_ERROR)) {
      index += snprintf(&buf[index], sizeof(buf) - index, ", socket error");
   }
   if (atomic_test_bit(&general_states, PM_PREVENT_SUSPEND)) {
      index += snprintf(&buf[index], sizeof(buf) - index, ", prevent suspend");
   } else if (atomic_test_bit(&general_states, PM_SUSPENDED)) {
      index += snprintf(&buf[index], sizeof(buf) - index, ", suspended");
   }
   LOG_INF("State: %s", buf);
}

static void dtls_power_management(void)
{
   bool previous = false;
   bool suspend = false;

   if (!appl_reboots()) {
      k_mutex_lock(&dtls_pm_mutex, K_FOREVER);
      suspend = atomic_test_bit(&general_states, LTE_SLEEPING) &&
                !atomic_test_bit(&general_states, PM_PREVENT_SUSPEND) &&
                app_data.request_state == NONE;
      k_mutex_unlock(&dtls_pm_mutex);
   }

   if (suspend) {
      previous = atomic_test_and_set_bit(&general_states, PM_SUSPENDED);
   } else {
      previous = atomic_test_and_clear_bit(&general_states, PM_SUSPENDED);
   }

   if (previous != suspend) {
      if (suspend) {
         ui_led_op(LED_COLOR_ALL, LED_CLEAR);
      }
      power_manager_suspend(suspend);
   }
}

static void reboot(int error, bool factoryReset)
{
   // write error code, reboot in 120s
   appl_reboot(error, K_SECONDS(120));

   atomic_set_bit(&general_states, PM_PREVENT_SUSPEND);
   dtls_power_management();
   ui_led_op(LED_COLOR_RED, LED_BLINKING);

   modem_power_off();
   dtls_info("> modem off");
   if (factoryReset) {
      modem_factory_reset();
   }
   ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
   ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
   ui_led_op(LED_COLOR_RED, LED_SET);
   k_sleep(K_MSEC(500));
   for (int i = 0; i < 4; ++i) {
      ui_led_op(LED_COLOR_RED, LED_CLEAR);
      ui_led_op(LED_COLOR_BLUE, LED_SET);
      k_sleep(K_MSEC(500));
      ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
      ui_led_op(LED_COLOR_RED, LED_SET);
      k_sleep(K_MSEC(500));
   }
   // reboot now
   appl_reboot(error, K_NO_WAIT);
}

static void check_reboot(void)
{
   if (trigger_duration) {
      // Thingy:91 and nRF9160 feather will reboot
      // nRF9160-DK reboots with button2 also pressed
      int ui = ui_config();
      if (ui < 0) {
         dtls_info("> modem reboot / factory reset");
         reboot(ERROR_CODE_MANUAL_TRIGGERED, true);
      } else if (ui & 2) {
         dtls_info("> modem reboot");
         reboot(ERROR_CODE_MANUAL_TRIGGERED, false);
      }
      trigger_duration = 0;
   }
}

static int get_socket_error(dtls_app_data_t *app)
{
   int error = -ENOTCONN;
   if (app->fd >= 0) {
      socklen_t len = sizeof(error);
      int result = getsockopt(app->fd, SOL_SOCKET, SO_ERROR, &error, &len);
      if (result) {
         error = errno;
      }
   }
   return error;
}

static bool restart_modem(void)
{
   watchdog_feed();
   check_reboot();

   dtls_info("> modem restart");
   atomic_set_bit(&general_states, PM_PREVENT_SUSPEND);
   dtls_power_management();
   ui_led_op(LED_COLOR_BLUE, LED_BLINKING);
   ui_led_op(LED_COLOR_RED, LED_BLINKING);
   modem_set_lte_offline();
   dtls_info("> modem offline");
   k_sleep(K_MSEC(2000));
   ui_led_op(LED_COLOR_ALL, LED_CLEAR);
   dtls_info("> modem restarting ...");
   modem_start(K_SECONDS(CONFIG_MODEM_SEARCH_TIMEOUT), false);
   atomic_clear_bit(&general_states, PM_PREVENT_SUSPEND);
   dtls_power_management();
   watchdog_feed();
   if (atomic_test_bit(&general_states, LTE_READY)) {
      dtls_info("> modem ready.");
      return true;
   } else if (atomic_test_bit(&general_states, LTE_REGISTERED)) {
      dtls_info("> modem registered, not ready.");
      return false;
   } else {
      dtls_info("> modem not registered.");
      return false;
   }
}

static void close_socket(dtls_app_data_t *app)
{
   if (app->fd >= 0) {
      modem_set_rai_mode(RAI_OFF, app->fd);
      (void)close(app->fd);
      app->fd = -1;
   }
}

static bool reopen_socket(dtls_app_data_t *app, const char *loc)
{
   bool ready = atomic_test_bit(&general_states, LTE_READY);
   if (!ready) {
      bool registered = atomic_test_bit(&general_states, LTE_REGISTERED);
      dtls_info("> %s, reopen socket (modem %s)", loc,
                registered ? "registered, not ready" : "not ready");
   } else {
      dtls_info("> %s, reopen socket (modem ready)", loc);
   }
   close_socket(app);
   if (!ready) {
      return false;
   }

   app->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
   if (app->fd < 0) {
      dtls_warn("> %s, reopen UDP socket failed, %d, errno %d (%s), reboot",
                loc, app->fd, errno, strerror(errno));
      reboot(ERROR_CODE(ERROR_CODE_OPEN_SOCKET, errno), false);
   }
   modem_set_psm(CONFIG_UDP_PSM_CONNECT_RAT);
#ifdef CONFIG_UDP_USE_CONNECT
   // using SO_RAI_NO_DATA requires a destination, for what ever
   connect(app->fd, (struct sockaddr *)&app->destination->addr.sin, sizeof(struct sockaddr_in));
#endif
   modem_set_rai_mode(RAI_OFF, app->fd);
   dtls_info("> %s, reopened socket.", loc);
   return true;
}

static int check_socket(dtls_app_data_t *app)
{
   int error = -1;
   if (app->fd >= 0) {
      error = get_socket_error(app);
      if (error) {
         dtls_info("socket error %d", error);
         close_socket(app);
      }
   }
   return error;
}

static inline bool dtls_no_pending_request(void)
{
   request_state_t state = app_data.request_state;
   return (state == NONE) || (state == WAIT_SUSPEND);
}

static void dtls_trigger(void)
{
   if (appl_reboots()) {
      return;
   }
   if (dtls_no_pending_request()) {
      // read battery status
      power_manager_status(NULL, NULL, NULL, NULL);
      k_sem_give(&dtls_trigger_msg);
   }
}

static void dtls_manual_trigger(int duration)
{
   if (!appl_ready) {
      trigger_duration = 0;
   } else {
      trigger_duration = duration;
   }
   // LEDs for manual trigger
   ui_enable(true);
   ui_led_op(LED_COLOR_RED, LED_CLEAR);
   dtls_trigger();

   if (!atomic_test_bit(&general_states, LTE_READY)) {
      trigger_search = MANUAL_SEARCH;
      modem_start_search();
      k_sem_give(&dtls_trigger_search);
   }
}

void dtls_cmd_trigger(bool led, int mode)
{
   bool ready = atomic_test_bit(&general_states, LTE_READY);
   if (mode & 1) {
      if (dtls_no_pending_request()) {
         ui_enable(led);
         dtls_trigger();
         if (!ready && !(mode & 2)) {
            LOG_INF("No network ...");
         }
      } else if (ready) {
         LOG_INF("Busy, request pending ... (state %d)", app_data.request_state);
      } else {
         LOG_INF("Busy, searching network");
      }
   }
   if (!ready && mode & 2) {
      ui_enable(led);
      trigger_search = CMD_SEARCH;
      modem_start_search();
      k_sem_give(&dtls_trigger_search);
   }
}

#if CONFIG_COAP_SEND_INTERVAL > 0

static void dtls_timer_trigger_fn(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(dtls_timer_trigger_work, dtls_timer_trigger_fn);

static void dtls_timer_trigger_fn(struct k_work *work)
{
   if (dtls_no_pending_request()) {
      // no LEDs for time trigger
      ui_enable(false);
      dtls_trigger();
   } else {
      LOG_DBG("Busy, schedule again in %d s.", CONFIG_COAP_SEND_INTERVAL);
      work_schedule_for_io_queue(&dtls_timer_trigger_work, K_SECONDS(CONFIG_COAP_SEND_INTERVAL));
   }
}
#endif

static void dtls_coap_next(dtls_app_data_t *app)
{
   int64_t now;
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
      LOG_DBG("Failure, schedule in %d s.", CONFIG_COAP_FAILURE_SEND_INTERVAL);
      work_reschedule_for_io_queue(&dtls_timer_trigger_work, K_SECONDS(CONFIG_COAP_FAILURE_SEND_INTERVAL));
   } else {
      LOG_DBG("Success, schedule in %d s.", CONFIG_COAP_SEND_INTERVAL);
      work_schedule_for_io_queue(&dtls_timer_trigger_work, K_SECONDS(CONFIG_COAP_SEND_INTERVAL));
   }
#else  /*CONFIG_COAP_FAILURE_SEND_INTERVAL*/
   LOG_DBG("Next, schedule in %d s.", CONFIG_COAP_SEND_INTERVAL);
   work_schedule_for_io_queue(&dtls_timer_trigger_work, K_SECONDS(CONFIG_COAP_SEND_INTERVAL));
#endif /*CONFIG_COAP_FAILURE_SEND_INTERVAL*/
#endif /*CONFIG_COAP_SEND_INTERVAL*/

   appl_get_now(&now);
   if (appl_format_time(now, buf, sizeof(buf))) {
      dtls_info("%s", buf);
   }
   app->request_state = WAIT_SUSPEND;
   k_sem_reset(&dtls_trigger_msg);
}

static void dtls_coap_success(dtls_app_data_t *app)
{
   int index;
   int time1 = (int)(atomic_get(&connected_time) - connect_time);
   int time2 = (int)(response_time - connect_time);
   if (time1 < 0) {
      time1 = -1;
   }
   if (time2 < 0) {
      time2 = -1;
   }
   ui_led_op(LED_COLOR_ALL, LED_CLEAR);
   dtls_info("%dms/%dms: success", time1, time2);
   if (app->retransmission <= COAP_MAX_RETRANSMISSION) {
      transmissions[app->retransmission]++;
   }
   if (app->retransmission == 0) {
      modem_set_psm(0);
   }
   index = time2 / RTT_INTERVAL;
   if (index < RTT_SLOTS) {
      rtts[index]++;
   } else {
      rtts[RTT_SLOTS]++;
      time2 /= MSEC_PER_SEC;
      if (time2 > rtts[RTT_SLOTS + 1]) {
         // new max. time
         rtts[RTT_SLOTS + 1] = time2;
      }
   }
   if (time1 < 2000) {
      unsigned long sum = 0;
      unsigned int num = 0;
      unsigned int rtt = 0;
      dtls_info("retrans: 0*%u, 1*%u, 2*%u, 3*%u, failures %u", transmissions[0], transmissions[1], transmissions[2], transmissions[3], transmissions[4]);
      dtls_info("rtt: 0-2s: %u, 2-4s: %u, 4-6s: %u, 6-8s: %u, 8-10s: %u", rtts[0], rtts[1], rtts[2], rtts[3], rtts[4]);
      dtls_info("rtt: 10-12s: %u, 12-14s: %u, 14-16s: %u, 16-18s: %u, 18-%u: %u", rtts[5], rtts[6], rtts[7], rtts[8], rtts[10], rtts[9]);
      for (index = 0; index <= RTT_SLOTS; ++index) {
         rtt = rtts[index];
         if (rtt > 0) {
            num += rtt;
            sum += rtt * (index * 2 + 1);
         }
      }
      if (num > 0) {
         dtls_info("rtt: avg. %lus (%u#)", sum / num, num);
      }
   }
   // reset failures on success
   current_failures = 0;
   handled_failures = 0;
   if (!initial_success) {
      initial_success = true;
#ifdef CONFIG_UART_UPDATE
      appl_update_image_verified();
#endif
   }
   dtls_coap_next(app);
}

static void dtls_coap_failure(dtls_app_data_t *app, const char *cause)
{
   int time1 = (int)(atomic_get(&connected_time) - connect_time);
   int time2 = (int)(response_time - connect_time);
   if (time1 < 0) {
      time1 = -1;
   }
   if (time2 < 0) {
      time2 = -1;
   }
   if (!ui_led_op(LED_COLOR_RED, LED_SET)) {
      atomic_set_bit(&general_states, PM_PREVENT_SUSPEND);
      work_reschedule_for_io_queue(&dtls_power_management_suspend_work, K_SECONDS(10));
   }
   ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
   ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
   dtls_info("%dms/%dms: failure, %s", time1, time2, cause);
   transmissions[COAP_MAX_RETRANSMISSION + 1]++;
   if (initial_success) {
      ++current_failures;
      dtls_info("current failures %d.", current_failures);
   }
   dtls_coap_next(app);
}

static int
read_from_peer(dtls_app_data_t *app, session_t *session, uint8 *data, size_t len)
{
   (void)session;

   int err = coap_client_parse_data(data, len);
   if (err < 0) {
      return err;
   }

   switch (err) {
      case PARSE_IGN:
         break;
      case PARSE_RST:
         if (NONE != app->request_state) {
            response_time = (long)k_uptime_get();
            dtls_coap_failure(app, "rst");
         }
         break;
      case PARSE_ACK:
         if (NONE != app->request_state) {
            app->request_state = WAIT_RESPONSE;
         }
         break;
      case PARSE_RESPONSE:
         if (NONE != app->request_state) {
            response_time = (long)k_uptime_get();
            dtls_coap_success(app);
         }
         break;
      case PARSE_CON_RESPONSE:
         if (NONE != app->request_state) {
            response_time = (long)k_uptime_get();
            app->request_state = SEND_ACK;
         }
         break;
   }

   return 0;
}

static int
dtls_read_from_peer(dtls_context_t *ctx, session_t *session, uint8 *data, size_t len)
{
   dtls_app_data_t *app = dtls_get_app_data(ctx);
   return read_from_peer(app, session, data, len);
}

static void prepare_socket(dtls_app_data_t *app)
{
   atomic_clear_bit(&general_states, LTE_CONNECTED_SEND);
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
   bool connected;
   int result = 0;
   const char *tag = app->dtls_pending ? (app->retransmission ? "hs_re" : "hs_") : (app->retransmission ? "re" : "");

   if (!lte_power_on_off) {
      if (!app->retransmission) {
         connect_time = (long)k_uptime_get();
      }
      prepare_socket(app);
   }
   result = sendto(app->fd, data, len, MSG_DONTWAIT, &session->addr.sa, session->size);
   if (result < 0) {
      dtls_warn("%ssend_to_peer failed: %d, errno %d (%s)", tag, result, errno, strerror(errno));
      return result;
   }
   connected = atomic_test_bit(&general_states, LTE_CONNECTED);
   if (connected) {
      modem_set_transmission_time();
   }
   if (app->retransmission == 0) {
      app->timeout = COAP_ACK_TIMEOUT;
   }
#ifndef NDEBUG
   /* logging */
   if (SEND == app->request_state) {
      if (connected) {
         dtls_info("%ssent_to_peer %d", tag, result);
      } else {
         dtls_info("%ssend_to_peer %d", tag, result);
      }
   } else if (RECEIVE == app->request_state) {
      if (connected) {
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
   if (app->dtls_pending) {
      if (app->request_state == SEND) {
         app->request_state = RECEIVE;
      }
      if (result < 0) {
         /* don't forward send errors,
            the dtls state machine will suffer */
         result = len;
      }
   }
   return result;
}

static int
dtls_handle_event(dtls_context_t *ctx, session_t *session,
                  dtls_alert_level_t level, unsigned short code)
{
   if (appl_reboots()) {
      return 0;
   }

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
   } else {
      dtls_info("dtls failure 0x%04x", code);
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
      if (app->dtls_pending) {
         app->retransmission = 0;
      }
      return dtls_handle_message(ctx, &session, receive_buffer, result);
   } else {
      return read_from_peer(app, &session, receive_buffer, result);
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
   if (!coap_message_len) {
      dtls_warn("send empty UDP message.");
   }
   if (ctx && coap_message_len) {
      result = dtls_write(ctx, dst, (uint8_t *)coap_message_buf, coap_message_len);
      if (result < 0) {
         dtls_warn("Failed to send CoAP request with %d bytes via DTLS, %d (%s)",
                   coap_message_len, errno, strerror(errno));
      }
   } else {
      result = send_to_peer(app, dst, coap_message_buf, coap_message_len);
      if (result < 0) {
         dtls_warn("Failed to send CoAP request with %d bytes via UDP, %d (%s)",
                   coap_message_len, errno, strerror(errno));
      }
   }
   if (result < 0) {
      if (!check_socket(app)) {
         dtls_coap_failure(app, "send");
      }
   } else {
      if (!lte_power_off) {
         ui_led_op(LED_COLOR_GREEN, LED_SET);
      }
      if (atomic_test_bit(&general_states, LTE_CONNECTED)) {
         ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
         app->request_state = RECEIVE;
#ifdef CONFIG_COAP_NO_RESPONSE_ENABLE
         if (!app->dtls_pending) {
            dtls_coap_success(app);
         }
#endif
      }
   }
   return result;
}

static int
dtls_start_connect(struct dtls_context_t *ctx, session_t *dst)
{
   dtls_app_data_t *app = dtls_get_app_data(ctx);
   dtls_info("Start DTLS 1.2 handshake.");
   app->retransmission = 0;
   app->request_state = SEND;
   return dtls_connect(ctx, dst);
}
/*---------------------------------------------------------------------------*/

static dtls_handler_t cb = {
    .write = dtls_send_to_peer,
    .read = dtls_read_from_peer,
    .event = dtls_handle_event,
};

static void dtls_lte_state_handler(enum lte_state_type type, bool active)
{
   long now;
   int bit = -1;
   const char *desc = NULL;

   if (appl_reboots()) {
      return;
   }
   now = (long)k_uptime_get();
   switch (type) {
      case LTE_STATE_REGISTRATION:
         desc = "registration";
         bit = LTE_REGISTERED;
         break;
      case LTE_STATE_READY:
         desc = "ready";
         bit = LTE_READY;
         break;
      case LTE_STATE_CONNECTED:
         desc = "connect";
         bit = LTE_CONNECTED;
         break;
      case LTE_STATE_SLEEPING:
         desc = "sleeping";
         bit = LTE_SLEEPING;
         break;
      default:
         break;
   }

   if (desc) {
      dtls_info("modem state: %s %s", desc, active ? "on" : "off");
   } else {
      dtls_info("modem state: %d %s", type, active ? "on" : "off");
   }
   bool previous = active;
   if (bit >= 0) {
      if (active) {
         previous = atomic_test_and_set_bit(&general_states, bit);
      } else {
         previous = atomic_test_and_clear_bit(&general_states, bit);
      }
   }

   if (type == LTE_STATE_REGISTRATION) {
      if (active) {
         if (!atomic_test_bit(&general_states, LTE_READY)) {
            ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
            ui_led_op(LED_COLOR_RED, LED_CLEAR);
         }
      } else {
         atomic_clear_bit(&general_states, LTE_READY);
         atomic_clear_bit(&general_states, LTE_CONNECTED);
         led_op_t op = LED_SET;
         if (lte_power_off || atomic_test_bit(&general_states, LTE_SLEEPING)) {
            op = LED_CLEAR;
         } else {
            trigger_search = EVENT_SEARCH;
            k_sem_give(&dtls_trigger_search);
         }
         ui_led_op(LED_COLOR_BLUE, op);
         ui_led_op(LED_COLOR_RED, op);
         ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
      }
   } else if (type == LTE_STATE_READY) {
      if (previous != active) {
         if (active) {
            atomic_set(&not_ready_time, 0);
            trigger_search = READY_SEARCH;
            k_sem_give(&dtls_trigger_search);
         } else {
            atomic_set(&not_ready_time, now == 0 ? -1 : now);
            atomic_clear_bit(&general_states, LTE_CONNECTED);
         }
      }
   } else if (type == LTE_STATE_CONNECTED) {
      if (!app_data.dtls_pending) {
         if (active) {
            ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
            ui_led_op(LED_COLOR_GREEN, LED_SET);
         } else if (NONE == app_data.request_state || WAIT_SUSPEND == app_data.request_state) {
            ui_led_op(LED_COLOR_RED, LED_CLEAR);
            ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
         }
      }
      if (active) {
         if (!previous) {
            atomic_set(&connected_time, now);
            atomic_inc(&lte_connections);
            atomic_set_bit(&general_states, LTE_CONNECTED_SEND);
         }
      }
   } else if (type == LTE_STATE_SLEEPING) {
      if (active) {
         atomic_set_bit(&general_states, PM_PREVENT_SUSPEND);
         work_schedule_for_io_queue(&dtls_power_management_suspend_work, K_SECONDS(2));
      } else {
         trigger_search = EVENT_SEARCH;
         k_sem_give(&dtls_trigger_search);
         work_submit_to_io_queue(&dtls_power_management_work);
      }
   }
}

#ifdef CONFIG_MOTION_DETECTION
static void accelerometer_handler(const struct accelerometer_evt *const evt)
{
   moved = true;
   dtls_info("accelerometer trigger, x %.02f, y %.02f, z %.02f", evt->values[0], evt->values[1], evt->values[2]);
#ifdef MOTION_DETECTION_LED
   ui_led_op(LED_COLOR_GREEN, LED_BLINK);
   ui_led_op(LED_COLOR_RED, LED_BLINK);
#endif /* MOTION_DETECTION_LED */
}
#endif

#define MAX_MULTI_IMSI_SEARCH_TIME_S (30 * 60)

union lte_info {
   struct lte_sim_info sim_info;
   struct lte_network_info net_info;
};

static int dtls_network_searching(const k_timeout_t timeout)
{
   union lte_info info;
   bool off = false;
   const int64_t start_time = k_uptime_get();
   int64_t timeout_ms = k_ticks_to_ms_floor64(timeout.ticks);
   long last_not_ready_time = atomic_get(&not_ready_time);
   int trigger = MANUAL_SEARCH;
   int swap_state = 1;

   while (true) {
      int64_t now = k_uptime_get();
      long time = atomic_get(&not_ready_time);
      if (time != last_not_ready_time) {
         last_not_ready_time = time;
         trigger = READY_SEARCH;
      }
      if (time == 0) {
         // not_ready_time unavailable
         time = (long)now - start_time;
      } else {
         time = (long)now - time;
      }
      if (time > timeout_ms) {
         modem_read_network_info(&info.net_info, false);
         if (info.net_info.registered == LTE_NETWORK_STATE_ON) {
            dtls_info("Network found");
            return false;
         } else {
            dtls_info("Network not found (%ld s)", time / MSEC_PER_SEC);
            return true;
         }
      }
      if (trigger != NO_SEARCH) {
         trigger = NO_SEARCH;
         if (off) {
            watchdog_feed();
            modem_set_normal();
            off = false;
         }
         if (trigger != READY_SEARCH) {
            dtls_info("Start network search");
            modem_start_search();
         }
         if (modem_wait_ready(K_SECONDS(60)) == 0) {
            dtls_info("Network found");
            return false;
         }
         ui_led_op(LED_APPLICATION, LED_CLEAR);
         dtls_info("Pause LEDs");
      }

      if (!off) {
         if (modem_get_sim_info(&info.sim_info)) {
            // multi sim with preference => swap
            int timeout_s = info.sim_info.imsi_interval * (1 << swap_state);
            int time_s = (now - start_time) / MSEC_PER_SEC;
            dtls_info("Multi IMSI interval %d s, swap timeout %d, last %d s.",
                      info.sim_info.imsi_interval, timeout_s, time_s);
            if (time_s > timeout_s) {
               dtls_info("Multi IMSI, timeout => swap preferences");
               modem_set_preference(SWAP_PREFERENCE);
               swap_state++;
               trigger = EVENT_SEARCH;
            }
         } else if (info.sim_info.imsi_interval > 0) {
            int timeout_s = info.sim_info.imsi_interval;
            if (timeout_s < MAX_MULTI_IMSI_SEARCH_TIME_S) {
               timeout_s = MAX_MULTI_IMSI_SEARCH_TIME_S;
            }
            dtls_info("Multi IMSI, interval %d s.", info.sim_info.imsi_interval);
            if (((long)now - last_not_ready_time) > (MSEC_PER_SEC * timeout_s)) {
               dtls_info("Multi IMSI, offline");
               watchdog_feed();
               modem_set_offline();
               off = true;
            }
         }
      }
      if (trigger == NO_SEARCH) {
         k_sem_reset(&dtls_trigger_search);
         watchdog_feed();
         if (k_sem_take(&dtls_trigger_search, K_SECONDS(WATCHDOG_TIMEOUT_S)) == 0) {
            trigger = trigger_search;
            trigger_search = NO_SEARCH;
         }
         watchdog_feed();
         if (trigger < EVENT_SEARCH) {
            modem_read_network_info(&info.net_info, false);
            if (info.net_info.registered == LTE_NETWORK_STATE_ON) {
               return false;
            }
         }
      }
   }
   // network not found
   return true;
}

static int dtls_loop(session_t *dst, int flags)
{
   struct pollfd udp_poll;
   dtls_context_t *dtls_context = NULL;
   const char *reopen_cause = NULL;
   int result;
   int loops = 0;
   long time;
   bool send_request = false;
   bool restarting_modem = false;
#ifdef CONFIG_COAP_WAIT_ON_POWERMANAGER
   uint16_t battery_voltage = 0xffff;
#endif

#ifdef CONFIG_LOCATION_ENABLE
   bool location_init = true;
#endif

   if (flags & FLAG_TLS) {
      dtls_info("Start CoAP/DTLS 1.2");
   } else {
      dtls_info("Start CoAP/UDP");
   }
   app_data.destination = dst;
   app_data.fd = -1;

   if (flags & FLAG_TLS) {
      dtls_init();
      dtls_context = dtls_new_context(&app_data);
      if (!dtls_context) {
         dtls_emerg("cannot create dtls context");
         reboot(ERROR_CODE_INIT_NO_DTLS, false);
      }
      dtls_credentials_init_handler(&cb);
      dtls_set_handler(dtls_context, &cb);
      app_data.dtls_pending = true;
   }

   app_data.timeout = COAP_ACK_TIMEOUT;
   app_data.request_state = NONE;

   while (1) {

#ifdef CONFIG_LOCATION_ENABLE
      power_manager_status_t battery_status = POWER_UNKNOWN;
      uint8_t battery_level = 0xff;
      bool force = false;
#ifdef CONFIG_MOTION_DETECTION
      force = moved;
      moved = false;
#endif
      power_manager_status(&battery_level, NULL, &battery_status, NULL);
      if (location_enabled()) {
         if (battery_level < 20 && battery_status == FROM_BATTERY) {
            dtls_info("Low battery, switch off GNSS");
            location_stop();
         } else if (force) {
            dtls_info("Motion detected, force GNSS");
            location_start(force);
         }
      } else if (!app_data.dtls_pending) {
         if ((battery_level > 80 && battery_level < 0xff) ||
             (battery_status != FROM_BATTERY && battery_status != POWER_UNKNOWN)) {
            dtls_info("High battery, switch on GNSS");
            location_start(false);
         } else if (location_init && (battery_level == 0xff || battery_level >= 20)) {
            location_init = false;
            dtls_info("Starting, switch on GNSS");
            location_start(false);
         }
      }
#endif
      watchdog_feed();
      if (!initial_success) {
         if (k_uptime_get() > ((flags & FLAG_REBOOT_1) ? (4 * MSEC_PER_HOUR) : MSEC_PER_DAY)) {
            // no initial_success for 4 hours / 1 day => reboot
            dtls_info("> No initial success, reboot%s", (flags & FLAG_REBOOT_1) ? " 1" : " N");
            reboot(ERROR_CODE_INIT_NO_SUCCESS, true);
         }
      }

      if (!atomic_test_bit(&general_states, LTE_READY) || app_data.fd < 0) {
         if (dtls_network_searching(K_HOURS(4))) {
            ++current_failures;
            dtls_info("no registration, failures %d.", current_failures);
            reopen_cause = "modem not registered, failure.";
         } else {
            reopen_cause = "modem registered";
         }
      }

      if (current_failures > handled_failures) {
         handled_failures = current_failures;
         dtls_info("handle failure %d.", current_failures);
         if (current_failures == 3 && (flags & FLAG_TLS)) {
            // restart dtls
            app_data.dtls_pending = true;
            dtls_trigger();
         } else if (current_failures >= 3) {
            // reboot
            dtls_info("> Too many failures, reboot");
            reboot(ERROR_CODE_TOO_MANY_FAILURES, false);
         } else if (current_failures == 2) {
            // restart modem
            restarting_modem = true;
         }
      }

      if (trigger_duration) {
         restarting_modem = true;
      }

      if (restarting_modem) {
         dtls_info("Trigger restart modem.");
         restarting_modem = false;
         if (restart_modem()) {
            reopen_cause = "restart modem";
            dtls_trigger();
         }
      }

      if (!atomic_test_bit(&general_states, LTE_READY)) {
         dtls_info("Modem not ready.");
         k_sleep(K_MSEC(1000));
         continue;
      }

      if (check_socket(&app_data)) {
         if (!reopen_cause) {
            reopen_cause = "check";
         }
         if (reopen_socket(&app_data, reopen_cause)) {
            if (app_data.request_state == SEND ||
                app_data.request_state == RECEIVE) {
               loops = 0;
               app_data.retransmission = 0;
               app_data.request_state = SEND;

               ui_led_op(LED_APPLICATION, LED_SET);
               if (app_data.dtls_pending) {
                  dtls_info("DTLS hs send again");
                  dtls_check_retransmit(dtls_context, NULL);
               } else {
                  dtls_info("CoAP request send again");
                  sendto_peer(&app_data, dst, dtls_context);
               }
            }
         }
         reopen_cause = NULL;
      }
      udp_poll.fd = app_data.fd;
      udp_poll.events = POLLIN;
      udp_poll.revents = 0;

      if (app_data.request_state != NONE) {
         result = poll(&udp_poll, 1, 1000);
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
            if (!trigger_duration) {
               app_data.request_state = SEND;
               dtls_power_management();
               ui_led_op(LED_APPLICATION, LED_SET);
#if CONFIG_COAP_SEND_INTERVAL > 0
               work_reschedule_for_io_queue(&dtls_timer_trigger_work, K_SECONDS(CONFIG_COAP_SEND_INTERVAL));
#endif
               if (lte_power_off) {
                  dtls_info("modem on");
                  lte_power_off = false;
                  restarting_modem = false;
                  connect_time = (long)k_uptime_get();
                  modem_start(K_SECONDS(CONFIG_MODEM_SEARCH_TIMEOUT), false);
                  reopen_socket(&app_data, "on");
               }
               loops = 0;
               app_data.retransmission = 0;
               if (coap_client_prepare_post(receive_buffer, sizeof(receive_buffer), COAP_SEND_FLAGS) < 0) {
                  dtls_coap_failure(&app_data, "prepare post");
               } else if (app_data.dtls_pending) {
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
         continue;
      }
      if (result < 0) { /* error */
         if (errno != EINTR) {
            dtls_warn("select failed: errno %d (%s)", result, strerror(errno));
         }
      } else if (result == 0) { /* timeout */
         const char *type = app_data.dtls_pending ? "DTLS hs" : "CoAP request";
         ++loops;
         if (app_data.request_state == SEND) {
            if (atomic_test_bit(&general_states, LTE_CONNECTED_SEND)) {
               loops = 0;
               time = (long)(connected_time - connect_time);
               if (time < 0) {
                  time = -1;
               }
               dtls_log_state();
               if (app_data.request_state == SEND) {
                  dtls_info("%ld ms: connected => sent",
                            time);
               } else {
                  dtls_info("%ld ms: connected => resent",
                            time);
               }
               app_data.request_state = RECEIVE;
#ifdef CONFIG_COAP_NO_RESPONSE_ENABLE
               if (!app_data.dtls_pending) {
                  dtls_coap_success(&app_data);
               }
#endif
            } else if (loops > 60) {
               dtls_log_state();
               dtls_info("%s send timeout %d s", type, loops);
               dtls_coap_failure(&app_data, "timeout");
            }
         } else if (app_data.request_state == RECEIVE) {
            int temp = app_data.timeout;
            if (!atomic_test_bit(&general_states, LTE_READY)) {
               if (app_data.retransmission >= COAP_MAX_RETRANSMISSION) {
                  // stop waiting ...
                  temp = loops - 1;
               } else {
                  temp += ADD_ACK_TIMEOUT;
               }
            }
            dtls_log_state();
            if (app_data.retransmission > 0) {
               dtls_info("%s wait %d of %d s, retrans. %d", type, loops, temp, app_data.retransmission);
            } else {
               dtls_info("%s wait %d of %d s", type, loops, temp);
            }
            if (loops > temp) {
               result = -1;
               if (app_data.retransmission < COAP_MAX_RETRANSMISSION) {
                  if (app_data.retransmission == 0) {
                     modem_set_psm(CONFIG_UDP_PSM_RETRANS_RAT);
                  }
                  ++app_data.retransmission;
                  loops = 0;
                  app_data.timeout <<= 1;
                  app_data.request_state = SEND;

                  dtls_info("%s resend, timeout %d s", type, app_data.timeout);
                  if (app_data.dtls_pending) {
                     dtls_check_retransmit(dtls_context, NULL);
                  } else {
                     sendto_peer(&app_data, dst, dtls_context);
                  }
               } else {
                  // maximum retransmissions reached
                  dtls_info("%s receive timeout %d s", type, app_data.timeout);
                  dtls_coap_failure(&app_data, "receive timeout");
               }
            }
         } else if (app_data.request_state == WAIT_RESPONSE) {
            if (loops > 60) {
               dtls_log_state();
               dtls_info("%s response timeout %d s", type, loops);
               dtls_coap_failure(&app_data, "response timeout");
            }
         } else if (app_data.request_state == WAIT_SUSPEND) {
            // wait for late received data
            if (atomic_test_bit(&general_states, LTE_SLEEPING)) {
               // modem enters sleep, no more data
               app_data.request_state = NONE;
               dtls_info("%s suspend after %d s", type, loops);
            } else if (k_sem_count_get(&dtls_trigger_msg)) {
               // send button pressed
               app_data.request_state = NONE;
               dtls_info("%s next trigger after %d s", type, loops);
            }
         } else if (app_data.request_state != NONE) {
            dtls_log_state();
            dtls_info("%s wait state %d, %d s", type, app_data.request_state, loops);
         }
      } else { /* ok */
         if (udp_poll.revents & POLLIN) {
            recvfrom_peer(&app_data, dtls_context);
            if (app_data.request_state == SEND_ACK) {
               int64_t temp_time = connect_time;
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
            if ((app_data.request_state == NONE || app_data.request_state == WAIT_SUSPEND) && !lte_power_on_off) {
               modem_set_rai_mode(RAI_NOW, app_data.fd);
            }
            if (app_data.request_state == NONE &&
                (flags & FLAG_TLS) &&
                !(flags & FLAG_KEEP_CONNECTION) &&
                !app_data.dtls_pending) {
               app_data.dtls_pending = true;
               ui_led_op(LED_DTLS, LED_CLEAR);
            }
         } else if (udp_poll.revents & (POLLERR | POLLNVAL)) {
            if (check_socket(&app_data)) {
               k_sleep(K_MSEC(1000));
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
      dtls_info("DNS lookup: %s", host);
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

int main(void)
{
   int config = 0;
   int protocol = -1;
   int flags = 0;
   char imei[MODEM_ID_SIZE];
   session_t dst;

   memset(&app_data, 0, sizeof(app_data));
   LOG_INF("CoAP/DTLS 1.2 CID sample %s has started", appl_get_version());
   appl_reset_cause(&flags);

   dtls_set_log_level(DTLS_LOG_INFO);

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
         dtls_info("CoAP/UDP");
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

#ifdef CONFIG_MOTION_SENSOR
#ifdef CONFIG_MOTION_DETECTION
   accelerometer_init(accelerometer_handler);
   accelerometer_enable(true);
#else  /* CONFIG_MOTION_DETECTION */
   accelerometer_init(NULL);
//   accelerometer_enable(false);
#endif /* CONFIG_MOTION_DETECTION */
#endif /* CONFIG_MOTION_SENSOR */

#ifdef CONFIG_ENVIRONMENT_SENSOR
   environment_init();
#endif

   if (modem_start(K_SECONDS(CONFIG_MODEM_SEARCH_TIMEOUT), true) != 0) {
      appl_ready = true;
      if (dtls_network_searching(K_HOURS(8))) {
         reboot(ERROR_CODE_INIT_NO_LTE, false);
      }
   }
   appl_ready = true;

   modem_get_imei(imei, sizeof(imei));
   dtls_credentials_init_psk(imei);
   coap_client_init(dtls_credentials_get_psk_identity());

   memset(&dst, 0, sizeof(session_t));
   init_destination(protocol, &dst);

   dtls_trigger();
   dtls_loop(&dst, flags);
   return 0;
}

int main_(void)
{
   modem_power_off();
   power_manager_init();
   power_manager_suspend(true);
   // power_manager_3v3(false);
   // power_manager_1v8(false);
   k_sleep(K_MSEC(1000));
   NRF_REGULATORS->SYSTEMOFF = 1;
   return 0;
}
