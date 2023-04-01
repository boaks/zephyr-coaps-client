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

#include <modem/lte_lc.h>
#include <nrf_errno.h>
#include <nrf_modem_gnss.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "io_job_queue.h"
#include "location.h"
#include "ui.h"

LOG_MODULE_REGISTER(GNSS_CLIENT, CONFIG_GNSS_CLIENT_LOG_LEVEL);

#ifndef CONFIG_NRF_MODEM_LIB
#error "requires CONFIG_NRF_MODEM_LIB"
#endif

typedef enum location_state {
   LOCATION_NONE,
   LOCATION_PENDING,
   LOCATION_WAITING_FOR_SLEEPING,
   LOCATION_GNSS_RUNNING,
   LOCATION_DONE,
} location_state_t;

#define GNSS_TIMEOUT_INITIAL 180
#define GNSS_TIMEOUT_MAXIMUM 300
#ifdef CONFIG_LOCATION_ENABLE_CONTINUES_MODE
#define GNSS_TIMEOUT_SCAN 180
#else
#define GNSS_TIMEOUT_SCAN 30
#endif

#define GNSS_INTERVAL_INITIAL_PROBE 300
#define GNSS_INTERVAL_MAXIMUM_PROBE 3600
#define GNSS_INTERVAL_SCAN 60
#define GNSS_INTERVAL_MAXIMUM_SCAN 300

#define GNSS_MAXIMUM_NO_POSITION (30 * 60)

#define GNSS_TIME_PER_INTERVAL 3

#define GNSS_VISIBILITY_DETECTION_EXEC_TIME 15000
#define GNSS_VISIBILITY_DETECTION_SAT_LIMIT 3

static K_MUTEX_DEFINE(location_mutex);

static void location_lte_start_work_fn(struct k_work *work);
static void location_gnss_pvt_work_fn(struct k_work *work);
static void location_gnss_timeout_work_fn(struct k_work *work);
static void location_gnss_start_work_fn(struct k_work *work);
static void location_scan_start_work_fn(struct k_work *work);

static K_WORK_DEFINE(location_lte_start_work, location_lte_start_work_fn);
static K_WORK_DEFINE(location_gnss_pvt_work, location_gnss_pvt_work_fn);
static K_WORK_DEFINE(location_scan_start_work, location_scan_start_work_fn);
static K_WORK_DELAYABLE_DEFINE(location_gnss_timeout_work, location_gnss_timeout_work_fn);
static K_WORK_DELAYABLE_DEFINE(location_gnss_start_work, location_gnss_start_work_fn);

static location_callback_handler_t s_location_handler;

static atomic_t s_location_start = ATOMIC_INIT(0);

static uint8_t s_location_gnss_use_case = NRF_MODEM_GNSS_USE_CASE_MULTIPLE_HOT_START;
static uint16_t s_location_gnss_timeout = GNSS_TIMEOUT_INITIAL;
static uint16_t s_location_interval = GNSS_INTERVAL_INITIAL_PROBE;

static volatile int64_t s_location_last_position;
static volatile int64_t s_location_last_request;
static volatile int64_t s_location_last_result;
static volatile location_state_t s_location_state = LOCATION_NONE;
static volatile bool s_modem_sleeping;
static volatile bool s_gnss_blocked;
static volatile bool s_location_init;
static volatile uint16_t s_gnss_pvt_counter;

static bool s_location_visibility_detection = false;

static struct modem_gnss_state s_location_gnss_result = {.result = MODEM_GNSS_NOT_AVAILABLE, .valid = false};
static struct modem_gnss_state s_location_gnss_state = {.result = MODEM_GNSS_NOT_AVAILABLE, .valid = false};
static struct nrf_modem_gnss_agps_expiry gnss_expiry;

static inline uint16_t backoff(uint16_t time, const uint16_t max)
{
   time *= 2;
   if (time > max) {
      time = max;
   }
   return time;
}

