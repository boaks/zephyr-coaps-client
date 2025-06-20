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

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>

#include "appl_diagnose.h"
#include "io_job_queue.h"
#include "modem.h"
#include "modem_at.h"
#include "modem_desc.h"
#include "modem_sim.h"
#include "parse.h"
#include "ui.h"

/* auto generated header file during west build */
#include "ncs_version.h"

LOG_MODULE_REGISTER(MODEM, CONFIG_MODEM_LOG_LEVEL);

#if defined(CONFIG_NRF_MODEM_LIB)

#include <modem/at_monitor.h>
#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <nrf_modem_at.h>

#define MSEC_TO_SEC(X) (((X) + (MSEC_PER_SEC / 2)) / MSEC_PER_SEC)

#define MULTI_IMSI_MINIMUM_TIMEOUT_MS (300 * MSEC_PER_SEC)

#define LED_READY LED_LTE_2
#define LED_CONNECTED LED_LTE_3
#define LED_SEARCH LED_NONE

static K_MUTEX_DEFINE(lte_mutex);
static K_CONDVAR_DEFINE(lte_condvar);

#define MODEM_LIB_INITIALIZED 0
#define MODEM_INITIALIZED 1
#define MODEM_INTERRUPT_SEARCH 2
#define MODEM_FIRMWARE_2 3
#define MODEM_SIGNAL_READY 4
#define MODEM_READY 5
#define MODEM_CONNECTED 6
#define MODEM_LOW_POWER 7
#define MODEM_LTE_MODE_INITIALIZED 8
#define MODEM_LTE_MODE_PREFERENCE 9
#define MODEM_LTE_MODE_FORCE 10

static atomic_t modem_states = ATOMIC_INIT(0);

static inline bool modem_states_changed(int bit, bool value)
{
   if (value) {
      return !atomic_test_and_set_bit(&modem_states, bit);
   } else {
      return atomic_test_and_clear_bit(&modem_states, bit);
   }
}

static lte_state_change_callback_handler_t lte_state_change_handler = NULL;

static int lte_initial_config = 0;
static enum lte_lc_system_mode lte_initial_mode = LTE_LC_SYSTEM_MODE_LTEM;

static struct lte_lc_edrx_cfg edrx_status = {LTE_LC_LTE_MODE_NONE, 0.0, 0.0};
static struct lte_lc_psm_cfg psm_status = {0, -1};

#ifdef CONFIG_UDP_PSM_ENABLE
static int psm_rat = -1;
#endif

static int rai_lock = 0;
static int16_t requested_edrx_time_s = 0;
static int16_t requested_ptw_time_s = 0;

static uint32_t lte_starts = 0;
static uint32_t lte_searchs = 0;
static uint32_t lte_psm_delays = 0;
static uint32_t lte_cell_updates = 0;
static uint32_t lte_wakeups = 0;
static uint32_t lte_low_voltage = 0;
static int64_t lte_search_time = 0;
static int64_t lte_wakeup_time = 0;
static int64_t lte_connected_time = 0;
static int64_t lte_asleep_time = 0;
static int64_t lte_psm_delay_time = 0;

static struct lte_modem_info modem_info;
static struct lte_network_info network_info;
static struct lte_ce_info ce_info;

static int64_t transmission_time = 0;
static int64_t network_search_time = 0;
static int64_t scan_time = 0;

#define CP_RAI_MAX_DELAY 500
#define AS_RAI_MAX_DELAY 3000

static volatile enum rai_mode rai_current_mode = RAI_MODE_OFF;
static volatile enum lte_network_rai rai_network_state = LTE_NETWORK_RAI_UNKNOWN;
static volatile int rai_time = -1;

#define SUSPEND_DELAY_MILLIS 100

#ifdef CONFIG_NRF_MODEM_LIB_ON_FAULT_APPLICATION_SPECIFIC

#define FAULT_COUNTER_MASK 0xABCDEF
#define WEEK_IN_MILLIS (MSEC_PER_SEC * 60 * 60 * 24 * 7)

static int64_t fault_time1 = 0;
static int64_t fault_time2 = FAULT_COUNTER_MASK;
static uint32_t fault_counter1 = 0;
static uint32_t fault_counter2 = FAULT_COUNTER_MASK;

void nrf_modem_fault_handler(struct nrf_modem_fault_info *fault_info)
{
   bool reboot = true;
#if (CONFIG_MODEM_FAULT_THRESHOLD > 0)
   if (fault_counter1 == (fault_counter2 ^ FAULT_COUNTER_MASK) &&
       fault_time1 == (fault_time2 ^ FAULT_COUNTER_MASK)) {
      // counter & timer are valid
      int64_t now = k_uptime_get();
      if (fault_time1 + WEEK_IN_MILLIS < now) {
         // timeout => reset
         fault_counter1 = 0;
      }
      if (!fault_counter1) {
         // first fault => start timeout
         fault_time1 = now;
         fault_time2 = fault_time1 ^ FAULT_COUNTER_MASK;
      }
      if (fault_counter1++ < CONFIG_MODEM_FAULT_THRESHOLD) {
         fault_counter2 = fault_counter1 ^ FAULT_COUNTER_MASK;
         reboot = false;
      }
   }
#endif
   if (fault_info) {
      LOG_ERR("Modem error: 0x%x, PC: 0x%x, %u", fault_info->reason, fault_info->program_counter, fault_counter1);
   } else {
      LOG_ERR("Modem error: %u", fault_counter1);
   }
   if (reboot) {
      appl_reboot(ERROR_CODE_MODEM_FAULT, K_NO_WAIT);
   }
}
#endif

static void modem_read_sim_work_fn(struct k_work *work)
{
   modem_sim_read_info(NULL, false);
}

static K_WORK_DEFINE(modem_read_sim_work, modem_read_sim_work_fn);

static void modem_read_info_work_fn(struct k_work *work)
{
   modem_read_network_info(NULL, false);
}

static K_WORK_DEFINE(modem_read_network_info_work, modem_read_info_work_fn);

static void modem_read_coverage_enhancement_info_work_fn(struct k_work *work)
{
   bool rate_limit;
   modem_read_coverage_enhancement_info(NULL);
   k_mutex_lock(&lte_mutex, K_FOREVER);
   rate_limit = network_info.rate_limit;
   k_mutex_unlock(&lte_mutex);
   if (rate_limit) {
      modem_read_rate_limit_time(NULL);
   }
}

static K_WORK_DEFINE(modem_read_coverage_enhancement_info_work, modem_read_coverage_enhancement_info_work_fn);

static void modem_state_change_callback_work_fn(struct k_work *work);
static void modem_ready_work_fn(struct k_work *work);

static K_WORK_DEFINE(modem_registered_callback_work, modem_state_change_callback_work_fn);
static K_WORK_DEFINE(modem_unregistered_callback_work, modem_state_change_callback_work_fn);
static K_WORK_DEFINE(modem_ready_callback_work, modem_state_change_callback_work_fn);
static K_WORK_DEFINE(modem_not_ready_callback_work, modem_state_change_callback_work_fn);
static K_WORK_DEFINE(modem_connected_callback_work, modem_state_change_callback_work_fn);
static K_WORK_DEFINE(modem_unconnected_callback_work, modem_state_change_callback_work_fn);
static K_WORK_DEFINE(modem_low_voltage_callback_work, modem_state_change_callback_work_fn);
static K_WORK_DEFINE(modem_power_management_resume_work, modem_state_change_callback_work_fn);
static K_WORK_DEFINE(modem_power_management_suspend_work, modem_state_change_callback_work_fn);
static K_WORK_DEFINE(modem_psm_active_work, modem_state_change_callback_work_fn);
static K_WORK_DEFINE(modem_psm_inactive_work, modem_state_change_callback_work_fn);

static K_WORK_DELAYABLE_DEFINE(modem_ready_work, modem_ready_work_fn);

static void modem_ready_work_fn(struct k_work *work)
{
   bool ready = atomic_test_bit(&modem_states, MODEM_READY);
   if (ready) {
      lte_state_change_callback_handler_t callback = lte_state_change_handler;
      if (callback) {
         callback(LTE_STATE_READY_1S, true);
      }
   }
   if (ready) {
      k_mutex_lock(&lte_mutex, K_FOREVER);
      atomic_set_bit(&modem_states, MODEM_SIGNAL_READY);
      k_condvar_broadcast(&lte_condvar);
      k_mutex_unlock(&lte_mutex);
      LOG_INF("modem signaled ready.");
   }
}

static void modem_state_change_callback_work_fn(struct k_work *work)
{
   lte_state_change_callback_handler_t callback = lte_state_change_handler;
   if (callback) {
      if (work == &modem_low_voltage_callback_work) {
         callback(LTE_STATE_LOW_VOLTAGE, true);
      } else if (work == &modem_connected_callback_work) {
         callback(LTE_STATE_CONNECTED, true);
      } else if (work == &modem_unconnected_callback_work) {
         callback(LTE_STATE_CONNECTED, false);
      } else if (work == &modem_ready_callback_work) {
         callback(LTE_STATE_READY, true);
      } else if (work == &modem_not_ready_callback_work) {
         callback(LTE_STATE_READY, false);
      } else if (work == &modem_registered_callback_work) {
         callback(LTE_STATE_REGISTRATION, true);
         modem_sim_network(true);
      } else if (work == &modem_unregistered_callback_work) {
         callback(LTE_STATE_REGISTRATION, false);
         modem_sim_network(false);
      } else if (work == &modem_power_management_resume_work) {
         callback(LTE_STATE_SLEEPING, false);
      } else if (work == &modem_power_management_suspend_work) {
         callback(LTE_STATE_SLEEPING, true);
      } else if (work == &modem_psm_active_work) {
         callback(LTE_STATE_PSM_ACTIVE, true);
      } else if (work == &modem_psm_inactive_work) {
         callback(LTE_STATE_PSM_ACTIVE, false);
      }
   }
}

bool modem_set_preference(enum preference_mode mode)
{
   enum lte_lc_system_mode lte_mode;
   enum lte_lc_system_mode_preference lte_preference;

   if (!lte_lc_system_mode_get(&lte_mode, &lte_preference)) {
      if (lte_mode == LTE_LC_SYSTEM_MODE_LTEM_NBIOT ||
          lte_mode == LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS) {
         bool nbiot_preference = false;
         enum lte_lc_system_mode_preference lte_new_preference = lte_preference;

         switch (lte_preference) {
            case LTE_LC_SYSTEM_MODE_PREFER_NBIOT:
               lte_new_preference = LTE_LC_SYSTEM_MODE_PREFER_LTEM;
               nbiot_preference = true;
               break;
            case LTE_LC_SYSTEM_MODE_PREFER_LTEM:
               lte_new_preference = LTE_LC_SYSTEM_MODE_PREFER_NBIOT;
               break;
            case LTE_LC_SYSTEM_MODE_PREFER_NBIOT_PLMN_PRIO:
               lte_new_preference = LTE_LC_SYSTEM_MODE_PREFER_LTEM_PLMN_PRIO;
               nbiot_preference = true;
               break;
            case LTE_LC_SYSTEM_MODE_PREFER_LTEM_PLMN_PRIO:
               lte_new_preference = LTE_LC_SYSTEM_MODE_PREFER_NBIOT_PLMN_PRIO;
               break;
            default:
               break;
         }
         if (lte_new_preference != lte_preference) {
            const char *op = "Set";
            const char *sys_mode = nbiot_preference ? "LTE-M" : "NB-IoT";
            switch (mode) {
               case RESET_PREFERENCE:
                  op = "Reset";
                  lte_new_preference = CONFIG_LTE_MODE_PREFERENCE_VALUE;
                  break;
               case SWAP_PREFERENCE:
                  op = "Swap";
                  break;
               case NBIOT_PREFERENCE:
                  lte_new_preference = LTE_LC_SYSTEM_MODE_NBIOT;
                  break;
               case LTE_M_PREFERENCE:
                  lte_new_preference = LTE_LC_SYSTEM_MODE_LTEM;
                  break;
            }
            if (lte_new_preference != lte_preference) {
               LOG_INF("%s LTE mode preference to %s", op, sys_mode);
               enum lte_lc_func_mode func_mode;
               if (!lte_lc_func_mode_get(&func_mode)) {
                  if (func_mode != LTE_LC_FUNC_MODE_POWER_OFF) {
                     watchdog_feed();
                     lte_lc_func_mode_set(LTE_LC_FUNC_MODE_POWER_OFF);
                  }
                  lte_lc_system_mode_set(lte_mode, lte_new_preference);
                  if (func_mode != LTE_LC_FUNC_MODE_POWER_OFF) {
                     watchdog_feed();
                     lte_lc_func_mode_set(func_mode);
                  }
               }
            } else {
               sys_mode = nbiot_preference ? "NB-IoT" : "LTE-M";
               LOG_INF("Keep LTE mode preference %s", sys_mode);
            }
            atomic_set_bit(&modem_states, MODEM_LTE_MODE_PREFERENCE);
            return true;
         }
      } else {
         atomic_clear_bit(&modem_states, MODEM_LTE_MODE_PREFERENCE);
      }
   }
   return false;
}

bool modem_uses_preference(void)
{
   return atomic_test_bit(&modem_states, MODEM_LTE_MODE_PREFERENCE);
}

