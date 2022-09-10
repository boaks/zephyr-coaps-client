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

LOG_MODULE_REGISTER(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

typedef enum {
   NONE,
   SEND,
   RESEND,
   RECEIVE,
   WAIT_RESPONSE,
   SEND_ACK
} request_state_t;

static volatile int network_connected = 0;
static volatile int dtls_connected = 0;
static volatile int lte_connected = 0;
static volatile bool lte_connected_send = false;
static volatile unsigned int lte_connections = 0;
static volatile request_state_t request_state = NONE;
static volatile unsigned long connected_time = 0;
#ifdef CONFIG_ADXL362_MOTION_DETECTION
static volatile bool moved = false;
#endif
static unsigned long connect_time = 0;
static unsigned long response_time = 0;
static unsigned int transmission = 0;
static int timeout = 0;

/* the wakeup send interval is only effecitve on PSM wakeup */
/* the granularity of this time is therefore the PSM time */

//#define NETWORK_WAKEUP_SEND_INTERVAL_S (3600 * 1 - 100) /* approx. 1h */

#if (defined CONFIG_LTE_MODE_PREFERENCE_NBIOT_PLMN_PRIO || defined CONFIG_LTE_MODE_PREFERENCE_LTE_M_PLMN_PRIO)
#define NETWORK_TIMEOUT_S 360
#else
#define NETWORK_TIMEOUT_S 240
#endif

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

static void reconnect()
{
   int timeout_seconds = NETWORK_TIMEOUT_S;
   int sleep_minutes = 15;

   k_sem_reset(&dtls_trigger_search);

   while (!network_connected) {
      dtls_info("> modem offline (%d minutes)", sleep_minutes);
      modem_set_lte_offline();
      k_sleep(K_MSEC(2000));
      ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
      ui_led_op(LED_COLOR_RED, LED_CLEAR);
      ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
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
      modem_set_normal();
      timeout_seconds *= 2;
      dtls_info("> modem search network (%d minutes)", timeout_seconds / 60);
      if (modem_start(K_SECONDS(timeout_seconds)) == 0) {
         break;
      }
   }
}

static void reopen_socket(struct dtls_context_t *ctx)
{
   int err;
   dtls_warn("> reconnect modem");
   modem_set_rai(0);
   err = modem_start(K_SECONDS(NETWORK_TIMEOUT_S));
   if (err) {
      reconnect();
   }
   int *fd = (int *)dtls_get_app_data(ctx);
   (void)close(*fd);
   *fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
   if (*fd < 0) {
      dtls_warn("> reopen UDP socket failed, %d, errno %d (%s), reboot", *fd, errno, strerror(errno));
      reboot();
   }
   modem_set_rai(1);
}

static void dtls_trigger(void)
{
   if (request_state == NONE) {
      k_sem_give(&dtls_trigger_msg);
   }
}

static void dtls_manual_trigger(void)
{
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
      k_work_schedule(&dtls_timer_trigger_work, K_SECONDS(CONFIG_COAP_SEND_INTERVAL));
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
   wakeup_next_sent = now + ((CONFIG_COAP_WAKEUP_SEND_INTERVAL) * MSEC_PER_SEC);
}
#endif

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
   lte_connected_send = false;
   k_sem_reset(&dtls_trigger_msg);
   request_state = NONE;
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
#if CONFIG_COAP_SEND_INTERVAL > 0
   k_work_schedule(&dtls_timer_trigger_work, K_SECONDS(CONFIG_COAP_SEND_INTERVAL));
#endif
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
   lte_connected_send = false;
   k_sem_reset(&dtls_trigger_msg);
   request_state = NONE;
   ui_led_op(LED_COLOR_RED, LED_SET);
   ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
   ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
   dtls_info("%u/%ldms/%ldms: failure", lte_connections, time1, time2);
   transmissions[COAP_MAX_RETRANSMISSION + 1]++;
#if CONFIG_COAP_SEND_INTERVAL > 0
   k_work_schedule(&dtls_timer_trigger_work, K_SECONDS(CONFIG_COAP_SEND_INTERVAL));
#endif
}

static int
read_from_peer(struct dtls_context_t *ctx,
               session_t *session, uint8 *data, size_t len)
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