static void location_gnss_event_handler(int event)
{
   switch (event) {
      case NRF_MODEM_GNSS_EVT_PVT:
         work_submit_to_io_queue(&location_gnss_pvt_work);
         break;
      case NRF_MODEM_GNSS_EVT_FIX:
         work_submit_to_io_queue(&location_gnss_pvt_work);
         break;
      case NRF_MODEM_GNSS_EVT_AGPS_REQ:
         LOG_INF("GNSS: A-GPS request!");
         break;
      case NRF_MODEM_GNSS_EVT_BLOCKED:
         LOG_INF("GNSS: blocked by LTE!");
         s_gnss_blocked = true;
         break;
      case NRF_MODEM_GNSS_EVT_UNBLOCKED:
         LOG_INF("GNSS: unblocked by LTE!");
         s_gnss_blocked = false;
         break;
      default:
         LOG_INF("GNSS event: %d", event);
         break;
   }
}

static void location_lte_ind_handler(const struct lte_lc_evt *const evt)
{
   switch (evt->type) {
      case LTE_LC_EVT_MODEM_SLEEP_ENTER:
         if (evt->modem_sleep.type != LTE_LC_MODEM_SLEEP_FLIGHT_MODE) {
            s_modem_sleeping = true;
            work_submit_to_io_queue(&location_lte_start_work);
         }
         break;
      case LTE_LC_EVT_MODEM_SLEEP_EXIT:
         s_modem_sleeping = false;
         break;
#ifdef CONFIG_LOCATION_ENABLE_CONTINUES_MODE
      case LTE_LC_EVT_RRC_UPDATE:
         if (evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED) {
            s_modem_sleeping = false;
         } else {
            s_modem_sleeping = true;
            work_submit_to_io_queue(&location_lte_start_work);
         }
         break;
#endif
      default:
         break;
   }
}

static int location_stop_works(bool timeout)
{
   int err = 0;

#ifdef CONFIG_LOCATION_ENABLE_CONTINUES_MODE
   if (timeout || atomic_get(&s_location_start) == 0) {
      err = nrf_modem_gnss_stop();
   }
#else
   err = nrf_modem_gnss_stop();
#endif
   if ((err != 0) && (err != -NRF_EPERM)) {
      LOG_ERR("Failed to stop GNSS");
   }

   /* prevent read gnss pvt */
   s_location_state = LOCATION_DONE;

   /* Cancel any work that has not been started yet */
   (void)k_work_cancel(&location_gnss_pvt_work);
   (void)k_work_cancel(&location_lte_start_work);
   (void)k_work_cancel(&location_scan_start_work);
   (void)k_work_cancel_delayable(&location_gnss_start_work);
   (void)k_work_cancel_delayable(&location_gnss_timeout_work);

   return err;
}