static int lte_ready_wait(k_timeout_t timeout)
{
   int status = -EINPROGRESS;
   int res = -EINPROGRESS;
   if (!k_mutex_lock(&lte_mutex, timeout)) {
      if (atomic_test_bit(&modem_states, MODEM_LOW_POWER)) {
         res = -EINVAL;
      } else if (atomic_test_bit(&modem_states, MODEM_SIGNAL_READY)) {
         status = 0;
         res = 0;
      } else {
         if (!k_condvar_wait(&lte_condvar, &lte_mutex, timeout)) {
            if (atomic_test_bit(&modem_states, MODEM_SIGNAL_READY)) {
               res = 0;
            }
         }
      }
      k_mutex_unlock(&lte_mutex);
   }
   if (res == -EINVAL) {
      LOG_INF("Modem low voltage.");
   } else if (status == 0) {
      LOG_INF("Modem is ready.");
   } else if (res == 0) {
      LOG_INF("Modem becomes ready.");
   } else {
      LOG_DBG("Modem searching ...");
   }
   return res;
}

#if defined(CONFIG_LTE_LC_EDRX_MODULE)
static void lte_set_edrx_status(const struct lte_lc_edrx_cfg *edrx)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   edrx_status = *edrx;
   k_mutex_unlock(&lte_mutex);
}
#endif /* CONFIG_LTE_LC_EDRX_MODULE */

#if defined(CONFIG_LTE_LC_PSM_MODULE)
static void lte_set_psm_status(const struct lte_lc_psm_cfg *psm)
{
   bool active = 0 <= psm->active_time;

   k_mutex_lock(&lte_mutex, K_FOREVER);
   psm_status = *psm;
   if (network_info.psm_active != (active ? LTE_NETWORK_STATE_ON : LTE_NETWORK_STATE_OFF)) {
      network_info.psm_active = active ? LTE_NETWORK_STATE_ON : LTE_NETWORK_STATE_OFF;
      if (active) {
         work_submit_to_io_queue(&modem_psm_active_work);
      } else {
         work_submit_to_io_queue(&modem_psm_inactive_work);
      }
   }
   k_mutex_unlock(&lte_mutex);
}
#endif /* CONFIG_LTE_LC_PSM_MODULE */

#if defined(CONFIG_LTE_LC_MODEM_SLEEP_MODULE)
static void lte_inc_psm_delays(int64_t time)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   ++lte_psm_delays;
   lte_psm_delay_time += time;
   k_mutex_unlock(&lte_mutex);
}
#endif /* CONFIG_LTE_LC_MODEM_SLEEP_MODULE */

static void lte_start_search(void)
{
   int64_t now = k_uptime_get();
   k_mutex_lock(&lte_mutex, K_FOREVER);
   ++lte_searchs;
   if (network_search_time) {
      lte_search_time += (now - network_search_time);
   }
   network_search_time = now;
   k_mutex_unlock(&lte_mutex);
   ui_led_op(LED_SEARCH, LED_SET);
}

static void lte_end_search(void)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   if (network_search_time) {
      lte_search_time += (k_uptime_get() - network_search_time);
      network_search_time = 0;
   }
   k_mutex_unlock(&lte_mutex);
   ui_led_op(LED_SEARCH, LED_CLEAR);
}

static void lte_update_cell(uint16_t tac, uint32_t id)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   if (network_info.cell != id || network_info.tac != tac) {
      lte_cell_updates++;
      network_info.tac = tac;
      network_info.cell = id;
   }
   k_mutex_unlock(&lte_mutex);
}

static void lte_inc_wakeups(int64_t time)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   ++lte_wakeups;
   lte_wakeup_time += time;
   k_mutex_unlock(&lte_mutex);
}

static void lte_add_connected(int64_t time)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   lte_connected_time += time;
   k_mutex_unlock(&lte_mutex);
}

#if defined(CONFIG_LTE_LC_MODEM_SLEEP_MODULE)
static void lte_add_asleep(int64_t time)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   lte_asleep_time += time;
   k_mutex_unlock(&lte_mutex);
}
#endif /* CONFIG_LTE_LC_MODEM_SLEEP_MODULE */

static int64_t get_transmission_time(void)
{
   int64_t time;
   k_mutex_lock(&lte_mutex, K_FOREVER);
   time = transmission_time;
   k_mutex_unlock(&lte_mutex);
   return time;
}

static void lte_connection_status(void)
{
#ifdef CONFIG_PDN
   bool ready = network_info.registered == LTE_NETWORK_STATE_ON &&
                network_info.pdn_active == LTE_NETWORK_STATE_ON;
#else
   bool ready = network_info.registered == LTE_NETWORK_STATE_ON;
#endif

   bool connected = ready &&
                    network_info.rrc_active == LTE_NETWORK_STATE_ON;
   if (!connected && atomic_test_and_clear_bit(&modem_states, MODEM_CONNECTED)) {
      work_submit_to_io_queue(&modem_unconnected_callback_work);
   }
   if (modem_states_changed(MODEM_READY, ready)) {
      atomic_clear_bit(&modem_states, MODEM_SIGNAL_READY);
      ui_led_op(LED_READY, ready ? LED_SET : LED_CLEAR);
      if (ready) {
         ui_led_op(LED_SEARCH, LED_CLEAR);
         work_submit_to_io_queue(&modem_read_network_info_work);
         work_submit_to_io_queue(&modem_ready_callback_work);
         work_reschedule_for_io_queue(&modem_ready_work, K_MSEC(1000));
         LOG_INF("Modem ready.");
      } else {
         k_work_cancel_delayable(&modem_ready_work);
         work_submit_to_io_queue(&modem_not_ready_callback_work);
#ifdef CONFIG_PDN
         LOG_INF("Modem not ready. con=%s/reg=%s/pdn=%s",
                 modem_get_state_type(network_info.rrc_active),
                 modem_get_state_type(network_info.registered),
                 modem_get_state_type(network_info.pdn_active));
#else
         LOG_INF("Modem not ready. con=%s/reg=%s",
                 modem_get_state_type(network_info.rrc_active),
                 modem_get_state_type(network_info.registered));
#endif
      }
   }
   if (connected && !atomic_test_and_set_bit(&modem_states, MODEM_CONNECTED)) {
      work_submit_to_io_queue(&modem_read_coverage_enhancement_info_work);
      work_submit_to_io_queue(&modem_connected_callback_work);
   }
}

static void lte_registration_set(bool registered)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   if (network_info.registered != (registered ? LTE_NETWORK_STATE_ON : LTE_NETWORK_STATE_OFF)) {
      network_info.registered = registered ? LTE_NETWORK_STATE_ON : LTE_NETWORK_STATE_OFF;
      rai_time = -1;
      lte_connection_status();
   }
   k_mutex_unlock(&lte_mutex);
}

static void lte_connection_status_set(bool connect)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   if (network_info.rrc_active != (connect ? LTE_NETWORK_STATE_ON : LTE_NETWORK_STATE_OFF)) {
      ui_led_op(LED_CONNECTED, connect ? LED_SET : LED_CLEAR);
      network_info.rrc_active = connect ? LTE_NETWORK_STATE_ON : LTE_NETWORK_STATE_OFF;
      lte_connection_status();
   }
   k_mutex_unlock(&lte_mutex);
}

static void lte_network_mode_set(enum lte_lc_lte_mode mode)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   if (network_info.mode != mode) {
      network_info.mode = mode;
      rai_time = -1;
   }
   k_mutex_unlock(&lte_mutex);
}

static void lte_network_sleeping_set(bool sleep)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   if (network_info.sleeping != (sleep ? LTE_NETWORK_STATE_ON : LTE_NETWORK_STATE_OFF)) {
      network_info.sleeping = sleep ? LTE_NETWORK_STATE_ON : LTE_NETWORK_STATE_OFF;
      if (sleep) {
         work_submit_to_io_queue(&modem_power_management_suspend_work);
      } else {
         work_submit_to_io_queue(&modem_power_management_resume_work);
      }
   }
   k_mutex_unlock(&lte_mutex);
}

AT_MONITOR(modem_monitor, ANY, modem_monitor_handler);

static const char *IGNORE_NOTIFY[] = {"%NCELLMEAS:", NULL};
// static const char *IGNORE_NOTIFY[] = {"%NCELLMEAS:", "%XMODEMSLEEP:", NULL};

static int modem_monitor_ignore_notify(const char *notif)
{
   int index = 0;
   while (IGNORE_NOTIFY[index]) {
      if (strstart(notif, IGNORE_NOTIFY[index], false)) {
         return index;
      }
      ++index;
   }
   return -1;
}

static void modem_monitor_handler(const char *notif)
{
   int index;

   if (appl_reboots()) {
      return;
   }
   index = modem_monitor_ignore_notify(notif);
   if (index < 0) {
      int len;
      printk("%s", notif);
      len = strstart(notif, "+CEREG:", false);
      if (len > 0) {
         const char *cur = parse_next_chars(notif + len, ',', 4);
         if (*cur && strstart(cur, "0,", false)) {
            int code = atoi(cur + 2);
            const char *desc = modem_get_emm_cause_description(code);
            if (desc) {
               LOG_INF("LTE +CEREG: rejected, %s", desc);
            } else {
               LOG_INF("LTE +CEREG: rejected, cause %d", code);
            }
         }
      }
   }
}

static void lte_registration(enum lte_lc_nw_reg_status reg_status)
{
   bool registered = false;
   bool search = false;
   const char *description = modem_get_registration_description(reg_status);

   switch (reg_status) {
      case LTE_LC_NW_REG_REGISTERED_HOME:
      case LTE_LC_NW_REG_REGISTERED_ROAMING:
         registered = true;
         break;
      case LTE_LC_NW_REG_SEARCHING:
         search = true;
         break;
      default:
         break;
   }
   if (search) {
      lte_start_search();
      work_submit_to_io_queue(&modem_read_sim_work);
   } else {
      lte_end_search();
   }
   if (registered) {
      work_submit_to_io_queue(&modem_registered_callback_work);
   } else {
      work_submit_to_io_queue(&modem_unregistered_callback_work);
   }
   LOG_INF("Network status: %s", description);
   lte_registration_set(registered);
}

static inline int lte_lc_ncell_quality(const struct lte_lc_ncell *ncell)
{
   return ncell->rsrp + (ncell->rsrq / 2);
}

static inline int lte_lc_cell_quality(const struct lte_lc_cell *gci_cell)
{
   return gci_cell->rsrp + (gci_cell->rsrq / 2);
}

#define MIN_QUALITY_DELTA 15

static size_t modem_last_neighbor_cell_meas_len = 0;
static char modem_last_neighbor_cell_meas[800];

int modem_get_last_neighbor_cell_meas(char *buf, size_t len)
{
   int res = 0;
   k_mutex_lock(&lte_mutex, K_FOREVER);
   if (modem_last_neighbor_cell_meas_len) {
      res = MIN(modem_last_neighbor_cell_meas_len, len - 1);
      if (buf) {
         memcpy(buf, modem_last_neighbor_cell_meas, res);
      }
   }
   k_mutex_unlock(&lte_mutex);
   if (buf) {
      buf[res] = 0;
   }
   return res;
}

int modem_clear_last_neighbor_cell_meas(void)
{
   int res = 0;
   k_mutex_lock(&lte_mutex, K_FOREVER);
   modem_last_neighbor_cell_meas_len = 0;
   k_mutex_unlock(&lte_mutex);
   return res;
}

#if defined(CONFIG_LTE_LC_NEIGHBOR_CELL_MEAS_MODULE)

#define RSRP(X) ((X) - 140)
#define RSRQ(X) (((X) - 39) / 2)

static int append_result(char *buf, size_t idx, size_t len, const char *line)
{
   LOG_INF("%s", line);
   if (idx < len) {
      size_t line_len = strlen(line);
      size_t left = len - idx;
      len = MIN(left - 1, line_len);
      if (len > 0) {
         memmove(&buf[idx], line, len);
         idx += len;
         buf[idx++] = '\n';
         if (idx == len) {
            --idx;
         }
         buf[idx] = 0;
      }
   }
   return idx;
}