static inline bool lte_lost_network()
{
#ifdef __LINUX_ERRNO_EXTENSIONS__
   return (ENETDOWN == errno || ESHUTDOWN == errno || ENETUNREACH == errno);
#else
   return (ENETDOWN == errno || ENETUNREACH == errno);
#endif
}

static int
send_to_peer(struct dtls_context_t *ctx,
             session_t *session, uint8 *data, size_t len)
{

   int *fd = (int *)dtls_get_app_data(ctx);
   int res = 0;

   lte_connected_send = false;
   connect_time = (unsigned long)k_uptime_get();

   res = sendto(*fd, data, len, MSG_DONTWAIT, &session->addr.sa, session->size);
   if (res < 0) {
      dtls_warn("send_to_peer failed: %d, errno %d (%s)", res, errno, strerror(errno));
      if (lte_lost_network()) {
         reopen_socket(ctx);
         res = sendto(*fd, data, len, MSG_DONTWAIT, &session->addr.sa, session->size);
         if (res < 0) {
            dtls_warn("retry send_to_peer failed: %d, errno %d (%s)", res, errno, strerror(errno));
            if (lte_lost_network()) {
               network_connected = 0;
            }
         }
      }
   }
   if (res >= 0) {
      if (!dtls_connected) {
         dtls_dsrv_log_addr(DTLS_LOG_DEBUG, "peer", session);
         if (RESEND == request_state) {
            if (lte_connected) {
               request_state = RECEIVE;
               dtls_info("hs_resent_to_peer %d", res);
            } else {
               dtls_info("hs_resend_to_peer %d", res);
            }
         } else {
            timeout = COAP_ACK_TIMEOUT;
            if (lte_connected) {
               request_state = RECEIVE;
               dtls_info("hs_sent_to_peer %d", res);
            } else {
               request_state = SEND;
               dtls_info("hs_send_to_peer %d", res);
            }
         }
      } else if (SEND == request_state) {
         if (lte_connected) {
            request_state = RECEIVE;
            dtls_info("sent_to_peer %d", res);
         } else {
            dtls_info("send_to_peer %d", res);
         }
      } else if (RESEND == request_state) {
         if (lte_connected) {
            request_state = RECEIVE;
            dtls_info("resent_to_peer %d", res);
         } else {
            dtls_info("resend_to_peer %d", res);
         }
      } else if (RECEIVE == request_state) {
         if (lte_connected) {
            dtls_info("unintended resent_to_peer %d", res);
         } else {
            dtls_info("unintended resend_to_peer %d", res);
         }
      }
      if (lte_connected) {
         modem_set_transmission_time();
      }
   }
   return res;
}

static int
dtls_handle_event(struct dtls_context_t *ctx, session_t *session,
                  dtls_alert_level_t level, unsigned short code)
{
   if (DTLS_EVENT_CONNECTED == code) {
      dtls_info("dtls connected.");
      dtls_connected = 1;
      request_state = NONE;
      ui_led_op(LED_COLOR_RED, LED_CLEAR);
      ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
      modem_set_rai(1);
   } else if (DTLS_EVENT_CONNECT == code) {
      dtls_info("dtls connect ...");
      dtls_connected = 0;
      ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
      ui_led_op(LED_COLOR_RED, LED_SET);
      ui_led_op(LED_COLOR_GREEN, LED_SET);
      modem_set_rai(0);
   }
   return 0;
}

static int
dtls_handle_read(struct dtls_context_t *ctx)
{
   int fd;
   session_t session;
#define MAX_READ_BUF 1600
   static uint8 buf[MAX_READ_BUF];
   int len;

   fd = *(int *)dtls_get_app_data(ctx);

   if (fd < 0)
      return -1;

   memset(&session, 0, sizeof(session_t));
   session.size = sizeof(session.addr);
   len = recvfrom(fd, buf, MAX_READ_BUF, 0,
                  &session.addr.sa, &session.size);

   if (len < 0) {
      dtls_warn("recv_from_peer failed: errno %d (%s)", len, strerror(errno));
      return -1;
   } else {
      dtls_dsrv_log_addr(DTLS_LOG_DEBUG, "peer", &session);
      dtls_debug_dump("bytes from peer", buf, len);
      modem_set_transmission_time();
   }
   if (len >= 0) {
      dtls_info("received_from_peer %d bytes", len);
      if (!dtls_connected && request_state == RESEND) {
         request_state = SEND;
      }
   }
   return dtls_handle_message(ctx, &session, buf, len);
}

