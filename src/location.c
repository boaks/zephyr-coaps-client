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
#include <ncs_version.h>
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

static struct k_spinlock location_lock;

static void location_gnss_start(void);

static void location_lte_start_work_fn(struct k_work *work);
static void location_gnss_pvt_work_fn(struct k_work *work);
static void location_gnss_timeout_work_fn(struct k_work *work);
static void location_gnss_start_work_fn(struct k_work *work);
static void location_scan_start_work_fn(struct k_work *work);

static K_WORK_DEFINE(location_lte_start_work, location_lte_start_work_fn);
static K_WORK_DEFINE(location_gnss_pvt_work, location_gnss_pvt_work_fn);
static K_WORK_DEFINE(location_gnss_fix_work, location_gnss_pvt_work_fn);
static K_WORK_DEFINE(location_scan_start_work, location_scan_start_work_fn);
static K_WORK_DEFINE(location_gnss_timeout_work, location_gnss_timeout_work_fn);
static K_WORK_DEFINE(location_gnss_start_work, location_gnss_start_work_fn);
static K_WORK_DELAYABLE_DEFINE(location_gnss_next_work, location_gnss_start_work_fn);

static location_callback_handler_t s_location_handler;

#define LOCATION_STATE_STARTED 0
#define LOCATION_STATE_INITIAL_POS 1
#define LOCATION_STATE_LOCKED 2
#define LOCATION_STATE_GNSS_BLOCKED 3
#define LOCATION_STATE_LTE_SLEEPING 4
#define LOCATION_STATE_WAKEUP 5
#define LOCATION_STATE_RUNNING 5

static atomic_t s_location_flags = ATOMIC_INIT(0);

struct location_timing {
   uint16_t timeout;
   uint16_t interval;
   uint8_t power_mode;
};

static struct location_timing s_location_timing = {
    .timeout = GNSS_TIMEOUT_INITIAL,
    .interval = GNSS_INTERVAL_INITIAL_PROBE,
    .power_mode = NRF_MODEM_GNSS_PSM_DUTY_CYCLING_POWER};

static struct location_timing s_location_timing_current = {
    .timeout = 0,
    .interval = 0,
    .power_mode = 0};

static uint32_t s_location_dynamics = NRF_MODEM_GNSS_DYNAMICS_PEDESTRIAN;

static unsigned int s_location_initial_counter = 1;

static volatile int64_t s_location_last_position;
static volatile int64_t s_location_last_request;
static volatile int64_t s_location_last_result;
static volatile location_state_t s_location_state = LOCATION_NONE;
static atomic_t s_gnss_pvt_counter = ATOMIC_INIT(0);

static bool s_location_visibility_detection = false;

static struct modem_gnss_state s_location_gnss_result = {.result = MODEM_GNSS_NOT_AVAILABLE, .valid = false};
static struct modem_gnss_state s_location_gnss_state = {.result = MODEM_GNSS_NOT_AVAILABLE, .valid = false};
static struct modem_gnss_state s_location_gnss_current = {.result = MODEM_GNSS_NOT_AVAILABLE, .valid = false};
static struct nrf_modem_gnss_agnss_expiry gnss_expiry;

static inline uint16_t backoff(const uint16_t time, const uint16_t max, const int count)
{
   uint32_t value = time;

   value <<= (count < 5 ? count - 1 : 4);

   if (value > max) {
      value = max;
   }
   return value;
}

static inline uint16_t location_gnss_get_interval(void)
{
   uint16_t res;
   K_SPINLOCK(&location_lock)
   {
      res = s_location_timing.interval;
   }
   return res;
}

static inline uint16_t location_gnss_get_timeout(void)
{
   uint16_t res;
   K_SPINLOCK(&location_lock)
   {
      res = s_location_timing.timeout;
   }
   return res;
}

static bool location_gnss_calc_timing(struct location_timing *timing)
{
   bool change = false;

   K_SPINLOCK(&location_lock)
   {
      if (0 == s_location_initial_counter) {
         *timing = s_location_timing;
      } else {
         uint16_t interval = s_location_timing.interval;
         if (1 < interval) {
            // keep 0 (once) and 1 (continues)
            interval = backoff(interval, GNSS_INTERVAL_MAXIMUM_PROBE, s_location_initial_counter);
         }
         timing->interval = interval;
         timing->timeout = backoff(s_location_timing.timeout, GNSS_TIMEOUT_MAXIMUM, s_location_initial_counter);
         timing->power_mode = s_location_timing.power_mode;
      }
      change = timing->interval != s_location_timing_current.interval ||
               timing->timeout != s_location_timing_current.timeout ||
               timing->power_mode != s_location_timing_current.power_mode;
   }

   return change;
}

