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

#include "coap_client.h"
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

#define COAP_ACK_TIMEOUT 3
#define ADD_ACK_TIMEOUT 3

#define LED_APPLICATION LED_LTE_1
#define LED_DTLS LED_LTE_2

LOG_MODULE_REGISTER(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

typedef enum {
   NONE,
   SEND,
   RESEND,
   RECEIVE,
   WAIT_RESPONSE,
   SEND_ACK,
   WAIT_SUSPEND,
} request_state_t;

typedef struct dtls_app_data_t {
   int fd;
} dtls_app_data_t;

static volatile bool network_registered = false;
static volatile bool network_ready = false;
static volatile bool network_sleeping = false;
static volatile bool lte_connected_send = false;
static volatile unsigned int lte_connections = 0;
static volatile unsigned long connected_time = 0;

static volatile bool dtls_pending = false;
static volatile request_state_t request_state = NONE;

static volatile bool trigger_restart_modem = false;

static volatile bool lte_power_off = false;

static bool lte_power_on_off = false;

#ifdef CONFIG_ADXL362_MOTION_DETECTION
static volatile bool moved = false;
#endif
static unsigned long connect_time = 0;
static unsigned long response_time = 0;
static unsigned int transmission = 0;
static int timeout = 0;

#define MAX_READ_BUF 1600
static uint8_t receive_buffer[MAX_READ_BUF];

#define RTT_SLOTS 9
#define RTT_INTERVAL 2000
static unsigned int rtts[RTT_SLOTS + 2] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 20};

K_SEM_DEFINE(dtls_trigger_msg, 0, 1);
K_SEM_DEFINE(dtls_trigger_search, 0, 1);

static void reboot()
{
   modem_power_off();
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
   k_sleep(K_MSEC(500));
   sys_reboot(SYS_REBOOT_COLD);
}

static void restart_modem()
{
   int timeout_seconds = CONFIG_MODEM_SEARCH_TIMEOUT;
   int sleep_minutes = trigger_restart_modem ? 0 : 15;

   dtls_warn("> reconnect modem ...");
   k_sem_reset(&dtls_trigger_search);

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
         } else {
            dtls_info("> modem reboot");
            reboot();
         }
      }
      trigger_restart_modem = false;
      modem_set_normal();
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

   dtls_info("> reopen socket ...");
   modem_set_rai_mode(RAI_OFF, app->fd);

   err = modem_wait_ready(K_SECONDS(CONFIG_MODEM_SEARCH_TIMEOUT));
   if (err) {
      restart_modem();
   }
   (void)close(app->fd);
   app->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
   if (app->fd < 0) {
      dtls_warn("> reopen UDP socket failed, %d, errno %d (%s), reboot", app->fd, errno, strerror(errno));
      reboot();
   }

   modem_set_rai_mode(RAI_OFF, app->fd);
   dtls_info("> reopend socket.");
}

static int check_socket(dtls_app_data_t *app, bool event)
{
   int error = 0;
   socklen_t len = sizeof(error);
   int result = getsockopt(app->fd, SOL_SOCKET, SO_ERROR, &error, &len);
   if (result) {
      dtls_info("I/O: get last socket error failed, %d (%s)", errno, strerror(errno));
      error = errno;
   } else if (error) {
      dtls_info("I/O: last socket error %d (%s)", error, strerror(error));
   } else {
      dtls_debug("No socket error.");
   }
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
   if (request_state == NONE || request_state == WAIT_SUSPEND) {
      k_sem_give(&dtls_trigger_msg);
   }
}

static void dtls_manual_trigger(int duration)
{
   if (duration == 1) {
      trigger_restart_modem = true;
   }
   ui_led_op(LED_COLOR_RED, LED_CLEAR);
   k_sem_give(&dtls_trigger_search);
   dtls_trigger();
}

#if CONFIG_COAP_SEND_INTERVAL > 0

