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
   INCOMING_CONNECT,
} request_state_t;

static const char *get_request_state_description(request_state_t request_state)
{
   switch (request_state) {
      case NONE:
         return "NONE";
      case SEND:
         return "SEND";
      case RECEIVE:
         return "RECEIVE";
      case WAIT_RESPONSE:
         return "WAIT_RESPONSED";
      case SEND_ACK:
         return "SEND_ACK";
      case WAIT_SUSPEND:
         return "WAIT_SUSPEND";
      case INCOMING_CONNECT:
         return "INCOMING_CONNECT";
   }
   return "UNKNOWN";
}

typedef enum {
   NO_SEARCH,
   MANUAL_SEARCH,
   CMD_SEARCH,
   EVENT_SEARCH,
   READY_SEARCH
} search_trigger_t;

struct dtls_app_data_t;

typedef int (*dtls_app_result_handler)(struct dtls_app_data_t *app, bool success);

typedef struct dtls_app_data_t {
   char host[MAX_SETTINGS_VALUE_LENGTH];
   session_t destination;
   coap_handler_t coap_handler;
   dtls_app_result_handler result_handler;
   int fd;
   int protocol;
#if defined(CONFIG_UDP_EDRX_WAKEUP_ENABLE)
   int fd2;
   uint16_t port;
#endif /* CONFIG_UDP_EDRX_WAKEUP_ENABLE */
   uint8_t keep_connection : 1;
   uint8_t send_request_pending : 1;
   uint8_t dtls_pending : 1;
   uint8_t dtls_next_flight : 1;
   uint8_t dtls_cid : 1;
   uint8_t no_response : 1;
   uint8_t rai : 1;
   uint8_t dtls_flight;
#ifdef CONFIG_COAP_UPDATE
   uint8_t download_progress;
#endif /* CONFIG_COAP_UPDATE */
   uint8_t retransmission;
   request_state_t request_state;
   uint16_t timeout;
   int64_t start_time;
   int64_t response_time;
   const char *dtls_cipher_suite;
} dtls_app_data_t;

#define DOWNLOAD_PROGRESS_STATUS_MESSAGE 32
#define DOWNLOAD_PROGRESS_LAST_STATUS_MESSAGE 33
#define DOWNLOAD_PROGRESS_REBOOT 34

#define LTE_REGISTERED 0
#define LTE_READY 1
#define LTE_CONNECTED 2
#define LTE_SLEEPING 3
#define LTE_READY_1S 4
#define LTE_PSM_ACTIVE 5
#define LTE_LOW_VOLTAGE 6
#define LTE_CONNECTED_SEND 7
#define LTE_SOCKET_ERROR 8
#define PM_PREVENT_SUSPEND 9
#define PM_SUSPENDED 10
#define APN_RATE_LIMIT 11
#define APN_RATE_LIMIT_RESTART 12
#define SETUP_MODE 13

#define APPL_READY 14
#define APPL_INITIAL_SUCCESS 15

#define TRIGGER_SEND 16
#define TRIGGER_DURATION 17
#define TRIGGER_RECV 18

static atomic_t general_states = ATOMIC_INIT(0);

static atomic_t lte_connections = ATOMIC_INIT(0);
static atomic_t not_ready_time = ATOMIC_INIT(0);
static atomic_t connected_time = ATOMIC_INIT(0);

static dtls_app_data_t app_data_context;

static unsigned int current_failures = 0;
static unsigned int handled_failures = 0;

static volatile search_trigger_t trigger_search = NO_SEARCH;

static volatile bool lte_power_off = false;
static bool lte_power_on_off = false;

#ifdef CONFIG_MOTION_DETECTION
static volatile bool moved = false;
#endif

#define MAX_APPL_BUF 1600
static uint8_t appl_buffer[MAX_APPL_BUF];

#define MAX_SEND_BUF 1024
static uint8_t send_buffer[MAX_SEND_BUF];
static volatile size_t send_buffer_len = 0;
static const char *send_trigger = NULL;
static K_MUTEX_DEFINE(send_buffer_mutex);

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

#ifdef CONFIG_COAP_NO_RESPONSE_ENABLE
#define COAP_SEND_FLAGS_ (COAP_SEND_FLAGS | COAP_SEND_FLAG_INITIAL | COAP_SEND_FLAG_NO_RESPONSE)
#else
#define COAP_SEND_FLAGS_ (COAP_SEND_FLAGS | COAP_SEND_FLAG_INITIAL)
#endif

static atomic_t send_interval = ATOMIC_INIT(CONFIG_COAP_SEND_INTERVAL);

#if defined(CONFIG_UDP_EDRX_WAKEUP_ENABLE)
volatile uint32_t edrx_wakeup_on_connect_timeout = CONFIG_UDP_EDRX_WAKEUP_ON_CONNECT_TIMEOUT;
#endif /* CONFIG_UDP_EDRX_WAKEUP_ENABLE */

volatile uint32_t coap_timeout = COAP_ACK_TIMEOUT;

volatile int coap_send_flags = COAP_SEND_FLAGS_;
volatile int coap_send_flags_next = COAP_SEND_FLAGS_;

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
   if (atomic_test_bit(&general_states, TRIGGER_SEND)) {
      index += snprintf(&buf[index], sizeof(buf) - index, ", trigger send");
   }
   if (atomic_test_bit(&general_states, TRIGGER_RECV)) {
      index += snprintf(&buf[index], sizeof(buf) - index, ", trigger recv");
   }
   if (atomic_test_bit(&general_states, TRIGGER_DURATION)) {
      index += snprintf(&buf[index], sizeof(buf) - index, ", trigger duration");
   }
   if (atomic_test_bit(&general_states, LTE_SLEEPING)) {
      index += snprintf(&buf[index], sizeof(buf) - index, ", modem sleeping");
   }
   if (atomic_test_bit(&general_states, LTE_SOCKET_ERROR)) {
      index += snprintf(&buf[index], sizeof(buf) - index, ", socket error");
   }
   if (atomic_test_bit(&general_states, LTE_PSM_ACTIVE)) {
      index += snprintf(&buf[index], sizeof(buf) - index, ", psm active");
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
                app_data_context.request_state == NONE;
   }

   if (suspend) {
      previous = atomic_test_and_set_bit(&general_states, PM_SUSPENDED);
   } else {
      previous = atomic_test_and_clear_bit(&general_states, PM_SUSPENDED);
   }

   if (previous != suspend) {
      if (suspend) {
         ui_enable(false);
      }
      power_manager_suspend(suspend);
   }
}

static int dtls_low_voltage(const k_timeout_t timeout)
{
   const int64_t start_time_low_voltage = k_uptime_get();
   int64_t timeout_ms = k_ticks_to_ms_floor64(timeout.ticks);

   while (!atomic_test_bit(&general_states, TRIGGER_DURATION)) {
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
      if (k_uptime_get() - start_time_low_voltage > timeout_ms) {
         break;
      }
      k_sem_reset(&dtls_trigger_search);
      watchdog_feed();
      k_sem_take(&dtls_trigger_search, K_SECONDS(WATCHDOG_TIMEOUT_S));
   }
   watchdog_feed();
   return true;
}

int get_local_address(uint8_t *buf, size_t length)
{
   int rc = 0;
#if defined(CONFIG_UDP_EDRX_WAKEUP_ENABLE)
   struct lte_network_info info;

   rc = modem_get_network_info(&info);
   if (!rc && buf && length) {
      rc = strlen(info.local_ip);
      memcpy(buf, info.local_ip, rc);
      buf[rc++] = ':';
      rc += snprintf(&buf[rc], length - rc, "%u", app_data_context.port);
      dtls_info("dtls: recv. address: %s", buf);
   }
#endif /* CONFIG_UDP_EDRX_WAKEUP_ENABLE */
   return rc;
}

int get_receive_interval(void)
{
#if defined(CONFIG_UDP_EDRX_WAKEUP_ENABLE)
   return modem_get_recv_interval_ms();
#else  /* CONFIG_UDP_EDRX_WAKEUP_ENABLE */
   return 0;
#endif /* CONFIG_UDP_EDRX_WAKEUP_ENABLE */
}

int get_send_interval(void)
{
   return atomic_get(&send_interval) & 0xffffff;
}

static void set_send_interval(int interval)
{
   atomic_set(&send_interval, interval & 0xffffff);
}

static bool set_next_send_interval(int new_interval)
{
   long current = atomic_get(&send_interval);
   long new_value = current & 0xffffff;

   if (new_value == new_interval) {
      atomic_cas(&send_interval, current, new_value);
      return false;
   } else {
      new_value |= (new_interval << 24);
      return atomic_cas(&send_interval, current, new_value);
   }
}

