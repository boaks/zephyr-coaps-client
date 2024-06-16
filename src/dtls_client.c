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
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/socket.h>

#include "appl_diagnose.h"
#include "appl_settings.h"
#include "appl_storage.h"
#include "appl_storage_config.h"
#include "appl_time.h"
#ifdef CONFIG_UPDATE
#include "appl_update.h"
#endif
#ifdef CONFIG_COAP_UPDATE
#include "appl_update_coap.h"
#endif
#ifdef CONFIG_DTLS_ECDSA_AUTO_PROVISIONING
#include "coap_prov_client.h"
#endif
#include "coap_appl_client.h"
#include "coap_client.h"
#include "dtls.h"
#include "dtls_client.h"
#include "dtls_debug.h"
#include "global.h"
#include "io_job_queue.h"
#include "modem.h"
#include "modem_at.h"
#include "modem_sim.h"
#include "parse.h"
#include "power_manager.h"
#include "sh_cmd.h"
#include "ui.h"

#ifdef CONFIG_LOCATION_ENABLE
#include "location.h"
#endif

#ifdef CONFIG_MOTION_SENSOR
#include "accelerometer_sensor.h"
#endif

#ifdef CONFIG_NAU7802_SCALE
#include "nau7802.h"
#endif

#include "environment_sensor.h"

#define COAP_ACK_TIMEOUT 3
#define ADD_ACK_TIMEOUT 3

#define LED_APPLICATION LED_LTE_1
#define LED_DTLS LED_NONE

#define MSEC_PER_MINUTE (MSEC_PER_SEC * 60)
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
   session_t destination;
   int fd;
   bool dtls_pending;
   bool dtls_next_flight;
   bool no_response;
   bool no_rai;
   bool download;
   bool provisioning;
   uint16_t download_progress;
   uint8_t retransmission;
   uint8_t dtls_flight;
   request_state_t request_state;
   uint16_t timeout;
   int64_t start_time;
   char host[MAX_SETTINGS_VALUE_LENGTH];
   const char *dtls_cipher_suite;
   bool dtls_cid;
} dtls_app_data_t;

#define LTE_REGISTERED 0
#define LTE_READY 1
#define LTE_CONNECTED 2
#define LTE_SLEEPING 3
#define LTE_READY_1S 4
#define LTE_LOW_VOLTAGE 5
#define LTE_CONNECTED_SEND 6
#define LTE_SOCKET_ERROR 7
#define PM_PREVENT_SUSPEND 8
#define PM_SUSPENDED 9
#define APN_RATE_LIMIT 10
#define APN_RATE_LIMIT_RESTART 11
#define SETUP_MODE 12

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

#define MAX_APPL_BUF 1600
static uint8_t appl_buffer[MAX_APPL_BUF];
static volatile size_t appl_buffer_len = MAX_APPL_BUF;

#define RTT_SLOTS 9
#define RTT_INTERVAL (2 * MSEC_PER_SEC)
// last item for maximum rtt
static unsigned int rtts[RTT_SLOTS + 2] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

unsigned int transmissions[COAP_MAX_RETRANSMISSION + 1];
unsigned int connect_time_ms;
unsigned int coap_rtt_ms;
unsigned int retransmissions;
unsigned int failures = 0;
unsigned int sockets = 0;
unsigned int dtls_handshakes = 0;

volatile uint32_t send_interval = CONFIG_COAP_SEND_INTERVAL;
volatile uint32_t coap_timeout = COAP_ACK_TIMEOUT;