static void dtls_timer_trigger_fn(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(dtls_timer_trigger_work, dtls_timer_trigger_fn);

static void dtls_timer_trigger_fn(struct k_work *work)
{
   if (request_state == NONE) {
      dtls_trigger();
   } else {
      work_schedule_for_io_queue(&dtls_timer_trigger_work, K_SECONDS(CONFIG_COAP_SEND_INTERVAL));
   }
}

#elif CONFIG_COAP_WAKEUP_SEND_INTERVAL > 0
static void dtls_wakeup_trigger(void)
{
   static unsigned long wakeup_next_sent = 0;
   unsigned long now = (unsigned long)k_uptime_get();

   if (request_state == NONE) {
      if ((now - wakeup_next_sent) > 0) {
         dtls_trigger();
      }
   } else {
      dtls_info("Wakeup with request state %d", request_state);
   }
   wakeup_next_sent = now + ((CONFIG_COAP_WAKEUP_SEND_INTERVAL)*MSEC_PER_SEC);
}
#endif

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
   work_schedule_for_io_queue(&dtls_timer_trigger_work, K_SECONDS(CONFIG_COAP_SEND_INTERVAL));
#endif
   if (coap_client_time(buf, sizeof(buf))) {
      dtls_info("%s", buf);
   }
}

static void dtls_coap_success(void)
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
   request_state = WAIT_SUSPEND;
   ui_led_op(LED_COLOR_RED, LED_CLEAR);
   ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
   ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
   dtls_info("%u/%ldms/%ldms: success", lte_connections, time1, time2);
   if (transmission <= COAP_MAX_RETRANSMISSION) {
      transmissions[transmission]++;
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
   dtls_coap_next();
}

static void dtls_coap_failure(void)
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
   request_state = WAIT_SUSPEND;
   ui_led_op(LED_COLOR_RED, LED_SET);
   ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
   ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
   dtls_info("%u/%ldms/%ldms: failure", lte_connections, time1, time2);
   transmissions[COAP_MAX_RETRANSMISSION + 1]++;
   dtls_coap_next();
}

static int
read_from_peer(dtls_context_t *ctx, session_t *session, uint8 *data, size_t len)
{
   (void)ctx;
   (void)session;

   int err = coap_client_parse_data(data, len);
   if (err < 0) {
      return err;
   }

   switch (err) {
      case PARSE_IGN:
         break;
      case PARSE_RST:
         if (NONE != request_state) {
            response_time = (unsigned long)k_uptime_get();
            dtls_coap_failure();
         }
         break;
      case PARSE_ACK:
         if (NONE != request_state) {
            request_state = WAIT_RESPONSE;
         }
         break;
      case PARSE_RESPONSE:
         if (NONE != request_state) {
            response_time = (unsigned long)k_uptime_get();
            dtls_coap_success();
         }
         break;
      case PARSE_CON_RESPONSE:
         if (NONE != request_state) {
            response_time = (unsigned long)k_uptime_get();
            request_state = SEND_ACK;
         }
         break;
   }

   return 0;
}

static void prepare_socket(int fd)
{
   lte_connected_send = false;
   if (!dtls_pending && !lte_power_on_off) {
#ifdef CONFIG_COAP_NO_RESPONSE_ENABLE
      modem_set_rai_mode(RAI_LAST, fd);
#else
      modem_set_rai_mode(RAI_ONE_RESPONSE, fd);
#endif
   } else {
      modem_set_rai_mode(RAI_OFF, fd);
   }
}

static int
send_to_peer(dtls_app_data_t *app, session_t *session, const uint8_t *data, size_t len)
{
   int result = 0;
   const char *tag = dtls_pending ? "hs_" : "";

   if (!lte_power_on_off) {
      connect_time = (unsigned long)k_uptime_get();
      prepare_socket(app->fd);
   }
   result = sendto(app->fd, data, len, MSG_DONTWAIT, &session->addr.sa, session->size);
   if (result < 0) {
      dtls_warn("%ssend_to_peer failed: %d, errno %d (%s)", tag, result, errno, strerror(errno));
      return result;
   }
   if (network_ready) {
      modem_set_transmission_time();
   }
   if (RESEND != request_state) {
      timeout = COAP_ACK_TIMEOUT;
   }
#ifndef NDEBUG
   /* logging */
   if (SEND == request_state) {
      if (network_ready) {
         dtls_info("%ssent_to_peer %d", tag, result);
      } else {
         dtls_info("%ssend_to_peer %d", tag, result);
      }
   } else if (RESEND == request_state) {
      if (network_ready) {
         dtls_info("%sresent_to_peer %d", tag, result);
      } else {
         dtls_info("%sresend_to_peer %d", tag, result);
      }
   } else if (RECEIVE == request_state) {
      if (network_ready) {
         dtls_info("%sunintended resent_to_peer %d", tag, result);
      } else {
         dtls_info("%sunintended resend_to_peer %d", tag, result);
      }
   }
#endif
   if (!dtls_pending && network_ready) {
      if (request_state == SEND || request_state == RESEND) {
#ifdef CONFIG_COAP_NO_RESPONSE_ENABLE
         dtls_coap_success();
#else
         request_state = RECEIVE;
#endif
      }
   }
   return result;
}