static void location_event_handler(const struct modem_gnss_state *gnss_state)
{
   int64_t now = k_uptime_get();
   modem_gnss_result_t state = gnss_state->result;
   bool timeout = false;

   switch (state) {
      case MODEM_GNSS_POSITION:
         LOG_INF("GNSS:%s", gnss_state->valid ? " valid position" : "");
         break;
      case MODEM_GNSS_ERROR:
         LOG_INF("GNSS error");
         timeout = true;
         break;
      case MODEM_GNSS_TIMEOUT:
         LOG_INF("GNSS timeout");
         timeout = true;
         break;
      case MODEM_GNSS_INVISIBLE:
         LOG_INF("GNSS invisible");
         break;
      default:
         break;
   }

   location_stop_works(timeout);

   k_mutex_lock(&location_mutex, K_FOREVER);
   s_location_last_result = now;
   if (MODEM_GNSS_POSITION == state) {
      s_location_last_position = now;
      s_location_gnss_state = *gnss_state;
   } else {
      s_location_gnss_state.result = gnss_state->result;
      s_location_gnss_state.execution_time = gnss_state->execution_time;
      s_location_gnss_state.satellites_time = gnss_state->satellites_time;
      s_location_gnss_state.max_satellites = gnss_state->max_satellites;
   }
   s_location_state = LOCATION_NONE;
   k_mutex_unlock(&location_mutex);

   if (MODEM_GNSS_POSITION == state && s_location_handler) {
      s_location_handler();
   }

   if (atomic_get(&s_location_start)) {
      LOG_INF("Location: last execution time %u[ms]", gnss_state->execution_time);
      now = k_uptime_get();
      int32_t time = (int32_t)(s_location_last_request - now);
      if (s_location_init) {
         if (MODEM_GNSS_POSITION == state) {
            s_location_gnss_timeout = GNSS_TIMEOUT_SCAN;
            s_location_visibility_detection = true;
         } else {
            int32_t last_position = (int32_t)((now - s_location_last_position) / MSEC_PER_SEC);
            if (last_position > GNSS_MAXIMUM_NO_POSITION) {
               LOG_INF("Location: no position since %d[s]", last_position);
               s_location_visibility_detection = false;
               s_location_gnss_timeout = GNSS_TIMEOUT_INITIAL;
            } else {
               LOG_INF("Location: last position %d[s] ago", last_position);
            }
         }
         int32_t scan_interval = GNSS_INTERVAL_SCAN * gnss_state->execution_time / GNSS_TIME_PER_INTERVAL;
         if (scan_interval > GNSS_INTERVAL_MAXIMUM_SCAN * MSEC_PER_SEC) {
            scan_interval = GNSS_INTERVAL_MAXIMUM_SCAN * MSEC_PER_SEC;
         } else if (scan_interval < GNSS_INTERVAL_SCAN * MSEC_PER_SEC) {
            scan_interval = GNSS_INTERVAL_SCAN * MSEC_PER_SEC;
         }
         time += scan_interval;
      } else {
         if (MODEM_GNSS_POSITION == state) {
            s_location_init = true;
            s_location_visibility_detection = true;
            s_location_gnss_timeout = GNSS_TIMEOUT_SCAN;
            s_location_interval = GNSS_INTERVAL_SCAN;
            time += (s_location_interval * MSEC_PER_SEC);
         } else {
            s_location_gnss_timeout = backoff(s_location_gnss_timeout, GNSS_TIMEOUT_MAXIMUM);
            time += (s_location_interval * MSEC_PER_SEC);
            s_location_interval = backoff(s_location_interval, GNSS_INTERVAL_MAXIMUM_PROBE);
         }
      }
      if (time < 1) {
         time = 1;
      }
      s_location_state = LOCATION_PENDING;
#ifdef CONFIG_LOCATION_ENABLE_CONTINUES_MODE
      work_schedule_for_io_queue(&location_gnss_start_work, K_NO_WAIT);
      LOG_INF("Location: continues mode, timeout %d[s]", s_location_gnss_timeout);
#else
      work_schedule_for_io_queue(&location_gnss_start_work, K_MSEC(time));
      LOG_INF("Location: next request in %d[s], timeout %d[s]", time / MSEC_PER_SEC, s_location_gnss_timeout);
#endif
   }
}

static void location_gnss_timeout_work_fn(struct k_work *work)
{
   ARG_UNUSED(work);

   if (s_location_state != LOCATION_GNSS_RUNNING) {
      /* ignore the timeout */
      return;
   }

   s_location_gnss_result.execution_time = k_uptime_get() - s_location_last_request;
   s_location_gnss_result.result = MODEM_GNSS_TIMEOUT;
   LOG_WRN("Timeout occurred after %u[s]", s_location_gnss_result.execution_time / MSEC_PER_SEC);

   location_event_handler(&s_location_gnss_result);
}

static uint8_t location_tracked_satellites(const struct nrf_modem_gnss_pvt_data_frame *pvt_data)
{
   uint8_t tracked = 0;

   for (uint8_t i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
      if (pvt_data->sv[i].sv == 0) {
         break;
      }
      tracked++;
   }

   return tracked;
}

static void location_print_pvt(uint8_t tracked, const struct nrf_modem_gnss_pvt_data_frame *pvt_data)
{
   LOG_DBG("Tracked satellites: %d, flags: %02x, fix %d", tracked, pvt_data->flags,
           pvt_data->flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID ? 1 : 0);

   /* Print details for each satellite */
   for (uint32_t i = 0; i < tracked; i++) {

      const struct nrf_modem_gnss_sv *sv_data = &pvt_data->sv[i];

      LOG_DBG(" Sp.Veh.: %3d, C/N0: %4d, fix: %d, unhealthy: %d",
              sv_data->sv,
              sv_data->cn0,
              sv_data->flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX ? 1 : 0,
              sv_data->flags & NRF_MODEM_GNSS_SV_FLAG_UNHEALTHY ? 1 : 0);
   }
}