static void lte_neighbor_cell_meas(const struct lte_lc_cells_info *cells_info)
{
   static unsigned int scans = 0;
   static unsigned int hits = 0;
   static int64_t all_scan_time = 0;

   int64_t now = k_uptime_get();
   int64_t all = 0;
   int current_cell;
   int current_earfcn;
   enum lte_lc_lte_mode mode;
   size_t idx = 0;
   char mnc[4];
   char line[128];

   memset(mnc, 0, sizeof(mnc));

   k_mutex_lock(&lte_mutex, K_FOREVER);
   mode = network_info.mode;
   modem_last_neighbor_cell_meas_len = 0;
   current_cell = network_info.cell;
   current_earfcn = network_info.earfcn;
   if (scan_time) {
      now -= scan_time;
      scan_time = 0;
      if (cells_info->gci_cells_count) {
         all_scan_time += now;
      }
   } else {
      now = 0;
   }
   all = all_scan_time;
   k_mutex_unlock(&lte_mutex);

   snprintf(line, sizeof(line), "%s neighbor cell measurements %d/%d",
            modem_get_network_mode_description(mode),
            cells_info->ncells_count, cells_info->gci_cells_count);
   idx = append_result(modem_last_neighbor_cell_meas, idx, sizeof(modem_last_neighbor_cell_meas), line);

   if (cells_info->current_cell.id != LTE_LC_CELL_EUTRAN_ID_INVALID) {
      const struct lte_lc_cell *gci_cells = &(cells_info->current_cell);
      snprintf(line, sizeof(line), "[*]: plmn %3d%02d, tac 0x%04x, cell 0x%08X, band %d, earfnc %d, pid %d, rsrp %d dBm, rsrq %d dB",
               gci_cells->mcc, gci_cells->mnc, gci_cells->tac, gci_cells->id,
               modem_get_band(gci_cells->earfcn), gci_cells->earfcn, gci_cells->phys_cell_id,
               RSRP(gci_cells->rsrp), RSRQ(gci_cells->rsrq));
      idx = append_result(modem_last_neighbor_cell_meas, idx, sizeof(modem_last_neighbor_cell_meas), line);
   }
   if (cells_info->gci_cells_count) {
      const struct lte_lc_cell *gci_cells = cells_info->gci_cells;
      const struct lte_lc_cell *gci_cells_sorted[cells_info->gci_cells_count];
      int w = cells_info->gci_cells_count > 9 ? 2 : 1;
      int quality;
      int max_quality = 0;
      int matched_current = -1;
      bool match_current = false;

      for (int index = 0; index < cells_info->gci_cells_count; ++index) {
         quality = lte_lc_cell_quality(gci_cells);
         if (quality > max_quality) {
            max_quality = quality;
         }
         gci_cells_sorted[index] = gci_cells;
         for (int index2 = index; index2 > 0; --index2) {
            if (quality <= lte_lc_cell_quality(gci_cells_sorted[index2 - 1])) {
               break;
            }
            gci_cells_sorted[index2] = gci_cells_sorted[index2 - 1];
            gci_cells_sorted[index2 - 1] = gci_cells;
         }
         ++gci_cells;
      }
      ++scans;
      snprintf(line, sizeof(line), "  %*c :  plmn    tac      cell  band earfnc pid rsrp/q dB(m)", w, '#');
      idx = append_result(modem_last_neighbor_cell_meas, idx, sizeof(modem_last_neighbor_cell_meas), line);
      for (int index = 0; index < cells_info->gci_cells_count; ++index) {
         gci_cells = gci_cells_sorted[index];
         match_current = current_cell == gci_cells->id && current_earfcn == gci_cells->earfcn;
         if (match_current) {
            matched_current = index;
            // compare current cell with best quality.
            if ((max_quality - lte_lc_cell_quality(gci_cells)) > MIN_QUALITY_DELTA) {
               ++hits;
            }
         }
         if (gci_cells->mnc >= 100 && gci_cells->mnc <= 999) {
            snprintf(mnc, sizeof(mnc), "%3d", gci_cells->mnc);
         } else if (gci_cells->mnc >= 0 && gci_cells->mnc <= 99) {
            snprintf(mnc, sizeof(mnc), "%02d ", gci_cells->mnc);
         } else {
            snprintf(mnc, sizeof(mnc), "xxx");
         }
         snprintf(line, sizeof(line), "[%c%*d]: %3d%s 0x%04x 0x%08X  %2d %5d  %3d  %4d/%3d",
                  match_current ? '*' : ' ', w, index,
                  gci_cells->mcc, mnc, gci_cells->tac, gci_cells->id,
                  modem_get_band(gci_cells->earfcn), gci_cells->earfcn, gci_cells->phys_cell_id,
                  RSRP(gci_cells->rsrp), RSRQ(gci_cells->rsrq));
         idx = append_result(modem_last_neighbor_cell_meas, idx, sizeof(modem_last_neighbor_cell_meas), line);
      }
      if (matched_current >= 0) {
         snprintf(line, sizeof(line), "(*%*d : current cell)", w, matched_current);
         idx = append_result(modem_last_neighbor_cell_meas, idx, sizeof(modem_last_neighbor_cell_meas), line);
      }
      if (now) {
         snprintf(line, sizeof(line), "Scans %u, improves %u, %lu s, overall %lu s", scans, hits,
                  (unsigned long)MSEC_TO_SEC(now),
                  (unsigned long)MSEC_TO_SEC(all));
         idx = append_result(modem_last_neighbor_cell_meas, idx, sizeof(modem_last_neighbor_cell_meas), line);
      }
   } else {
      if (cells_info->ncells_count > 0) {
         const struct lte_lc_ncell *neighbor_cells = cells_info->neighbor_cells;
         const struct lte_lc_ncell *neighbor_cells_sorted[cells_info->ncells_count];
         int w = cells_info->ncells_count > 9 ? 2 : 1;
         for (int index = 0; index < cells_info->ncells_count; ++index) {
            int quality = lte_lc_ncell_quality(neighbor_cells);
            neighbor_cells_sorted[index] = neighbor_cells;
            for (int index2 = index; index2 > 0; --index2) {
               if (quality <= lte_lc_ncell_quality(neighbor_cells_sorted[index2 - 1])) {
                  break;
               }
               neighbor_cells_sorted[index2] = neighbor_cells_sorted[index2 - 1];
               neighbor_cells_sorted[index2 - 1] = neighbor_cells;
            }
            ++neighbor_cells;
         }
         snprintf(line, sizeof(line), " %*s : bd earfnc pid rsrp/q dB(m)", w, "#");
         idx = append_result(modem_last_neighbor_cell_meas, idx, sizeof(modem_last_neighbor_cell_meas), line);
         for (int index = 0; index < cells_info->ncells_count; ++index) {
            neighbor_cells = neighbor_cells_sorted[index];
            snprintf(line, sizeof(line), "[%*d]: %2d %5d  %3d  %4d/%3d", w,
                     index, modem_get_band(neighbor_cells->earfcn), neighbor_cells->earfcn, neighbor_cells->phys_cell_id,
                     RSRP(neighbor_cells->rsrp), RSRQ(neighbor_cells->rsrq));
            idx = append_result(modem_last_neighbor_cell_meas, idx, sizeof(modem_last_neighbor_cell_meas), line);
         }
      }
      if (now) {
         snprintf(line, sizeof(line), "Scan %lu s", (unsigned long)MSEC_TO_SEC(now));
         idx = append_result(modem_last_neighbor_cell_meas, idx, sizeof(modem_last_neighbor_cell_meas), line);
      }
   }
   k_mutex_lock(&lte_mutex, K_FOREVER);
   modem_last_neighbor_cell_meas_len = idx;
   k_mutex_unlock(&lte_mutex);
}
#endif /* CONFIG_LTE_LC_NEIGHBOR_CELL_MEAS_MODULE */

#if defined(CONFIG_LTE_LC_RAI_MODULE)

static void lte_rai(const struct lte_lc_rai_cfg *rai_cfg)
{
   const char *desc = "RAI not";
   enum lte_network_rai state = LTE_NETWORK_NO_RAI;

   if (rai_cfg->as_rai && rai_cfg->cp_rai) {
      desc = "AS-RAI and CP-RAI";
      state = LTE_NETWORK_AS_AND_CP_RAI;
   } else if (rai_cfg->as_rai) {
      desc = "AS-RAI";
      state = LTE_NETWORK_AS_RAI;
   } else if (rai_cfg->cp_rai) {
      desc = "CP-RAI";
      state = LTE_NETWORK_CP_RAI;
   }
   LOG_INF("%s supported by network.", desc);

   k_mutex_lock(&lte_mutex, K_FOREVER);
   rai_network_state = state;
   k_mutex_unlock(&lte_mutex);
}

#endif /* CONFIG_LTE_LC_RAI_MODULE */

static void lte_handler(const struct lte_lc_evt *const evt)
{
   static uint8_t phase = 0;
   static int64_t phase_start_time = 0;
   static int active_time = -1;

   if (appl_reboots()) {
      return;
   }

   int64_t now = k_uptime_get();

   switch (evt->type) {
      case LTE_LC_EVT_NW_REG_STATUS:
         lte_registration(evt->nw_reg_status);
         break;
      case LTE_LC_EVT_LTE_MODE_UPDATE:
         lte_network_mode_set(evt->lte_mode);
         LOG_INF("LTE Mode: %s", modem_get_network_mode_description(evt->lte_mode));
         break;
#if defined(CONFIG_LTE_LC_PSM_MODULE)
      case LTE_LC_EVT_PSM_UPDATE:
         LOG_INF("PSM parameter update: TAU: %d s, Active time: %d s",
                 evt->psm_cfg.tau, evt->psm_cfg.active_time);
         active_time = evt->psm_cfg.active_time;
         lte_set_psm_status(&evt->psm_cfg);
         break;
#endif /* CONFIG_LTE_LC_PSM_MODULE */
#if defined(CONFIG_LTE_LC_EDRX_MODULE)
      case LTE_LC_EVT_EDRX_UPDATE:
         {
            const char *mode = "none";
            if (evt->edrx_cfg.mode == LTE_LC_LTE_MODE_LTEM) {
               mode = "LTE-M";
            } else if (evt->edrx_cfg.mode == LTE_LC_LTE_MODE_NBIOT) {
               mode = "NB-IoT";
            }
            LOG_INF("eDRX cell update: %s, eDRX: %.2fs, PTW: %.2fs",
                    mode, (double)evt->edrx_cfg.edrx, (double)evt->edrx_cfg.ptw);
            lte_set_edrx_status(&evt->edrx_cfg);
            break;
         }
#endif /* CONFIG_LTE_LC_EDRX_MODULE */
      case LTE_LC_EVT_RRC_UPDATE:
         {
            if (evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED) {
               if (phase == 1) {
                  lte_inc_wakeups(now - phase_start_time);
               }
               phase = 2;
               phase_start_time = now;
               lte_connection_status_set(true);
               lte_network_sleeping_set(false);
               LOG_INF("RRC mode: Connected");
            } else {
               int64_t transmission_time = get_transmission_time();
               lte_connection_status_set(false);
               if (phase == 2) {
                  int64_t time = now - phase_start_time;
                  lte_add_connected(time);
                  if ((transmission_time - phase_start_time) > 0) {
                     rai_time = (int)(now - transmission_time);
                     LOG_INF("RRC mode: Idle after %lld ms (%d ms inactivity)", now - phase_start_time, rai_time);
                  } else {
                     rai_time = -1;
                     LOG_INF("RRC mode: Idle after %lld ms", now - phase_start_time);
                  }
               }
               phase = 3;
               phase_start_time = now;
            }
            break;
         }
#if defined(CONFIG_LTE_LC_TAU_PRE_WARNING_MODULE)
      case LTE_LC_EVT_TAU_PRE_WARNING:
         LOG_INF("LTE Tracking area Update");
#endif /* CONFIG_LTE_LC_TAU_PRE_WARNING_MODULE */
         break;
      case LTE_LC_EVT_CELL_UPDATE:
         if (evt->cell.id == LTE_LC_CELL_EUTRAN_ID_INVALID) {
            LOG_INF("LTE cell changed: n.a");
         } else if (evt->cell.mcc == 0) {
            // CEREG update
            LOG_INF("LTE cell changed: Cell ID: %d, Tracking area: %d",
                    evt->cell.id, evt->cell.tac);
         } else {
            LOG_INF("LTE cell changed: PLMN %d.%d, Cell ID: %d, Tracking area: %d",
                    evt->cell.mcc, evt->cell.mnc, evt->cell.id, evt->cell.tac);
            LOG_INF("LTE cell changed: RSRP %d dBm, RSRQ %d dB", evt->cell.rsrp - 140, (evt->cell.rsrq - 39) / 2);
         }
         lte_update_cell(evt->cell.tac, evt->cell.id);
         break;
#if defined(CONFIG_LTE_LC_MODEM_SLEEP_MODULE)
      case LTE_LC_EVT_MODEM_SLEEP_ENTER:
         {
            const char *mode = "";
            switch (evt->modem_sleep.type) {
               case LTE_LC_MODEM_SLEEP_PSM:
                  mode = "(PSM)";
                  break;
               case LTE_LC_MODEM_SLEEP_RF_INACTIVITY:
                  mode = "(RF inactive)";
                  phase = 0;
                  break;
               case LTE_LC_MODEM_SLEEP_LIMITED_SERVICE:
                  mode = "(limited service)";
                  phase = 0;
                  break;
               case LTE_LC_MODEM_SLEEP_FLIGHT_MODE:
                  mode = "(flight mode)";
                  phase = 0;
                  break;
               case LTE_LC_MODEM_SLEEP_PROPRIETARY_PSM:
                  mode = "(proprietary PSM)";
                  phase = 0;
                  break;
            }
            if (phase == 3) {
               int64_t time = now - phase_start_time;
               lte_add_asleep(time);
               bool delayed = active_time >= 0 && MSEC_TO_SEC(time) > (active_time + 5);
               if (delayed) {
                  lte_inc_psm_delays(time);
               }
               LOG_INF("LTE modem sleeps after %lld ms idle%s %s", time, delayed ? ", delayed" : "", mode);
            } else {
               LOG_INF("LTE modem sleeps %s", mode);
            }
            phase = 0;
            lte_network_sleeping_set(true);
            break;
         }
      case LTE_LC_EVT_MODEM_SLEEP_EXIT:
         phase = 1;
         phase_start_time = now;
         lte_network_sleeping_set(false);
         LOG_INF("LTE modem wakes up");
         break;
#endif /* CONFIG_LTE_LC_MODEM_SLEEP_MODULE */
      case LTE_LC_EVT_MODEM_EVENT:
         if (evt->modem_evt == LTE_LC_MODEM_EVT_BATTERY_LOW) {
            LOG_INF("LTE modem Battery Low!");
            work_submit_to_io_queue(&modem_low_voltage_callback_work);
            k_mutex_lock(&lte_mutex, K_FOREVER);
            ++lte_low_voltage;
            atomic_set_bit(&modem_states, MODEM_LOW_POWER);
            k_mutex_unlock(&lte_mutex);
         } else if (evt->modem_evt == LTE_LC_MODEM_EVT_OVERHEATED) {
            LOG_INF("LTE modem Overheated!");
         } else if (evt->modem_evt == LTE_LC_MODEM_EVT_RESET_LOOP) {
            LOG_INF("LTE modem Reset Loop!");
         } else if (evt->modem_evt == LTE_LC_MODEM_EVT_SEARCH_DONE) {
            LOG_INF("LTE modem search done.");
         } else if (evt->modem_evt == LTE_LC_MODEM_EVT_LIGHT_SEARCH_DONE) {
            LOG_INF("LTE modem light search done.");
         }
         break;
#if defined(CONFIG_LTE_LC_NEIGHBOR_CELL_MEAS_MODULE)
      case LTE_LC_EVT_NEIGHBOR_CELL_MEAS:
         lte_neighbor_cell_meas(&evt->cells_info);
         break;
#endif /* CONFIG_LTE_LC_NEIGHBOR_CELL_MEAS_MODULE */
#if defined(CONFIG_LTE_LC_RAI_MODULE)
      case LTE_LC_EVT_RAI_UPDATE:
         lte_rai(&evt->rai_cfg);
         break;
#endif /* CONFIG_LTE_LC_RAI_MODULE */
      default:
         break;
   }
}