/*---------------------------------------------------------------------------*/

static dtls_handler_t cb = {
    .write = send_to_peer,
    .read = read_from_peer,
    .event = dtls_handle_event,
};

static void dtls_lte_connected(enum dtls_lte_connect_type type, bool connected)
{
   if (type == LTE_CONNECT_NETWORK) {
      network_connected = connected;
      if (connected) {
         if (!lte_connected) {
            ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
            ui_led_op(LED_COLOR_RED, LED_CLEAR);
         }
      } else {
         lte_connected = connected;
         ui_led_op(LED_COLOR_BLUE, LED_SET);
         ui_led_op(LED_COLOR_RED, LED_SET);
         ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
      }
   } else if (type == LTE_CONNECT_TRANSMISSION) {
      if (dtls_connected) {
         if (connected) {
            ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
            ui_led_op(LED_COLOR_GREEN, LED_SET);
         } else if (NONE == request_state) {
            ui_led_op(LED_COLOR_RED, LED_CLEAR);
            ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
         } else {
            ui_led_op(LED_COLOR_BLUE, LED_SET);
            ui_led_op(LED_COLOR_GREEN, LED_SET);
         }
      }
      lte_connected = connected;
      if (connected) {
         connected_time = (unsigned long)k_uptime_get();
         ++lte_connections;
         lte_connected_send = true;
      }
   }
}

static int dtls_init_destination(session_t *destination)
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

   destination->addr.sin.sin_port = htons(CONFIG_COAP_SERVER_PORT);
   destination->size = sizeof(struct sockaddr_in);

   inet_ntop(AF_INET, &destination->addr.sin.sin_addr.s_addr, ipv4_addr,
             sizeof(ipv4_addr));
   dtls_info("Destination: %s", host);
   dtls_info("IPv4 Address found %s", ipv4_addr);

   return 0;
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