static void location_gnss_pvt_work_fn(struct k_work *item)
{
   uint8_t tracked;
   int64_t now = k_uptime_get();

   if (s_location_state != LOCATION_GNSS_RUNNING) {
      /* ignore the notification. */
      return;
   }

   if (nrf_modem_gnss_read(&s_location_gnss_result.position, sizeof(s_location_gnss_result.position), NRF_MODEM_GNSS_DATA_PVT) != 0) {
      LOG_ERR("Failed to read PVT data from GNSS");
      return;
   }

   tracked = location_tracked_satellites(&s_location_gnss_result.position);
   s_gnss_pvt_counter++;
   if (s_gnss_pvt_counter > 60) {
      s_gnss_pvt_counter = 0;
      LOG_INF("GNSS PVT, tracked satellites: %d, flags: %02x, fix %d", tracked, s_location_gnss_result.position.flags,
              s_location_gnss_result.position.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID ? 1 : 0);
   }

   s_location_gnss_result.execution_time = now - s_location_last_request;
   if (s_location_gnss_result.max_satellites < tracked) {
      s_location_gnss_result.max_satellites = tracked;
      if (s_location_visibility_detection && s_location_gnss_result.satellites_time == 0 &&
          GNSS_VISIBILITY_DETECTION_SAT_LIMIT <= s_location_gnss_result.max_satellites) {
         s_location_gnss_result.satellites_time = s_location_gnss_result.execution_time;
         LOG_INF("GNSS visibility in %us", s_location_gnss_result.satellites_time / 1000);
      }
   }
   location_print_pvt(tracked, &s_location_gnss_result.position);

   if (s_location_gnss_result.position.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {

      s_location_gnss_result.result = MODEM_GNSS_POSITION;
      s_location_gnss_result.valid = true;

      location_event_handler(&s_location_gnss_result);
   } else if (s_location_visibility_detection) {
      if (s_location_gnss_result.position.execution_time >= GNSS_VISIBILITY_DETECTION_EXEC_TIME &&
          s_location_gnss_result.max_satellites < GNSS_VISIBILITY_DETECTION_SAT_LIMIT) {
         LOG_INF("GNSS visibility obstructed, canceling");
         s_location_gnss_result.result = MODEM_GNSS_INVISIBLE;
         location_event_handler(&s_location_gnss_result);
      }
   }
}

static void location_print_expiry(const struct nrf_modem_gnss_agps_expiry *gnss_expiry)
{
   LOG_DBG("GNSS: A-GPS - flags %02x, utc %u, klob %u, neq %u, integ %u",
           gnss_expiry->data_flags, gnss_expiry->utc_expiry, gnss_expiry->klob_expiry,
           gnss_expiry->neq_expiry, gnss_expiry->integrity_expiry);

#ifndef CONFIG_LOCATION_ENABLE_CONTINUES_MODE
   if (Z_LOG_CONST_LEVEL_CHECK(LOG_LEVEL_DBG)) {
      for (uint32_t i = 0; i < NRF_MODEM_GNSS_NUM_GPS_SATELLITES; i++) {
         if (gnss_expiry->alm_expiry[i] || gnss_expiry->ephe_expiry[i]) {
            LOG_DBG(" Sp.Veh.: %3d, alm. %d, ephe. %d",
                    i + 1, gnss_expiry->alm_expiry[i], gnss_expiry->ephe_expiry[i]);
            k_sleep(K_MSEC(50));
         }
      }
   }
#endif
}

static void location_gnss_start(void)
{
   bool running = false;
   int err = 0;
   uint16_t timeout = s_location_gnss_timeout;

   if (atomic_get(&s_location_start) == 0) {
      return;
   }
   if (s_location_state == LOCATION_GNSS_RUNNING) {
      return;
   }

   s_location_last_request = k_uptime_get();

   /* By default we take the first fix. */
   s_location_gnss_result.result = MODEM_GNSS_ERROR;
   s_location_gnss_result.max_satellites = 0;
   s_location_gnss_result.execution_time = 0;
   s_location_gnss_result.satellites_time = 0;

   /* Configure GNSS to continuous tracking mode */
   err = nrf_modem_gnss_fix_interval_set(1);
#ifdef CONFIG_LOCATION_ENABLE_CONTINUES_MODE
   if (err == -NRF_EPERM || err == -NRF_EINVAL) {
      running = true;
      err = 0;
   }
#endif
   if (err) {
      LOG_ERR("Failed to configure GNSS fix interval! err %d %s", -err, strerror(-err));
      location_event_handler(&s_location_gnss_result);
      return;
   }

   if (!running) {
      err = nrf_modem_gnss_use_case_set(s_location_gnss_use_case);

      if (err) {
         LOG_ERR("Failed to configure GNSS use case! err %d %s", -err, strerror(-err));
         location_event_handler(&s_location_gnss_result);
         return;
      }

      err = nrf_modem_gnss_agps_expiry_get(&gnss_expiry);
      if (err) {
         LOG_ERR("GNSS get A-GPS expiry failed! err %d %s", -err, strerror(-err));
      } else {
         location_print_expiry(&gnss_expiry);
      }

      s_location_state = LOCATION_GNSS_RUNNING;
      err = nrf_modem_gnss_start();
      if (err) {
         LOG_ERR("Failed to start GNSS! err %d %s", -err, strerror(-err));
         location_event_handler(&s_location_gnss_result);
         return;
      }
      LOG_INF("GNSS request started.");
   } else {
      s_location_state = LOCATION_GNSS_RUNNING;
      LOG_INF("GNSS request continued.");
   }
   if (timeout > 0) {
      LOG_DBG("Starting timer with timeout=%d", timeout);
      work_reschedule_for_io_queue(&location_gnss_timeout_work, K_SECONDS(timeout));
   }
}

static void location_gnss_start_work_fn(struct k_work *work)
{
   if (s_location_state != LOCATION_PENDING) {
      return;
   }

   LOG_INF("GNSS request ...");
   s_location_state = LOCATION_WAITING_FOR_SLEEPING;
   if (!s_modem_sleeping) {
      LOG_INF("GNSS wait for modem sleeping ...");
      return;
   }

   location_gnss_start();
}

static void location_lte_start_work_fn(struct k_work *work)
{
   if (s_location_state == LOCATION_WAITING_FOR_SLEEPING) {
      /* start gnss */
      LOG_INF("GNSS modem sleeping ...");
      location_gnss_start();
   }
}

int location_init(location_callback_handler_t handler)
{
   int err;

   s_location_state = LOCATION_NONE;

   err = nrf_modem_gnss_event_handler_set(location_gnss_event_handler);
   if (err) {
      LOG_ERR("Failed to set GNSS event handler, error %d", err);
      return err;
   }

   lte_lc_register_handler(location_lte_ind_handler);
   s_location_handler = handler;

   return err;
}

static void location_scan_start_work_fn(struct k_work *work)
{
   if (atomic_get(&s_location_start) == 1) {
      s_location_init = false;
      s_location_gnss_timeout = GNSS_TIMEOUT_INITIAL;
      s_location_interval = GNSS_INTERVAL_INITIAL_PROBE;
      if (s_location_state == LOCATION_GNSS_RUNNING) {
         LOG_DBG("Restarting timer with timeout=%d", s_location_gnss_timeout);
         work_reschedule_for_io_queue(&location_gnss_timeout_work, K_SECONDS(s_location_gnss_timeout));
      } else {
         s_location_state = LOCATION_PENDING;
         work_reschedule_for_io_queue(&location_gnss_start_work, K_NO_WAIT);
      }
   }
}

bool location_enabled(void)
{
   return atomic_get(&s_location_start);
}

void location_start(bool force)
{
   if (force && s_location_init) {
      force = false;
   }
   if (force) {
      LOG_INF("Location: force init");
   }
   if (atomic_cas(&s_location_start, 0, 1) || force) {
      work_submit_to_io_queue(&location_scan_start_work);
   }
}

void location_stop(void)
{
   atomic_set(&s_location_start, 0);
   s_location_init = false;
   s_location_state = LOCATION_NONE;
   location_stop_works(false);
   k_work_cancel(&location_scan_start_work);
}

modem_gnss_result_t location_get(struct modem_gnss_state *location, bool *running)
{
   modem_gnss_result_t result;

   k_mutex_lock(&location_mutex, K_FOREVER);
   result = s_location_gnss_state.result;
   if (location) {
      *location = s_location_gnss_state;
   }
   if (running) {
      *running = (s_location_state == LOCATION_GNSS_RUNNING);
   }
   k_mutex_unlock(&location_mutex);

   return result;
}