#ifdef CONFIG_PDN

#include "appl_settings.h"
#include <modem/pdn.h>

static void lte_pdn_status_set(bool pdn_active)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   if (network_info.pdn_active != (pdn_active ? LTE_NETWORK_STATE_ON : LTE_NETWORK_STATE_OFF)) {
      network_info.pdn_active = pdn_active ? LTE_NETWORK_STATE_ON : LTE_NETWORK_STATE_OFF;
      lte_connection_status();
   }
   k_mutex_unlock(&lte_mutex);
}

static void pdn_handler(uint8_t cid, enum pdn_event event,
                        int reason)
{
   if (appl_reboots()) {
      return;
   }

   const char *reason_description = NULL;
   char reason_bin[9] = "00000000";
   if (event == PDN_EVENT_CNEC_ESM) {
      print_bin(reason_bin, 8, reason);
#if CONFIG_PDN_ESM_STRERROR
      reason_description = pdn_esm_strerror(reason);
#endif
   }

   switch (event) {
      case PDN_EVENT_CNEC_ESM:
         if (reason_description) {
            LOG_INF("PDN CID %u, error %d, 0b%s, %s", cid, reason, reason_bin, reason_description);
         } else {
            LOG_INF("PDN CID %u, error %d, 0b%s", cid, reason, reason_bin);
         }
         break;
      case PDN_EVENT_ACTIVATED:
         LOG_INF("PDN CID %u, activated", cid);
         lte_pdn_status_set(true);
         break;
      case PDN_EVENT_DEACTIVATED:
         LOG_INF("PDN CID %u, deactivated", cid);
         lte_pdn_status_set(false);
         break;
      case PDN_EVENT_IPV6_UP:
         LOG_INF("PDN CID %u, IPv6 up", cid);
         break;
      case PDN_EVENT_IPV6_DOWN:
         LOG_INF("PDN CID %u, IPv6 down", cid);
         break;
      case PDN_EVENT_NETWORK_DETACH:
         LOG_INF("PDN CID %u, detach", cid);
         lte_pdn_status_set(false);
         break;
      case PDN_EVENT_APN_RATE_CONTROL_ON:
         LOG_INF("PDN CID %u, rate limit reached", cid);
         break;
      case PDN_EVENT_APN_RATE_CONTROL_OFF:
         LOG_INF("PDN CID %u, rate limit off", cid);
         break;
      case PDN_EVENT_CTX_DESTROYED:
         LOG_INF("PDN CID %u, context destroyed", cid);
         break;
   }
}

static void modem_apply_apn(void)
{
   char apn[MODEM_APN_SIZE];
   if (appl_settings_get_apn(apn, sizeof(apn))) {
      int err = pdn_ctx_configure(0, apn, PDN_FAM_IPV4V6, NULL);
      if (err) {
         LOG_WRN("Failed to set PDN '%s': %d (%s)", apn, err, strerror(err));
      }
   }
}
#endif /* CONFIG_PDN */

#ifdef CONFIG_SMS

#include <modem/sms.h>

static int modem_sms_callback_id = -1;

static void modem_sms_callback(struct sms_data *const data, void *context)
{
   if (data == NULL) {
      LOG_INF("SMS with NULL data");
      return;
   }

   if (data->type == SMS_TYPE_DELIVER) {
      /* When SMS message is received, print information */
      struct sms_deliver_header *header = &data->header.deliver;

      LOG_INF("SMS received, time:   %02d-%02d-%02d %02d:%02d:%02d",
              header->time.year,
              header->time.month,
              header->time.day,
              header->time.hour,
              header->time.minute,
              header->time.second);

      LOG_INF("\tText:   '%s'", data->payload);
      LOG_INF("\tLength: %d", data->payload_len);

      if (header->app_port.present) {
         LOG_INF("\tApplication port addressing scheme: dest_port=%d, src_port=%d",
                 header->app_port.dest_port,
                 header->app_port.src_port);
      }
      if (header->concatenated.present) {
         LOG_INF("\tConcatenated short message: ref_number=%d, msg %d/%d",
                 header->concatenated.ref_number,
                 header->concatenated.seq_number,
                 header->concatenated.total_msgs);
      }
   } else if (data->type == SMS_TYPE_STATUS_REPORT) {
      LOG_INF("SMS status report received");
   } else {
      LOG_INF("SMS protocol message with unknown type received");
   }
}
#endif

#if defined(CONFIG_LTE_LINK_CONTROL)

LTE_LC_ON_CFUN(modem_on_cfun_hook, modem_on_cfun, NULL);

static void modem_on_cfun(enum lte_lc_func_mode mode, void *ctx)
{
   if (mode == LTE_LC_FUNC_MODE_NORMAL ||
       mode == LTE_LC_FUNC_MODE_ACTIVATE_LTE) {
      int16_t edrx_time_s = 0;
      int16_t ptw_time_s = 0;
      k_mutex_lock(&lte_mutex, K_FOREVER);
      ++lte_starts;
      atomic_clear_bit(&modem_states, MODEM_LOW_POWER);
      edrx_status.mode = LTE_LC_LTE_MODE_NONE;
      edrx_time_s = requested_edrx_time_s;
      ptw_time_s = requested_ptw_time_s;
      k_mutex_unlock(&lte_mutex);
      if (edrx_time_s) {
         modem_set_edrx(edrx_time_s);
      }
      if (ptw_time_s) {
         modem_set_ptw(ptw_time_s);
      }
      modem_read_network_info(NULL, true);
      return;
   }
#ifdef CONFIG_PDN
   if (mode == LTE_LC_FUNC_MODE_POWER_OFF) {
      modem_apply_apn();
   }
#endif
}
#endif /* CONFIG_LTE_LINK_CONTROL */

static int modem_connect(void)
{
   int err = 0;

   if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT)) {
      /* Do nothing, modem is already configured and LTE connected. */
   } else {
      enum lte_lc_system_mode lte_mode;
      enum lte_lc_system_mode_preference lte_preference;

      lte_lc_modem_events_enable();
      err = lte_lc_connect_async(lte_handler);
      if (err) {
         if (err == -EINPROGRESS) {
            LOG_INF("Connecting to LTE network in progress");
            err = 0;
         } else {
            LOG_WRN("Connecting to LTE network failed, error: %d", err);
         }
      }
      if (!err && !lte_lc_system_mode_get(&lte_mode, &lte_preference)) {
         if (!atomic_test_and_set_bit(&modem_states, MODEM_LTE_MODE_INITIALIZED)) {
            lte_initial_mode = lte_mode;
            LOG_INF("Start %s", modem_get_system_mode_description(lte_mode, lte_preference));
         } else {
            LOG_INF("Restart %s", modem_get_system_mode_description(lte_mode, lte_preference));
         }
      }
   }
   return err;
}

static void modem_cancel_all_job(void)
{
   k_work_cancel(&modem_read_sim_work);
   k_work_cancel(&modem_read_network_info_work);
   k_work_cancel(&modem_read_coverage_enhancement_info_work);
   k_work_cancel(&modem_registered_callback_work);
   k_work_cancel(&modem_unregistered_callback_work);
   k_work_cancel(&modem_ready_callback_work);
   k_work_cancel(&modem_not_ready_callback_work);
   k_work_cancel(&modem_connected_callback_work);
   k_work_cancel(&modem_unconnected_callback_work);
   k_work_cancel(&modem_power_management_resume_work);
   k_work_cancel(&modem_power_management_suspend_work);
   k_work_cancel(&modem_psm_active_work);
   k_work_cancel(&modem_psm_inactive_work);
   k_work_cancel_delayable(&modem_ready_work);
}

static void modem_init_rai(void)
{
#ifdef CONFIG_CP_RAI_ON
   int err = modem_at_cmd(NULL, 0, NULL, "AT%XRAI=0");
   if (err < 0) {
      LOG_WRN("Failed to disable control plane RAI, err %d", err);
   }
#endif
}

int modem_init(int config, lte_state_change_callback_handler_t state_handler)
{
   int err = 0;

   if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT)) {
      /* Do nothing, modem is already configured and LTE connected. */
   } else if (!atomic_test_and_set_bit(&modem_states, MODEM_INITIALIZED)) {
      char buf[128];
      const char *plmn = NULL;

      modem_cancel_all_job();
      modem_sim_init();
      k_mutex_lock(&lte_mutex, K_FOREVER);
      memset(&modem_info, 0, sizeof(modem_info));
      memset(&network_info, 0, sizeof(network_info));
      atomic_clear_bit(&modem_states, MODEM_SIGNAL_READY);
      atomic_clear_bit(&modem_states, MODEM_READY);
      lte_initial_config = config;
      lte_state_change_handler = state_handler;
      rai_time = -1;
      rai_network_state = LTE_NETWORK_RAI_UNKNOWN;
      k_mutex_unlock(&lte_mutex);
#ifdef CONFIG_NRF_MODEM_LIB_TRACE
      LOG_INF("Modem trace enabled");
#else
      LOG_INF("Modem trace disabled");
#endif
      if (!atomic_test_and_set_bit(&modem_states, MODEM_LIB_INITIALIZED)) {
         nrf_modem_lib_init();
      }

      err = modem_at_cmd(buf, sizeof(buf), "%HWVERSION: ", "AT%HWVERSION");
      if (err > 0) {
         LOG_INF("hw: %s", buf);
         int index = strstart(buf, "nRF9160 SICA ", true);
         if (!index) {
            index = strstart(buf, "nRF9161 LACA ", true);
         }
         strncpy(modem_info.version, &buf[index], sizeof(modem_info.version) - 1);
      }
      atomic_clear_bit(&modem_states, MODEM_FIRMWARE_2);
      err = modem_at_cmd(buf, sizeof(buf), NULL, "AT+CGMR");
      if (err > 0) {
         LOG_INF("rev: %s", buf);
         int index = strstart(buf, "mfw_nrf9160_", true);
         if (!index) {
            index = strstart(buf, "mfw_nrf91x1_", true);
         }
         strncpy(modem_info.firmware, &buf[index], sizeof(modem_info.firmware) - 1);
         if (modem_info.firmware[0] >= '2') {
            atomic_set_bit(&modem_states, MODEM_FIRMWARE_2);
         }
      }
      err = modem_at_cmd(buf, sizeof(buf), NULL, "AT+CGSN");
      if (err < 0) {
         LOG_INF("Failed to read IMEI.");
      } else {
         LOG_INF("imei: %s", buf);
         strncpy(modem_info.imei, buf, sizeof(modem_info.imei) - 1);
      }

      if ((config & 3) == 3) {
         err = modem_at_cmd(buf, sizeof(buf), NULL, "AT%XFACTORYRESET=0");
         LOG_INF("Factory reset: %s", buf);
         k_sleep(K_SECONDS(10));
      } else if (config & 2) {
         // force NB-IoT only
         atomic_set_bit(&modem_states, MODEM_LTE_MODE_FORCE);
         lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_NBIOT, LTE_LC_SYSTEM_MODE_PREFER_NBIOT);
      } else if (config & 1) {
         // force LTE-M only
         atomic_set_bit(&modem_states, MODEM_LTE_MODE_FORCE);
         lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_LTEM, LTE_LC_SYSTEM_MODE_PREFER_LTEM);
      }
      if (atomic_test_bit(&modem_states, MODEM_LTE_MODE_INITIALIZED) &&
          !atomic_test_bit(&modem_states, MODEM_LTE_MODE_FORCE)) {
         lte_lc_system_mode_set(lte_initial_mode, CONFIG_LTE_MODE_PREFERENCE_VALUE);
      }

      if (atomic_test_bit(&modem_states, MODEM_FIRMWARE_2)) {
#ifdef CONFIG_LTE_FEATURE_HPPLMN_SKIP
         err = modem_at_cmd(buf, sizeof(buf), NULL, "AT%FEACONF=0,1,1");
#else
         err = modem_at_cmd(buf, sizeof(buf), NULL, "AT%FEACONF=0,1,0");
#endif
         if (err > 0) {
            LOG_INF("Set feaconv skip HPPLMN: %s", buf);
         }
         err = modem_at_cmd(buf, sizeof(buf), "%FEACONF: ", "AT%FEACONF=1,1");
         if (err > 0) {
            LOG_INF("Get feaconv skip HPPLMN: %s", buf);
         }
         if (strcmp(modem_info.firmware, "2.0.1") >= 0) {
#ifdef CONFIG_LTE_FEATURE_PLMN_SELECT_OPTIMIZATION
            err = modem_at_cmd(buf, sizeof(buf), NULL, "AT%FEACONF=0,3,1");
#else
            err = modem_at_cmd(buf, sizeof(buf), NULL, "AT%FEACONF=0,3,0");
#endif
            if (err > 0) {
               LOG_INF("Set feaconv PLMN opt.sel: %s", buf);
            }
            err = modem_at_cmd(buf, sizeof(buf), "%FEACONF: ", "AT%FEACONF=1,3");
            if (err > 0) {
               LOG_INF("Get feaconv PLMN opt.sel: %s", buf);
            }
         }
      } else {
#ifdef CONFIG_AS_RAI_ON
         err = modem_at_cmd(buf, sizeof(buf), NULL, "AT%REL14FEAT=0,1,0,0,0");
         if (err > 0) {
            LOG_INF("rel14feat AS RAI: %s", buf);
         }
#else
         err = modem_at_cmd(buf, sizeof(buf), NULL, "AT%REL14FEAT=0,0,0,0,0");
         if (err > 0) {
            LOG_INF("rel14feat none: %s", buf);
         }
#endif
         err = modem_at_cmd(buf, sizeof(buf), "%REL14FEAT: ", "AT%REL14FEAT?");
         if (err > 0) {
            LOG_INF("rel14feat: %s", buf);
         }
      }
      err = modem_at_cmd(buf, sizeof(buf), NULL, "AT%XCONNSTAT=1");
      if (err > 0) {
         LOG_INF("stat: %s", buf);
      }