static K_SEM_DEFINE(dtls_trigger_msg, 0, 1);
static K_SEM_DEFINE(dtls_trigger_search, 0, 1);

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
   } else if (atomic_test_bit(&general_states, LTE_LOW_VOLTAGE)) {
      index = snprintf(buf, sizeof(buf), "low voltage");
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

static void dtls_log_now(void)
{
   int64_t now;
   char buf[64];
   appl_get_now(&now);
   if (appl_format_time(now, buf, sizeof(buf))) {
      dtls_info("%s", buf);
   }
}

static void dtls_power_management(void)
{
   bool previous = false;
   bool suspend = false;

   if (!appl_reboots()) {
      suspend = atomic_test_bit(&general_states, LTE_SLEEPING) &&
                !atomic_test_bit(&general_states, PM_PREVENT_SUSPEND) &&
                !atomic_test_bit(&general_states, SETUP_MODE) &&
                app_data.request_state == NONE;
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

static int dtls_low_voltage(const k_timeout_t timeout)
{
   const int64_t start_time = k_uptime_get();
   int64_t timeout_ms = k_ticks_to_ms_floor64(timeout.ticks);

   while (!trigger_duration) {
      uint16_t battery_voltage = 0xffff;
      power_manager_status_t battery_status = POWER_UNKNOWN;

      if (!power_manager_status(NULL, &battery_voltage, &battery_status, NULL)) {
         if (battery_voltage > 3300 || battery_status >= CHARGING_I) {
            atomic_clear_bit(&general_states, LTE_LOW_VOLTAGE);
            modem_set_normal();
            return false;
         }
         dtls_info("waiting, low voltage %d mV.", battery_voltage);
      }
      if (k_uptime_get() - start_time > timeout_ms) {
         break;
      }
      k_sem_reset(&dtls_trigger_search);
      watchdog_feed();
      k_sem_take(&dtls_trigger_search, K_SECONDS(WATCHDOG_TIMEOUT_S));
   }
   watchdog_feed();
   return true;
}

static void restart(int error, bool factoryReset)
{
   int res;

   // write error code, reboot in 120s
   appl_reboot(error, K_SECONDS(120));

   atomic_set_bit(&general_states, PM_PREVENT_SUSPEND);
   dtls_power_management();
   ui_led_op(LED_COLOR_RED, LED_BLINKING);

   res = modem_at_lock_no_warn(K_MSEC(2000));
   if (!res) {
      modem_sim_reset(false);
      modem_power_off();
      if (factoryReset) {
         modem_factory_reset();
      }
      dtls_info("> modem switched off-");
      modem_at_unlock();
   } else {
      dtls_info("> modem busy, not switched off.");
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

static void check_restart(void)
{
   if (trigger_duration) {
      // Thingy:91 and nRF9160 feather will restart
      // nRF9160-DK restart with button2 also pressed
      int ui = ui_config();
      if (ui < 0) {
         dtls_info("> modem restart / factory reset");
         restart(ERROR_CODE_MANUAL_TRIGGERED, true);
      } else if (ui & 2) {
         dtls_info("> modem restart");
         restart(ERROR_CODE_MANUAL_TRIGGERED, false);
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

static bool restart_modem(bool power_off)
{
   watchdog_feed();
   check_restart();

   dtls_info("> modem restart");
   atomic_set_bit(&general_states, PM_PREVENT_SUSPEND);
   dtls_power_management();
   ui_led_op(LED_COLOR_BLUE, LED_BLINKING);
   ui_led_op(LED_COLOR_RED, LED_BLINKING);
   if (power_off) {
      modem_power_off();
   } else {
      modem_set_lte_offline();
   }
   dtls_info("> modem offline");
   ui_led_op(LED_COLOR_ALL, LED_CLEAR);
   k_sleep(K_MSEC(2000));
   if (dtls_low_voltage(K_HOURS(24))) {
      restart(ERROR_CODE_LOW_VOLTAGE, false);
   }
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
      modem_set_rai_mode(RAI_MODE_OFF, app->fd);
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
      dtls_warn("> %s, reopen UDP socket failed, %d, errno %d (%s), restart",
                loc, app->fd, errno, strerror(errno));
      restart(ERROR_CODE(ERROR_CODE_OPEN_SOCKET, errno), false);
   }
   ++sockets;
   modem_set_psm(CONFIG_UDP_PSM_CONNECT_RAT);
#ifdef CONFIG_UDP_USE_CONNECT
   // using SO_RAI_NO_DATA requires a destination, for what ever
   connect(app->fd, (struct sockaddr *)&app->destination.addr.sin, sizeof(struct sockaddr_in));
#endif
   modem_set_rai_mode(RAI_MODE_OFF, app->fd);
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
   return app_data.download || (state == NONE) || (state == WAIT_SUSPEND);
}

static void dtls_trigger(void)
{
   if (appl_reboots()) {
      return;
   }
   if (trigger_duration) {
      modem_interrupt_wait();
   }
   if (dtls_no_pending_request()) {
      // read battery status before modem wakes up
      power_manager_status(NULL, NULL, NULL, NULL);
      k_sem_give(&dtls_trigger_msg);
   }
}

static bool dtls_trigger_pending(void)
{
   return k_sem_count_get(&dtls_trigger_msg) ? true : false;
}

static void dtls_manual_trigger(int duration)
{
   if (atomic_test_bit(&general_states, SETUP_MODE)) {
      return;
   }

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
      k_sem_give(&dtls_trigger_search);
   }
}

void dtls_cmd_trigger(bool led, int mode, const uint8_t *data, size_t len)
{
   bool ready = atomic_test_bit(&general_states, LTE_READY);
   if (mode & 1) {
      if (dtls_no_pending_request()) {
         if (data && len) {
            if (len > sizeof(appl_buffer)) {
               len = sizeof(appl_buffer);
            }
            memmove(appl_buffer, data, len);
            appl_buffer_len = len;
         }
         ui_enable(led);
         dtls_trigger();
         if (!ready && !(mode & 2)) {
            dtls_info("No network ...");
         }
      } else if (ready) {
         dtls_info("Busy, request pending ... (state %d)", app_data.request_state);
      } else {
         dtls_info("Busy, searching network");
      }
   }
   if (!ready && mode & 2) {
      ui_enable(led);
      trigger_search = CMD_SEARCH;
      k_sem_give(&dtls_trigger_search);
   }
}

static void dtls_timer_trigger_fn(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(dtls_timer_trigger_work, dtls_timer_trigger_fn);

static void dtls_timer_trigger_fn(struct k_work *work)
{
   if (dtls_no_pending_request()) {
      // no LEDs for time trigger
      ui_enable(false);
      dtls_trigger();
   } else {
      dtls_debug("Busy, schedule again in %d s.", send_interval);
      work_schedule_for_io_queue(&dtls_timer_trigger_work, K_SECONDS(send_interval));
   }
}

static int dtls_coap_inc_failures(void)
{
   if (current_failures == handled_failures) {
      ++current_failures;
   }
   return current_failures;
}

static int dtls_coap_next_failures(void)
{
   if (current_failures == handled_failures) {
      return -1;
   }
   handled_failures = current_failures;
   return current_failures;
}

static void dtls_coap_clear_failures(void)
{
   current_failures = 0;
   handled_failures = 0;
}

static void dtls_coap_next(dtls_app_data_t *app, uint32_t interval, bool reset_trigger)
{
   ui_led_op(LED_APPLICATION, LED_CLEAR);
   if (lte_power_on_off) {
      dtls_debug("> modem switching off ...");
      lte_power_off = true;
      modem_power_off();
      dtls_debug("modem off");
   }
   if (interval > 0) {
      if (interval != send_interval) {
         work_reschedule_for_io_queue(&dtls_timer_trigger_work, K_SECONDS(interval));
         dtls_debug("Next request, schedule in %d s.", interval);
      } else {
         if (work_schedule_for_io_queue(&dtls_timer_trigger_work, K_SECONDS(interval)) == 1) {
            dtls_debug("Next request, schedule in %d s.", interval);
         }
      }
   }

   dtls_log_now();
   app->request_state = WAIT_SUSPEND;
   memset(appl_buffer, 0, sizeof(appl_buffer));
   appl_buffer_len = MAX_APPL_BUF;
   if (reset_trigger) {
      k_sem_reset(&dtls_trigger_msg);
   }

#ifdef CONFIG_COAP_UPDATE
   if (!app->download) {
      appl_update_coap_reboot();
   }
#endif
}

static void dtls_coap_success(dtls_app_data_t *app)
{
   uint32_t interval = send_interval;
   int index = 0;
   int time1 = (int)(atomic_get(&connected_time) - connect_time);
   int time2 = (int)(response_time - connect_time);
   bool reset_trigger = true;

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
   if (time2 >= 0) {
      if (time1 > 0) {
         connect_time_ms = time1;
         coap_rtt_ms = time2 - time1;
      } else {
         connect_time_ms = 0;
         coap_rtt_ms = time2;
      }
      retransmissions = app->retransmission;
      if (retransmissions == 0 && coap_rtt_ms < 4000) {
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
   } else {
      connect_time_ms = 0;
      coap_rtt_ms = 0;
   }
   if (time1 < 2000) {
      unsigned long sum = 0;
      unsigned int num = 0;
      unsigned int rtt = 0;
      dtls_info("retrans: 0*%u, 1*%u, 2*%u, 3*%u, failures %u", transmissions[0], transmissions[1], transmissions[2], transmissions[3], failures);
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
   dtls_coap_clear_failures();
   if (!initial_success) {
      initial_success = true;
#ifdef CONFIG_UPDATE
      appl_update_image_verified();
#endif
   }
   if (app->provisioning) {
      if (!appl_settings_is_provisioning()) {
         app->provisioning = false;
         // new handshake with provisioned credentials
         app->dtls_pending = true;
         if (interval) {
            interval = 5;
         }
      }
   }
#ifdef CONFIG_COAP_UPDATE
   else if (app->download) {
      if (!appl_update_coap_pending()) {
         dtls_trigger();
         reset_trigger = false;
      }
   }
#endif
   atomic_clear_bit(&general_states, APN_RATE_LIMIT);
   atomic_clear_bit(&general_states, APN_RATE_LIMIT_RESTART);
   if (interval) {
      dtls_debug("Success, schedule in %d s.", interval);
   }
   dtls_coap_next(app, interval, reset_trigger);
}

static void dtls_coap_failure(dtls_app_data_t *app, const char *cause)
{
   uint32_t interval = send_interval;
   int time1 = (int)(atomic_get(&connected_time) - connect_time);
   int time2 = (int)(response_time - connect_time);
   bool reset_trigger = true;

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
   failures++;
   if (initial_success) {
      int f = dtls_coap_inc_failures();
      dtls_info("current failures %d.", f);
   }
#ifdef CONFIG_COAP_UPDATE
   if (app->download) {
      appl_update_coap_cancel();
      dtls_trigger();
      reset_trigger = false;
   }
#endif

   if (interval) {
#if CONFIG_COAP_FAILURE_SEND_INTERVAL > 0
      interval = CONFIG_COAP_FAILURE_SEND_INTERVAL;
#endif /*CONFIG_COAP_FAILURE_SEND_INTERVAL*/
      dtls_debug("Failure, schedule in %d s.", interval);
   }
   dtls_coap_next(app, interval, reset_trigger);
}

static uint32_t
network_timeout_scale(uint32_t timeout)
{
   int factor = modem_get_time_scale();
   if (factor > 100) {
      return (timeout * factor) / 100;
   } else {
      return timeout;
   }
}

static uint32_t
network_additional_timeout(void)
{
   struct lte_lc_edrx_cfg edrx;

   if (!atomic_test_bit(&general_states, LTE_CONNECTED) &&
       modem_get_edrx_status(&edrx) >= 0 && edrx.mode != LTE_LC_LTE_MODE_NONE) {
      return (uint32_t)ceil(edrx.edrx);
   } else {
      return ADD_ACK_TIMEOUT;
   }
}

static int
read_from_peer(dtls_app_data_t *app, session_t *session, uint8 *data, size_t len)
{
   (void)session;
   int err = PARSE_NONE;

   if (app->provisioning) {
#ifdef CONFIG_DTLS_ECDSA_AUTO_PROVISIONING
      err = coap_prov_client_parse_data(data, len);
#endif
   } else {
#ifdef CONFIG_COAP_UPDATE
      if (app->download) {
         err = appl_update_coap_parse_data(data, len);
      }
#endif
      if (err == PARSE_NONE) {
         err = coap_appl_client_parse_data(data, len);
      }
   }
   if (err < 0) {
      return err;
   }

   switch (err) {
      case PARSE_NONE:
         break;
      case PARSE_IGN:
         break;
      case PARSE_RST:
         if (NONE != app->request_state && WAIT_SUSPEND != app->request_state) {
            response_time = (long)k_uptime_get();
            dtls_coap_failure(app, "rst");
         }
         break;
      case PARSE_ACK:
         if (NONE != app->request_state && app->request_state < WAIT_RESPONSE) {
            app->request_state = WAIT_RESPONSE;
         }
         break;
      case PARSE_RESPONSE:
         if (NONE != app->request_state && WAIT_SUSPEND != app->request_state) {
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
   if (!app->dtls_pending && !app->no_rai && !lte_power_on_off) {
      modem_set_rai_mode(app->no_response ? RAI_MODE_LAST : RAI_MODE_ONE_RESPONSE, app->fd);
   } else {
      modem_set_rai_mode(RAI_MODE_OFF, app->fd);
   }
}

static int
send_to_peer(dtls_app_data_t *app, session_t *session, const uint8_t *data, size_t len)
{
   bool first = !app->dtls_pending || app->dtls_next_flight;
   bool connected;
   int result = 0;
   const char *tag = app->dtls_pending ? (app->retransmission ? "hs_re" : "hs_") : (app->retransmission ? "re" : "");

   if (!lte_power_on_off) {
      if (first && app->retransmission == 0) {
         connect_time = (long)k_uptime_get();
      }
      prepare_socket(app);
   }
   result = sendto(app->fd, data, len, MSG_DONTWAIT, &session->addr.sa, session->size);
   if (result < 0) {
      dtls_warn("%ssend_to_peer failed: %d, errno %d (%s)", tag, result, errno, strerror(errno));
      if (EAGAIN == errno || ECANCELED == errno) {
         uint32_t time = 0;
         int err = modem_read_rate_limit_time(&time);
         if (err > 0) {
            dtls_warn("%ssend_to_peer failed: rate limit, %u s", tag, time);
         }
         atomic_set_bit(&general_states, APN_RATE_LIMIT);
         result = 0;
      }
      return result;
   }
   connected = atomic_test_bit(&general_states, LTE_CONNECTED);
   if (connected) {
      modem_set_transmission_time();
   }

#ifndef NDEBUG
   /* logging */
   if (SEND == app->request_state || app->dtls_pending) {
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

   if (app->dtls_next_flight) {
      // 1. messages in flight
      app->dtls_next_flight = false;
      dtls_info("hs_flight %d", app->dtls_flight);
      app->dtls_flight += 2;
   }
   if (first && app->retransmission == 0) {
      app->timeout = network_timeout_scale(coap_timeout);
      dtls_info("%sresponse timeout %d s", tag, app->timeout);
   }
   return result;
}

static int
dtls_send_to_peer(dtls_context_t *ctx,
                  session_t *session, uint8 *data, size_t len)
{
   dtls_app_data_t *app = dtls_get_app_data(ctx);
   int result = send_to_peer(app, session, data, len);
   if (app->dtls_pending && result < 0) {
      /* don't forward send errors,
         the dtls state machine will suffer */
      result = len;
   }
   return result;
}

struct cipher_entry {
   const char *name;
   const dtls_cipher_t cipher;
};

#define CIPHER_ENTRY(X)       \
   {                          \
      .name = #X, .cipher = X \
   }

static const struct cipher_entry ciphers_map[] = {
#ifdef DTLS_PSK
    CIPHER_ENTRY(TLS_PSK_WITH_AES_128_CCM),
    CIPHER_ENTRY(TLS_PSK_WITH_AES_128_CCM_8),
#endif /* DTLS_PSK */
#ifdef DTLS_ECC
    CIPHER_ENTRY(TLS_ECDHE_ECDSA_WITH_AES_128_CCM),
    CIPHER_ENTRY(TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8),
#endif /* DTLS_ECC */
    {.name = NULL, .cipher = TLS_NULL_WITH_NULL_NULL}};

static int
dtls_handle_event(dtls_context_t *ctx, session_t *session,
                  dtls_alert_level_t level, unsigned short code)
{
   dtls_peer_t *peer = NULL;
   dtls_app_data_t *app = NULL;

   if (appl_reboots()) {
      return 0;
   }
   app = dtls_get_app_data(ctx);

   if (DTLS_EVENT_CONNECTED == code) {
      dtls_info("dtls connected.");
      app->dtls_pending = false;
      app->dtls_next_flight = false;
      app->dtls_flight = 0;
      app->request_state = NONE;
      peer = dtls_get_peer(ctx, session);
      if (peer) {
         const dtls_security_parameters_t *security_params = peer->security_params[0];
         const dtls_cipher_t cipher = dtls_get_cipher_suite(security_params->cipher_index);
         const struct cipher_entry *cur = ciphers_map;
         app_data.dtls_cid = security_params->write_cid_length > 0;
         app_data.dtls_cipher_suite = "none";
         while (cur && cur->cipher != TLS_NULL_WITH_NULL_NULL) {
            if (cur->cipher == cipher) {
               app_data.dtls_cipher_suite = cur->name;
               break;
            }
            ++cur;
         }
      }
      ui_led_op(LED_COLOR_RED, LED_CLEAR);
      ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
      ui_led_op(LED_DTLS, LED_SET);
   } else if (DTLS_EVENT_CONNECT == code) {
      dtls_info("dtls connect ...");
      app_data.dtls_pending = true;
      app_data.dtls_cipher_suite = NULL;
      app_data.dtls_cid = false;
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
   dtls_info("recvfrom_peer ...");
   result = recvfrom(app->fd, appl_buffer, MAX_APPL_BUF, 0,
                     &session.addr.sa, &session.size);
   if (result < 0) {
      dtls_warn("recv_from_peer failed: errno %d (%s)", result, strerror(errno));
      return result;
   } else {
      dtls_dsrv_log_addr(DTLS_LOG_DEBUG, "peer", &session);
      dtls_debug_dump("bytes from peer", appl_buffer, result);
      modem_set_transmission_time();
   }
   dtls_info("received_from_peer %d bytes", result);
   if (ctx) {
      if (app->dtls_pending) {
         app->dtls_next_flight = true;
      }
      result = dtls_handle_message(ctx, &session, appl_buffer, result);
      if (app->dtls_pending) {
         app->request_state = RECEIVE;
      }
      return result;
   } else {
      return read_from_peer(app, &session, appl_buffer, result);
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

   if (app->request_state == SEND_ACK) {
      coap_message_len = coap_client_message(&coap_message_buf);
   } else {
      if (app->provisioning) {
#ifdef CONFIG_DTLS_ECDSA_AUTO_PROVISIONING
         coap_message_len = coap_prov_client_message(&coap_message_buf);
#endif
      } else {
#ifdef CONFIG_COAP_UPDATE
         if (app->download) {
            coap_message_len = appl_update_coap_message(&coap_message_buf);
         }
#endif
         if (!coap_message_len) {
            coap_message_len = coap_appl_client_message(&coap_message_buf);
         }
      }
   }
   if (!coap_message_len) {
      dtls_warn("send empty UDP message.");
   } else {
      dtls_info("send %d bytes.", coap_message_len);
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
         if (!app->dtls_pending && app->no_response) {
            dtls_coap_success(app);
         }
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
   app->dtls_next_flight = true;
   app->dtls_flight = 1;
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
      case LTE_STATE_READY_1S:
         desc = "ready 1s";
         bit = LTE_READY_1S;
         break;
      case LTE_STATE_SLEEPING:
         desc = "sleeping";
         bit = LTE_SLEEPING;
         break;
      case LTE_STATE_LOW_VOLTAGE:
         desc = "low voltage";
         bit = LTE_LOW_VOLTAGE;
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
         atomic_and(&general_states, ~(BIT(LTE_READY) | BIT(LTE_READY_1S) | BIT(LTE_CONNECTED)));
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
            trigger_search = READY_SEARCH;
            k_sem_give(&dtls_trigger_search);
         } else {
            atomic_set(&not_ready_time, now);
            atomic_and(&general_states, ~(BIT(LTE_READY_1S) | BIT(LTE_CONNECTED)));
         }
      }
   } else if (type == LTE_STATE_READY_1S) {
      if (active) {
         modem_sim_ready();
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
   } else if (type == LTE_STATE_LOW_VOLTAGE) {
      if (active) {
         work_schedule_for_io_queue(&dtls_power_management_suspend_work, K_NO_WAIT);
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

#ifdef CONFIG_ADC_SCALE

static bool dtls_setup_mode(void)
{
   bool restart = false;
   bool request = false;
   int select_mode = 1;
   int trigger = 0;

   atomic_set_bit(&general_states, SETUP_MODE);
   dtls_power_management();
   k_sleep(K_MSEC(500));
   ui_led_op(LED_COLOR_ALL, LED_CLEAR);
   while (select_mode < 10) {
      if (select_mode & 1) {
         ui_led_op(LED_COLOR_GREEN, LED_SET);
      } else {
         ui_led_op(LED_COLOR_BLUE, LED_SET);
      }
      dtls_info("Select mode.");
      trigger = ui_input(K_MSEC(7000));
      ui_led_op(LED_COLOR_ALL, LED_CLEAR);
      if (trigger >= 0) {
         break;
      }
      ++select_mode;
      if (select_mode >= 10) {
         break;
      }
      k_sleep(K_MSEC(300));
   }
   if (trigger == 1) {
      // cancel setup
      dtls_info("Cancel.");
   } else if (select_mode & 1) {
      // calibrate setup
      request = scale_calibrate_setup();
   } else {
      // modem reset
      dtls_info("Reset modem.");
      restart = true;
   }
   k_sem_reset(&dtls_trigger_msg);
   if (request) {
      dtls_trigger();
   }
   atomic_clear_bit(&general_states, SETUP_MODE);

   return restart;
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

   while (!trigger_duration) {
      int64_t now = k_uptime_get();
      long time = atomic_get(&not_ready_time);

      if (time != last_not_ready_time) {
         last_not_ready_time = time;
         trigger = READY_SEARCH;
         dtls_info("Network search, not longer ready.");
      }
      if (time) {
         time = (long)now - time;
      } else {
         // not_ready_time unavailable
         time = (long)now - start_time;
      }
      if (time > timeout_ms) {
         if (atomic_test_bit(&general_states, LTE_LOW_VOLTAGE)) {
            if (dtls_low_voltage(K_NO_WAIT)) {
               restart(ERROR_CODE_LOW_VOLTAGE, false);
            }
         }
         modem_read_network_info(&info.net_info, false);
         if (info.net_info.registered == LTE_NETWORK_STATE_ON) {
            dtls_info("Network found");
            atomic_set(&not_ready_time, 0);
            return false;
         } else {
            dtls_info("Network not found (%ld s)", time / MSEC_PER_SEC);
            atomic_set(&not_ready_time, (long)now);
            return true;
         }
      } else {
         dtls_info("Network searching since %ld minutes, up to %ld minutes",
                   time / MSEC_PER_MINUTE, (long)(timeout_ms / MSEC_PER_MINUTE));
      }
      if (atomic_test_bit(&general_states, LTE_LOW_VOLTAGE)) {
         if (dtls_low_voltage(timeout)) {
            restart(ERROR_CODE_LOW_VOLTAGE, false);
         }
      }
      if (trigger != NO_SEARCH) {
         trigger = NO_SEARCH;
         if (off) {
            modem_set_normal();
            off = false;
         }
         if (trigger != READY_SEARCH) {
            dtls_info("Start network search");
            modem_start_search();
         }
         if (modem_wait_ready(K_SECONDS(CONFIG_MODEM_SEARCH_TIMEOUT)) == 0) {
            dtls_info("Network found");
            return false;
         }
         ui_led_op(LED_APPLICATION, LED_CLEAR);
         dtls_info("Pause LEDs");
      }

      if (modem_sim_get_info(&info.sim_info)) {
         // automatic switching multi sim
         int timeout_s = info.sim_info.imsi_interval;
         if (timeout_s > 0) {
            if (modem_uses_preference()) {
               // multi sim "auto select" with preference => swap
               timeout_s *= (1 << swap_state);
               int time_s = (now - start_time) / MSEC_PER_SEC;
               dtls_info("Multi IMSI interval %d s, swap timeout %d, last %d s.",
                         info.sim_info.imsi_interval, timeout_s, time_s);
               if (time_s > timeout_s) {
                  dtls_info("Multi IMSI, timeout => swap preferences");
                  modem_set_preference(SWAP_PREFERENCE);
                  swap_state++;
                  trigger = EVENT_SEARCH;
               }
            } else {
               // switching offline
               // prevent modem from restarting the network search
               // on frequent imsi changes
               int timeout_s = info.sim_info.imsi_interval;
               if (timeout_s < MAX_MULTI_IMSI_SEARCH_TIME_S) {
                  timeout_s = MAX_MULTI_IMSI_SEARCH_TIME_S;
               }
               dtls_info("Multi IMSI, interval %d s.", info.sim_info.imsi_interval);
               if (((long)now - last_not_ready_time) > (MSEC_PER_SEC * timeout_s)) {
                  dtls_info("Multi IMSI, offline");
                  modem_set_offline();
                  off = true;
               }
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
         if (EVENT_SEARCH < trigger) {
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

static int dtls_loop(int flags)
{
   struct pollfd udp_poll;
   dtls_context_t *dtls_context = NULL;
   const char *reopen_cause = NULL;
   int result;
   int loops = 0;
   int coap_send_flags;
   long time;
   bool send_request = false;
   bool restarting_modem = false;
   bool restarting_modem_power_off = false;
   bool network_not_found = false;
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
   app_data.fd = -1;
#ifdef CONFIG_COAP_NO_RESPONSE_ENABLE
   app_data.no_response = true;
#endif
   if (flags & FLAG_TLS) {
      dtls_context = dtls_new_context(&app_data);
      if (!dtls_context) {
         dtls_emerg("cannot create dtls context");
         restart(ERROR_CODE_INIT_NO_DTLS, false);
      }
      dtls_set_handler(dtls_context, &cb);
      app_data.dtls_pending = true;
      ++dtls_handshakes;
   }

   app_data.timeout = coap_timeout;
   app_data.request_state = NONE;

   while (1) {
      int f = -1;
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
            restart(ERROR_CODE_INIT_NO_SUCCESS, true);
         }
      }

      network_not_found = false;
      if (!atomic_test_bit(&general_states, LTE_READY) || app_data.fd < 0) {
         if (dtls_network_searching(K_MINUTES(CONFIG_MODEM_SEARCH_TIMEOUT_RESTART))) {
            network_not_found = true;
            f = dtls_coap_inc_failures();
            dtls_info("no registration, failures %d.", f);
            reopen_cause = "modem not registered, failure.";
         } else {
            reopen_cause = "modem registered";
         }
      }

      if (atomic_test_bit(&general_states, APN_RATE_LIMIT)) {
         if (atomic_test_and_set_bit(&general_states, APN_RATE_LIMIT_RESTART)) {
            uint32_t time = 0;
            int err = modem_read_rate_limit_time(&time);
            if (err < 0) {
               dtls_info("Modem read rate limit failed, %d", err);
            } else {
               if (!time) {
                  atomic_clear_bit(&general_states, APN_RATE_LIMIT);
               }
               if (time > 60) {
                  time = 60;
               } else if (time < 10) {
                  time = 10;
               }
               dtls_info("Modem rate limit exceeded, wait %u s.", time);
               k_sem_reset(&dtls_trigger_msg);
               if (k_sem_take(&dtls_trigger_msg, K_SECONDS(time)) == 0) {
                  k_sem_give(&dtls_trigger_msg);
               } else {
                  continue;
               }
            }
         } else {
            atomic_clear_bit(&general_states, APN_RATE_LIMIT);
            restarting_modem = true;
            reopen_cause = "rate limit";
         }
      }
      f = dtls_coap_next_failures();
      if (f > 0) {
         int strategy = coap_appl_client_retry_strategy(f, flags & FLAG_TLS);
         if (strategy) {
            if (strategy & DTLS_CLIENT_RETRY_STRATEGY_RESTARTS) {
               dtls_info("Too many failures, reboot");
               restart(ERROR_CODE_TOO_MANY_FAILURES, false);
            }
            if (strategy & DTLS_CLIENT_RETRY_STRATEGY_DTLS_HANDSHAKE) {
               dtls_info("handle failure %d. new DTLS handshake.", f);
               app_data.dtls_pending = true;
               ++dtls_handshakes;
               dtls_trigger();
            }
            if (strategy & DTLS_CLIENT_RETRY_STRATEGY_OFF) {
               dtls_info("handle failure %d. switch modem off.", f);
               restarting_modem_power_off = true;
               restarting_modem = true;
            } else if (strategy & DTLS_CLIENT_RETRY_STRATEGY_OFFLINE) {
               dtls_info("handle failure %d. switch modem offline.", f);
               restarting_modem_power_off = false;
               restarting_modem = true;
            }
         } else {
            dtls_info("handle failure %d. new message", f);
         }
         if (network_not_found) {
            restarting_modem_power_off = true;
            restarting_modem = true;
            network_not_found = false;
         }
      }
#ifdef CONFIG_ADC_SCALE
      if (trigger_duration) {
         trigger_duration = dtls_setup_mode();
      }
#endif
      if (trigger_duration) {
         restarting_modem = true;
      }

      if (restarting_modem) {
         dtls_info("Trigger restart modem %s.", restarting_modem_power_off ? "power off" : "offline");
         restarting_modem = false;
         if (restart_modem(restarting_modem_power_off)) {
            if (!reopen_cause) {
               reopen_cause = "restart modem";
            }
            dtls_trigger();
         }
         restarting_modem_power_off = false;
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
                  sendto_peer(&app_data, &app_data.destination, dtls_context);
               }
            }
         }
         reopen_cause = NULL;
      }
      udp_poll.fd = app_data.fd;
      udp_poll.events = POLLIN;
      udp_poll.revents = 0;

#ifdef CONFIG_COAP_UPDATE
      if (app_data.request_state == WAIT_SUSPEND ||
          app_data.request_state == NONE) {

         app_data.download = false;
         app_data.no_rai = false;
         if (appl_update_coap_pending()) {
            if (!appl_update_coap_pending_next() &&
                !dtls_trigger_pending()) {
               dtls_info("wait for download ...");
               loops = 0;
               while (appl_update_coap_pending() &&
                      !appl_update_coap_pending_next() &&
                      !dtls_trigger_pending()) {
                  k_sleep(K_MSEC(1000));
                  ++loops;
                  if (loops > 30) {
                     dtls_info("wait for download timeout!");
                     appl_update_coap_cancel();
                     break;
                  }
               }
            }
            if (appl_update_coap_pending()) {
               app_data.no_rai = true;
               if (!dtls_trigger_pending()) {
                  app_data.download = app_data.download_progress % 32;
                  ++app_data.download_progress;
               }
               if (app_data.download) {
                  loops = 0;
                  app_data.request_state = SEND;
                  app_data.retransmission = 0;
                  appl_update_coap_next();
                  if (app_data.dtls_pending) {
                     dtls_peer_t *peer = dtls_get_peer(dtls_context, &app_data.destination);
                     if (peer) {
                        dtls_reset_peer(dtls_context, peer);
                     }
                     ui_led_op(LED_COLOR_GREEN, LED_SET);
                     dtls_start_connect(dtls_context, &app_data.destination);
                     send_request = true;
                  } else {
                     dtls_info("next download request");
                     sendto_peer(&app_data, &app_data.destination, dtls_context);
                  }
                  continue;
               } else {
                  dtls_trigger();
               }
            } else {
               dtls_info("download canceled");
            }
         }
      }
#endif

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
               int res = 0;
               app_data.request_state = SEND;
               dtls_power_management();
               ui_led_op(LED_APPLICATION, LED_SET);
               if (send_interval > 0) {
                  work_reschedule_for_io_queue(&dtls_timer_trigger_work, K_SECONDS(send_interval));
               }
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
               coap_send_flags = COAP_SEND_FLAGS | (app_data.no_response ? COAP_SEND_FLAG_NO_RESPONSE : 0);
#ifdef CONFIG_DTLS_ECDSA_AUTO_PROVISIONING
               if (appl_settings_is_provisioning()) {
                  app_data.provisioning = true;
                  res = coap_prov_client_prepare_post(appl_buffer, appl_buffer_len);
               } else
#endif
                  res = coap_appl_client_prepare_post(appl_buffer, appl_buffer_len, coap_send_flags);

               if (res < 0) {
                  dtls_coap_failure(&app_data, "prepare post");
               } else if (app_data.dtls_pending) {
                  dtls_peer_t *peer = dtls_get_peer(dtls_context, &app_data.destination);
                  if (peer) {
                     dtls_reset_peer(dtls_context, peer);
                  }
                  ui_led_op(LED_COLOR_GREEN, LED_SET);
                  dtls_start_connect(dtls_context, &app_data.destination);
                  send_request = true;
               } else {
                  sendto_peer(&app_data, &app_data.destination, dtls_context);
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
               time = (long)(atomic_get(&connected_time) - connect_time);
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
               if (!app_data.dtls_pending && app_data.no_response) {
                  dtls_coap_success(&app_data);
               }
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
                  temp += network_additional_timeout();
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
                     int rat = CONFIG_UDP_PSM_RETRANS_RAT;
                     app_data.timeout = network_timeout_scale(coap_timeout);
                     if ((app_data.timeout + 5) > rat) {
                        rat = app_data.timeout + 5;
                     }
                     modem_set_psm(rat);
                  }
                  ++app_data.retransmission;
                  loops = 0;
                  app_data.timeout <<= 1;
                  app_data.request_state = SEND;

                  dtls_info("%s resend, timeout %d s", type, app_data.timeout);
                  if (app_data.dtls_pending) {
                     app_data.dtls_next_flight = false;
                     dtls_check_retransmit(dtls_context, NULL);
                  } else {
                     sendto_peer(&app_data, &app_data.destination, dtls_context);
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
            } else if (dtls_trigger_pending()) {
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
            uint8_t flight = app_data.dtls_flight;
            recvfrom_peer(&app_data, dtls_context);
            if (flight && flight < app_data.dtls_flight) {
               loops = 0;
            }
            if (app_data.request_state == SEND_ACK) {
               int64_t temp_time = connect_time;
               sendto_peer(&app_data, &app_data.destination, dtls_context);
               connect_time = temp_time;
               dtls_coap_success(&app_data);
               dtls_info("CoAP ACK sent.");
            } else if (!app_data.dtls_pending && send_request) {
               dtls_info("DTLS finished, send coap request.");
               send_request = false;
               loops = 0;
               app_data.retransmission = 0;
               sendto_peer(&app_data, &app_data.destination, dtls_context);
            }
            if (!lte_power_on_off && !app_data.no_rai &&
                (app_data.request_state == NONE || app_data.request_state == WAIT_SUSPEND)) {
               modem_set_rai_mode(RAI_MODE_NOW, app_data.fd);
            }
            if (app_data.request_state == NONE &&
                (flags & FLAG_TLS) &&
                !(flags & FLAG_KEEP_CONNECTION) &&
                !app_data.dtls_pending) {
               app_data.dtls_pending = true;
               ++dtls_handshakes;
               ui_led_op(LED_DTLS, LED_CLEAR);
            }
         } else if (udp_poll.revents & (POLLERR | POLLNVAL)) {
            dtls_info("Poll: 0x%x", udp_poll.revents);
            if (check_socket(&app_data)) {
               k_sleep(K_MSEC(1000));
            }
         }
      }
   }

   dtls_info("Exit.");
   dtls_free_context(dtls_context);
   return 0;
}

static void dump_destination(const char *host, const session_t *destination)
{
   if (host) {
      char value[MAX_SETTINGS_VALUE_LENGTH];
      char ipv4_addr[NET_IPV4_ADDR_LEN] = {0};
      inet_ntop(AF_INET, &destination->addr.sin.sin_addr.s_addr, ipv4_addr,
                sizeof(ipv4_addr));
      dtls_info("Destination: '%s'", host);
      if (strcmp(host, ipv4_addr)) {
         dtls_info("IPv4 Address found %s", ipv4_addr);
      }
      dtls_info("Port       : %u", ntohs(destination->addr.sin.sin_port));
      if (appl_settings_get_coap_path(value, sizeof(value))) {
         dtls_info("CoAP-path  : '%s'", value);
      }
      if (appl_settings_get_coap_query(value, sizeof(value))) {
         dtls_info("CoAP-query : '%s'", value);
      }
   }
}

static int init_destination(session_t *destination, int protocol)
{
   int err = 0;
   int rc = -ENOENT;

   appl_settings_get_destination(app_data.host, sizeof(app_data.host));

   if (app_data.host[0]) {
      int count = 0;
      struct addrinfo *result = NULL;
      struct addrinfo hints = {
          .ai_family = AF_INET,
          .ai_socktype = SOCK_DGRAM};

      dtls_info("DNS lookup: %s", app_data.host);
      watchdog_feed();
      err = getaddrinfo(app_data.host, NULL, &hints, &result);
      while (-err == EAGAIN && count < 10) {
         k_sleep(K_MSEC(1000));
         ++count;
         watchdog_feed();
         err = getaddrinfo(app_data.host, NULL, &hints, &result);
      }
      if (err != 0) {
         dtls_warn("ERROR: getaddrinfo failed %d", err);
         rc = -EIO;
      } else if (result == NULL) {
         dtls_warn("ERROR: Address not found");
         rc = -ENOENT;
      } else {
         /* Free the address. */
         rc = 0;
         destination->addr.sin = *((struct sockaddr_in *)result->ai_addr);
         freeaddrinfo(result);
      }
   }
   if (rc) {
      return rc;
   }
   destination->addr.sin.sin_port = htons(appl_settings_get_destination_port(!(protocol & 1)));
   destination->size = sizeof(struct sockaddr_in);
   dump_destination(app_data.host, destination);
   return 0;
}

#ifdef CONFIG_SH_CMD

static int sh_cmd_send(const char *parameter)
{
   LOG_INF(">> send %s", parameter);
   dtls_cmd_trigger(true, 3, parameter, strlen(parameter));
   return 0;
}

static void sh_cmd_send_help(void)
{
   LOG_INF("> help send:");
   LOG_INF("  send            : send application message.");
   LOG_INF("  send <message>  : send provided message.");
}

static int sh_cmd_send_interval(const char *parameter)
{
   int res = 0;
   uint32_t interval = send_interval;
   char unit = 's';
   const char *cur = parameter;
   char value[10];

   memset(value, 0, sizeof(value));
   cur = parse_next_text(cur, ' ', value, sizeof(value));

   if (value[0]) {
      res = sscanf(value, "%u%c", &interval, &unit);
      if (res >= 1) {
         if (unit == 's' || unit == 'h') {
            LOG_INF("set send interval %u%c", interval, unit);
            if (unit == 'h') {
               interval *= 3600;
            }
            send_interval = interval;
            if (send_interval > 0) {
               work_reschedule_for_io_queue(&dtls_timer_trigger_work, K_SECONDS(send_interval));
            }
            res = 0;
         } else {
            LOG_INF("interval %s", parameter);
            LOG_INF("   unit '%c' not supported", unit);
            res = -EINVAL;
         }
      } else {
         res = -EINVAL;
      }
   } else {
      if ((interval % 3600) == 0) {
         LOG_INF("send interval %uh", interval / 3600);
      } else {
         LOG_INF("send interval %us", interval);
      }
   }
   return res;
}

static void sh_cmd_send_interval_help(void)
{
   LOG_INF("> help interval:");
   LOG_INF("  interval             : read send interval.");
   LOG_INF("  interval <time>[s|h] : set send interval.");
   LOG_INF("        <time>|<time>s : interval in seconds.");
   LOG_INF("               <time>h : interval in hours.");
}

static int sh_cmd_coap_timeout(const char *parameter)
{
   int res = 0;
   uint32_t timeout = coap_timeout;
   const char *cur = parameter;
   char value[10];

   memset(value, 0, sizeof(value));
   cur = parse_next_text(cur, ' ', value, sizeof(value));

   if (value[0]) {
      res = sscanf(value, "%u", &timeout);
      if (res == 1) {
         coap_timeout = timeout ? timeout : 1;
         res = 0;
         cur = "set ";
      } else {
         res = -EINVAL;
      }
   } else {
      cur = "";
   }
   if (!res) {
      uint32_t ntimeout = network_timeout_scale(coap_timeout);
      uint32_t atimeout = network_additional_timeout();
      if (coap_timeout != ntimeout) {
         LOG_INF("%sinitial coap timeout %us(+%us, *rsrp %us)", cur, timeout, atimeout, ntimeout);
      } else {
         LOG_INF("%sinitial coap timeout %us(+%us)", cur, timeout, atimeout);
      }
   }
   return res;
}

static void sh_cmd_send_coap_timeout_help(void)
{
   LOG_INF("> help timeout:");
   LOG_INF("  timeout        : read initial coap timeout.");
   LOG_INF("  timeout <time> : set initial coap timeout in seconds.");
}

static int sh_cmd_restart(const char *parameter)
{
   ARG_UNUSED(parameter);
   restart(ERROR_CODE_CMD, true);
   return 0;
}

static int sh_cmd_destination(const char *parameter)
{
   ARG_UNUSED(parameter);
   dump_destination(app_data.host, &app_data.destination);
   return 0;
}

static int sh_cmd_time(const char *parameter)
{
   ARG_UNUSED(parameter);
   dtls_log_now();
   return 0;
}

static int sh_cmd_dtls(const char *parameter)
{
   ARG_UNUSED(parameter);

   if (app_data.dtls_cipher_suite) {
      LOG_INF("DTLS: %s, %s", app_data.dtls_cipher_suite, app_data.dtls_cid ? "CID" : "(no CID)");
   }
   return 0;
}

SH_CMD(send, NULL, "send message.", sh_cmd_send, sh_cmd_send_help, 0);
SH_CMD(interval, NULL, "send interval.", sh_cmd_send_interval, sh_cmd_send_interval_help, 0);
SH_CMD(timeout, NULL, "initial coap timeout.", sh_cmd_coap_timeout, sh_cmd_send_coap_timeout_help, 0);
SH_CMD(restart, NULL, "try to switch off the modem and restart device.", sh_cmd_restart, NULL, 0);
SH_CMD(dest, NULL, "show destination.", sh_cmd_destination, NULL, 0);
SH_CMD(time, NULL, "show system time.", sh_cmd_time, NULL, 0);
SH_CMD(dtls, NULL, "show dtls information.", sh_cmd_dtls, NULL, 0);
#endif /* CONFIG_SH_CMD */

#ifdef CONFIG_ALL_POWER_OFF
int main(void)
{
   nrf_modem_lib_init();
   lte_lc_power_off();
   power_manager_init();
#ifdef CONFIG_MOTION_SENSOR
   accelerometer_init(NULL);
#endif /* CONFIG_MOTION_SENSOR */
   power_manager_suspend(true);
   // power_manager_3v3(false);
   // power_manager_1v8(false);
   k_sleep(K_MSEC(1000));
   NRF_REGULATORS->SYSTEMOFF = 1;
   return 0;
}
#else /* CONFIG_ALL_POWER_OFF */

static void init(int config, int protocol)
{
   char imei[MODEM_ID_SIZE];

   memset(&imei, 0, sizeof(imei));
   modem_init(config, dtls_lte_state_handler);
   modem_get_imei(imei, sizeof(imei) - 1);

   if (protocol == 0) {
      dtls_init();
      appl_settings_init(imei, &cb);
   } else {
      appl_settings_init(imei, NULL);
   }
}

int main(void)
{
   int config = 0;
   int protocol = -1;
   int flags = 0;

   memset(&app_data, 0, sizeof(app_data));
   memset(transmissions, 0, sizeof(transmissions));
   memset(appl_buffer, 0, sizeof(appl_buffer));

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

   init(config, protocol);

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
      if (dtls_network_searching(K_MINUTES(CONFIG_MODEM_SEARCH_TIMEOUT_REBOOT))) {
         restart(ERROR_CODE_INIT_NO_LTE, false);
      }
   }
   appl_ready = true;
   coap_client_init();

   init_destination(&app_data.destination, protocol);

   dtls_trigger();
   dtls_loop(flags);
   return 0;
}
#endif /* CONFIG_ALL_POWER_OFF */