static void location_gnss_event_handler(int event)
{
   switch (event) {
      case NRF_MODEM_GNSS_EVT_PVT:
         work_submit_to_io_queue(&location_gnss_pvt_work);
         break;
      case NRF_MODEM_GNSS_EVT_FIX:
         work_submit_to_io_queue(&location_gnss_fix_work);
         break;
      case NRF_MODEM_GNSS_EVT_AGNSS_REQ:
         LOG_INF("GNSS: A-GNSS request!");
         break;
      case NRF_MODEM_GNSS_EVT_BLOCKED:
         LOG_INF("GNSS: blocked by LTE!");
         atomic_set_bit(&s_location_flags, LOCATION_STATE_GNSS_BLOCKED);
         break;
      case NRF_MODEM_GNSS_EVT_UNBLOCKED:
         LOG_INF("GNSS: unblocked by LTE!");
         atomic_clear_bit(&s_location_flags, LOCATION_STATE_GNSS_BLOCKED);
         break;
      case NRF_MODEM_GNSS_EVT_PERIODIC_WAKEUP:
         LOG_INF("GNSS: periodic wakeup.");
         atomic_set_bit(&s_location_flags, LOCATION_STATE_WAKEUP);
         atomic_set(&s_gnss_pvt_counter, 0);
         break;
      case NRF_MODEM_GNSS_EVT_SLEEP_AFTER_TIMEOUT:
         LOG_INF("GNSS: sleep after timeout.");
         atomic_clear_bit(&s_location_flags, LOCATION_STATE_WAKEUP);
         work_submit_to_io_queue(&location_gnss_timeout_work);
         break;
      case NRF_MODEM_GNSS_EVT_SLEEP_AFTER_FIX:
         LOG_INF("GNSS: sleep after fix.");
         atomic_clear_bit(&s_location_flags, LOCATION_STATE_WAKEUP);
         break;
      case NRF_MODEM_GNSS_EVT_REF_ALT_EXPIRED:
         LOG_INF("GNSS: ref alt expired.");
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
            atomic_set_bit(&s_location_flags, LOCATION_STATE_LTE_SLEEPING);
            work_submit_to_io_queue(&location_lte_start_work);
         }
         break;
      case LTE_LC_EVT_MODEM_SLEEP_EXIT:
         atomic_clear_bit(&s_location_flags, LOCATION_STATE_LTE_SLEEPING);
         break;
#ifdef CONFIG_LOCATION_ENABLE_CONTINUES_MODE
      case LTE_LC_EVT_RRC_UPDATE:
         if (evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED) {
            atomic_clear_bit(&s_location_flags, LOCATION_STATE_LTE_SLEEPING);
         } else {
            atomic_set_bit(&s_location_flags, LOCATION_STATE_LTE_SLEEPING);
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
   if (timeout || !atomic_test_bit(&s_location_flags, LOCATION_STATE_STARTED)) {
      err = nrf_modem_gnss_stop();
      atomic_clear_bit(&s_location_flags, LOCATION_STATE_RUNNING);
   }
#else
   err = nrf_modem_gnss_stop();
   atomic_clear_bit(&s_location_flags, LOCATION_STATE_RUNNING);
#endif
   if ((err != 0) && (err != -NRF_EPERM)) {
      LOG_ERR("Failed to stop GNSS");
   }

   /* prevent read gnss pvt */
   s_location_state = LOCATION_DONE;

   /* Cancel any work that has not been started yet */
   (void)k_work_cancel(&location_gnss_pvt_work);
   (void)k_work_cancel(&location_gnss_fix_work);
   (void)k_work_cancel(&location_lte_start_work);
   (void)k_work_cancel(&location_scan_start_work);
   (void)k_work_cancel(&location_gnss_timeout_work);
   (void)k_work_cancel(&location_gnss_start_work);
   (void)k_work_cancel_delayable(&location_gnss_next_work);

   return err;
}

static void location_event_handler(const struct modem_gnss_state *gnss_state)
{
   int64_t now = k_uptime_get();
   modem_gnss_result_t state = gnss_state->result;
   bool stop = false;

   switch (state) {
      case MODEM_GNSS_POSITION:
         LOG_INF("GNSS:%s", gnss_state->valid ? " valid position" : "");
         break;
      case MODEM_GNSS_ERROR:
         LOG_INF("GNSS error");
         stop = true;
         break;
      case MODEM_GNSS_TIMEOUT:
         LOG_INF("GNSS timeout");
         break;
      case MODEM_GNSS_INVISIBLE:
         LOG_INF("GNSS invisible");
         break;
      default:
         break;
   }

   K_SPINLOCK(&location_lock)
   {
      s_location_last_result = now;
      if (MODEM_GNSS_POSITION == state) {
         s_location_last_position = now;
         s_location_gnss_state = *gnss_state;
      } else {
         s_location_gnss_state.result = gnss_state->result;
         s_location_gnss_state.execution_time = gnss_state->execution_time;
         s_location_gnss_state.satellites_time = gnss_state->satellites_time;
         s_location_gnss_state.fixes = gnss_state->fixes;
         s_location_gnss_state.pvts = gnss_state->pvts;
         s_location_gnss_state.timeouts = gnss_state->timeouts;
         s_location_gnss_state.max_satellites = gnss_state->max_satellites;
         s_location_gnss_state.max_healthy_satellites = gnss_state->max_healthy_satellites;
      }
   }
   if (stop) {
      location_stop_works(true);
   }
   s_location_state = LOCATION_NONE;

   if (atomic_test_bit(&s_location_flags, LOCATION_STATE_STARTED)) {
      LOG_INF("Location: last execution time %u[ms]", gnss_state->execution_time);
      struct location_timing timing;
      bool change;

      if (MODEM_GNSS_POSITION == state && s_location_handler) {
         s_location_handler();
         if (!atomic_test_and_set_bit(&s_location_flags, LOCATION_STATE_INITIAL_POS)) {
            s_location_visibility_detection = true;
            K_SPINLOCK(&location_lock)
            {
               s_location_initial_counter = 0;
            }
         }
      } else if (atomic_test_bit(&s_location_flags, LOCATION_STATE_INITIAL_POS)) {
         int32_t last_position = (int32_t)((now - s_location_last_position) / MSEC_PER_SEC);
         if (last_position > GNSS_MAXIMUM_NO_POSITION) {
            LOG_INF("Location: no position since %d[s]", last_position);
            s_location_visibility_detection = false;
            K_SPINLOCK(&location_lock)
            {
               s_location_initial_counter = 1;
            }
         } else {
            LOG_INF("Location: last position %d[s] ago", last_position);
         }
      } else {
         K_SPINLOCK(&location_lock)
         {
            ++s_location_initial_counter;
         }
      }
      change = location_gnss_calc_timing(&timing);
      int32_t time = timing.interval ? 0 : -1;
      if (1 < timing.interval) {
         time = timing.interval - (gnss_state->execution_time / MSEC_PER_SEC);
         if (time < 1) {
            time = timing.interval;
         }
      }
      if (0 <= time) {
         s_location_state = LOCATION_PENDING;
         k_timeout_t delay = (stop || change) ? K_MSEC(time) : K_NO_WAIT;
         if (stop || change) {
            LOG_INF("Location: next request in %d[s], timeout %us", time / MSEC_PER_SEC, timing.timeout);
         } else if (0 == time) {
            LOG_INF("Location: continues request, timeout %us", timing.timeout);
         } else {
            LOG_INF("Location: next request, interval %us timeout %u[s]", timing.interval, timing.timeout);
         }
         work_reschedule_for_io_queue(&location_gnss_next_work, delay);
      } else {
         if (!stop) {
            location_stop_works(true);
         }
         atomic_clear_bit(&s_location_flags, LOCATION_STATE_STARTED);
         atomic_clear_bit(&s_location_flags, LOCATION_STATE_INITIAL_POS);
      }
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
   s_location_gnss_result.timeouts++;
   LOG_WRN("Timeout occurred after %u[s]", s_location_gnss_result.execution_time / MSEC_PER_SEC);
   K_SPINLOCK(&location_lock)
   {
      s_location_gnss_current = s_location_gnss_result;
   }
   location_event_handler(&s_location_gnss_result);
}

static uint8_t location_tracked_satellites(bool all, const struct nrf_modem_gnss_pvt_data_frame *pvt_data)
{
   uint8_t tracked = 0;

   for (uint8_t i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
      if (pvt_data->sv[i].sv == 0) {
         break;
      }
      if (all || pvt_data->sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX) {
         tracked++;
      }
   }

   return tracked;
}

static void location_print_pvt(uint8_t tracked, const struct nrf_modem_gnss_pvt_data_frame *pvt_data)
{
   LOG_DBG("Tracked satellites: %d, flags: %02x, fix %d, deadline %d, window %d", tracked, pvt_data->flags,
           pvt_data->flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID ? 1 : 0,
           pvt_data->flags & NRF_MODEM_GNSS_PVT_FLAG_DEADLINE_MISSED ? 1 : 0,
           pvt_data->flags & NRF_MODEM_GNSS_PVT_FLAG_NOT_ENOUGH_WINDOW_TIME ? 1 : 0);

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

static void location_gnss_pvt_work_fn(struct k_work *work)
{
   bool report = false;
   uint8_t pvt_counter;
   uint8_t tracked;
   uint8_t healthy;
   int64_t now = k_uptime_get();

   if (s_location_state != LOCATION_GNSS_RUNNING) {
      /* ignore the notification. */
      LOG_INF("Ignored, GNSS not running");
      return;
   }

   if (nrf_modem_gnss_read(&s_location_gnss_result.position, sizeof(s_location_gnss_result.position), NRF_MODEM_GNSS_DATA_PVT) != 0) {
      LOG_ERR("Failed to read PVT data from GNSS");
      return;
   }

   tracked = location_tracked_satellites(true, &s_location_gnss_result.position);
   healthy = location_tracked_satellites(false, &s_location_gnss_result.position);
   pvt_counter = atomic_inc(&s_gnss_pvt_counter);
   if (pvt_counter == 3 || pvt_counter == 63) {
      LOG_INF("GNSS PVT, tracked satellites: %d/%d, flags: %02x, fix %d", healthy, tracked,
              s_location_gnss_result.position.flags,
              s_location_gnss_result.position.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID ? 1 : 0);
      if (pvt_counter == 63) {
         atomic_set(&s_gnss_pvt_counter, 4);
      }
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
   if (s_location_gnss_result.max_healthy_satellites < healthy) {
      s_location_gnss_result.max_healthy_satellites = healthy;
   }
   location_print_pvt(tracked, &s_location_gnss_result.position);

   s_location_gnss_result.records++;

   if (s_location_gnss_result.position.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
      s_location_gnss_result.result = MODEM_GNSS_POSITION;
      s_location_gnss_result.valid = true;
      if (&location_gnss_fix_work == work) {
         s_location_gnss_result.fixes++;
         report = true;
      } else {
         s_location_gnss_result.pvts++;
      }
   } else if (s_location_visibility_detection) {
      if (s_location_gnss_result.position.execution_time >= GNSS_VISIBILITY_DETECTION_EXEC_TIME &&
          s_location_gnss_result.max_satellites < GNSS_VISIBILITY_DETECTION_SAT_LIMIT) {
         LOG_INF("GNSS visibility obstructed, canceling");
         s_location_gnss_result.result = MODEM_GNSS_INVISIBLE;
         report = true;
      }
   }
   K_SPINLOCK(&location_lock)
   {
      s_location_gnss_current = s_location_gnss_result;
   }
   if (report) {
      location_event_handler(&s_location_gnss_result);
   }
}

static void location_print_expiry(const struct nrf_modem_gnss_agnss_expiry *gnss_expiry)
{
   LOG_DBG("GNSS: A-GPS - flags %02x, utc %u, klob %u, neq %u, integ %u",
           gnss_expiry->data_flags, gnss_expiry->utc_expiry, gnss_expiry->klob_expiry,
           gnss_expiry->neq_expiry, gnss_expiry->integrity_expiry);

#ifndef CONFIG_LOCATION_ENABLE_CONTINUES_MODE
   if (Z_LOG_CONST_LEVEL_CHECK(LOG_LEVEL_DBG)) {
      for (uint32_t i = 0; i < NRF_MODEM_GNSS_NUM_GPS_SATELLITES; i++) {
         if (gnss_expiry->sv[i].alm_expiry || gnss_expiry->sv[i].ephe_expiry) {
            LOG_DBG(" Sp.Veh.: %3d, alm. %d, ephe. %d",
                    i + 1, gnss_expiry->sv[i].alm_expiry, gnss_expiry->sv[i].ephe_expiry);
            k_sleep(K_MSEC(50));
         }
      }
   }
#endif
}

static void location_gnss_start(void)
{
   struct location_timing timing;
   bool changed = false;
   bool running = false;
   int err = 0;

   if (!atomic_test_bit(&s_location_flags, LOCATION_STATE_STARTED)) {
      return;
   }

   changed = location_gnss_calc_timing(&timing);
   err = nrf_modem_gnss_use_case_set(NRF_MODEM_GNSS_USE_CASE_MULTIPLE_HOT_START);
   running = err == -NRF_EINVAL;
   if (!running && err) {
      LOG_ERR("Failed to configure GNSS use case! err %d %s", -err, strerror(-err));
   }

   s_location_last_request = k_uptime_get();

   /* By default we take the first fix. */

   s_location_gnss_result.max_satellites = 0;
   s_location_gnss_result.max_healthy_satellites = 0;
   s_location_gnss_result.execution_time = 0;
   s_location_gnss_result.satellites_time = 0;

   LOG_INF("GNSS: timming %u/%u %s %s", timing.interval, timing.timeout, changed ? "changed" : "unchanged", running ? "running" : "stopped");

   if (changed || !running) {
      uint32_t dyn = 0;
      enum modem_gnss_result last_result = s_location_gnss_result.result;
      s_location_gnss_result.result = MODEM_GNSS_ERROR;
      nrf_modem_gnss_stop();
      err = nrf_modem_gnss_fix_retry_set(timing.timeout);
      if (err) {
         LOG_ERR("Failed to configure GNSS fix retry! err %d %s", -err, strerror(-err));
         location_event_handler(&s_location_gnss_result);
         return;
      }
      err = nrf_modem_gnss_fix_interval_set(timing.interval);
      if (err) {
         LOG_ERR("Failed to configure GNSS fix interval! err %d %s", -err, strerror(-err));
         location_event_handler(&s_location_gnss_result);
         return;
      }
      err = nrf_modem_gnss_power_mode_set(timing.power_mode);
      if (err) {
         LOG_ERR("Failed to configure GNSS power mode! err %d %s", -err, strerror(-err));
      }
      err = nrf_modem_gnss_agnss_expiry_get(&gnss_expiry);
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
      s_location_gnss_result.result = last_result;
      K_SPINLOCK(&location_lock)
      {
         dyn = s_location_dynamics;
      }
      err = nrf_modem_gnss_dyn_mode_change(dyn);
      if (err) {
         LOG_ERR("Failed to configure GNSS dyn mode! err %d %s", -err, strerror(-err));
      }
      atomic_set_bit(&s_location_flags, LOCATION_STATE_RUNNING);
      K_SPINLOCK(&location_lock)
      {
         s_location_timing_current = timing;
         s_location_gnss_current = s_location_gnss_result;
      }
      LOG_INF("GNSS request started.");
   } else {
      s_location_state = LOCATION_GNSS_RUNNING;
      LOG_INF("GNSS request continued.");
      K_SPINLOCK(&location_lock)
      {
         s_location_gnss_current = s_location_gnss_result;
      }
   }
}

static void location_gnss_start_work_fn(struct k_work *work)
{
   if (s_location_state != LOCATION_PENDING) {
      return;
   }

   if (&location_gnss_start_work == work) {
      LOG_INF("GNSS start request ...");
   } else {
      LOG_INF("GNSS next request ...");
   }
   s_location_state = LOCATION_WAITING_FOR_SLEEPING;
   if (!atomic_test_bit(&s_location_flags, LOCATION_STATE_LTE_SLEEPING)) {
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
   if (atomic_test_bit(&s_location_flags, LOCATION_STATE_STARTED)) {
      atomic_clear_bit(&s_location_flags, LOCATION_STATE_INITIAL_POS);
      uint16_t timeout;
      K_SPINLOCK(&location_lock)
      {
         s_location_initial_counter = 1;
         timeout = s_location_timing.timeout;
      }
      if (s_location_state == LOCATION_GNSS_RUNNING) {
         LOG_DBG("Restarting timer with timeout=%d", timeout);
         //         work_reschedule_for_io_queue(&location_gnss_timeout_work, K_SECONDS(timeout));
      } else {
         s_location_state = LOCATION_PENDING;
         work_submit_to_io_queue(&location_gnss_start_work);
      }
   }
}

static void location_start_internal(bool force)
{
   if (force && atomic_test_bit(&s_location_flags, LOCATION_STATE_INITIAL_POS)) {
      force = false;
   }
   if (force) {
      LOG_INF("Location: force init");
   }
   if (!atomic_test_and_set_bit(&s_location_flags, LOCATION_STATE_STARTED) || force) {
      work_submit_to_io_queue(&location_scan_start_work);
   }
}

static void location_stop_internal(void)
{
   atomic_clear_bit(&s_location_flags, LOCATION_STATE_STARTED);
   atomic_clear_bit(&s_location_flags, LOCATION_STATE_INITIAL_POS);
   s_location_state = LOCATION_NONE;
   location_stop_works(false);
   k_work_cancel(&location_scan_start_work);
}

bool location_enabled(void)
{
   return atomic_test_bit(&s_location_flags, LOCATION_STATE_STARTED);
}

void location_start(bool force)
{
   if (!atomic_test_bit(&s_location_flags, LOCATION_STATE_LOCKED)) {
      location_start_internal(force);
   }
}

void location_stop(void)
{
   if (!atomic_test_bit(&s_location_flags, LOCATION_STATE_LOCKED)) {
      location_stop_internal();
   }
}

modem_gnss_result_t location_get(struct modem_gnss_state *location, bool *running)
{
   modem_gnss_result_t result;

   K_SPINLOCK(&location_lock)
   {
      result = s_location_gnss_state.result;
      if (location) {
         *location = s_location_gnss_state;
      }
      if (running) {
         *running = (s_location_state == LOCATION_GNSS_RUNNING);
      }
   }

   return result;
}

#ifdef CONFIG_SH_CMD
#include "parse.h"
#include "sh_cmd.h"
#include <ctype.h>

static bool sh_cmd_loc_param(const char *desc, const char *parameter, uint16_t *config)
{
   const char *cur = parameter;
   const char *t = parameter;
   long value = 0;
   bool changed = false;

   LOG_DBG("Location parameter: %s", parameter);

   t = parse_next_long(cur, 10, &value);
   if (t != cur) {
      K_SPINLOCK(&location_lock)
      {
         if (value != *config) {
            *config = value;
            changed = true;
         }
      }
      LOG_INF("Location %s: %u s", desc, (unsigned int)value);
   }
   return changed;
}

static int sh_cmd_loc(const char *parameter)
{
   int res = 0;
   const char *cur = parameter;
   char value[10];

   memset(value, 0, sizeof(value));
   cur = parse_next_text(cur, ' ', value, sizeof(value));

   if (value[0]) {
      if (!stricmp("start", value)) {
         atomic_set_bit(&s_location_flags, LOCATION_STATE_LOCKED);
         sh_cmd_loc_param("interval", cur, &s_location_timing.interval);
         location_start_internal(false);
      } else if (!stricmp("stop", value)) {
         atomic_set_bit(&s_location_flags, LOCATION_STATE_LOCKED);
         location_stop_internal();
      } else if (!stricmp("force", value)) {
         atomic_set_bit(&s_location_flags, LOCATION_STATE_LOCKED);
         sh_cmd_loc_param("interval", cur, &s_location_timing.interval);
         location_start_internal(true);
#ifdef CONFIG_LOCATION_ENABLE_AUTO_MODE
      } else if (!stricmp("auto", value)) {
         atomic_clear_bit(&s_location_flags, LOCATION_STATE_LOCKED);
         location_stop_internal();
#endif /* CONFIG_LOCATION_ENABLE_AUTO_MODE */
      }
   } else {
      location_state_t state = s_location_state;
      struct modem_gnss_state gnss_current;
      struct modem_gnss_state gnss_state;
      uint16_t interval;
      uint16_t timeout;

      K_SPINLOCK(&location_lock)
      {
         gnss_current = s_location_gnss_current;
         gnss_state = s_location_gnss_state;
         interval = s_location_timing.interval;
         timeout = s_location_timing.timeout;
      }

      const char *desc = "unknown";
      switch (state) {
         case LOCATION_NONE:
            desc = "none";
            break;
         case LOCATION_PENDING:
            desc = "pending";
            break;
         case LOCATION_WAITING_FOR_SLEEPING:
            desc = "waiting for sleeping";
            break;
         case LOCATION_GNSS_RUNNING:
            desc = "GNSS enabled";
            break;
         case LOCATION_DONE:
            desc = "done";
            break;
      }
      const char *res = "unknown";
      switch (gnss_state.result) {
         case MODEM_GNSS_NOT_AVAILABLE:
            res = "n.a.";
            break;
         case MODEM_GNSS_TIMEOUT:
            res = "timeout";
            break;
         case MODEM_GNSS_ERROR:
            res = "error";
            break;
         case MODEM_GNSS_INVISIBLE:
            res = "invisible";
            break;
         case MODEM_GNSS_POSITION:
            res = "position";
            break;
      }

      if (0 == interval) {
         LOG_INF("Location: %s, once, timeout %us", desc, timeout);
      } else if (1 == interval) {
         LOG_INF("Location: %s, continue, timeout %us", desc, timeout);
      } else {
         LOG_INF("Location: %s, interval: %us, timeout %us", desc, interval, timeout);
      }
      if (LOCATION_NONE != state) {
         const char *mode = "paused";
         if (atomic_test_bit(&s_location_flags, LOCATION_STATE_RUNNING)) {
            if (atomic_test_bit(&s_location_flags, LOCATION_STATE_WAKEUP)) {
               mode = "active";
            } else {
               mode = "sleeping";
            }
         }
         if (gnss_current.max_satellites) {
            LOG_INF("Location: %s, satellites %d/%d last %d/%d", mode, gnss_current.max_healthy_satellites, gnss_current.max_satellites,
                    gnss_state.max_healthy_satellites, gnss_state.max_satellites);
         } else {
            LOG_INF("Location: %s, satellites %d/%d", mode, gnss_state.max_healthy_satellites, gnss_state.max_satellites);
         }
         LOG_INF("Location: %d records, %d/%d fixes, %d timeouts", gnss_current.records, gnss_current.fixes, gnss_current.pvts, gnss_current.timeouts);
         if (gnss_state.valid) {
            LOG_INF("Location: %f, %f, %f", gnss_state.position.latitude, gnss_state.position.longitude, (double)gnss_state.position.altitude);
            LOG_INF("location: %04d-%02d-%02dT%02d:%02d:%02dZ",
                    gnss_state.position.datetime.year, gnss_state.position.datetime.month, gnss_state.position.datetime.day,
                    gnss_state.position.datetime.hour, gnss_state.position.datetime.minute, gnss_state.position.datetime.seconds);
         }
      }
   }

   return res;
}

static void sh_cmd_loc_help(void)
{
   LOG_INF("> help loc:");
   LOG_INF("  loc                            : show location mode.");
#ifdef CONFIG_LOCATION_ENABLE_AUTO_MODE
   LOG_INF("  loc [stop|auto]                : set location mode.");
#else /* CONFIG_LOCATION_ENABLE_AUTO_MODE */
   LOG_INF("  loc stop                       : set location mode.");
#endif /* CONFIG_LOCATION_ENABLE_AUTO_MODE */
   LOG_INF("  loc [start|force [<interval>]] : set location mode and interval.");
   LOG_INF("      interval                   : 0 := single position, 1 := continues mode");
   LOG_INF("                                 : n := interval in seconds");
}

static const sh_cmd_catalog_t s_location_psm[] = {
    {.name = "disabled", .desc = NULL, .value = NRF_MODEM_GNSS_PSM_DISABLED},
    {.name = "performance", .desc = NULL, .value = NRF_MODEM_GNSS_PSM_DUTY_CYCLING_PERFORMANCE},
    {.name = "power", .desc = NULL, .value = NRF_MODEM_GNSS_PSM_DUTY_CYCLING_POWER},
    {.name = NULL, .desc = NULL, .value = -EINVAL},
};

static const sh_cmd_catalog_t s_location_dyn[] = {
    {.name = "general", .desc = "max. 100 km/h", .value = NRF_MODEM_GNSS_DYNAMICS_GENERAL_PURPOSE},
    {.name = "static", .desc = "max. 5 km/h", .value = NRF_MODEM_GNSS_DYNAMICS_STATIONARY},
    {.name = "pedestrian", .desc = "max. 30 km/h", .value = NRF_MODEM_GNSS_DYNAMICS_PEDESTRIAN},
    {.name = "automotive", .desc = "more than 100 km/h", .value = NRF_MODEM_GNSS_DYNAMICS_AUTOMOTIVE},
    {.name = NULL, .desc = NULL, .value = -EINVAL},
};

static int sh_cmd_locfg(const char *parameter)
{
   int res = 0;
   const char *cur = parameter;
   char value[32];

   memset(value, 0, sizeof(value));
   cur = parse_next_text(cur, ' ', value, sizeof(value));

   if (value[0]) {
      if (!stricmp("timeout", value)) {
         sh_cmd_loc_param("timeout", cur, &s_location_timing.timeout);
      } else if (!stricmp("interval", value)) {
         sh_cmd_loc_param("interval", cur, &s_location_timing.interval);
      } else if (!stricmp("psm", value)) {
         cur = parse_next_text(cur, ' ', value, sizeof(value));
         res = sh_cmd_get_catalog_value(s_location_psm, value, 3);
         if (0 <= res) {
            K_SPINLOCK(&location_lock)
            {
               s_location_timing.power_mode = res;
            }
         }
      } else if (!stricmp("dyn", value)) {
         cur = parse_next_text(cur, ' ', value, sizeof(value));
         res = sh_cmd_get_catalog_value(s_location_dyn, value, 3);
         if (0 <= res) {
            K_SPINLOCK(&location_lock)
            {
               s_location_dynamics = res;
            }
         }
      }
   } else {
      const char *psm = "";
      const char *dyn = "";
      uint32_t dyn_mode;
      uint16_t interval;
      uint16_t timeout;
      uint8_t psm_mode;

      K_SPINLOCK(&location_lock)
      {
         psm_mode = s_location_timing.power_mode;
         interval = s_location_timing.interval;
         timeout = s_location_timing.timeout;
         dyn_mode = s_location_dynamics;
      }

      psm = sh_cmd_get_catalog_name(s_location_psm, psm_mode);
      dyn = sh_cmd_get_catalog_name(s_location_dyn, dyn_mode);

      LOG_INF("Location: interval %us, timeout %us, psm %s, dynamics %s", interval, timeout, psm ? psm : "??", dyn ? dyn : "??");
   }

   return res;
}

static void sh_cmd_locfg_help(void)
{
   LOG_INF("> help locfg:");
   LOG_INF("  locfg                 : show location config.");
   LOG_INF("  locfg timeout <value> : set location timeout in s.");
   LOG_INF("  locfg psm <value>     : set location psm. Values disabled,");
   LOG_INF("                          performance, or power.");
   LOG_INF("  locfg dyn <value>     : set location dynamic mode.");
   LOG_INF("                          general, static, pedestrian");
   LOG_INF("                          or automotive.");
}

#include <tfm_ioctl_api.h>

static int sh_cmd_read_otp(const char *parameter)
{
   const char *cur = parameter;
   uint8_t buf[190 * sizeof(uint32_t)];
   long addr = 0;
   long len = sizeof(buf);
   long left = 0;
   int err = 0;
   enum tfm_platform_err_t plt_err;

   cur = parse_next_long_text(cur, ' ', 0, &addr);
   cur = parse_next_long_text(cur, ' ', 0, &len);
   if (0 > addr) {
      addr = 0;
   } else if (189 < addr) {
      addr = 189;
   }
   left = sizeof(buf) - (addr * sizeof(uint32_t));
   if (0 > len) {
      len = 1;
   } else if (len > left) {
      len = left;
   }

   plt_err = tfm_platform_mem_read(buf, (intptr_t)&NRF_UICR_S->OTP[addr], len, &err);
   if (plt_err != TFM_PLATFORM_ERR_SUCCESS || err != 0) {
      /* Handle error */
   } else {
      LOG_INF("OTP: 0x%03x 0x%03x", (int)addr * sizeof(uint32_t), (int)len);
      LOG_HEXDUMP_INF(buf, len, "OTP");
   }
   return 0;
}

static void sh_cmd_read_otp_help(void)
{
   LOG_INF("> help otp:");
   LOG_INF("  otp [<addr> [<len>]]  : read otp.");
   LOG_INF("      <addr>            : 0-189");
   LOG_INF("      <len>             : 0-760");
}

SH_CMD(loc, NULL, "location mode.", sh_cmd_loc, sh_cmd_loc_help, 0);
SH_CMD(locfg, NULL, "location config.", sh_cmd_locfg, sh_cmd_locfg_help, 0);
SH_CMD(otp, NULL, "read OTP.", sh_cmd_read_otp, sh_cmd_read_otp_help, 0);

#endif /* CONFIG_SH_CMD */