#ifndef CONFIG_LTE_LOCK_BANDS
      // 1,2,3,4,5,8,12,13,17,19,20,25,26,28,66
      // 20,8,3
      // AT%XBANDLOCK=1,"10000000000010000100"
      err = modem_at_cmd(buf, sizeof(buf), "%XBANDLOCK: ", "AT%XBANDLOCK?");
      if (err > 0) {
         LOG_INF("band-lock: %s", buf);
      }
#endif

#ifdef CONFIG_LTE_LOCK_PLMN
      plmn = CONFIG_LTE_LOCK_PLMN_STRING;
#elif CONFIG_LTE_LOCK_PLMN_CONFIG_SWITCH
      switch ((config >> 2) & 3) {
         case 0:
            break;
         case 1:
            plmn = CONFIG_LTE_LOCK_PLMN_CONFIG_SWITCH_STRING_1;
            break;
         case 2:
            plmn = CONFIG_LTE_LOCK_PLMN_CONFIG_SWITCH_STRING_2;
            break;
         case 3:
            plmn = CONFIG_LTE_LOCK_PLMN_CONFIG_SWITCH_STRING_3;
            break;
      }
#endif
      if (plmn) {
         LOG_INF("Lock PLMN %s", plmn);
         network_info.plmn_lock = LTE_NETWORK_STATE_ON;
#ifdef CONFIG_LTE_LOCK_PLMN
         err = 0;
#else
         err = modem_at_cmdf(buf, sizeof(buf), NULL, "AT+COPS=1,2,\"%s\"", plmn);
#endif
      } else {
         LOG_INF("Unlock PLMN");
         err = modem_at_cmd(buf, sizeof(buf), NULL, "AT+COPS=0");
         network_info.plmn_lock = LTE_NETWORK_STATE_OFF;
      }
      if (err < 0) {
         LOG_WRN("Failed to lock PLMN, err %d", err);
      }

#if defined(CONFIG_LTE_LC_PSM_MODULE)
#ifdef CONFIG_UDP_PSM_ENABLE
      err = modem_set_psm(CONFIG_UDP_PSM_CONNECT_RAT);
      if (err) {
         if (err == -EFAULT) {
            LOG_WRN("Modem set PSM failed, AT cmd failed!");
         } else {
            LOG_WRN("Modem set PSM failed, error: %d!", err);
         }
      } else {
         err = modem_at_cmd(buf, sizeof(buf), "+CPSMS: ", "AT+CPSMS?");
         if (err > 0) {
            LOG_INF("psm: %s", buf);
         }
      }
#else
      err = lte_lc_psm_req(false);
      if (err) {
         LOG_WRN("Modem disable PSM failed!");
      }
#endif
#endif /* CONFIG_LTE_LC_PSM_MODULE */

      if (!atomic_test_bit(&modem_states, MODEM_FIRMWARE_2)) {
         err = modem_at_cmd(buf, sizeof(buf), "%XRAI: ", "AT%XRAI=0");
         if (err < 0) {
            LOG_WRN("Failed to disable control plane RAI, err %d", err);
         } else {
#ifdef CONFIG_CP_RAI_ON
            LOG_INF("Control plane RAI initial disabled");
#endif
         }
      }

      {
         /** Release Assistance Indication  */
         int mode = 0; // disable
#ifdef CONFIG_AS_RAI_ON
         // enable or enable with notifications fro mfw 2.x.y
         mode = atomic_test_bit(&modem_states, MODEM_FIRMWARE_2) ? 2 : 1;
#endif /* CONFIG_AS_RAI_ON */
         err = modem_at_cmdf(buf, sizeof(buf), "%RAI: ", "AT%%RAI=%d", mode);
         if (err < 0) {
            LOG_WRN("Failed to set RAI %d, err %d (%s)", mode, err, strerror(-err));
         } else {
            LOG_INF("Set RAI %d", mode);
         }
      }

#ifdef CONFIG_RAI_OFF
      LOG_INF("No AS- nor CP-RAI mode configured!");
#endif

#if defined(CONFIG_LTE_LC_EDRX_MODULE)
#ifdef CONFIG_UDP_EDRX_ENABLE
      err = lte_lc_edrx_req(true);
      if (err) {
         if (err == -EFAULT) {
            LOG_WRN("Modem set eDRX failed, AT cmd failed!");
         } else {
            LOG_WRN("Modem set eDRX failed, error: %d!", err);
         }
      } else {
         err = modem_at_cmd(buf, sizeof(buf), "+CEDRXS: ", "AT+CEDRXS?");
         if (err > 0) {
            LOG_INF("eDRX: %s", buf);
         }
      }
#else
      err = lte_lc_edrx_req(false);
      if (err) {
         LOG_WRN("Modem disable eDRX failed!");
      }
#endif
#endif /* CONFIG_LTE_LC_EDRX_MODULE */

#ifdef CONFIG_STATIONARY_MODE_ENABLE
      err = modem_at_cmd(buf, sizeof(buf), NULL, "AT%REDMOB=1");
      if (err >= 0) {
         LOG_INF("REDMOB=1: OK");
      }
#else
      err = modem_at_cmd(buf, sizeof(buf), NULL, "AT%REDMOB=2");
      if (err >= 0) {
         LOG_INF("REDMOB=2: OK");
      }
#endif

#ifdef CONFIG_STATIONARY_MODE_ENABLE
      err = modem_at_cmd(buf, sizeof(buf), NULL, "AT%XDATAPRFL=0");
      if (err >= 0) {
         LOG_INF("DATAPRFL=0: OK");
      }
#else
      err = modem_at_cmd(buf, sizeof(buf), NULL, "AT%XDATAPRFL=2");
      if (err >= 0) {
         LOG_INF("DATAPRFL=2: OK");
      }
#endif
      err = modem_at_cmd(buf, sizeof(buf), "%XDATAPRFL: ", "AT%XDATAPRFL?");
      if (err > 0) {
         LOG_INF("DATAPRFL: %s", buf);
      }
      // no-loop, return to 1, default band opt,
      // pause search for 300, 600, 1200, 3600, 7200 seconds
      err = modem_at_cmd(buf, sizeof(buf), NULL,
                         "AT%PERIODICSEARCHCONF=0,0,1,1,"
                         "\"1,300,600,1200,3600,7200\"");
      if (err > 0) {
         LOG_INF("PERIODICSEARCHCONF: %s", buf);
      }

      err = modem_at_cmd(buf, sizeof(buf), "%PERIODICSEARCHCONF: ", "AT%PERIODICSEARCHCONF=1");
      if (err > 0) {
         LOG_INF("PERIODICSEARCHCONF: %s", buf);
      }
      // UICC suspend, deactivate, 5s
      err = modem_at_cmd(buf, sizeof(buf), NULL, "AT+SSRDA=1,1,5");
      if (err >= 0) {
         LOG_INF("SSRDA: OK");
      }

#ifdef CONFIG_PDN
      pdn_default_ctx_cb_reg(pdn_handler);
      modem_apply_apn();
#if defined(CONFIG_PDN_LEGACY_PCO)
      LOG_INF("Legacy ePCO=0 used");
#else  /* CONFIG_PDN_LEGACY_PCO */
      LOG_INF("ePCO=1 used");
#endif /* CONFIG_PDN_LEGACY_PCO */
#endif /* CONFIG_PDN */

#if NCS_VERSION_NUMBER < 0x20600
      /* deprecated with NCS 2.6.0 */
      err = lte_lc_init();
      if (err) {
         if (err == -EFAULT) {
            LOG_WRN("Modem initialization failed, AT cmd failed!");
         } else if (err == -EALREADY) {
            LOG_WRN("Modem initialization failed, already initialized");
         } else {
            LOG_WRN("Modem initialization failed, error: %d", err);
         }
         atomic_clear_bit(&modem_states, MODEM_INITIALIZED);
         return err;
      }
#endif

#ifdef CONFIG_SMS
      if (modem_sms_callback_id >= 0) {
         sms_unregister_listener(modem_sms_callback_id);
         modem_sms_callback_id = -1;
      }
      err = sms_register_listener(modem_sms_callback, NULL);
      if (err < 0) {
         LOG_WRN("sms_register_listener returned err: %d", err);
      } else {
         LOG_INF("sms_register_listener returned success");
         modem_sms_callback_id = err;
      }
#endif

      modem_set_preference(RESET_PREFERENCE);
      LOG_INF("Modem initialized");
   }

   return err;
}

int modem_reinit(bool lib)
{
   atomic_clear_bit(&modem_states, MODEM_INITIALIZED);
   if (lib && atomic_test_and_clear_bit(&modem_states, MODEM_LIB_INITIALIZED)) {
      nrf_modem_lib_shutdown();
   }
   return modem_init(lte_initial_config, lte_state_change_handler);
}

int modem_wait_ready(const k_timeout_t timeout)
{
   int err = 0;
   int led_on = 0;
   uint64_t timeout_ms = k_ticks_to_ms_floor64(timeout.ticks);
   int64_t now = k_uptime_get();
   int64_t start = now;
   int64_t last = now;

   watchdog_feed();
   err = lte_ready_wait(K_MSEC(10));
   if (err == -EINPROGRESS) {
      atomic_clear_bit(&modem_states, MODEM_INTERRUPT_SEARCH);
      LOG_INF("Modem connects for %ld s", (long)MSEC_TO_SEC(timeout_ms));
      while (!atomic_test_bit(&modem_states, MODEM_INTERRUPT_SEARCH) && -EINPROGRESS == (err = lte_ready_wait(K_MSEC(1500)))) {
         now = k_uptime_get();
         led_on = !led_on;
         if (led_on) {
            ui_led_op(LED_COLOR_BLUE, LED_SET);
            ui_led_op(LED_COLOR_RED, LED_SET);
         } else {
            ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
            ui_led_op(LED_COLOR_RED, LED_CLEAR);
         }
         if ((now - start) > timeout_ms) {
            err = -1;
            break;
         }
         if ((now - last) > (MSEC_PER_SEC * 30)) {
            watchdog_feed();
            LOG_INF("Modem connects for %ld s of %ld s",
                    (long)MSEC_TO_SEC(now - start), (long)MSEC_TO_SEC(timeout_ms));
            last = now;
         }
      }
   }
   ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
   ui_led_op(LED_COLOR_RED, LED_CLEAR);
   now = k_uptime_get();
   LOG_INF("Modem network %sconnected in %ld s", err ? "not " : "", (long)MSEC_TO_SEC(now - start));
   return err;
}

void modem_interrupt_wait(void)
{
   atomic_set_bit(&modem_states, MODEM_INTERRUPT_SEARCH);
}