static int
dtls_send_to_peer(dtls_context_t *ctx,
                  session_t *session, uint8 *data, size_t len)
{

   dtls_app_data_t *app = (dtls_app_data_t *)dtls_get_app_data(ctx);
   int res = send_to_peer(app, session, data, len);
   if (dtls_pending && res >= 0) {
      if (network_ready) {
         request_state = RECEIVE;
      } else if (RESEND != request_state) {
         request_state = SEND;
      }
   }
   return res;
}

static int
dtls_handle_event(dtls_context_t *ctx, session_t *session,
                  dtls_alert_level_t level, unsigned short code)
{
   if (DTLS_EVENT_CONNECTED == code) {
      dtls_info("dtls connected.");
      dtls_pending = false;
      request_state = NONE;
      ui_led_op(LED_COLOR_RED, LED_CLEAR);
      ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
      ui_led_op(LED_DTLS, LED_SET);
   } else if (DTLS_EVENT_CONNECT == code) {
      dtls_info("dtls connect ...");
      dtls_pending = true;
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
      if (dtls_pending && request_state == RESEND) {
         request_state = SEND;
      }
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
   } else {
      dtls_coap_failure();
   }
   return result;
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
            ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
            ui_led_op(LED_COLOR_RED, LED_CLEAR);
         }
      } else {
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
      if (!dtls_pending) {
         if (active) {
            ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
            ui_led_op(LED_COLOR_GREEN, LED_SET);
         } else if (NONE == request_state || WAIT_SUSPEND == request_state) {
            ui_led_op(LED_COLOR_RED, LED_CLEAR);
            ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
         } else {
            ui_led_op(LED_COLOR_BLUE, LED_SET);
            ui_led_op(LED_COLOR_GREEN, LED_SET);
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
      power_manager_suspend(active);
      network_sleeping = active;
#if CONFIG_COAP_WAKEUP_SEND_INTERVAL > 0 && CONFIG_COAP_SEND_INTERVAL == 0
      if (!active) {
         dtls_wakeup_trigger();
      }
#endif
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
   dtls_app_data_t dtls_add_data = {0};
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

   dtls_add_data.fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

   if (dtls_add_data.fd < 0) {
      dtls_warn("Failed to create UDP socket: %d (%s)", errno, strerror(errno));
      reboot();
   }
   modem_set_rai_mode(RAI_OFF, dtls_add_data.fd);

   if (flags & FLAG_TLS) {
      dtls_init();
      dtls_context = dtls_new_context(&dtls_add_data);
      if (!dtls_context) {
         dtls_emerg("cannot create context");
         reboot();
      }
      dtls_credentials_init_handler(&cb);
      dtls_set_handler(dtls_context, &cb);
      dtls_pending = true;
   }
#if defined(CONFIG_UDP_AS_RAI_ENABLE) && defined(USE_SO_RAI_NO_DATA)
   // using SO_RAI_NO_DATA requires a destination, for what ever
   connect(fd, (struct sockaddr *)&dst.addr.sin, sizeof(struct sockaddr_in));
#endif

   timeout = COAP_ACK_TIMEOUT;
   if ((flags & FLAG_KEEP_CONNECTION) && (flags & FLAG_TLS)) {
      request_state = SEND;
      dtls_connect(dtls_context, dst);
   } else {
      request_state = NONE;
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
      if (trigger_restart_modem) {
         restart_modem();
         reopen_socket(&dtls_add_data);
      }
#ifdef USE_POLL
      udp_poll.fd = dtls_add_data.fd;
      udp_poll.events = POLLIN;
      udp_poll.revents = 0;
#else
      FD_ZERO(&rfds);
      FD_ZERO(&efds);
      FD_SET(dtls_add_data.fd, &rfds);
      FD_SET(dtls_add_data.fd, &efds);
#endif

      if (request_state != NONE) {
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
         if (k_sem_take(&dtls_trigger_msg, K_SECONDS(60)) == 0) {
            power_manager_suspend(false);
            ui_led_op(LED_APPLICATION, LED_SET);
#if CONFIG_COAP_SEND_INTERVAL > 0
            work_reschedule_for_io_queue(&dtls_timer_trigger_work, K_SECONDS(CONFIG_COAP_SEND_INTERVAL));
#endif
            if (lte_power_off) {
               dtls_info("modem on");
               lte_power_off = false;
               trigger_restart_modem = false;
               connect_time = (unsigned long)k_uptime_get();
               modem_set_normal();
               modem_start(K_SECONDS(CONFIG_MODEM_SEARCH_TIMEOUT));
               reopen_socket(&dtls_add_data);
            } else if (trigger_restart_modem) {
               restart_modem();
               reopen_socket(&dtls_add_data);
            } else {
               check_socket(&dtls_add_data, false);
            }
            request_state = SEND;
            loops = 0;
            transmission = 0;
            if (coap_client_prepare_post() < 0) {
               dtls_coap_failure();
            } else {
               if (dtls_pending) {
                  dtls_peer_t *peer = dtls_get_peer(dtls_context, dst);
                  if (peer) {
                     dtls_reset_peer(dtls_context, peer);
                  }
                  ui_led_op(LED_COLOR_GREEN, LED_SET);
                  dtls_connect(dtls_context, dst);
                  send_request = true;
               } else {
                  sendto_peer(&dtls_add_data, dst, dtls_context);
               }
            }
         }
      }
      if (result < 0) { /* error */
         if (errno != EINTR) {
            dtls_warn("select failed: errno %d (%s)", result, strerror(errno));
         }
      } else if (result == 0) { /* timeout */
         if (request_state == SEND || request_state == RESEND) {
            if (lte_connected_send) {
               loops = 0;
               time = connected_time - connect_time;
               if (time < 0)
                  time = -1;
               if (request_state == SEND) {
                  dtls_info("%d/%d/%u-%ld ms: connected => sent", network_ready, lte_connected_send, lte_connections, time);
               } else {
                  dtls_info("%d/%d/%u-%ld ms: connected => resent", network_ready, lte_connected_send, lte_connections, time);
               }
#ifdef CONFIG_COAP_NO_RESPONSE_ENABLE
               dtls_coap_success();
#else
               request_state = RECEIVE;
#endif
            } else if (loops < 60) {
               ++loops;
               if (loops % 5 == 4) {
                  check_socket(&dtls_add_data, false);
               }
            } else {
               dtls_coap_failure();
               reopen_socket(&dtls_add_data);
            }
         } else if (request_state == RECEIVE) {
            int temp = timeout;
            if (!network_ready) {
               temp += ADD_ACK_TIMEOUT;
            }
            ++loops;
            dtls_info("CoAP wait %d/%d/%d", loops, timeout, temp);
            if (loops > temp) {
               result = -1;
               if (transmission < COAP_MAX_RETRANSMISSION) {
                  ++transmission;
                  loops = 0;
                  timeout <<= 1;
                  request_state = RESEND;

                  if (dtls_pending) {
                     dtls_info("hs resend, timeout %d", timeout);
                     dtls_check_retransmit(dtls_context, NULL);
                  } else {
                     dtls_info("CoAP request resend, timeout %d", timeout);
                     sendto_peer(&dtls_add_data, dst, dtls_context);
                  }
               } else {
                  // maximum retransmissions reached
                  dtls_coap_failure();
               }
            }
         } else if (request_state == WAIT_RESPONSE) {
            if (loops < 60) {
               ++loops;
            } else {
               dtls_coap_failure();
            }
         } else if (request_state == WAIT_SUSPEND) {
            if (network_sleeping) {
               request_state = NONE;
               dtls_info("CoAP suspend after %d", loops);
            } else {
               if (k_sem_count_get(&dtls_trigger_msg)) {
                  request_state = NONE;
               }
               ++loops;
            }
         } else {
            ++loops;
            dtls_info("CoAP wait state %d, %d", request_state, loops);
         }
      } else { /* ok */
#ifdef USE_POLL
         if (udp_poll.revents & POLLIN) {
#else
         if (FD_ISSET(dtls_add_data.fd, &rfds)) {
#endif
            recvfrom_peer(&dtls_add_data, dtls_context);
            if (request_state == SEND_ACK) {
               unsigned long temp_time = connect_time;
               sendto_peer(&dtls_add_data, dst, dtls_context);
               connect_time = temp_time;
               request_state = NONE;
               dtls_coap_success();
               dtls_info("CoAP ACK sent.");
            } else if (!dtls_pending && send_request) {
               send_request = false;
               loops = 0;
               request_state = SEND;
               transmission = 0;
               sendto_peer(&dtls_add_data, dst, dtls_context);
            }
            if (request_state == NONE && !lte_power_on_off) {
               modem_set_rai_mode(RAI_NOW, dtls_add_data.fd);
            }
            if (request_state == NONE &&
                (flags & FLAG_TLS) &&
                !(flags & FLAG_KEEP_CONNECTION) &&
                !dtls_pending) {
               dtls_pending = true;
               ui_led_op(LED_DTLS, LED_CLEAR);
            }
#ifdef USE_POLL
         } else if (udp_poll.revents & (POLLERR | POLLNVAL)) {
#else
         } else if (FD_ISSET(dtls_add_data.fd, &efds)) {
#endif
            result = check_socket(&dtls_add_data, true);
            if (result) {
               if (request_state == SEND || request_state == RESEND || request_state == RECEIVE) {
                  loops = 0;
                  request_state = SEND;
                  transmission = 0;
                  if (dtls_pending) {
                     dtls_info("hs send again");
                     dtls_check_retransmit(dtls_context, NULL);
                  } else {
                     dtls_info("CoAP request send again");
                     sendto_peer(&dtls_add_data, dst, dtls_context);
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

#ifdef CONFIG_COAP_SERVER_HOSTNAME
   int err;
   struct addrinfo *result;
   struct addrinfo hints = {
       .ai_family = AF_INET,
       .ai_socktype = SOCK_DGRAM};

   host = CONFIG_COAP_SERVER_HOSTNAME;

   err = getaddrinfo(CONFIG_COAP_SERVER_HOSTNAME, NULL, &hints, &result);
   while (-err == EAGAIN) {
      k_sleep(K_MSEC(1000));
      err = getaddrinfo(CONFIG_COAP_SERVER_HOSTNAME, NULL, &hints, &result);
   }
   if (err != 0) {
      dtls_warn("ERROR: getaddrinfo failed %d", err);
      return -EIO;
   }

   if (result == NULL) {
      dtls_warn("ERROR: Address not found");
      return -ENOENT;
   }

   destination->addr.sin = *((struct sockaddr_in *)result->ai_addr);

   /* Free the address. */
   freeaddrinfo(result);

#elif defined(CONFIG_COAP_SERVER_ADDRESS_STATIC)
   host = CONFIG_UDP_SERVER_ADDRESS_STATIC;
   int res = inet_pton(AF_INET, CONFIG_UDP_SERVER_ADDRESS_STATIC,
                       &(destination->addr.sin.sin_addr));
   if (res != 1) {
      dtls_warn("ERROR: inet_pton failed %d", res);
      return -EIO;
   }
#endif

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
   dtls_info("Destination: %s", host);
   dtls_info("IPv4 Address found %s", ipv4_addr);

   return 0;
}

void main(void)
{
   int config = 0;
   int protocol = -1;
   int flags = 0;
   char imei[24];
   session_t dst;

   LOG_INF("CoAP/DTLS 1.2 CID sample " CLIENT_VERSION " has started");

   dtls_set_log_level(DTLS_LOG_INFO);

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

   if (modem_start(K_SECONDS(CONFIG_MODEM_SEARCH_TIMEOUT)) != 0) {
      restart_modem();
   }

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