static const led_task_t led_reboot[] = {
    {.loop = 4, .time_ms = 499, .led = LED_COLOR_RED, .op = LED_SET},
    {.time_ms = 1, .led = LED_COLOR_RED, .op = LED_CLEAR},
    {.time_ms = 499, .led = LED_COLOR_BLUE, .op = LED_SET},
    {.time_ms = 1, .led = LED_COLOR_BLUE, .op = LED_CLEAR},
    {.time_ms = 0, .led = LED_COLOR_ALL, .op = LED_CLEAR},
};

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

   ui_led_op(LED_COLOR_ALL, LED_CLEAR);
   for (int i = 0; i < 4; ++i) {
      ui_led_tasks(led_reboot);
      k_sleep(K_MSEC(4000));
   }
   // reboot now
   appl_reboot(error, K_NO_WAIT);
}

static void check_restart(void)
{
   if (atomic_test_bit(&general_states, TRIGGER_DURATION)) {
      // Thingy:91 and nRF9160 feather will restart
      // nRF9160-DK restart with button2 also pressed
      int ui = ui_config();
      if (ui < 0) {
         dtls_info("> modem restart / factory reset");
         restart(ERROR_CODE_REBOOT_MANUAL, true);
      } else if (ui & 2) {
         dtls_info("> modem restart");
         restart(ERROR_CODE_REBOOT_MANUAL, false);
      }
      atomic_clear_bit(&general_states, TRIGGER_DURATION);
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
#if defined(CONFIG_UDP_EDRX_WAKEUP_ENABLE)
   if (app->fd2 >= 0) {
      (void)close(app->fd2);
      app->fd2 = -1;
      app->port = 0;
   }
#endif /* CONFIG_UDP_EDRX_WAKEUP_ENABLE */
}

static bool reopen_socket(dtls_app_data_t *app, const char *loc)
{
   const struct timeval tv = {.tv_sec = 1};
   int rc = 0;
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
   rc = setsockopt(app->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
   if (rc) {
      dtls_warn("> %s, set timeout for socket failed, errno %d (%s)",
                loc, errno, strerror(errno));
   }

   modem_set_psm(CONFIG_UDP_PSM_CONNECT_RAT);

#ifdef CONFIG_UDP_USE_CONNECT
   // using SO_RAI_NO_DATA requires a destination, for what ever
   rc = connect(app->fd, (struct sockaddr *)&app->destination.addr.sin, sizeof(struct sockaddr_in));
   if (rc) {
      dtls_warn("> %s, connect socket failed, errno %d (%s)",
                loc, errno, strerror(errno));
   }
#endif
   modem_set_rai_mode(RAI_MODE_OFF, app->fd);
   dtls_info("> %s, reopened socket.", loc);

#if defined(CONFIG_UDP_EDRX_WAKEUP_ENABLE)
   app->fd2 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
   if (app->fd2 < 0) {
      dtls_warn("> %s, reopen UDP wakeup socket failed, %d, errno %d (%s), restart",
                loc, app->fd2, errno, strerror(errno));
   } else {
      struct sockaddr_in listen_addr;

      app->port = CONFIG_UDP_EDRX_WAKEUP_PORT;
      memset(&listen_addr, 0, sizeof(listen_addr));

      rc = setsockopt(app->fd2, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      if (rc) {
         dtls_warn("> %s, set timeout for wakeup socket failed, errno %d (%s)",
                   loc, errno, strerror(errno));
      }

      listen_addr.sin_family = AF_INET;
      listen_addr.sin_port = htons(app->port);
      listen_addr.sin_addr.s_addr = INADDR_ANY;

      rc = bind(app->fd2, (struct sockaddr *)&listen_addr, sizeof(listen_addr));
      if (rc) {
         dtls_warn("> %s, bind wakeup socket failed, errno %d (%s)",
                   loc, errno, strerror(errno));
      } else {
         dtls_info("> %s, bind wakeup socket to port: %u", loc, app->port);
      }
   }
#endif

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

static void dtls_set_send_trigger(const char *trigger)
{
   k_mutex_lock(&send_buffer_mutex, K_FOREVER);
   send_trigger = trigger;
   k_mutex_unlock(&send_buffer_mutex);
}

static inline bool dtls_pending_request(request_state_t state)
{
   return (NONE != state) && (WAIT_SUSPEND != state) && (INCOMING_CONNECT != state);
}

static inline bool dtls_no_pending_request(request_state_t state)
{
   return !dtls_pending_request(state);
}

static void dtls_trigger(const char *cause, bool send)
{
   if (appl_reboots()) {
      return;
   }
   if (atomic_test_bit(&general_states, TRIGGER_DURATION)) {
      modem_interrupt_wait();
   }
   if (dtls_no_pending_request(app_data_context.request_state)) {
      // read battery status before modem wakes up
      power_manager_status(NULL, NULL, NULL, NULL);
      dtls_info("trigger %s%s", cause, send ? " send message" : "");
      atomic_set_bit_to(&general_states, TRIGGER_SEND, send);
      k_sem_give(&dtls_trigger_msg);
   }
}

static bool dtls_trigger_pending(void)
{
   return k_sem_count_get(&dtls_trigger_msg) ? true : false;
}

static void dtls_manual_trigger(int duration)
{
   bool send = false;

   if (atomic_test_bit(&general_states, SETUP_MODE)) {
      return;
   }

   if (atomic_test_bit(&general_states, APPL_READY) && duration) {
      atomic_set_bit(&general_states, TRIGGER_DURATION);
   } else {
      atomic_clear_bit(&general_states, TRIGGER_DURATION);
      send = true;
      dtls_set_send_trigger("button");
   }

   // LEDs for manual trigger
   ui_led_op(LED_COLOR_RED, LED_CLEAR);
   dtls_trigger("manual", send);

   if (!atomic_test_bit(&general_states, LTE_READY)) {
      trigger_search = MANUAL_SEARCH;
      k_sem_give(&dtls_trigger_search);
   }
}

static void dtls_cmd_trigger(const char *source, bool led, int mode)
{
   bool ready = atomic_test_bit(&general_states, LTE_READY);
   if (mode & 1) {
      if (dtls_no_pending_request(app_data_context.request_state)) {
         ui_enable(led);
         dtls_set_send_trigger(source);
         dtls_trigger(source, true);
         if (!ready && !(mode & 2)) {
            dtls_info("%s: no network ...", source);
         }
      } else if (ready) {
         dtls_info("%s: busy, request pending ... state %d (%s)", source, app_data_context.request_state, get_request_state_description(app_data_context.request_state));
      } else {
         dtls_info("%s: busy, searching network", source);
      }
   }
   if (!ready && (mode & 2)) {
      ui_enable(led);
      trigger_search = CMD_SEARCH;
      k_sem_give(&dtls_trigger_search);
   }
}

static void dtls_timer_trigger_fn(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(dtls_timer_trigger_work, dtls_timer_trigger_fn);

static void dtls_timer_trigger_fn(struct k_work *work)
{
   long interval = atomic_get(&send_interval);

   if (dtls_no_pending_request(app_data_context.request_state)) {
      // no LEDs for time trigger
      ui_enable(false);
      dtls_set_send_trigger("timer");
      dtls_trigger("timer", true);
   } else {
      long next_interval = interval;
      if (next_interval & 0xffffff000000) {
         next_interval >>= 24;
         dtls_info("Busy, schedule again in %d s.", (int)next_interval);
      } else {
         dtls_debug("Busy, schedule again in %d s.", (int)next_interval);
      }
      if (next_interval > 0) {
         work_schedule_for_io_queue(&dtls_timer_trigger_work, K_SECONDS(next_interval));
      }
   }
   atomic_cas(&send_interval, interval, interval & 0xffffff);
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

static bool dtls_pending(dtls_app_data_t *app)
{
   if (app->protocol == PROTOCOL_COAP_DTLS) {
      app->dtls_pending = 1;
   }
   return app->dtls_pending;
}

static void dtls_coap_set_request_state(const char *desc, dtls_app_data_t *app, request_state_t request_state);

static void dtls_coap_next(dtls_app_data_t *app, int interval)
{
   bool pending = false;

   ui_led_op(LED_APPLICATION, LED_CLEAR);
   if (lte_power_on_off) {
      dtls_debug("> modem switching off ...");
      lte_power_off = true;
      modem_power_off();
      dtls_debug("modem off");
   }

   dtls_log_now();

#ifdef CONFIG_COAP_UPDATE
   if (app->download_progress == DOWNLOAD_PROGRESS_REBOOT) {
      appl_update_coap_reboot();
   }
#endif

   dtls_coap_set_request_state("next request", app, lte_power_off ? NONE : WAIT_SUSPEND);

   k_mutex_lock(&send_buffer_mutex, K_FOREVER);
   pending = send_buffer_len > 0;
   k_mutex_unlock(&send_buffer_mutex);

   if (pending) {
      // send pending custom request
      atomic_set_bit(&general_states, TRIGGER_SEND);
      k_sem_give(&dtls_trigger_msg);
   } else {
      k_sem_reset(&dtls_trigger_msg);
      if (interval > 0 && set_next_send_interval(interval)) {
         // special interval
         work_reschedule_for_io_queue(&dtls_timer_trigger_work, K_SECONDS(interval));
         dtls_info("Next request, schedule in %d s.", interval);
      } else {
         interval = get_send_interval();
         if (interval > 0 && work_schedule_for_io_queue(&dtls_timer_trigger_work, K_SECONDS(interval)) == 1) {
            // standard interval
            dtls_debug("Next request, schedule in %d s.", interval);
         }
      }
   }
}

static int dtls_app_coap_result_handler(struct dtls_app_data_t *app, bool success)
{
   if (success) {
      coap_send_flags &= ~COAP_SEND_FLAG_INITIAL;
      coap_send_flags_next = coap_send_flags;
   }
   return 0;
}

#ifdef CONFIG_DTLS_ECDSA_AUTO_PROVISIONING
static int dtls_app_prov_result_handler(struct dtls_app_data_t *app, bool success)
{
   if (success) {
      if (!appl_settings_is_provisioning()) {
         // new handshake with provisioned credentials
         dtls_pending(app);
         return 5;
      }
   }
   return 0;
}
#endif /* CONFIG_DTLS_ECDSA_AUTO_PROVISIONING */

#ifdef CONFIG_COAP_UPDATE
static int dtls_app_download_result_handler(struct dtls_app_data_t *app, bool success)
{
   if (success) {
      if (!appl_update_coap_pending()) {
         app->download_progress = DOWNLOAD_PROGRESS_LAST_STATUS_MESSAGE;
         return 2;
      }
   } else {
      app->download_progress = 0;
      appl_update_coap_cancel();
      return 2;
   }
   return 0;
}
#endif /* CONFIG_COAP_UPDATE */

static void dtls_coap_success(dtls_app_data_t *app)
{
   int interval = 0;
   int index = 0;
   int time1 = (int)(atomic_get(&connected_time) - app->start_time);
   int time2 = (int)(app->response_time - app->start_time);

   if (time1 < 0) {
      time1 = -1;
   }
   if (time2 < 0) {
      time2 = -1;
   }

   if (time2 >= 0) {
      if (time1 > 0) {
         connect_time_ms = time1;
         coap_rtt_ms = time2 - time1;
      } else {
         connect_time_ms = 0;
         coap_rtt_ms = time2;
      }
   } else {
      connect_time_ms = 0;
      coap_rtt_ms = 0;
   }

   if (coap_rtt_ms > 500) {
      ui_enable(false);
   } else {
      ui_led_op(LED_COLOR_RED, LED_CLEAR);
      ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
      ui_led_op(LED_COLOR_GREEN, LED_BLINK);
   }

   dtls_info("%dms/%dms: success", time1, time2);
   if (app->retransmission <= COAP_MAX_RETRANSMISSION) {
      transmissions[app->retransmission]++;
   }

   if (time2 >= 0) {
      retransmissions = app->retransmission;
      if (retransmissions == 0 && time2 < 4000) {
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
   if (!atomic_test_and_set_bit(&general_states, APPL_INITIAL_SUCCESS)) {
#ifdef CONFIG_UPDATE
      appl_update_image_verified();
#endif
   }
   interval = app->result_handler(app, true);
   atomic_clear_bit(&general_states, APN_RATE_LIMIT);
   atomic_clear_bit(&general_states, APN_RATE_LIMIT_RESTART);
   if (interval) {
      dtls_debug("Success, schedule in %d s.", interval);
   }
   dtls_coap_next(app, interval);
}

static void dtls_coap_failure(dtls_app_data_t *app, const char *cause)
{
   int interval = 0;
   int time1 = (int)(atomic_get(&connected_time) - app->start_time);
   int time2 = (int)(app->response_time - app->start_time);

   if (time1 < 0) {
      time1 = -1;
   }
   if (time2 < 0) {
      time2 = -1;
   }
   if (!ui_led_op(LED_COLOR_RED, LED_SET)) {
      ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
      ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
      atomic_set_bit(&general_states, PM_PREVENT_SUSPEND);
      work_reschedule_for_io_queue(&dtls_power_management_suspend_work, K_SECONDS(10));
   }
   dtls_info("%dms/%dms: failure, %s", time1, time2, cause);
   failures++;
   if (!atomic_test_bit(&general_states, APPL_INITIAL_SUCCESS)) {
      int f = dtls_coap_inc_failures();
      dtls_info("current failures %d.", f);
   }
   interval = app->result_handler(app, false);

   if (app->dtls_pending) {
      dtls_info("dtls, restart handshake.");
      app->dtls_next_flight = 0;
      app->dtls_flight = 0;
   }

#if CONFIG_COAP_FAILURE_SEND_INTERVAL > 0
   if (interval == 0) {
      interval = CONFIG_COAP_FAILURE_SEND_INTERVAL;
   }
#endif /*CONFIG_COAP_FAILURE_SEND_INTERVAL*/

   if (interval > 0) {
      dtls_debug("Failure, schedule in %d s.", interval);
   }
   dtls_coap_next(app, interval);
}

static void dtls_coap_set_request_state(const char *desc, dtls_app_data_t *app, request_state_t request_state)
{
   request_state_t previous = app->request_state;
   if (previous == request_state) {
      dtls_info("Req-State %s keep %d (%s)", desc, request_state, get_request_state_description(request_state));
   } else {
      dtls_info("Req-State %s change from %d (%s) to %d (%s)", desc, previous, get_request_state_description(previous),
                request_state, get_request_state_description(request_state));
      app->request_state = request_state;
      if (RECEIVE == request_state && app->no_response && !app->dtls_flight) {
         dtls_coap_success(app);
      }
   }
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
   int err = app->coap_handler.parse_data(data, len);

   if (err < 0) {
      return err;
   }

   switch (err) {
      case PARSE_NONE:
         break;
      case PARSE_IGN:
         break;
      case PARSE_RST:
         if (dtls_pending_request(app->request_state)) {
            app->response_time = k_uptime_get();
            dtls_coap_failure(app, "rst");
         }
         break;
      case PARSE_ACK:
         if (NONE != app->request_state && app->request_state < WAIT_RESPONSE) {
            dtls_coap_set_request_state("coap  ack", app, WAIT_RESPONSE);
         }
         break;
      case PARSE_RESPONSE:
         if (dtls_pending_request(app->request_state)) {
            app->response_time = k_uptime_get();
            dtls_coap_success(app);
         }
         break;
      case PARSE_CON_RESPONSE:
         if (NONE != app->request_state) {
            app->response_time = k_uptime_get();
            dtls_coap_set_request_state("coap  con-resp", app, SEND_ACK);
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
   if (app->rai && !lte_power_on_off) {
      modem_set_rai_mode(app->no_response ? RAI_MODE_LAST : RAI_MODE_ONE_RESPONSE, app->fd);
   } else {
      modem_set_rai_mode(RAI_MODE_OFF, app->fd);
   }
}

static int
send_to_peer(dtls_app_data_t *app, const uint8_t *data, size_t len)
{
   bool first = app->retransmission == 0 &&
                (!app->dtls_flight || app->dtls_next_flight);
   bool connected;
   int result = 0;
   const char *tag = app->dtls_flight ? (app->retransmission ? "hs_re" : "hs_") : (app->retransmission ? "re" : "");

   if (!lte_power_on_off) {
      prepare_socket(app);
   }
   result = sendto(app->fd, data, len, MSG_DONTWAIT, &app->destination.addr.sa, app->destination.size);
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
   if (SEND == app->request_state || app->dtls_flight) {
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
      app->dtls_next_flight = 0;
      dtls_info("hs_flight %d", app->dtls_flight);
      app->dtls_flight += 2;
   }
   if (first) {
      app->timeout = network_timeout_scale(coap_timeout);
      dtls_info("%sresponse timeout %d s", tag, app->timeout);
   }
   return result;
}

static int
dtls_send_to_peer(dtls_context_t *ctx,
                  session_t *session, uint8 *data, size_t len)
{
   (void)session;
   dtls_app_data_t *app = dtls_get_app_data(ctx);
   int result = send_to_peer(app, data, len);
   if (app->dtls_flight && result < 0) {
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

#define CIPHER_ENTRY(X) \
   {                    \
       .name = #X, .cipher = X}

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
   if (level == DTLS_ALERT_LEVEL_WARNING) {
      dtls_info("dtls event alert warning 0x%04x", code);
      dtls_coap_failure(app, "dtls warning");
   } else if (level == DTLS_ALERT_LEVEL_FATAL) {
      dtls_info("dtls event alert fatal 0x%04x", code);
      dtls_coap_failure(app, "dtls alert");
   } else if (level == 0) {
      if (DTLS_EVENT_CONNECTED == code) {
         dtls_coap_set_request_state("dtls event connected", app, NONE);
         app->dtls_pending = 0;
         app->dtls_next_flight = 0;
         app->dtls_flight = 0;
         peer = dtls_get_peer(ctx, session);
         if (peer) {
            const dtls_security_parameters_t *security_params = peer->security_params[0];
            const dtls_cipher_t cipher = dtls_get_cipher_suite(security_params->cipher_index);
            const struct cipher_entry *cur = ciphers_map;
            app->dtls_cid = security_params->write_cid_length > 0 ? 1 : 0;
            app->dtls_cipher_suite = "none";
            while (cur && cur->cipher != TLS_NULL_WITH_NULL_NULL) {
               if (cur->cipher == cipher) {
                  app->dtls_cipher_suite = cur->name;
                  break;
               }
               ++cur;
            }
         }
         ui_led_op(LED_COLOR_RED, LED_CLEAR);
         ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
         ui_led_op(LED_DTLS, LED_SET);
      } else if (DTLS_EVENT_CONNECT == code) {
         dtls_info("dtls event connect ...");
         app->dtls_pending = 1;
         app->dtls_cipher_suite = NULL;
         app->dtls_cid = 0;
         ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
         ui_led_op(LED_COLOR_RED, LED_SET);
         ui_led_op(LED_COLOR_GREEN, LED_SET);
         ui_led_op(LED_DTLS, LED_CLEAR);
      } else {
         dtls_info("dtls event, unknown code 0x%04x", code);
      }
   } else {
      dtls_info("dtls event, %d unknown level, 0x%04x", level, code);
   }
   return 0;
}

static int
recvfrom_peer(dtls_app_data_t *app, dtls_context_t *ctx)
{
   int result;
   session_t session;

   memset(&session, 0, sizeof(session_t));
   memset(appl_buffer, 0, sizeof(appl_buffer));
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
      if (app->dtls_flight) {
         app->dtls_next_flight = 1;
      }
      result = dtls_handle_message(ctx, &session, appl_buffer, result);
      if (app->dtls_flight) {
         dtls_coap_set_request_state("dtls received", app, RECEIVE);
      }
      return result;
   } else {
      return read_from_peer(app, &session, appl_buffer, result);
   }
}

#if defined(CONFIG_UDP_EDRX_WAKEUP_ENABLE)
static int
recvfrom_peer2(dtls_app_data_t *app)
{
   int result;
   struct sockaddr_in sin;
   socklen_t sin_len = sizeof(sin);

   memset(&sin, 0, sizeof(sin));
   memset(appl_buffer, 0, sizeof(appl_buffer));
   dtls_info("recvfrom_peer2 ...");
   result = recvfrom(app->fd2, appl_buffer, MAX_APPL_BUF, 0,
                     (struct sockaddr *)&sin, &sin_len);
   if (result < 0) {
      dtls_warn("recv_from_peer2 failed: errno %d (%s)", result, strerror(errno));
      return result;
   }
   dtls_info("received_from_peer2 %d bytes", result);
   if ((result == 2 || result == 3) && memcmp(appl_buffer, "up", 2) == 0) {
      atomic_clear_bit(&general_states, TRIGGER_RECV);
      dtls_cmd_trigger("wakeup", false, 1);
   }
   return result;
}
#endif /* CONFIG_UDP_EDRX_WAKEUP_ENABLE */

static int
sendto_peer(dtls_app_data_t *app, struct dtls_context_t *ctx)
{
   int result = 0;
   size_t coap_message_len = 1;
   const uint8_t *coap_message_buf = NULL;

   if (app->dtls_pending) {
      app->rai = 0;
      if (app->dtls_flight) {
         app->dtls_next_flight = 0;
         dtls_check_retransmit(ctx, NULL);
      } else {
         dtls_peer_t *peer = dtls_get_peer(ctx, &app->destination);
         if (peer) {
            dtls_reset_peer(ctx, peer);
         }
         ui_led_op(LED_COLOR_GREEN, LED_SET);
         ++dtls_handshakes;
         app->send_request_pending = 1;
         app->retransmission = 0;
         app->dtls_next_flight = 1;
         app->dtls_flight = 1;
         dtls_coap_set_request_state("DTLS 1.2 start handshake", app, SEND);
         result = dtls_connect(ctx, &app->destination);
      }
   } else {
      coap_message_len = app->coap_handler.get_message(&coap_message_buf);
      if (coap_message_len) {
         dtls_info("send %d bytes.", coap_message_len);
         if (ctx) {
            result = dtls_write(ctx, &app->destination, (uint8_t *)coap_message_buf, coap_message_len);
            if (result < 0) {
               dtls_warn("Failed to send CoAP request with %d bytes via DTLS, %d (%s)",
                         coap_message_len, errno, strerror(errno));
            }
         } else {
            result = send_to_peer(app, coap_message_buf, coap_message_len);
            if (result < 0) {
               dtls_warn("Failed to send CoAP request with %d bytes via UDP, %d (%s)",
                         coap_message_len, errno, strerror(errno));
            }
         }
      } else {
         ui_led_op(LED_COLOR_ALL, LED_CLEAR);
         dtls_coap_set_request_state("cancel request", app, WAIT_SUSPEND);
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
         if (!app->dtls_pending || app->dtls_flight) {
            dtls_coap_set_request_state("sent", app, RECEIVE);
         }
      }
   }
   return result;
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
      case LTE_STATE_PSM_ACTIVE:
         desc = "psm active";
         bit = LTE_PSM_ACTIVE;
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
      if (!app_data_context.dtls_flight) {
         if (active) {
            ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
            ui_led_op(LED_COLOR_GREEN, LED_SET);
         } else if (dtls_no_pending_request(app_data_context.request_state)) {
            ui_led_op(LED_COLOR_RED, LED_CLEAR);
            ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
         }
      }
      if (active) {
         if (!previous) {
            atomic_set(&connected_time, now);
            atomic_inc(&lte_connections);
            atomic_set_bit(&general_states, LTE_CONNECTED_SEND);
            if (!app_data_context.dtls_flight &&
                NONE == app_data_context.request_state &&
                atomic_test_bit(&general_states, APPL_READY)) {
               // start receiving
               atomic_set_bit(&general_states, TRIGGER_RECV);
               dtls_trigger("incoming connect", false);
            }
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
      dtls_trigger("setup", true);
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
   const int64_t start_time_network_search = k_uptime_get();
   int64_t timeout_ms = k_ticks_to_ms_floor64(timeout.ticks);
   long last_not_ready_time = atomic_get(&not_ready_time);
   int trigger = MANUAL_SEARCH;
   int swap_state = 1;

   while (!atomic_test_bit(&general_states, TRIGGER_DURATION)) {
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
         time = (long)now - start_time_network_search;
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
               int time_s = (now - start_time_network_search) / MSEC_PER_SEC;
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

static long dtls_calculate_reboot_timeout(int reboot)
{
#ifdef CONFIG_UPDATE
   if (appl_update_image_verified()) {
      return MSEC_PER_HOUR;
   }
#endif
   return reboot == 1 ? MSEC_PER_HOUR * 4 : MSEC_PER_DAY;
}

static int dtls_loop(dtls_app_data_t *app, int reboot)
{
#if defined(CONFIG_UDP_EDRX_WAKEUP_ENABLE)
   struct pollfd udp_poll[2];
#else  /* CONFIG_UDP_EDRX_WAKEUP_ENABLE */
   struct pollfd udp_poll[1];
#endif /* CONFIG_UDP_EDRX_WAKEUP_ENABLE */

   dtls_context_t *dtls_context = NULL;
   const char *reopen_cause = NULL;
   int result;
   int loops = 0;
   int udp_ports_to_poll = 1;
   long time;
   long reboot_timeout = dtls_calculate_reboot_timeout(reboot);
   bool restarting_modem = false;
   bool restarting_modem_power_off = false;
   bool network_not_found = false;
#ifdef CONFIG_COAP_WAIT_ON_POWERMANAGER
   uint16_t battery_voltage = 0xffff;
#endif

#ifdef CONFIG_LOCATION_ENABLE
   bool location_init = true;
#endif

   if (app->protocol == PROTOCOL_COAP_DTLS) {
      dtls_info("Start CoAP/DTLS 1.2");
   } else {
      dtls_info("Start CoAP/UDP");
   }
   app->fd = -1;

   if (app->protocol == PROTOCOL_COAP_DTLS) {
      dtls_context = dtls_new_context(app);
      if (!dtls_context) {
         dtls_emerg("cannot create dtls context");
         restart(ERROR_CODE_INIT_NO_DTLS, false);
      }
      dtls_set_handler(dtls_context, &cb);
      dtls_pending(app);
   }

   app->timeout = coap_timeout;
   dtls_coap_set_request_state("init", app, NONE);

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
      } else if (!app->dtls_pending) {
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

      if (!atomic_test_bit(&general_states, APPL_INITIAL_SUCCESS) &&
          k_uptime_get() > reboot_timeout) {
         // no initial success for 4 hours / 1 day => reboot
         ++reboot;
         dtls_info("> No initial success, reboot %d.", reboot);
         restart(ERROR_CODE(ERROR_CODE_INIT_NO_SUCCESS, reboot), true);
      }

      network_not_found = false;
      if (!lte_power_off && (!atomic_test_bit(&general_states, LTE_READY) || app->fd < 0)) {
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
         int strategy = coap_appl_client_retry_strategy(f, app->protocol == PROTOCOL_COAP_DTLS);
         if (strategy) {
            if (strategy & DTLS_CLIENT_RETRY_STRATEGY_RESTARTS) {
               dtls_info("Too many failures, reboot");
               restart(ERROR_CODE_TOO_MANY_FAILURES, false);
            }
            if (strategy & DTLS_CLIENT_RETRY_STRATEGY_DTLS_HANDSHAKE) {
               dtls_info("handle failure %d. new DTLS handshake.", f);
               dtls_pending(app);
               dtls_trigger("retry handshake", true);
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
      if (atomic_test_bit(&general_states, TRIGGER_DURATION)) {
         if (!dtls_setup_mode()) {
            atomic_clear_bit(&general_states, TRIGGER_DURATION);
         }
      }
#endif
      if (atomic_test_bit(&general_states, TRIGGER_DURATION)) {
         restarting_modem = true;
      }

      if (restarting_modem) {
         dtls_info("Trigger restart modem %s.", restarting_modem_power_off ? "power off" : "offline");
         restarting_modem = false;
         if (restart_modem(restarting_modem_power_off)) {
            if (!reopen_cause) {
               reopen_cause = "restart modem";
            }
            dtls_trigger("restart modem", true);
         }
         restarting_modem_power_off = false;
      }

      if (!lte_power_off && !atomic_test_bit(&general_states, LTE_READY)) {
         dtls_info("Modem not ready.");
         k_sleep(K_MSEC(1000));
         continue;
      }

      if (!lte_power_off && check_socket(app)) {
         if (!reopen_cause) {
            reopen_cause = "check";
         }
         if (reopen_socket(app, reopen_cause)) {
            if (app->request_state == SEND ||
                app->request_state == RECEIVE) {
               loops = 0;
               app->retransmission = 0;
               app->start_time = k_uptime_get();
               dtls_coap_set_request_state("reopen socket", app, SEND);

               ui_led_op(LED_APPLICATION, LED_SET);
               if (app->dtls_flight) {
                  dtls_info("DTLS hs send again");
               } else {
                  dtls_info("CoAP request send again");
               }
               sendto_peer(app, dtls_context);
            }
         }
         reopen_cause = NULL;
      }
      udp_ports_to_poll = 1;
      udp_poll[0].fd = app->fd;
      udp_poll[0].events = POLLIN;
      udp_poll[0].revents = 0;
#if defined(CONFIG_UDP_EDRX_WAKEUP_ENABLE)
      if (app->fd2 >= 0) {
         ++udp_ports_to_poll;
         udp_poll[1].fd = app->fd2;
         udp_poll[1].events = POLLIN;
         udp_poll[1].revents = 0;
      } else {
         udp_poll[1].fd = -1;
         udp_poll[1].events = 0;
         udp_poll[1].revents = 0;
      }
#endif /*CONFIG_UDP_EDRX_WAKEUP_ENABLE */
#ifdef CONFIG_COAP_UPDATE
      if (dtls_no_pending_request(app->request_state)) {
         bool pending = appl_update_coap_pending();
         if (pending) {
            if (!appl_update_coap_pending_next() &&
                !dtls_trigger_pending()) {
               dtls_info("wait for download ...");
               loops = 0;
               app->download_progress = 1;
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
            pending = appl_update_coap_pending();
            if (pending) {
               bool download = false;
               if (!dtls_trigger_pending()) {
                  download = app->download_progress > 1;
                  if (++app->download_progress >= DOWNLOAD_PROGRESS_STATUS_MESSAGE) {
                     // skip for status message
                     app->download_progress = 1;
                  }
                  if (download) {
                     dtls_info("download request");
                  } else {
                     dtls_set_send_trigger("download status");
                     dtls_trigger("download status report", true);
                  }
               } else {
                  dtls_info("manual download status report");
               }
               if (download) {
                  loops = 0;
                  dtls_coap_set_request_state("download", app, SEND);
                  app->retransmission = 0;
                  appl_update_coap_next();
                  app->coap_handler = coap_update_client_handler;
                  app->result_handler = dtls_app_download_result_handler;
                  app->rai = 0;
                  dtls_info("next download request");
                  app->start_time = k_uptime_get();
                  sendto_peer(app, dtls_context);
                  continue;
               }
            } else {
               dtls_info("download canceled");
            }
         }
         if (!pending && app->download_progress &&
             app->download_progress < DOWNLOAD_PROGRESS_STATUS_MESSAGE) {
            app->download_progress = 0;
            dtls_coap_set_request_state("download canceled", app, WAIT_SUSPEND);
         }
      }
#endif /* CONFIG_COAP_UPDATE */

      bool poll_recv = NONE != app->request_state;

      if (!poll_recv && atomic_test_bit(&general_states, TRIGGER_RECV)) {
         dtls_coap_set_request_state("incoming connect", app, INCOMING_CONNECT);
         loops = 0;
      }
      if (INCOMING_CONNECT == app->request_state) {
         poll_recv = atomic_test_bit(&general_states, TRIGGER_RECV);
      }

      if (poll_recv) {
         result = poll(udp_poll, udp_ports_to_poll, 1000);
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
#endif /* CONFIG_COAP_WAIT_ON_POWERMANAGER */
         result = 0;
         dtls_power_management();
         if (k_sem_take(&dtls_trigger_msg, K_SECONDS(60)) == 0) {
            if (atomic_test_and_clear_bit(&general_states, TRIGGER_SEND)) {
               int res = get_send_interval();
               atomic_clear_bit(&general_states, TRIGGER_RECV);
               dtls_coap_set_request_state("trigger", app, SEND);
               dtls_power_management();
               ui_led_op(LED_APPLICATION, LED_SET);
               if (res > 0) {
                  work_reschedule_for_io_queue(&dtls_timer_trigger_work, K_SECONDS(res));
               }
               if (lte_power_off) {
                  dtls_info("modem on");
                  lte_power_off = false;
                  restarting_modem = false;
                  app->start_time = k_uptime_get();
                  modem_start(K_SECONDS(CONFIG_MODEM_SEARCH_TIMEOUT), false);
                  reopen_socket(app, "on");
               }
               loops = 0;
               app->retransmission = 0;
#ifdef CONFIG_DTLS_ECDSA_AUTO_PROVISIONING
               if (appl_settings_is_provisioning()) {
                  res = coap_prov_client_prepare_post(appl_buffer, sizeof(appl_buffer));
                  app->coap_handler = coap_prov_client_handler;
                  app->result_handler = dtls_app_prov_result_handler;
                  app->rai = 0;
               } else
#endif /* CONFIG_DTLS_ECDSA_AUTO_PROVISIONING */
               {
                  app->no_response = (coap_send_flags_next & COAP_SEND_FLAG_NO_RESPONSE) ? 1 : 0;
                  k_mutex_lock(&send_buffer_mutex, K_FOREVER);
                  if (send_buffer_len) {
                     res = coap_appl_client_prepare_post(send_buffer, send_buffer_len,
                                                         coap_send_flags_next | COAP_SEND_FLAG_SET_PAYLOAD, NULL);
                     send_buffer_len = 0;
                  } else {
                     memset(appl_buffer, 0, sizeof(appl_buffer));
                     res = coap_appl_client_prepare_post(appl_buffer, sizeof(appl_buffer), coap_send_flags_next, send_trigger);
                     send_trigger = NULL;
                  }
                  k_mutex_unlock(&send_buffer_mutex);
                  app->coap_handler = coap_appl_client_handler;
                  app->result_handler = dtls_app_coap_result_handler;
#ifdef CONFIG_COAP_UPDATE
                  app->rai = app->download_progress ? 0 : 1;
                  if (app->download_progress == DOWNLOAD_PROGRESS_LAST_STATUS_MESSAGE) {
                     app->download_progress = DOWNLOAD_PROGRESS_REBOOT;
                  }
#else  /* CONFIG_COAP_UPDATE */
                  app->rai = 1;
#endif /* CONFIG_COAP_UPDATE */
               }
               if (res < 0) {
                  dtls_coap_failure(app, "prepare post");
               } else if (res > 0) {
                  if (!lte_power_off) {
                     app->start_time = k_uptime_get();
                  }
                  sendto_peer(app, dtls_context);
               } else {
                  dtls_coap_set_request_state("no payload", app, NONE);
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
         const char *type = app->dtls_flight ? "DTLS hs" : "CoAP request";
         ++loops;
         if (app->request_state == SEND) {
            if (atomic_test_bit(&general_states, LTE_CONNECTED_SEND)) {
               loops = 0;
               time = (long)(atomic_get(&connected_time) - app->start_time);
               if (time < 0) {
                  time = -1;
               }
               dtls_log_state();
               if (app->request_state == SEND) {
                  dtls_info("%ld ms: connected => sent",
                            time);
               } else {
                  dtls_info("%ld ms: connected => resent",
                            time);
               }
               dtls_coap_set_request_state("lte connected", app, RECEIVE);
            } else {
               if (loops > 60) {
                  dtls_log_state();
                  dtls_info("%s send timeout %d s", type, loops);
                  dtls_coap_failure(app, "timeout");
               } else if ((loops & 3) == 3) {
                  dtls_info("%s waiting for lte connection, %d s", type, loops);
               }
            }
         } else if (app->request_state == RECEIVE) {
            int temp = app->timeout;
            if (!atomic_test_bit(&general_states, LTE_READY)) {
               if (app->retransmission >= COAP_MAX_RETRANSMISSION) {
                  // stop waiting ...
                  temp = loops - 1;
               } else {
                  temp += network_additional_timeout();
               }
            }
            dtls_log_state();
            if (app->retransmission > 0) {
               dtls_info("%s wait %d of %d s, retrans. %d", type, loops, temp, app->retransmission);
            } else {
               dtls_info("%s wait %d of %d s", type, loops, temp);
            }
            if (loops > temp) {
               result = -1;
               if (app->retransmission < COAP_MAX_RETRANSMISSION) {
                  if (app->retransmission == 0) {
                     int rat = CONFIG_UDP_PSM_RETRANS_RAT;
                     app->timeout = network_timeout_scale(coap_timeout);
                     if ((app->timeout + 4) > rat) {
                        rat = app->timeout + 4;
                     }
                     modem_set_psm(rat);
                  }
                  ++app->retransmission;
                  loops = 0;
                  app->timeout <<= 1;
                  dtls_coap_set_request_state("resend", app, SEND);

                  dtls_info("%s resend, timeout %d s", type, app->timeout);
                  app->rai = 0;
                  sendto_peer(app, dtls_context);
               } else {
                  // maximum retransmissions reached
                  dtls_info("%s receive timeout %d s", type, app->timeout);
                  dtls_coap_failure(app, "receive timeout");
               }
            }
         } else if (app->request_state == WAIT_RESPONSE) {
            if (loops > 60) {
               dtls_log_state();
               dtls_info("%s response timeout %d s", type, loops);
               dtls_coap_failure(app, "response timeout");
            }
         } else if (app->request_state == WAIT_SUSPEND) {
            // wait for late received data
            if (atomic_test_bit(&general_states, LTE_SLEEPING)) {
               // modem enters sleep, no more data
               dtls_coap_set_request_state("lte sleeping", app, NONE);
               dtls_info("%s suspend after %d s", type, loops);
            } else if (dtls_trigger_pending()) {
               // send button pressed
               dtls_coap_set_request_state("trigger", app, NONE);
               dtls_info("%s next trigger after %d s", type, loops);
            } else if (!atomic_test_bit(&general_states, LTE_CONNECTED) &&
                       !atomic_test_bit(&general_states, LTE_PSM_ACTIVE)) {
               // modem without PSM enters idle, no more data
               dtls_coap_set_request_state("disconnect", app, NONE);
               dtls_info("%s suspend after %d s", type, loops);
            }
         } else if (app->request_state == INCOMING_CONNECT) {
#if defined(CONFIG_UDP_EDRX_WAKEUP_ENABLE)
            if (edrx_wakeup_on_connect_timeout && atomic_test_bit(&general_states, TRIGGER_RECV)) {
               // no data received after wakeup
               if (edrx_wakeup_on_connect_timeout <= loops) {
                  atomic_clear_bit(&general_states, TRIGGER_RECV);
                  dtls_cmd_trigger("incoming connect", false, 1);
               }
            } else
#endif /* CONFIG_UDP_EDRX_WAKEUP_ENABLE */
               if (!atomic_test_bit(&general_states, LTE_CONNECTED)) {
                  atomic_clear_bit(&general_states, TRIGGER_RECV);
                  dtls_coap_set_request_state("disconnect", app, NONE);
                  dtls_info("Disconnected after %d s", loops);
               }
         } else if (app->request_state != NONE) {
            dtls_log_state();
            dtls_info("%s wait state %d, %d s", type, app->request_state, loops);
         }
      } else { /* ok */
         if (udp_poll[0].revents & POLLIN) {
            uint8_t flight = app->dtls_flight;
            recvfrom_peer(app, dtls_context);
            if (flight && flight < app->dtls_flight) {
               loops = 0;
            }
            if (app->request_state == SEND_ACK) {
               app->coap_handler.get_message = coap_client_message;
               sendto_peer(app, dtls_context);
               dtls_coap_success(app);
               dtls_info("CoAP ACK sent.");
            } else if (!app->dtls_pending && app->send_request_pending) {
               dtls_info("DTLS finished, send coap request.");
               app->send_request_pending = 0;
               loops = 0;
               app->retransmission = 0;
               app->start_time = k_uptime_get();
               sendto_peer(app, dtls_context);
            }
            if (!lte_power_on_off && app->rai && dtls_no_pending_request(app->request_state)) {
               modem_set_rai_mode(RAI_MODE_NOW, app->fd);
            }
            if (app->request_state == NONE &&
                app->protocol == PROTOCOL_COAP_DTLS &&
                !app->keep_connection &&
                !app->dtls_pending) {
               dtls_pending(app);
               ui_led_op(LED_DTLS, LED_CLEAR);
            }
         } else if (udp_poll[0].revents & (POLLERR | POLLNVAL)) {
            dtls_info("Poll: 0x%x", udp_poll[0].revents);
            if (check_socket(app)) {
               k_sleep(K_MSEC(1000));
            }
         }
#if defined(CONFIG_UDP_EDRX_WAKEUP_ENABLE)
         if (udp_ports_to_poll > 1 && udp_poll[1].revents & POLLIN) {
            recvfrom_peer2(app);
         } else if (udp_poll[1].revents & (POLLERR | POLLNVAL)) {
            dtls_info("Poll2: 0x%x", udp_poll[1].revents);
            if (check_socket(app)) {
               k_sleep(K_MSEC(1000));
            }
         }
#endif /* CONFIG_UDP_EDRX_WAKEUP_ENABLE */
      }
   }

   dtls_info("Exit.");
   dtls_free_context(dtls_context);
   return 0;
}

static void dump_destination(const dtls_app_data_t *app)
{
   char value[MAX_SETTINGS_VALUE_LENGTH];
   char ipv4_addr[NET_IPV4_ADDR_LEN] = {0};
   const char *scheme = "";

   inet_ntop(AF_INET, &app->destination.addr.sin.sin_addr.s_addr, ipv4_addr,
             sizeof(ipv4_addr));
   switch (app->protocol) {
      case PROTOCOL_COAP_DTLS:
         scheme = "coaps ";
         break;
      case PROTOCOL_COAP_UDP:
         scheme = "coap ";
         break;
   }
   dtls_info("Destination: %s'%s'", scheme, app->host);
   if (app->destination.size) {
      if (strcmp(app->host, ipv4_addr)) {
         dtls_info("IPv4 Address found %s", ipv4_addr);
      }
   } else {
      dtls_info("DNS lookup pending ...");
   }
   dtls_info("Port       : %u", ntohs(app->destination.addr.sin.sin_port));
   if (appl_settings_get_coap_path(value, sizeof(value))) {
      dtls_info("CoAP-path  : '%s'", value);
   }
   if (appl_settings_get_coap_query(value, sizeof(value))) {
      dtls_info("CoAP-query : '%s'", value);
   }
}

static int init_destination(dtls_app_data_t *app)
{
   int rc = -ENOENT;

   appl_settings_get_destination(app->host, sizeof(app->host));

   if (app->host[0]) {
      int count = 0;
      struct addrinfo *result = NULL;
      struct addrinfo hints = {
          .ai_family = AF_INET,
          .ai_socktype = SOCK_DGRAM};

      dtls_info("DNS lookup: %s", app->host);
      watchdog_feed();
      rc = getaddrinfo(app->host, NULL, &hints, &result);
      while (rc == -EAGAIN && count < 10) {
         k_sleep(K_MSEC(1000));
         ++count;
         watchdog_feed();
         rc = getaddrinfo(app->host, NULL, &hints, &result);
      }
      if (rc < 0) {
         dtls_warn("ERROR: getaddrinfo failed %d %s", rc, strerror(-rc));
      } else if (result == NULL) {
         dtls_warn("ERROR: Address not found");
         rc = -ENOENT;
      } else {
         /* Free the address. */
         app->destination.addr.sin = *((struct sockaddr_in *)result->ai_addr);
         freeaddrinfo(result);
      }
   }
   if (rc) {
      return rc;
   }
   app->destination.addr.sin.sin_port = htons(appl_settings_get_destination_port(app->protocol == PROTOCOL_COAP_DTLS));
   app->destination.size = sizeof(struct sockaddr_in);
   dump_destination(app);
   return 0;
}

#ifdef CONFIG_SH_CMD

static int sh_cmd_send(const char *parameter)
{
   size_t len = strlen(parameter);
   LOG_INF(">> send %s", parameter);

   if (dtls_pending_request(app_data_context.request_state)) {
      dtls_info("Busy, request pending ... (state %d)", app_data_context.request_state);
      return -EBUSY;
   }

   if (parameter && len) {
      int res = 0;
      if (len > sizeof(send_buffer)) {
         len = sizeof(send_buffer);
      }
      k_mutex_lock(&send_buffer_mutex, K_FOREVER);
      if (send_buffer_len) {
         res = -EBUSY;
      } else {
         memmove(send_buffer, parameter, len);
         send_buffer_len = len;
      }
      k_mutex_unlock(&send_buffer_mutex);
      if (res) {
         dtls_info("Busy, custom request pending ...");
         return res;
      }
   }

   if (!atomic_test_bit(&general_states, LTE_CONNECTED)) {
      ui_led_op(LED_COLOR_BLUE, LED_SET);
   }
   dtls_cmd_trigger("cmd", true, 3);
   if (atomic_test_bit(&general_states, LTE_CONNECTED)) {
      ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
   }
   return 0;
}

static void sh_cmd_send_help(void)
{
   LOG_INF("> help send:");
   LOG_INF("  send            : send application message.");
   LOG_INF("  send <message>  : send provided message.");
}

static int sh_cmd_send_result(const char *parameter)
{
   LOG_INF(">> send result");
   coap_send_flags_next = COAP_SEND_FLAG_NET_SCAN_INFO;
   dtls_cmd_trigger("result", false, 3);
   return 0;
}

static int sh_cmd_send_interval(const char *parameter)
{
   int res = 0;
   int interval = get_send_interval();
   char unit = 's';
   const char *cur = parameter;
   char value[10];

   memset(value, 0, sizeof(value));
   cur = parse_next_text(cur, ' ', value, sizeof(value));

   if (value[0]) {
      int new_interval = interval;
      res = sscanf(value, "%u%c", &new_interval, &unit);
      if (res >= 1) {
         if (unit == 's' || unit == 'h' || unit == 'm') {
            int interval_s = new_interval;
            if (unit == 'h') {
               interval_s *= 3600;
            } else if (unit == 'm') {
               interval_s *= 60;
            }
            if (interval != interval_s) {
               LOG_INF("set send interval %u%c", new_interval, unit);
               set_send_interval(interval_s);
               sh_cmd_append("send", K_MSEC(2000));
            } else {
               LOG_INF("send interval %u%c already active", new_interval, unit);
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
      } else if ((interval % 60) == 0) {
         LOG_INF("send interval %um", interval / 60);
      } else {
         LOG_INF("send interval %us", interval);
      }
   }
   return res;
}

static void sh_cmd_send_interval_help(void)
{
   LOG_INF("> help interval:");
   LOG_INF("  interval               : read send interval.");
   LOG_INF("  interval <time>[s|m|h] : set send interval.");
   LOG_INF("        <time>|<time>s   : interval in seconds.");
   LOG_INF("               <time>m   : interval in minutes.");
   LOG_INF("               <time>h   : interval in hours.");
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

static int sh_cmd_edrx_wakeup_on_connect_timeout(const char *parameter)
{
   int res = 0;
   uint32_t timeout = edrx_wakeup_on_connect_timeout;
   const char *cur = parameter;
   char value[10];

   memset(value, 0, sizeof(value));
   cur = parse_next_text(cur, ' ', value, sizeof(value));

   if (value[0]) {
      res = sscanf(value, "%u", &timeout);
      if (res == 1) {
         edrx_wakeup_on_connect_timeout = timeout;
         res = 0;
         cur = "set ";
      } else {
         res = -EINVAL;
      }
   } else {
      cur = "";
   }
   if (!res) {
      if (!edrx_wakeup_on_connect_timeout) {
         LOG_INF("%sno edrx wakeup on connect.", cur);
      } else {
         LOG_INF("%sedrx wakeup on connect timeout %us", cur, edrx_wakeup_on_connect_timeout);
      }
   }
   return res;
}

static void sh_cmd_edrx_wakeup_on_connect_timeout_help(void)
{
   LOG_INF("> help ewoc:");
   LOG_INF("  ewoc        : read edrx wakeup on connect timeout. 0 disabled.");
   LOG_INF("  ewoc <time> : set edrx wakeup on connect timeout in seconds. 0 to disable.");
}

typedef struct flags_definition {
   const char *name;
   const char *desc;
   const int flag;
} flags_definition_t;

static flags_definition_t coap_send_flags_definitions[] = {
    {.name = "nores", .desc = "request without response", .flag = COAP_SEND_FLAG_NO_RESPONSE},
    {.name = "init", .desc = "initial infos", .flag = COAP_SEND_FLAG_INITIAL},
    {.name = "min", .desc = "minimal infos", .flag = COAP_SEND_FLAG_MINIMAL},
    {.name = "dev", .desc = "device info", .flag = COAP_SEND_FLAG_MODEM_INFO},
    {.name = "sim", .desc = "sim-card info", .flag = COAP_SEND_FLAG_SIM_INFO},
    {.name = "net", .desc = "network info", .flag = COAP_SEND_FLAG_NET_INFO},
    {.name = "stat", .desc = "network statistics", .flag = COAP_SEND_FLAG_NET_STATS},
    {.name = "env", .desc = "environment info", .flag = COAP_SEND_FLAG_ENV_INFO},
    {.name = "scan", .desc = "network scan result", .flag = COAP_SEND_FLAG_NET_SCAN_INFO},
#ifdef CONFIG_ADC_SCALE
    {.name = "scale", .desc = "scale info", .flag = COAP_SEND_FLAG_SCALE_INFO},
#else  /* CONFIG_ADC_SCALE */
    {.name = "scale", .desc = "scale info", .flag = 0},
#endif /* CONFIG_ADC_SCALE */
#ifdef CONFIG_LOCATION_ENABLE
    {.name = "loc", .desc = "location info", .flag = COAP_SEND_FLAG_LOCATION_INFO},
#else  /* CONFIG_LOCATION_ENABLE */
    {.name = "loc", .desc = "location info", .flag = 0},
#endif /* CONFIG_LOCATION_ENABLE */
    {.name = NULL, .desc = NULL, .flag = 0},
};

static int sh_cmd_get_coap_sendflag(const char *value)
{
   for (int index = 0; coap_send_flags_definitions[index].name; ++index) {
      if (!stricmp(value, coap_send_flags_definitions[index].name)) {
         return coap_send_flags_definitions[index].flag;
      }
   }
   return -EINVAL;
}

static int sh_cmd_dump_coap_sendflags(char *buf, size_t len, int flags)
{
   int idx = 0;

   for (int index = 0; coap_send_flags_definitions[index].name; ++index) {
      if (flags & coap_send_flags_definitions[index].flag) {
         idx += snprintf(&buf[idx], len - idx, "%s ", coap_send_flags_definitions[index].name);
         if (idx >= len) {
            break;
         }
      }
   }
   if (idx > 0) {
      buf[--idx] = 0;
   }
   return idx;
}

static int sh_cmd_coap_sendflags(const char *parameter)
{
   int res = 0;
   long flags = 0;
   const char *cur = parameter;

   if (cur[0]) {
      cur = parse_next_long_text(cur, ' ', 0, &flags);
      if (cur != parameter) {
         coap_send_flags = (int)flags;
      } else {
         char value[10];
         cur = parse_next_text(cur, ' ', value, sizeof(value));
         while (value[0]) {
            int flag = sh_cmd_get_coap_sendflag(value);
            if (flag >= 0) {
               flags |= flag;
            } else {
               return flag;
            }
            cur = parse_next_text(cur, ' ', value, sizeof(value));
         }
         coap_send_flags = (int)flags;
      }
      coap_send_flags_next = coap_send_flags;
      LOG_INF("set coap sendflags %d/0x%x", coap_send_flags, coap_send_flags);
   } else {
      char line[96] = {0};
      int idx = 0;

      if (coap_send_flags != coap_send_flags_next) {
         LOG_INF("coap sendflags %d/0x%x (next %d/0x%x)", coap_send_flags, coap_send_flags,
                 coap_send_flags_next, coap_send_flags_next);
         idx = sh_cmd_dump_coap_sendflags(line, sizeof(line), coap_send_flags);
         if (idx > 0) {
            LOG_INF("   %s", line);
         }
         idx = sh_cmd_dump_coap_sendflags(line, sizeof(line), coap_send_flags_next);
         if (idx > 0) {
            LOG_INF("   next: %s", line);
         }
      } else {
         LOG_INF("coap sendflags %d/0x%x", coap_send_flags, coap_send_flags);
         idx = sh_cmd_dump_coap_sendflags(line, sizeof(line), coap_send_flags);
         if (idx > 0) {
            LOG_INF("   %s", line);
         }
      }
   }
   return res;
}

static void sh_cmd_coap_sendflags_help(void)
{
   LOG_INF("> help sendflags:");
   LOG_INF("  sendflags                  : read coap sendflags.");
   LOG_INF("  sendflags <flags>          : set coap sendflags.");
   LOG_INF("            <flags>          : flags in decimal.");
   LOG_INF("            <0xflags>        : flags in hexadecimal.");
   LOG_INF("  sendflags <id> [<id2> ...] : set coap from names.");
   for (int index = 0; coap_send_flags_definitions[index].name; ++index) {
      LOG_INF("            %-17s: %s (flag %d).",
              coap_send_flags_definitions[index].name,
              coap_send_flags_definitions[index].desc,
              coap_send_flags_definitions[index].flag);
   }
}

static int sh_cmd_onoff(const char *parameter)
{
   ARG_UNUSED(parameter);
   int res = 0;
   long value = lte_power_on_off ? 1 : 0;
   const char *cur = parameter;

   if (cur[0]) {
      cur = parse_next_long_text(cur, ' ', 0, &value);
      if (cur != parameter) {
         lte_power_on_off = value ? true : false;
      } else {
         LOG_INF("onoff '%s', value not supported!", parameter);
         res = -EINVAL;
      }
   } else {
      LOG_INF("onoff %d", (int)value);
   }

   return 0;
}

static void sh_cmd_onoff_help(void)
{
   LOG_INF("> help onoff:");
   LOG_INF("  onoff                  : show on/off mode.");
   LOG_INF("  onoff 1                : enable on/off mode.");
   LOG_INF("  onoff 0                : disable on/off mode.");
}

static int sh_cmd_restart(const char *parameter)
{
   ARG_UNUSED(parameter);
   restart(ERROR_CODE_REBOOT_CMD, true);
   return 0;
}

static int sh_cmd_destination(const char *parameter)
{
   ARG_UNUSED(parameter);
   dump_destination(&app_data_context);
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
   const char *cur = parameter;
   char value[10];

   memset(value, 0, sizeof(value));
   cur = parse_next_text(cur, ' ', value, sizeof(value));

   if (app_data_context.dtls_cipher_suite) {
      LOG_INF("DTLS: %s, %s", app_data_context.dtls_cipher_suite, app_data_context.dtls_cid ? "CID" : "(no CID)");
   }

   if (!strcmp(value, "reset")) {
      if (dtls_pending(&app_data_context)) {
         LOG_INF("DTLS: reset => new handshake");
      } else {
         LOG_INF("DTLS: not active");
      }
   }
   return 0;
}

static void sh_cmd_dtls_help(void)
{
   LOG_INF("> help dtls:");
   LOG_INF("  dtls       : show dtls details.");
   LOG_INF("  dtls reset : reset dtls session.");
}

SH_CMD(send, NULL, "send message.", sh_cmd_send, sh_cmd_send_help, 0);
SH_CMD(sendresult, NULL, "send result message.", sh_cmd_send_result, NULL, 0);
SH_CMD(interval, NULL, "send interval.", sh_cmd_send_interval, sh_cmd_send_interval_help, 0);
SH_CMD(timeout, NULL, "initial coap timeout.", sh_cmd_coap_timeout, sh_cmd_send_coap_timeout_help, 0);
SH_CMD(ewoc, NULL, "edrx wakeup on connect timeout.", sh_cmd_edrx_wakeup_on_connect_timeout, sh_cmd_edrx_wakeup_on_connect_timeout_help, 0);
SH_CMD(sendflags, NULL, "sendflags.", sh_cmd_coap_sendflags, sh_cmd_coap_sendflags_help, 0);
SH_CMD(onoff, NULL, "on/off mode.", sh_cmd_onoff, sh_cmd_onoff_help, 0);
SH_CMD(restart, NULL, "try to switch off the modem and restart device.", sh_cmd_restart, NULL, 0);
SH_CMD(dest, NULL, "show destination.", sh_cmd_destination, NULL, 0);
SH_CMD(time, NULL, "show system time.", sh_cmd_time, NULL, 0);
SH_CMD(dtls, NULL, "show dtls information.", sh_cmd_dtls, sh_cmd_dtls_help, 0);
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

static void init(int config, int *protocol)
{
   char imei[MODEM_ID_SIZE];
   char scheme[12];

   memset(&imei, 0, sizeof(imei));
   modem_get_imei(imei, sizeof(imei) - 1);

   dtls_init();
   appl_settings_init(imei, &cb);
   modem_init(config, dtls_lte_state_handler);

   if (protocol) {
      appl_settings_get_scheme(scheme, sizeof(scheme));
      if (!stricmp(scheme, "coaps")) {
         *protocol = PROTOCOL_COAP_DTLS;
      } else if (!stricmp(scheme, "coap")) {
         *protocol = PROTOCOL_COAP_UDP;
      }
   }
}

static const led_task_t led_no_host[] = {
    {.time_ms = 1000, .led = LED_COLOR_ALL, .op = LED_SET},
    {.time_ms = 1000, .led = LED_COLOR_ALL, .op = LED_CLEAR},
    {.time_ms = 1000, .led = LED_COLOR_BLUE, .op = LED_SET},
    {.time_ms = 1000, .led = LED_COLOR_BLUE, .op = LED_CLEAR},
    {.loop = 2, .time_ms = 1000, .led = LED_COLOR_RED, .op = LED_SET},
    {.time_ms = 1000, .led = LED_COLOR_RED, .op = LED_CLEAR},
    {.time_ms = 0, .led = LED_COLOR_RED, .op = LED_CLEAR},
};

int main(void)
{
   int config = 0;
   int reset_cause = 0;
   uint16_t reboot_cause = 0;

   memset(&app_data_context, 0, sizeof(app_data_context));
   memset(transmissions, 0, sizeof(transmissions));
   memset(appl_buffer, 0, sizeof(appl_buffer));

   app_data_context.protocol = -1;

   LOG_INF("CoAP/DTLS 1.2 CID sample %s has started", appl_get_version());
   appl_reset_cause(&reset_cause, &reboot_cause);

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
         app_data_context.protocol = 1;
         dtls_info("CoAP/UDP");
      }
   }
#elif CONFIG_PROTOCOL_CONFIG_SWITCH
   if (config >= 0) {
      app_data_context.protocol = config >> 2;
   }
#endif
   if (app_data_context.protocol < 0) {
#ifdef CONFIG_PROTOCOL_MODE_UDP
      app_data_context.protocol = PROTOCOL_COAP_UDP;
#elif CONFIG_PROTOCOL_MODE_DTLS
      app_data_context.protocol = PROTOCOL_COAP_DTLS;
#else
      app_data_context.protocol = PROTOCOL_COAP_DTLS;
#endif
   }

   if (config < 0) {
      config = 0;
   }

   init(config, &app_data_context.protocol);
   switch (app_data_context.protocol) {
      case PROTOCOL_COAP_DTLS:
#ifndef CONFIG_DTLS_ALWAYS_HANDSHAKE
         app_data_context.keep_connection = 1;
#endif
         dtls_info("CoAP/DTLS 1.2 CID");
         break;
      case PROTOCOL_COAP_UDP:
         dtls_info("CoAP/UDP");
         break;
   }

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
      atomic_set_bit(&general_states, APPL_READY);
      if (dtls_network_searching(K_MINUTES(CONFIG_MODEM_SEARCH_TIMEOUT_REBOOT))) {
         restart(ERROR_CODE_INIT_NO_LTE, false);
      }
   }
   atomic_set_bit(&general_states, APPL_READY);
   coap_client_init();

   appl_settings_get_destination(app_data_context.host, sizeof(app_data_context.host));
   if (!app_data_context.host[0]) {
      // no hostname
      while (true) {
         ui_led_tasks(led_no_host);
         k_sem_take(&dtls_trigger_msg, K_MINUTES(10));
      }
   }
   init_destination(&app_data_context);

   dtls_set_send_trigger("initial message");
   dtls_trigger("initial message", true);
   dtls_loop(&app_data_context, (reset_cause & FLAG_REBOOT_RETRY) ? ERROR_DETAIL(reboot_cause) : 0);

   return 0;
}
#endif /* CONFIG_ALL_POWER_OFF */