int modem_start(const k_timeout_t timeout, bool save)
{
   int err = 0;
   int64_t time;

   modem_cancel_all_job();

   k_mutex_lock(&lte_mutex, K_FOREVER);
   err = network_info.plmn_lock;
   memset(&network_info, 0, sizeof(network_info));
   network_info.plmn_lock = err;
   memset(&ce_info, 0, sizeof(ce_info));
   rai_time = -1;
   rai_network_state = LTE_NETWORK_RAI_UNKNOWN;
   k_mutex_unlock(&lte_mutex);

   modem_init_rai();

   // activate UICC
   err = modem_at_cmd(NULL, 0, NULL, "AT+CFUN=41");
   if (err > 0) {
      modem_sim_read_info(NULL, true);
      if (atomic_test_bit(&modem_states, MODEM_LTE_MODE_PREFERENCE)) {
         modem_sim_apply_iccid_preference();
      }
      lte_lc_offline();
   }

   ui_led_op(LED_COLOR_BLUE, LED_SET);
   ui_led_op(LED_COLOR_RED, LED_SET);
   ui_led_op(LED_COLOR_GREEN, LED_CLEAR);

   err = modem_connect();
   if (!err) {
      time = k_uptime_get();
      err = modem_wait_ready(timeout);
      time = k_uptime_get() - time;
      if (!err) {
         LOG_INF("LTE attached in %ld [ms]", (long)time);
         if (modem_sim_automatic_multi_imsi()) {
            // multi imsi may get irritated by switching off the modem
            save = false;
         }
#if CONFIG_MODEM_SAVE_CONFIG_THRESHOLD > 0
#if CONFIG_MODEM_SAVE_CONFIG_THRESHOLD == 1
         if (save) {
#else
         if (save && time > CONFIG_MODEM_SAVE_CONFIG_THRESHOLD * MSEC_PER_SEC) {
#endif
            enum preference_mode current_mode = RESET_PREFERENCE;

            LOG_INF("Modem saving ...");

            k_mutex_lock(&lte_mutex, K_FOREVER);
            if (atomic_test_bit(&modem_states, MODEM_LTE_MODE_PREFERENCE)) {
               if (network_info.mode == LTE_LC_LTE_MODE_LTEM) {
                  current_mode = LTE_M_PREFERENCE;
               } else if (network_info.mode == LTE_LC_LTE_MODE_NBIOT) {
                  current_mode = NBIOT_PREFERENCE;
               }
            }
            --lte_searchs;
            k_mutex_unlock(&lte_mutex);

            lte_lc_power_off();
            if (current_mode != RESET_PREFERENCE) {
               modem_set_preference(current_mode);
            }
            lte_lc_normal();
            LOG_INF("Modem saved.");
            err = modem_wait_ready(timeout);
         } else {
            LOG_INF("Modem not saved.");
         }
#endif
      } else {
         LOG_INF("LTE attachment failed, %ld [ms]", (long)time);
      }
   }
   return err;
}

int modem_start_search(void)
{
   LOG_INF("Modem starts search.");
   modem_init_rai();
   return modem_at_cmd(NULL, 0, NULL, "AT%PERIODICSEARCHCONF=3");
}

int modem_get_power_state(enum lte_power_state *state)
{
   int res = 0;

   if (state) {
      k_mutex_lock(&lte_mutex, K_FOREVER);
      if (atomic_test_bit(&modem_states, MODEM_LOW_POWER)) {
         *state = LTE_POWER_STATE_LOW_VOLTAGE;
      } else if (network_info.sleeping == LTE_NETWORK_STATE_ON) {
         *state = LTE_POWER_STATE_SLEEPING;
      } else if (network_info.rrc_active == LTE_NETWORK_STATE_ON) {
         *state = LTE_POWER_STATE_ACTIVE;
      } else if (network_info.rrc_active == LTE_NETWORK_STATE_OFF) {
         *state = LTE_POWER_STATE_IDLE;
      } else {
         res = -ENODATA;
      }
      k_mutex_unlock(&lte_mutex);
   }
   return res;
}

int modem_get_edrx_status(struct lte_lc_edrx_cfg *edrx)
{
   int res = 0;
   k_mutex_lock(&lte_mutex, K_FOREVER);
   if (edrx) {
      *edrx = edrx_status;
   }
#ifndef CONFIG_UDP_EDRX_ENABLE
   if (LTE_LC_LTE_MODE_NONE == edrx_status.mode) {
      // don't display inactive eDRX when disabled
      res = -ENODATA;
   }
#endif
   k_mutex_unlock(&lte_mutex);
   return res;
}

int modem_get_recv_interval_ms(void)
{
   int res = -ENODATA;
   k_mutex_lock(&lte_mutex, K_FOREVER);
   if (0 > psm_status.active_time) {
      // psm off
      if (LTE_LC_LTE_MODE_NONE == edrx_status.mode) {
         // no edrx
         res = 2560;
      } else {
         // edrx
         res = round(edrx_status.edrx * 100.0F);
         res *= 10;
      }
   }
   k_mutex_unlock(&lte_mutex);
   return res;
}

int modem_get_psm_status(struct lte_lc_psm_cfg *psm)
{
   int res = 0;
   k_mutex_lock(&lte_mutex, K_FOREVER);
   if (psm) {
      *psm = psm_status;
   }
#ifndef CONFIG_UDP_PSM_ENABLE
   if (psm_status.active_time < 0) {
      // don't display inactive PSM when disabled
      res = -ENODATA;
   }
#endif
   k_mutex_unlock(&lte_mutex);
   return res;
}

int modem_get_rai_status(enum lte_network_rai *rai)
{
   enum lte_network_rai state = LTE_NETWORK_RAI_UNKNOWN;
   int res = 0;
   int time = -1;

   k_mutex_lock(&lte_mutex, K_FOREVER);
   state = rai_network_state;
   if (psm_status.active_time >= 0) {
      time = rai_time;
   }
   k_mutex_unlock(&lte_mutex);
   if (state == LTE_NETWORK_RAI_UNKNOWN) {
      // mfw 1.3.x support, guess RAI from release time
      if (time < 0) {
         res = -ENODATA;
      } else {
         int factor = modem_get_time_scale();
         if (factor > 100) {
            time = (time * 100) / factor;
         }
         if (time < CP_RAI_MAX_DELAY) {
            state = LTE_NETWORK_CP_RAI;
         } else if (time < AS_RAI_MAX_DELAY) {
            state = LTE_NETWORK_AS_RAI;
         } else {
            state = LTE_NETWORK_NO_RAI;
         }
      }
   }
   if (rai) {
      *rai = state;
   }
   return res;
}

int modem_get_release_time(void)
{
   return rai_time;
}

int modem_get_time_scale(void)
{
   int factor1 = 100;
   int factor2 = 100;
   struct lte_ce_info info;

   if (modem_get_coverage_enhancement_info(&info) >= 0) {
      uint8_t repetition = MAX(info.downlink_repetition, info.uplink_repetition << 2);
      if (repetition <= 8) {
         // no scale
      } else if (repetition <= 16) {
         factor1 = 150;
      } else if (repetition <= 32) {
         factor1 = 200;
      } else if (repetition <= 64) {
         factor1 = 350;
      } else if (repetition <= 128) {
         factor1 = 500;
      }
      if (info.rsrp > -110) {
         // no scale
      } else if (info.rsrp > -120) {
         factor2 = 150;
      } else if (info.rsrp > -125) {
         factor2 = 200;
      } else if (info.rsrp > -130) {
         factor2 = 350;
      } else {
         factor2 = 500;
      }
   }
   return MAX(factor1, factor2);
}

int modem_get_network_info(struct lte_network_info *info)
{
   if (info) {
      k_mutex_lock(&lte_mutex, K_FOREVER);
      *info = network_info;
      k_mutex_unlock(&lte_mutex);
   }
   return 0;
}

int modem_get_mcc(char *mcc)
{
   if (mcc) {
      k_mutex_lock(&lte_mutex, K_FOREVER);
      strncpy(mcc, network_info.provider, 3);
      k_mutex_unlock(&lte_mutex);
   }
   return 0;
}

int modem_get_coverage_enhancement_info(struct lte_ce_info *info)
{
   if (info) {
      k_mutex_lock(&lte_mutex, K_FOREVER);
      *info = ce_info;
      k_mutex_unlock(&lte_mutex);
   }
   return 0;
}

int modem_get_modem_info(struct lte_modem_info *info)
{
   if (info) {
      *info = modem_info;
   }
   return 0;
}

int modem_get_imei(char *buf, size_t len)
{
   int err = 0;

   if (!atomic_test_bit(&modem_states, MODEM_INITIALIZED)) {
      char temp[64];

      memset(temp, 0, sizeof(temp));
      if (!atomic_test_and_set_bit(&modem_states, MODEM_LIB_INITIALIZED)) {
         nrf_modem_lib_init();
      }
      err = modem_at_cmd(temp, sizeof(temp), NULL, "AT+CGSN");
      if (err < 0) {
         LOG_INF("Failed to read IMEI.");
      } else {
         LOG_INF("imei: %s", temp);
         if (buf) {
            --len;
            strncpy(buf, temp, len);
            buf[len] = 0;
         }
      }
   } else {
      k_mutex_lock(&lte_mutex, K_FOREVER);
      if (buf) {
         --len;
         strncpy(buf, modem_info.imei, len);
         buf[len] = 0;
      }
      err = strlen(modem_info.imei);
      k_mutex_unlock(&lte_mutex);
   }
   return err;
}

void modem_set_transmission_time(void)
{
   int64_t now = k_uptime_get();
   k_mutex_lock(&lte_mutex, K_FOREVER);
   transmission_time = now;
   k_mutex_unlock(&lte_mutex);
}

void modem_set_scan_time(void)
{
   int64_t now = k_uptime_get();
   k_mutex_lock(&lte_mutex, K_FOREVER);
   scan_time = now;
   k_mutex_unlock(&lte_mutex);
}

int psm_parse(const char *active_time_str, const char *tau_ext_str,
              const char *tau_legacy_str, struct lte_lc_psm_cfg *psm_cfg);

int modem_read_network_info(struct lte_network_info *info, bool callbacks)
{
   int result;
   char buf[160];
   struct lte_network_info temp;
   int16_t rsrp = NONE_SIGNAL_VALUE;
   int16_t snr = NONE_SIGNAL_VALUE;

   long value;
   const char *cur = buf;
   const char *n = cur;
   const char *edrx = NULL;
   const char *act = NULL;
   const char *tau_ext = NULL;
   const char *tau = NULL;
   char *t = NULL;

   result = modem_at_cmd(buf, sizeof(buf), "%XMONITOR: ", "AT%XMONITOR");
   if (result < 0) {
      return result;
   } else if (result == 0) {
      return -ENODATA;
   }
   LOG_INF("XMONITOR: %s", buf);

   memset(&temp, 0, sizeof(temp));
   n = parse_next_long(cur, 10, &value);
   if (cur != n) {
      switch (value) {
         case LTE_LC_NW_REG_REGISTERED_HOME:
         case LTE_LC_NW_REG_REGISTERED_ROAMING:
            temp.status = (enum lte_lc_nw_reg_status)value;
            temp.registered = LTE_NETWORK_STATE_ON;
            break;
         case LTE_LC_NW_REG_NOT_REGISTERED:
         case LTE_LC_NW_REG_SEARCHING:
         case LTE_LC_NW_REG_REGISTRATION_DENIED:
         case LTE_LC_NW_REG_UNKNOWN:
         case LTE_LC_NW_REG_UICC_FAIL:
            temp.status = (enum lte_lc_nw_reg_status)value;
            temp.registered = LTE_NETWORK_STATE_OFF;
            break;
         default:
            temp.status = LTE_LC_NW_REG_UNKNOWN;
            temp.registered = LTE_NETWORK_STATE_OFF;
            break;
      }
   }
   if (temp.registered == LTE_NETWORK_STATE_ON && *n == ',') {
      cur = n + 1;
      // skip 2 parameter .
      n = parse_next_chars(cur, ',', 2);
      if (*n == '"') {
         cur = n;
         // plmn
         LOG_DBG("PLMN> %s", cur);
         n = parse_next_qtext(cur, '"', temp.provider, sizeof(temp.provider));
      }
      if (*n == ',') {
         // skip ,
         cur = n + 1;
         // tac
         LOG_DBG("TAC> %s", cur);
         n = parse_next_long_qtext(cur, '"', 16, &value);
         if (cur != n && 0 <= value && value < 0x10000) {
            temp.tac = (uint16_t)value;
         }
      }
      if (*n == ',') {
         // skip ,
         cur = n + 1;
         // Act LTE-M/NB-IoT
         n = parse_next_long(cur, 10, &value);
         if (cur != n &&
             (value == LTE_LC_LTE_MODE_NONE || value == LTE_LC_LTE_MODE_NBIOT || value == LTE_LC_LTE_MODE_LTEM)) {
            temp.mode = (enum lte_lc_lte_mode)value;
         }
      }
      if (*n == ',') {
         // skip ,
         cur = n + 1;
         // Band
         n = parse_next_long(cur, 10, &value);
         if (cur != n && 0 <= value && value < 90) {
            temp.band = (uint8_t)value;
         }
      }
      if (*n == ',') {
         // skip ,"
         cur = n + 1;
         // copy 8 character cell
         LOG_DBG("CELL> %s", cur);
         n = parse_next_long_qtext(cur, '"', 16, &value);
         if (cur != n) {
            temp.cell = (uint32_t)value;
         }
      }
      if (*n == ',') {
         // skip ,
         cur = n + 1;
         // skip Physical Cell ID
         n = parse_next_long(cur, 10, &value);
      }
      if (*n == ',') {
         // skip ,
         cur = n + 1;
         // earfcn
         LOG_DBG("EARFCN> %s", cur);
         n = parse_next_long(cur, 10, &value);
         if (cur != n) {
            temp.earfcn = (uint32_t)value;
         }
      }
      if (*n == ',') {
         // skip ,
         cur = n + 1;
         // rsrp
         LOG_DBG("RSRP> %s", cur);
         n = parse_next_long(cur, 10, &value);
         if (cur != n) {
            if (value == 255) {
               rsrp = INVALID_SIGNAL_VALUE;
            } else {
               rsrp = (int16_t)(value - 140);
            }
         }
      }
      if (*n == ',') {
         // skip ,
         cur = n + 1;
         // snr
         LOG_DBG("SNR> %s", cur);
         n = parse_next_long(cur, 10, &value);
         if (cur != n) {
            if (value == 127) {
               snr = INVALID_SIGNAL_VALUE;
            } else {
               snr = (int16_t)(value - 24);
            }
         }
      }
      if (*n == ',') {
         // skip ,
         cur = n + 1;
         LOG_DBG("eDRX> %s", cur);
         edrx = cur;
         t = &buf[edrx - buf]; // t = edrx
         n = parse_next_qtext(cur, '"', t, 5);
         if (n == cur) {
            edrx = NULL;
         }
      }
      if (*n == ',') {
         // skip ,
         cur = n + 1;
         LOG_DBG("Act> %s", cur);
         act = cur;
         t = &buf[act - buf];
         n = parse_next_qtext(cur, '"', t, 9);
      }
      if (*n == ',') {
         // skip ,
         cur = n + 1;
         // tau_ext
         tau_ext = cur;
         LOG_DBG("TauExt> %s", cur);
         t = &buf[tau_ext - buf];
         n = parse_next_qtext(cur, '"', t, 9);
      }
      if (*n == ',') {
         // skip ,
         cur = n + 1;
         tau = cur;
         LOG_DBG("Tau> %s", cur);
         t = &buf[tau - buf];
         n = parse_next_qtext(cur, '"', t, 9);
      }
   }

   if (edrx) {
      t = NULL;
      int edrx_code = strtol(edrx, &t, 2);
      if (t > edrx) {
         double edrx_time = modem_get_edrx_multiplier(edrx_code) * 5.12F;
         LOG_INF("eDRX net: %s => %0.2fs", edrx, edrx_time);
      }
   }

#if defined(CONFIG_LTE_LC_PSM_MODULE)
   if (act && tau_ext && tau) {
      struct lte_lc_psm_cfg temp_psm_status = {0, -1};

      if (!psm_parse(act, tau_ext, tau, &temp_psm_status)) {
         LOG_INF("PSM update: TAU: %d s, Active time: %d s",
                 temp_psm_status.tau, temp_psm_status.active_time);
         lte_set_psm_status(&temp_psm_status);
      }
   } else {
      if (act) {
         LOG_INF("PSM Act.: %s", act);
      }
      if (tau_ext) {
         LOG_INF("PSM TauExt: %s", tau_ext);
      }
      if (tau) {
         LOG_INF("PSM Tau: %s", tau);
      }
   }
#endif /* CONFIG_LTE_LC_PSM_MODULE */

   result = modem_at_cmd(buf, sizeof(buf), "+CSCON: ", "AT+CSCON?");
   if (result > 0) {
      uint16_t con = 0;
      // CSCON: 0,1
      LOG_INF("+CSCON: %s", buf);
      result = sscanf(buf, "%*u,%hu", &con);
      if (con) {
         temp.rrc_active = LTE_NETWORK_STATE_ON;
      } else {
         temp.rrc_active = LTE_NETWORK_STATE_OFF;
      }
   }

   result = modem_at_cmd(buf, sizeof(buf), "+CGDCONT: ", "AT+CGDCONT?");
   if (result > 0) {
      // CGDCONT: 0,"IP","iot.1nce.net","10.223.63.3",0,0
      LOG_INF("CGDCONT: %s", buf);
      // skip 1 parameter "...", find start of 3th parameter.
      cur = parse_next_chars(buf, ',', 2);
      // apn
      cur = parse_next_qtext(cur, '"', temp.apn, sizeof(temp.apn));
      if (*cur == ',') {
         ++cur;
         // copy ip
         cur = parse_next_qtext(cur, '"', temp.local_ip, sizeof(temp.local_ip));
         temp.pdn_active = temp.local_ip[0] ? LTE_NETWORK_STATE_ON : LTE_NETWORK_STATE_OFF;
      }
   }

   result = modem_at_cmd(buf, sizeof(buf), "%APNRATECTRL: ", "AT%APNRATECTRL=1");
   if (result > 0) {
      // %APNRATECTRL: 1,0,256,86400
      LOG_INF("APNRATECTRL CFG: %s", buf);
      sscanf(buf, "%*u,%*u,%hu,%u", &temp.rate_limit, &temp.rate_limit_period);
      if (temp.rate_limit) {
         result = modem_at_cmd(buf, sizeof(buf), "%APNRATECTRL: ", "AT%APNRATECTRL=0,0");
         if (result > 0) {
            // %APNRATECTRL: 0,0,0[,time]
            LOG_INF("APNRATECTRL STATUS: %s", buf);
            sscanf(buf, "%*u,%*u,%*u,%u", &temp.rate_limit_time);
         }
      }
   }

   k_mutex_lock(&lte_mutex, K_FOREVER);
   if (network_info.cell != temp.cell || network_info.tac != temp.tac) {
      lte_cell_updates++;
   }
   temp.plmn_lock = network_info.plmn_lock;
   temp.sleeping = network_info.sleeping;
   network_info = temp;
   if (rsrp != NONE_SIGNAL_VALUE) {
      ce_info.rsrp = rsrp;
   }
   if (snr != NONE_SIGNAL_VALUE) {
      ce_info.snr = snr;
   }
   if (callbacks) {
      network_info.registered = LTE_NETWORK_STATE_INIT;
      network_info.rrc_active = LTE_NETWORK_STATE_INIT;
      lte_registration(temp.status);
      lte_connection_status_set(temp.rrc_active == LTE_NETWORK_STATE_ON);
   }
   k_mutex_unlock(&lte_mutex);
   if (info) {
      *info = temp;
   }
   return 0;
}

int modem_read_statistic(struct lte_network_statistic *statistic)
{
   int err;
   char buf[64];

   memset(statistic, 0, sizeof(struct lte_network_statistic));
   err = modem_at_cmd(buf, sizeof(buf), "%XCONNSTAT: ", "AT%XCONNSTAT?");
   if (err > 0) {
      sscanf(buf, " %*u,%*u,%u,%u,%hu,%hu",
             &statistic->transmitted,
             &statistic->received,
             &statistic->max_packet_size,
             &statistic->average_packet_size);
   }
   k_mutex_lock(&lte_mutex, K_FOREVER);
   statistic->searchs = lte_searchs;
   statistic->search_time = MSEC_TO_SEC(lte_search_time);
   statistic->psm_delays = lte_psm_delays;
   statistic->psm_delay_time = MSEC_TO_SEC(lte_psm_delay_time);
   statistic->restarts = lte_starts > 0 ? lte_starts - 1 : 0;
   statistic->cell_updates = lte_cell_updates;
   statistic->wakeups = lte_wakeups;
   statistic->wakeup_time = MSEC_TO_SEC(lte_wakeup_time);
   statistic->connected_time = MSEC_TO_SEC(lte_connected_time);
   statistic->asleep_time = MSEC_TO_SEC(lte_asleep_time);
   k_mutex_unlock(&lte_mutex);
   return 0;
}

int modem_read_coverage_enhancement_info(struct lte_ce_info *info)
{
   int err;
   int err2;
   char buf[64];
   struct lte_ce_info temp;

   memset(&temp, 0, sizeof(temp));
   err = modem_at_cmd(buf, sizeof(buf), "+CEINFO: ", "AT+CEINFO?");
   if (err > 0) {
      uint16_t values[3] = {0, 0, 0};
      // CEINFO: 0,1,I,8,2,-97,9
      // "%hhu" is not supported
      err = sscanf(buf, " %*u,%hu,%c,%hu,%hu,%hd,%hd",
                   &values[0],
                   &temp.state,
                   &values[1],
                   &values[2],
                   &temp.rsrp,
                   &temp.cinr);
      if (err == 6) {
         LOG_INF("CEINFO: %s", buf);
         temp.ce_supported = values[0];
         temp.downlink_repetition = values[1];
         temp.uplink_repetition = values[2];
         if (temp.rsrp == 255) {
            temp.rsrp = INVALID_SIGNAL_VALUE;
         }
         if (temp.cinr == 127) {
            temp.cinr = INVALID_SIGNAL_VALUE;
         }
         err2 = modem_at_cmd(buf, sizeof(buf), "%XSNRSQ: ", "AT%XSNRSQ?");
         if (err2 > 0) {
            LOG_INF("XSNRSQ: %s", buf);
            err2 = sscanf(buf, " %hd", &temp.snr);
            if (err2 == 1) {
               if (temp.snr == 127) {
                  temp.snr = INVALID_SIGNAL_VALUE;
               } else {
                  temp.snr -= 24;
               }
            }
         }
         k_mutex_lock(&lte_mutex, K_FOREVER);
         if (err2 <= 0) {
            temp.snr = ce_info.snr;
         }
         if (network_info.rrc_active != LTE_NETWORK_STATE_ON) {
            if (temp.downlink_repetition == 0) {
               temp.downlink_repetition = ce_info.downlink_repetition;
            }
            if (temp.uplink_repetition == 0) {
               temp.uplink_repetition = ce_info.uplink_repetition;
            }
            if (temp.rsrp == INVALID_SIGNAL_VALUE) {
               temp.rsrp = ce_info.rsrp;
            }
            if (temp.cinr == INVALID_SIGNAL_VALUE) {
               temp.cinr = ce_info.cinr;
            }
            if (temp.snr == INVALID_SIGNAL_VALUE) {
               temp.snr = ce_info.snr;
            }
         }
         ce_info = temp;
         k_mutex_unlock(&lte_mutex);
         if (info) {
            *info = temp;
         }
      } else {
         LOG_ERR("CEINFO: %s => %d", buf, err);
      }
   }
   return err;
}

int modem_read_rate_limit_time(uint32_t *time)
{
   char buf[32];
   int result = modem_at_cmd(buf, sizeof(buf), "%APNRATECTRL: ", "AT%APNRATECTRL=0,0");
   if (result > 0) {
      uint32_t rate_limit_time = 0;
      // %APNRATECTRL: 0,0,0[,time]
      LOG_INF("APNRATECTRL STATUS: %s", buf);
      result = sscanf(buf, "%*u,%*u,%*u,%u", &rate_limit_time);
      if (EOF == result) {
         result = 0;
      }
      k_mutex_lock(&lte_mutex, K_FOREVER);
      network_info.rate_limit_time = rate_limit_time;
      k_mutex_unlock(&lte_mutex);
      if (time) {
         *time = rate_limit_time;
      }
   }
   return result;
}

int modem_set_reduced_mobility(int mode)
{
   int err = -EINVAL;
   if (0 <= mode && mode <= 2) {
      char buf[32];
      err = modem_at_cmdf(buf, sizeof(buf), NULL, "AT%%REDMOB=%d", mode);
      if (err >= 0) {
         modem_get_reduced_mobility();
      }
   }
   return err;
}

int modem_get_reduced_mobility(void)
{
   char buf[32];
   unsigned int mode = 0;
   int err = modem_at_cmd(buf, sizeof(buf), "%REDMOB: ", "AT%REDMOB?");
   if (err >= 0) {
      LOG_INF("REDMOB: %s", buf);
      err = sscanf(buf, "%u", &mode);
      if (err == 1) {
         err = mode;
      } else {
         err = -EINVAL;
      }
   }
   return err;
}

int modem_set_power_level(int level)
{
   int err = -EINVAL;
   if (0 <= level && level <= 4) {
      char buf[32];
      err = modem_at_cmdf(buf, sizeof(buf), NULL, "AT%%XDATAPRFL=%d", level);
      if (err >= 0) {
         modem_get_power_level();
      }
   }
   return err;
}

int modem_get_power_level(void)
{
   char buf[32];
   unsigned int level = 0;
   int err = modem_at_cmd(buf, sizeof(buf), "%XDATAPRFL: ", "AT%XDATAPRFL?");
   if (err >= 0) {
      LOG_INF("XDATAPRFL: %s", buf);
      err = sscanf(buf, "%u", &level);
      if (err == 1) {
         err = level;
      } else {
         err = -EINVAL;
      }
   }
   return err;
}

int modem_set_psm(int16_t active_time_s)
{
#if defined(CONFIG_UDP_PSM_ENABLE) && defined(CONFIG_LTE_LC_PSM_MODULE)
   int current;
   if (active_time_s < -1) {
      active_time_s = -1;
   }
   k_mutex_lock(&lte_mutex, K_FOREVER);
   current = psm_rat;
   if (-2 <= psm_rat) {
      psm_rat = active_time_s;
   }
   k_mutex_unlock(&lte_mutex);
   if (-2 <= current && current != active_time_s) {
      if (active_time_s < 0) {
         LOG_INF("PSM disable");
         return lte_lc_psm_req(false);
      } else {
         char rat[9] = "00000000";
         int mul = 2;
         // 2s
         active_time_s /= 2;
         if (active_time_s > 31) {
            // 60s
            active_time_s /= 30;
            mul = 60;
            if (active_time_s > 31) {
               // 360s
               active_time_s /= 6;
               mul = 360;
               rat[1] = '1';
            } else {
               rat[2] = '1';
            }
         }
         print_bin(&rat[3], 5, active_time_s);
         lte_lc_psm_param_set(CONFIG_LTE_PSM_REQ_RPTAU, rat);
         LOG_INF("PSM enable, act: %d s", active_time_s * mul);
         return lte_lc_psm_req(true);
      }
   }
   return 0;
#else
   (void)active_time_s;
   return 0;
#endif
}

int modem_set_rai_mode(enum rai_mode mode, int socket)
{
   int err;

   k_mutex_lock(&lte_mutex, K_FOREVER);
   err = rai_lock;
   k_mutex_unlock(&lte_mutex);
   if (err) {
      return 0;
   }
#ifdef CONFIG_CP_RAI_ON
   if (rai_current_mode != mode) {
      int rai = -1;
      const char *desc = "";
      /** Control Plane Release Assistance Indication  */
      if (mode == RAI_MODE_OFF) {
         rai = 0;
         desc = "RAI disable";
      } else if (mode == RAI_MODE_ONE_RESPONSE) {
         rai = 3;
         desc = "RAI one response";
      } else if (mode == RAI_MODE_LAST) {
         rai = 4;
         desc = "RAI no response";
      }
      if (rai >= 0) {
         err = modem_at_cmdf(NULL, 0, NULL, "AT%%XRAI=%d", rai);
         if (err < 0) {
            LOG_WRN("%s, error: %d", desc, err);
         } else {
            LOG_INF("%s, success", desc);
            rai_current_mode = mode;
         }
      }
   }
#elif defined(CONFIG_AS_RAI_ON)
   /** Access stratum Release Assistance Indication  */
   int option = -1;
   const char *desc = "";

#if NCS_VERSION_NUMBER < 0x20600
   /* deprecated with NCS 2.6.0 */

   switch (mode) {
      case RAI_MODE_NOW:
#ifdef CONFIG_UDP_USE_CONNECT
         option = SO_RAI_NO_DATA;
         desc = "now";
#endif
         break;
      case RAI_MODE_LAST:
         option = SO_RAI_LAST;
         desc = "last";
         break;
      case RAI_MODE_ONE_RESPONSE:
         option = SO_RAI_ONE_RESP;
         desc = "one response";
         break;
      case RAI_MODE_OFF:
      default:
         if (rai_current_mode != SO_RAI_ONGOING) {
            option = SO_RAI_ONGOING;
            desc = "off";
         }
         break;
   }
   if (option >= 0) {
      if (socket < 0) {
         err = -EIO;
      } else {
         err = setsockopt(socket, SOL_SOCKET, option, NULL, 0);
         if (err) {
            LOG_WRN("RAI sockopt %d/%s, error %d (%s)", option, desc, errno, strerror(errno));
         } else {
            LOG_INF("RAI sockopt %d/%s, success", option, desc);
            rai_current_mode = option;
         }
      }
   }
#else /* NCS since 2.6.0*/
   switch (mode) {
      case RAI_MODE_NOW:
#ifdef CONFIG_UDP_USE_CONNECT
         option = RAI_NO_DATA;
         desc = "now";
#endif
         break;
      case RAI_MODE_LAST:
         option = RAI_LAST;
         desc = "last";
         break;
      case RAI_MODE_ONE_RESPONSE:
         option = RAI_ONE_RESP;
         desc = "one response";
         break;
      case RAI_MODE_OFF:
      default:
         if (rai_current_mode != RAI_ONGOING) {
            option = RAI_ONGOING;
            desc = "off";
         }
         break;
   }
   if (option >= 0) {
      if (socket < 0) {
         err = -EIO;
      } else {
         err = setsockopt(socket, SOL_SOCKET, SO_RAI, &option, sizeof(option));
         if (err) {
            LOG_WRN("RAI sockopt %d/%s, error %d (%s)", option, desc, errno, strerror(errno));
         } else {
            LOG_INF("RAI sockopt %d/%s, success", option, desc);
            rai_current_mode = option;
         }
      }
   }

#endif /* NCS 2.6.0 */
#else  /* !CONFIG_CP_RAI_ON && !CONFIG_AS_RAI_ON */
   (void)mode;
   LOG_INF("No AS nor CP RAI mode configured!");
#endif /* CONFIG_CP_RAI_ON || CONFIG_AS_RAI_ON */
   return err;
}

int modem_set_edrx(int16_t edrx_time_s)
{
   int res = 0;
   if (edrx_time_s == 0) {
      LOG_INF("eDRX off");
      res = modem_at_cmd(NULL, 0, NULL, "AT+CEDRXS=0");
      if (res >= 0) {
         // switching off eDRX, switches also off the notifications
         k_mutex_lock(&lte_mutex, K_FOREVER);
         edrx_status.mode = LTE_LC_LTE_MODE_NONE;
         requested_edrx_time_s = 0;
         k_mutex_unlock(&lte_mutex);
      }
   } else {
      int res2 = 0;
      int edrx_code = 0;
      char edrx[5] = "0000";
      float time = edrx_time_s;
      float edrx_time = 0.0F;

      for (edrx_code = 0;; ++edrx_code) {
         edrx_time = modem_get_edrx_multiplier(edrx_code) * 5.12F;
         if (time <= edrx_time || edrx_code >= 15) {
            break;
         }
      }
      print_bin(edrx, 4, edrx_code);
      LOG_INF("eDRX enable, %.2f s", (double)edrx_time);
      res2 = modem_at_cmdf(NULL, 0, NULL, "AT+CEDRXS=2,5,\"%s\"", edrx);
      res = modem_at_cmdf(NULL, 0, NULL, "AT+CEDRXS=2,4,\"%s\"", edrx);
      if (res2 < 0) {
         return res2;
      }
      if (res >= 0) {
         k_mutex_lock(&lte_mutex, K_FOREVER);
         requested_edrx_time_s = edrx_time_s;
         k_mutex_unlock(&lte_mutex);
      }
   }
   return res < 0 ? res : 0;
}

static const uint16_t edrx_multiplier[16] = {1, 2, 4, 8, 12, 16, 20, 24, 28, 32, 64, 128, 256, 512, 1024, 2048};

int modem_get_edrx_multiplier(int edrx_code)
{
   if (0 <= edrx_code && edrx_code < 16) {
      return edrx_multiplier[edrx_code];
   } else {
      return 0;
   }
}

int modem_print_edrx(const char *desc, struct lte_lc_edrx_cfg *edrx_cfg, char *buf, size_t len)
{
   const char *sep = (*desc == 0 || strend(desc, " ", false)) ? "" : " ";
   const char *mode = NULL;
   switch (edrx_cfg->mode) {
      case LTE_LC_LTE_MODE_NONE:
         return snprintf(buf, len, "eDRX %s%sdisabled.", desc, sep);
      case LTE_LC_LTE_MODE_LTEM:
         mode = "CAT-M1";
         break;
      case LTE_LC_LTE_MODE_NBIOT:
         mode = "NB-IoT";
         break;
      default:
         return snprintf(buf, len, "eDRX %s%sunknown.", desc, sep);
   }
   if (mode != NULL) {
      if (edrx_cfg->edrx < 1.0F) {
         return snprintf(buf, len, "eDRX %s%s%s disabled.", desc, sep, mode);
      } else if (edrx_cfg->ptw < 1.0F) {
         return snprintf(buf, len, "eDRX %s%s%s %.2fs", desc, sep, mode, (double)edrx_cfg->edrx);
      } else {
         return snprintf(buf, len, "eDRX %s%s%s %.2fs, ptw %.2fs", desc, sep, mode, (double)edrx_cfg->edrx, (double)edrx_cfg->ptw);
      }
   }
   return 0;
}

int modem_set_ptw(int16_t ptw_time_s)
{
   int res = 0;
   int res2 = 0;
   int ptw_code = 0;
   char ptw[5] = "0000";
   float time = ptw_time_s;
   float ptw_time = 0.0F;

   for (ptw_code = 0;; ++ptw_code) {
      ptw_time = (ptw_code + 1) * 1.28F;
      if (time <= ptw_time || ptw_code >= 15) {
         break;
      }
   }
   print_bin(ptw, 4, ptw_code);
   LOG_INF("LTE-M PTW %.2f s", (double)ptw_time);
   res = modem_at_cmdf(NULL, 0, NULL, "AT%%XPTW=4,\"%s\"", ptw);

   for (ptw_code = 0;; ++ptw_code) {
      ptw_time = (ptw_code + 1) * 2.56F;
      if (time <= ptw_time || ptw_code >= 15) {
         break;
      }
   }
   print_bin(ptw, 4, ptw_code);
   LOG_INF("NB-IoT PTW %.2f s", (double)ptw_time);
   res2 = modem_at_cmdf(NULL, 0, NULL, "AT%%XPTW=5,\"%s\"", ptw);
   if (res2 < 0) {
      return res2;
   }
   if (res >= 0) {
      k_mutex_lock(&lte_mutex, K_FOREVER);
      requested_ptw_time_s = ptw_time_s;
      k_mutex_unlock(&lte_mutex);
   }
   return res < 0 ? res : 0;
}

void modem_lock_psm(bool on)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   psm_rat = on ? -3 : -2;
   k_mutex_unlock(&lte_mutex);
}