int dtls_loop(void)
{
   char imei[24];
   int imei_len;
   fd_set rfds, wfds;
   dtls_context_t *dtls_context;
   struct timeval io_timeout;
   int fd;
   int result;
   session_t dst;
   int loops = 0;
   long time;

#ifdef CONFIG_COAP_WAIT_ON_POWERMANAGER
   uint16_t battery_voltage = 0xffff;
#endif

#ifdef CONFIG_LOCATION_ENABLE
   bool location_init = true;
#endif

   memset(&dst, 0, sizeof(session_t));
   dtls_init_destination(&dst);

   fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

   if (fd < 0) {
      dtls_warn("Failed to create UDP socket: %d", errno);
      reboot();
   }

   modem_set_rai(0);
   imei_len = modem_at_cmd("AT+CGSN", imei, sizeof(imei), NULL);
   dtls_credentials_init_psk(0 < imei_len ? imei : NULL);
   dtls_credentials_init_handler(&cb);

   coap_client_init();

   dtls_init();

   dtls_context = dtls_new_context(&fd);
   if (!dtls_context) {
      dtls_emerg("cannot create context");
      reboot();
   }

   dtls_set_handler(dtls_context, &cb);

   request_state = SEND;
   timeout = COAP_ACK_TIMEOUT;
   dtls_connect(dtls_context, &dst);

   while (1) {
      if (!network_connected) {
         reopen_socket(dtls_context);
         continue;
      }

#ifdef CONFIG_LOCATION_ENABLE
      uint8_t battery_level = 0xff;
      bool force = false;
#ifdef CONFIG_ADXL362_MOTION_DETECTION
      force = moved;
      moved = false;
#endif
      power_manager_status(&battery_level, NULL, NULL);
      if (location_enabled()) {
         if (battery_level < 20) {
            dtls_info("Low battery, switch off GNSS");
            location_stop();
         } else if (force) {
            dtls_info("Motion detected, force GNSS");
            location_start(force);
         }
      } else if (dtls_connected) {
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

      FD_ZERO(&rfds);
      FD_ZERO(&wfds);
      FD_SET(fd, &rfds);
      /* FD_SET(fd, &wfds); */

      if (request_state != NONE) {
         io_timeout.tv_sec = 1;
         io_timeout.tv_usec = 0;
         result = select(fd + 1, &rfds, &wfds, 0, &io_timeout);
      } else {
#ifdef CONFIG_COAP_WAIT_ON_POWERMANAGER
         if (0xffff == battery_voltage || 0 == battery_voltage) {
            /* wait until the power manager starts to report the battery voltage */
            if (!power_manager_voltage(&battery_voltage)) {
               if (0 == battery_voltage || 0xffff == battery_voltage) {
                  k_sleep(K_MSEC(200));
                  continue;
               }
               LOG_INF("Power-manager ready: %umV", battery_voltage);
            }
         }
#endif
         result = 0;
         if (k_sem_take(&dtls_trigger_msg, K_SECONDS(60)) == 0) {
#if CONFIG_COAP_SEND_INTERVAL > 0
            k_work_reschedule(&dtls_timer_trigger_work, K_SECONDS(CONFIG_COAP_SEND_INTERVAL));
#endif
            request_state = SEND;
            loops = 0;
            transmission = 0;
            timeout = COAP_ACK_TIMEOUT;
            if (dtls_connected) {
               if (coap_client_prepare_post() >= 0 && coap_client_send_message(dtls_context, &dst) >= 0) {
                  ui_led_op(LED_COLOR_GREEN, LED_SET);
               } else {
                  dtls_coap_failure();
               }
            } else {
               dtls_peer_t *peer = dtls_get_peer(dtls_context, &dst);
               dtls_reset_peer(dtls_context, peer);
               dtls_connect(dtls_context, &dst);
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
                  dtls_info("%d/%d/%u-%ld ms: connected => sent", lte_connected, lte_connected_send, lte_connections, time);
               } else {
                  dtls_info("%d/%d/%u-%ld ms: connected => resent", lte_connected, lte_connected_send, lte_connections, time);
               }
               request_state = RECEIVE;
            } else if (loops < 60) {
               ++loops;
            } else {
               dtls_coap_failure();
            }
         } else if (request_state == RECEIVE) {
            int temp = timeout;
            if (!lte_connected) {
               temp += ADD_ACK_TIMEOUT;
            }
            ++loops;
            if (loops > temp) {
               result = -1;
               if (transmission < COAP_MAX_RETRANSMISSION) {
                  ++transmission;
                  loops = 0;
                  timeout <<= 1;
                  request_state = RESEND;
                  if (dtls_connected) {
                     dtls_info("CoAP request resend, timeout %d", timeout);
                     result = coap_client_send_message(dtls_context, &dst);
                  } else {
                     dtls_info("hs resend, timeout %d", timeout);
                     dtls_check_retransmit(dtls_context, NULL);
                     result = 1;
                  }
               }
               if (result < 0) {
                  dtls_coap_failure();
               }
            }
         } else if (request_state == WAIT_RESPONSE) {
            if (loops < 60) {
               ++loops;
            } else {
               dtls_coap_failure();
            }
         }
      } else { /* ok */
         if (FD_ISSET(fd, &wfds))
            /* FIXME */;
         else if (FD_ISSET(fd, &rfds)) {
            dtls_handle_read(dtls_context);
            if (request_state == SEND_ACK) {
               unsigned long temp_time = connect_time;
               result = coap_client_send_message(dtls_context, &dst);
               connect_time = temp_time;
               request_state = NONE;
               dtls_coap_success();
               dtls_info("CoAP ACK sent.");
            }
         }
      }
   }

   dtls_free_context(dtls_context);
   return 0;
}

void main(void)
{
   LOG_INF("CoAP/DTLS CID sample " CLIENT_VERSION " has started");

   dtls_set_log_level(DTLS_LOG_INFO);

   ui_init(dtls_manual_trigger);

#if CONFIG_COAP_WAKEUP_SEND_INTERVAL > 0 && CONFIG_COAP_SEND_INTERVAL == 0
   modem_init(dtls_wakeup_trigger, dtls_lte_connected);
#else
   modem_init(NULL, dtls_lte_connected);
   dtls_trigger();
#endif

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

   if (modem_start(K_SECONDS(NETWORK_TIMEOUT_S)) != 0) {
      reconnect();
   }

   dtls_loop();
}

void main_(void)
{
   modem_power_off();
   k_sleep(K_MSEC(1000));
   NRF_REGULATORS->SYSTEMOFF = 1;
}
