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

#include <kernel.h>
#include <logging/log.h>
#include <modem/location.h>
#include <stdio.h>
#include <string.h>
#include <zephyr.h>

#include "dtls_client.h"
#include "modem_location.h"
#include "ui.h"

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

#if defined(CONFIG_NRF_MODEM_LIB)

static K_MUTEX_DEFINE(location_mutex);
static K_CONDVAR_DEFINE(location_condvar);

static location_callback_handler_t s_handler;

static volatile double s_latitude = 0.0;
static volatile double s_longitude = 0.0;
static volatile location_state_t s_location_state = NO_LOCATION;

static void location_event_handler(const struct location_event_data *event_data)
{
   location_state_t state = PREVIOUS_LOCATION;

   switch (event_data->id) {
      case LOCATION_EVT_LOCATION:
         LOG_INF("Got location:");
         LOG_INF("  method: %s", location_method_str(event_data->location.method));
         LOG_INF("  latitude: %.06f", event_data->location.latitude);
         LOG_INF("  longitude: %.06f", event_data->location.longitude);
         LOG_INF("  accuracy: %.01f m", event_data->location.accuracy);
         if (event_data->location.datetime.valid) {
            LOG_INF("  date: %04d-%02d-%02d",
                    event_data->location.datetime.year,
                    event_data->location.datetime.month,
                    event_data->location.datetime.day);
            LOG_INF("  time: %02d:%02d:%02d.%03d UTC",
                    event_data->location.datetime.hour,
                    event_data->location.datetime.minute,
                    event_data->location.datetime.second,
                    event_data->location.datetime.ms);
         }
         LOG_INF("  Google maps URL: https://maps.google.com/?q=%.06f,%.06f",
                 event_data->location.latitude, event_data->location.longitude);
         state = CURRENT_LOCATION;
         break;

      case LOCATION_EVT_TIMEOUT:
         LOG_INF("Getting location timed out");
         break;

      case LOCATION_EVT_ERROR:
         LOG_INF("Getting location failed");
         break;

      case LOCATION_EVT_GNSS_ASSISTANCE_REQUEST:
         LOG_INF("Getting location assistance requested (A-GPS). Not doing anything.");
         break;

      case LOCATION_EVT_GNSS_PREDICTION_REQUEST:
         LOG_INF("Getting location assistance requested (P-GPS). Not doing anything.");
         break;

      default:
         LOG_INF("Getting location: Unknown event.");
         break;
   }
   k_mutex_lock(&location_mutex, K_FOREVER);
   if (CURRENT_LOCATION == state) {
      s_latitude = event_data->location.latitude;
      s_longitude = event_data->location.longitude;
      s_location_state = state;
      k_condvar_broadcast(&location_condvar);
   } else if (NO_LOCATION != s_location_state) {
      s_location_state = state;
      k_condvar_broadcast(&location_condvar);
   }
   k_mutex_unlock(&location_mutex);
   if (CURRENT_LOCATION == state && s_handler) {
      (*s_handler)();
   }
}

static int modem_location_wait(k_timeout_t timeout)
{
   int res;
   k_mutex_lock(&location_mutex, timeout);
   res = k_condvar_wait(&location_condvar, &location_mutex, timeout);
   k_mutex_unlock(&location_mutex);
   return res;
}

int modem_location_init(location_callback_handler_t handler)
{
   int err;

   err = location_init(location_event_handler);
   if (err) {
      LOG_INF("Initializing the Location library failed, error: %d", err);
   } else {
      s_handler = handler;
   }
   return err;
}

int modem_location_start(int interval, int timeout)
{
   int err;
   struct location_config config;
   enum location_method methods[] = {LOCATION_METHOD_GNSS};

   location_config_defaults_set(&config, ARRAY_SIZE(methods), methods);
   config.interval = interval;
   config.methods[0].gnss.timeout = timeout;
   config.methods[0].gnss.visibility_detection = false;

   LOG_INF("Requesting location with GNSS for %d s, interval %d", timeout, interval);

   err = location_request(&config);
   if (err) {
      LOG_INF("Requesting location failed, error: %d\n", err);
   }
   return err;
}

location_state_t modem_location_get(int timeout, double *latitude, double *longitude)
{
   location_state_t result;

   if (timeout > 0) {
      int err;

      err = modem_location_start(0, timeout);
      if (err) {
         return NO_LOCATION;
      }

      int led_on = 1;
      ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
      ui_led_op(LED_COLOR_RED, LED_CLEAR);
      ui_led_op(LED_COLOR_BLUE, LED_SET);

      while (modem_location_wait(K_MSEC(2000))) {
         led_on = !led_on;
         if (led_on) {
            ui_led_op(LED_COLOR_BLUE, LED_SET);
            ui_led_op(LED_COLOR_RED, LED_CLEAR);
         } else {
            ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
            ui_led_op(LED_COLOR_RED, LED_SET);
         }
      }

      ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
      if (CURRENT_LOCATION == s_location_state) {
         ui_led_op(LED_COLOR_RED, LED_CLEAR);
         ui_led_op(LED_COLOR_GREEN, LED_SET);
      } else {
         ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
         ui_led_op(LED_COLOR_RED, LED_SET);
      }
      k_sleep(K_MSEC(5000));
      ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
      ui_led_op(LED_COLOR_RED, LED_CLEAR);
   }

   k_mutex_lock(&location_mutex, K_FOREVER);
   result = s_location_state;
   if (CURRENT_LOCATION == result) {
      if (latitude) {
         *latitude = s_latitude;
      }
      if (longitude) {
         *longitude = s_longitude;
      }
   }
   k_mutex_unlock(&location_mutex);

   return result;
}

#else

int modem_location_init(location_callback_handler_t handler)
{
   (void) handler;
   return 0;
}

int modem_location_start(int interval, int timeout)
{
   (void)interval;
   (void)timeout;
   return 0;
}

location_state_t modem_location_get(int timeout, double *latitude, double *longitude)
{
   (void)timeout;
   (void)latitude;
   (void)longitude;
   return NO_LOCATION;
}

#endif