void modem_lock_rai(bool on)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   rai_lock = on ? 1 : 0;
   k_mutex_unlock(&lte_mutex);
}

void modem_lock_plmn(bool on)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   network_info.plmn_lock = on ? LTE_NETWORK_STATE_ON : LTE_NETWORK_STATE_OFF;
   k_mutex_unlock(&lte_mutex);
}

int modem_set_offline(void)
{
   LOG_INF("modem offline");
   watchdog_feed();

   return lte_lc_offline();
}

int modem_set_normal(void)
{
   LOG_INF("modem normal");
   watchdog_feed();
   return lte_lc_normal();
}

int modem_set_lte_offline(void)
{
   LOG_INF("modem deactivate LTE");
   watchdog_feed();
   return lte_lc_func_mode_set(LTE_LC_FUNC_MODE_DEACTIVATE_LTE) ? -EFAULT : 0;
}

int modem_power_off(void)
{
   LOG_INF("modem off");
   watchdog_feed();
   return lte_lc_power_off();
}

int modem_factory_reset(void)
{
   char buf[16];
   int err = modem_at_cmd(buf, sizeof(buf), NULL, "AT%XFACTORYRESET=0");
   if (err > 0) {
      LOG_INF("Factory reset: %s", buf);
      k_sleep(K_SECONDS(5));
   }
   return err;
}
#else

int modem_init(int config, lte_state_change_callback_handler_t state_handler)
{
   (void)config;
   (void)state_handler;
   return 0;
}

int modem_wait_ready(const k_timeout_t timeout)
{
   (void)timeout;
   return 0;
}

int modem_start(const k_timeout_t timeout)
{
   (void)timeout;
   return 0;
}

int modem_get_edrx_status(struct lte_lc_edrx_cfg *edrx)
{
   (void)edrx;
   return -ENODATA;
}

int modem_get_psm_status(struct lte_lc_psm_cfg *psm, uint32_t *delays)
{
   (void)psm;
   (void)delays;
   return -ENODATA;
}

int modem_get_network_info(struct lte_network_info *info)
{
   (void)info;
   return -ENODATA;
}

int modem_get_coverage_enhancement_info(struct lte_ce_info *info)
{
   (void)info;
   return -ENODATA;
}

int modem_get_sim_info(struct lte_sim_info *info)
{
   (void)info;
   return -ENODATA;
}

int modem_get_modem_info(struct lte_modem_info *info)
{
   (void)info;
   return -ENODATA;
}

int modem_get_imei(char *buf, size_t len)
{
   (void)buf;
   (void)len;
   return 0;
}

int modem_read_network_info(struct lte_network_info *info)
{
   (void)info;
   return -ENODATA;
}

int modem_read_statistic(struct lte_network_statistic *statistic)
{
   (void)statistic;
   return -ENODATA;
}

int modem_read_coverage_enhancement_info(struct lte_ce_info *info)
{
   (void)info;
   return -ENODATA;
}

int modem_get_release_time(void)
{
   return -1;
}

void modem_set_transmission_time(void)
{
}

void modem_set_scan_time(void)
{
}

int modem_set_psm(int16_t active_time_s)
{
   (void)active_time_s;
   return 0;
}

int modem_set_rai_mode(enum rai_mode mode, int socket)
{
   (void)mode;
   (void)socket;
   return 0;
}

int modem_set_offline(void)
{
   return 0;
}

int modem_set_lte_offline(void)
{
   return 0;
}

int modem_set_normal(void)
{
   return 0;
}

int modem_power_off(void)
{
   return 0;
}

int modem_factory_reset(void)
{
   return 0;
}
#endif
