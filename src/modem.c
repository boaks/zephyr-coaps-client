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
#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <modem/pdn.h>
#include <net/socket.h>
#include <nrf_modem_at.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr.h>

#include "io_job_queue.h"
#include "modem.h"
#include "parse.h"
#include "power_manager.h"
#include "ui.h"

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

#if defined(CONFIG_NRF_MODEM_LIB)

#define LED_CONNECTED LED_NONE
#define LED_READY LED_LTE_3

static K_MUTEX_DEFINE(lte_mutex);
static K_CONDVAR_DEFINE(lte_condvar);

static K_SEM_DEFINE(ptau_update, 0, 1);

static wakeup_callback_handler_t s_wakeup_handler = NULL;
static lte_state_change_callback_handler_t s_lte_state_change_handler = NULL;
static bool initialized = 0;

static volatile bool lte_registered = false;

static bool lte_ready = false;
static bool lte_connected = false;
#ifdef CONFIG_PDN
static bool lte_pdn_active = false;
#endif

static const char *volatile network_mode = "init";

static struct lte_lc_edrx_cfg edrx_status = {LTE_LC_LTE_MODE_NONE, 0.0, 0.0};
static struct lte_lc_psm_cfg psm_status = {0, 0};
static uint32_t lte_searchs = 0;
static uint32_t lte_psm_delays = 0;
static uint32_t lte_cell_updates = 0;
static bool lte_plmn_lock = false;
static bool lte_force_nb_iot = false;
static bool lte_force_lte_m = false;
static struct lte_network_info network_info;
static int64_t transmission_time = 0;
static uint8_t iccid[MODEM_ID_SIZE];
static uint8_t imsi[MODEM_ID_SIZE];
static uint8_t imei[MODEM_ID_SIZE];

static volatile enum rai_mode rai_current_mode = RAI_OFF;
static volatile int rai_time = -1;

#define SUSPEND_DELAY_MILLIS 2000

static void modem_power_management_suspend_work_fn(struct k_work *work);

static K_WORK_DEFINE(modem_power_management_resume_work, modem_power_management_suspend_work_fn);
static K_WORK_DELAYABLE_DEFINE(modem_power_management_suspend_work, modem_power_management_suspend_work_fn);

static void modem_power_management_suspend_work_fn(struct k_work *work)
{
   if (work == &modem_power_management_resume_work) {
      power_manager_suspend(false);
   } else {
      struct k_work_delayable *dwork = k_work_delayable_from_work(work);
      if (dwork == &modem_power_management_suspend_work) {
         power_manager_suspend(true);
      }
   }
}

static void modem_read_network_info_work_fn(struct k_work *work)
{
   modem_read_network_info(NULL);
}

static K_WORK_DEFINE(modem_read_network_info_work, modem_read_network_info_work_fn);

static void modem_ready_work_fn(struct k_work *work)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   if (lte_ready) {
      k_condvar_broadcast(&lte_condvar);
   }
   k_mutex_unlock(&lte_mutex);
   LOG_INF("modem signaled ready.");
}

static K_WORK_DEFINE(modem_ready_work, modem_ready_work_fn);

static void modem_state_change_callback_work_fn(struct k_work *work);

static K_WORK_DEFINE(modem_registered_callback_work, modem_state_change_callback_work_fn);
static K_WORK_DEFINE(modem_unregistered_callback_work, modem_state_change_callback_work_fn);
static K_WORK_DEFINE(modem_connected_callback_work, modem_state_change_callback_work_fn);
static K_WORK_DEFINE(modem_idle_callback_work, modem_state_change_callback_work_fn);

static void modem_state_change_callback_work_fn(struct k_work *work)
{
   lte_state_change_callback_handler_t callback = s_lte_state_change_handler;
   if (callback) {
      if (work == &modem_connected_callback_work) {
         callback(LTE_STATE_CONNECT_NETWORK, true);
      } else if (work == &modem_idle_callback_work) {
         callback(LTE_STATE_CONNECT_NETWORK, false);
      } else if (work == &modem_registered_callback_work) {
         callback(LTE_STATE_REGISTER_NETWORK, true);
      } else if (work == &modem_unregistered_callback_work) {
         callback(LTE_STATE_REGISTER_NETWORK, false);
      }
   }
}

#ifdef CONFIG_PDN
static void modem_read_pdn_info_work_fn(struct k_work *work)
{
   modem_read_pdn_info(NULL, 0);
}

static K_WORK_DEFINE(modem_read_pdn_info_work, modem_read_pdn_info_work_fn);
#endif

static void modem_read_sim_work_fn(struct k_work *work)
{
   char buf[40];

   int err = modem_at_cmd("AT+CIMI", buf, sizeof(buf), NULL);
   if (err < 0) {
      LOG_INF("Failed to read IMSI.");
   } else {
      LOG_INF("imsi: %s", buf);
      k_mutex_lock(&lte_mutex, K_FOREVER);
      strncpy(imsi, buf, sizeof(imsi));
      k_mutex_unlock(&lte_mutex);
   }
   err = modem_at_cmd("AT%%XICCID", buf, sizeof(buf), "%XICCID: ");
   if (err < 0) {
      LOG_INF("Failed to read ICCID.");
   } else {
      LOG_INF("iccid: %s", buf);
      k_mutex_lock(&lte_mutex, K_FOREVER);
      strncpy(iccid, buf, sizeof(iccid));
      k_mutex_unlock(&lte_mutex);
   }
}

static K_WORK_DEFINE(modem_read_sim_work, modem_read_sim_work_fn);

static bool lte_ready_wait(k_timeout_t timeout)
{
   bool res = 0;
   k_mutex_lock(&lte_mutex, timeout);
   res = lte_ready;
   if (!res) {
      k_condvar_wait(&lte_condvar, &lte_mutex, timeout);
      res = lte_ready;
   }
   k_mutex_unlock(&lte_mutex);
   return res;
}

static void lte_set_edrx_status(const struct lte_lc_edrx_cfg *edrx)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   edrx_status = *edrx;
   k_mutex_unlock(&lte_mutex);
}

static void lte_set_psm_status(const struct lte_lc_psm_cfg *psm)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   psm_status = *psm;
   k_mutex_unlock(&lte_mutex);
}

static void lte_inc_psm_delays(void)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   ++lte_psm_delays;
   k_mutex_unlock(&lte_mutex);
}

static void lte_inc_searchs()
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   ++lte_searchs;
   k_mutex_unlock(&lte_mutex);
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

static int64_t get_transmission_time(void)
{
   int64_t time;
   k_mutex_lock(&lte_mutex, K_FOREVER);
   time = transmission_time;
   k_mutex_unlock(&lte_mutex);
   return time;
}

static void lte_connection_status()
{
#ifdef CONFIG_PDN
   bool ready = lte_registered && lte_connected && lte_pdn_active;
#else
   bool ready = lte_registered && lte_connected;
#endif
   if (lte_ready != ready) {
      lte_ready = ready;
      ui_led_op(LED_READY, ready ? LED_SET : LED_CLEAR);
      if (ready) {
         work_submit_to_io_queue(&modem_connected_callback_work);
         work_submit_to_io_queue(&modem_read_network_info_work);
#ifdef CONFIG_PDN
         work_submit_to_io_queue(&modem_read_pdn_info_work);
#endif
         work_submit_to_io_queue(&modem_ready_work);
         LOG_INF("modem ready.");
      } else {
         work_submit_to_io_queue(&modem_idle_callback_work);
#ifdef CONFIG_PDN
         LOG_INF("modem not ready. con=%d/pdn=%d/reg=%d", lte_connected, lte_pdn_active, lte_registered);
#else
         LOG_INF("modem not ready. con=%d/reg=%d", lte_connected, lte_registered);
#endif
      }
   }
}

static void lte_registration_set(bool registered)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   if (lte_registered != registered) {
      lte_registered = registered;
      lte_connection_status();
   }
   k_mutex_unlock(&lte_mutex);
}

static void lte_connection_status_set(bool connect)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   if (lte_connected != connect) {
      ui_led_op(LED_CONNECTED, connect ? LED_SET : LED_CLEAR);
      lte_connected = connect;
      lte_connection_status();
   }
   k_mutex_unlock(&lte_mutex);
}

#ifdef CONFIG_PDN
static void lte_pdn_status_set(bool pdn_active)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   if (lte_pdn_active != pdn_active) {
      lte_pdn_active = pdn_active;
      lte_connection_status();
   }
   k_mutex_unlock(&lte_mutex);
}
#endif

static void lte_registration(enum lte_lc_nw_reg_status reg_status)
{
   const char *description = "unknown";
   bool registered = false;

   switch (reg_status) {
      case LTE_LC_NW_REG_NOT_REGISTERED:
         description = "Not Connected";
         break;
      case LTE_LC_NW_REG_REGISTERED_HOME:
         description = "Connected - home network";
         registered = true;
         break;
      case LTE_LC_NW_REG_SEARCHING:
         description = "Searching ...";
         lte_inc_searchs();
         work_submit_to_io_queue(&modem_read_sim_work);
         break;
      case LTE_LC_NW_REG_REGISTRATION_DENIED:
         description = "Not Connected - denied";
         break;
      case LTE_LC_NW_REG_UNKNOWN:
         break;
      case LTE_LC_NW_REG_REGISTERED_ROAMING:
         description = "Connected - roaming network";
         registered = true;
         break;
      case LTE_LC_NW_REG_REGISTERED_EMERGENCY:
         description = "Connected - emergency network";
         registered = true;
         break;
      case LTE_LC_NW_REG_UICC_FAIL:
         description = "Not Connected - UICC fail";
         break;
   }

   lte_registration_set(registered);
   if (registered) {
      work_submit_to_io_queue(&modem_read_sim_work);
      work_submit_to_io_queue(&modem_registered_callback_work);
   } else {
      work_submit_to_io_queue(&modem_unregistered_callback_work);
   }
   LOG_INF("Network registration status: %s", description);
}

static void lte_handler(const struct lte_lc_evt *const evt)
{
   static int64_t connect_time = 0;
   static int64_t idle_time = 0;
   static int active_time = -1;

   switch (evt->type) {
      case LTE_LC_EVT_NW_REG_STATUS:
         lte_registration(evt->nw_reg_status);
         break;
      case LTE_LC_EVT_LTE_MODE_UPDATE:
         if (evt->lte_mode == LTE_LC_LTE_MODE_NONE) {
            network_mode = "none";
         } else if (evt->lte_mode == LTE_LC_LTE_MODE_LTEM) {
            network_mode = "CAT-M1";
         } else if (evt->lte_mode == LTE_LC_LTE_MODE_NBIOT) {
            network_mode = "NB-IoT";
         }
         LOG_INF("LTE Mode: %s", network_mode);
         break;
      case LTE_LC_EVT_PSM_UPDATE:
         LOG_INF("PSM parameter update: TAU: %d s, Active time: %d s",
                 evt->psm_cfg.tau, evt->psm_cfg.active_time);
         active_time = evt->psm_cfg.active_time;
         lte_set_psm_status(&evt->psm_cfg);
         if (evt->psm_cfg.active_time >= 0) {
            k_sem_give(&ptau_update);
         }
         break;
      case LTE_LC_EVT_EDRX_UPDATE:
         {
            char log_buf[60];
            ssize_t len;
            len = snprintf(log_buf, sizeof(log_buf),
                           "eDRX parameter update: eDRX: %f, PTW: %f",
                           evt->edrx_cfg.edrx, evt->edrx_cfg.ptw);
            if (len > 0) {
               LOG_INF("%s", log_buf);
            }
            lte_set_edrx_status(&evt->edrx_cfg);
            break;
         }
      case LTE_LC_EVT_RRC_UPDATE:
         {
            int64_t now = k_uptime_get();
            if (evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED) {
               lte_connection_status_set(true);
               connect_time = now;
               (void)k_work_cancel_delayable(&modem_power_management_suspend_work);
               work_submit_to_io_queue(&modem_power_management_resume_work);
               LOG_INF("RRC mode: Connected");
            } else {
               int64_t time = get_transmission_time();
               lte_connection_status_set(false);
               idle_time = now;
               if ((time - connect_time) > 0) {
                  rai_time = (int)(now - time);
                  LOG_INF("RRC mode: Idle after %lld ms (%d ms inactivity)", now - connect_time, rai_time);
               } else {
                  rai_time = -1;
                  LOG_INF("RRC mode: Idle after %lld ms", now - connect_time);
               }
               if (active_time >= 0) {
                  work_reschedule_for_io_queue(&modem_power_management_suspend_work, K_MSEC(SUSPEND_DELAY_MILLIS + active_time * MSEC_PER_SEC));
               } else {
                  work_reschedule_for_io_queue(&modem_power_management_suspend_work, K_MSEC(SUSPEND_DELAY_MILLIS));
               }
            }
            break;
         }
      case LTE_LC_EVT_TAU_PRE_WARNING:
         LOG_INF("LTE Tracking area Update");
         if (lte_registered) {
            //            work_submit_to_io_queue(&modem_read_network_info_work);
         }
         break;
      case LTE_LC_EVT_CELL_UPDATE:
         LOG_INF("LTE cell changed: Cell ID: %d, Tracking area: %d", evt->cell.id, evt->cell.tac);
         lte_update_cell(evt->cell.tac, evt->cell.id);
         break;
      case LTE_LC_EVT_MODEM_SLEEP_ENTER:
         if (idle_time) {
            int64_t time = (k_uptime_get() - idle_time);
            bool delayed = active_time >= 0 && ((time / MSEC_PER_SEC) > (active_time + 5));
            if (delayed) {
               lte_inc_psm_delays();
            }
            LOG_INF("LTE modem sleeps after %lld ms idle%s", time, delayed ? ", delayed" : "");
            idle_time = 0;
         } else {
            LOG_INF("LTE modem sleeps");
         }
         work_reschedule_for_io_queue(&modem_power_management_suspend_work, K_MSEC(SUSPEND_DELAY_MILLIS));
         break;
      case LTE_LC_EVT_MODEM_SLEEP_EXIT:
         (void)k_work_cancel_delayable(&modem_power_management_suspend_work);
         work_submit_to_io_queue(&modem_power_management_resume_work);
         LOG_INF("LTE modem wakes up");
         if (s_wakeup_handler) {
            s_wakeup_handler();
         }
         break;
      case LTE_LC_EVT_MODEM_EVENT:
         if (evt->modem_evt == LTE_LC_MODEM_EVT_BATTERY_LOW) {
            LOG_INF("LTE modem Battery Low!");
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
      default:
         break;
   }
}

#ifdef CONFIG_PDN
static void pdn_handler(uint8_t cid, enum pdn_event event,
                        int reason)
{
   switch (event) {
      case PDN_EVENT_CNEC_ESM:
         LOG_INF("PDN CID %u, error %d", cid, reason);
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
   }
}
#endif

static void terminate_at_buffer(char *line, size_t len)
{
   while (*line != 0 && *line != '\n' && *line != '\r' && len > 1) {
      ++line;
      --len;
   }
   if (*line != 0 && len > 0) {
      *line = 0;
   }
}

static int modem_connect(void)
{
   int err = 0;

   if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT)) {
      /* Do nothing, modem is already configured and LTE connected. */
   } else {
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
   }
   return err;
}

int modem_init(int config, wakeup_callback_handler_t wakeup_handler, lte_state_change_callback_handler_t state_handler)
{
   int err = 0;

   if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT)) {
      /* Do nothing, modem is already configured and LTE connected. */
   } else if (!initialized) {
      char buf[128];
      const char *plmn = NULL;

      nrf_modem_lib_init(NORMAL_MODE);
      err = modem_at_cmd("AT%%HWVERSION", buf, sizeof(buf), "%HWVERSION: ");
      if (err > 0) {
         LOG_INF("hw: %s", buf);
      }
      err = modem_at_cmd("AT+CGMR", buf, sizeof(buf), NULL);
      if (err > 0) {
         LOG_INF("rev: %s", buf);
      }
      err = modem_at_cmd("AT+CGSN", buf, sizeof(buf), NULL);
      if (err < 0) {
         LOG_INF("Failed to read IMEI.");
      } else {
         LOG_INF("imei: %s", buf);
         strncpy(imei, buf, sizeof(imei));
      }
      if ((config & 3) == 3) {
         err = modem_at_cmd("AT%%XFACTORYRESET=0", buf, sizeof(buf), NULL);
         LOG_INF("Factory reset: %s", buf);
         k_sleep(K_SECONDS(10));
      } else if (config & 2) {
         // force NB-IoT only
         lte_force_nb_iot = true;
         lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_NBIOT, LTE_LC_SYSTEM_MODE_NBIOT);
      } else if (config & 1) {
         // force LTE-M only
         lte_force_lte_m = true;
         lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_LTEM, LTE_LC_SYSTEM_MODE_LTEM);
      }
#ifdef CONFIG_UDP_AS_RAI_ENABLE
      err = modem_at_cmd("AT%%REL14FEAT=0,1,0,0,0", buf, sizeof(buf), NULL);
      if (err > 0) {
         LOG_INF("rel14feat: %s", buf);
      }
#endif
      err = modem_at_cmd("AT%%XEPCO=1", buf, sizeof(buf), NULL);
      if (err > 0) {
         LOG_INF("epco: %s", buf);
      }
      err = modem_at_cmd("AT%%XCONNSTAT=1", buf, sizeof(buf), NULL);
      if (err > 0) {
         LOG_INF("stat: %s", buf);
      }

#ifndef CONFIG_LTE_LOCK_BANDS
      err = modem_at_cmd("AT%%XBANDLOCK?", buf, sizeof(buf), "%XBANDLOCK: ");
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
         lte_plmn_lock = true;
#ifdef CONFIG_LTE_LOCK_PLMN
         err = 0;
#else
         err = nrf_modem_at_printf("AT+COPS=1,2,\"%s\"", plmn);
#endif
      } else {
         LOG_INF("Unlock PLMN");
         err = nrf_modem_at_printf("AT+COPS=0");
         lte_plmn_lock = false;
      }
      if (err) {
         LOG_WRN("Failed to lock PLMN, err %d", err);
      }

#ifdef CONFIG_UDP_PSM_ENABLE
      err = lte_lc_psm_req(true);
      if (err) {
         if (err == -EFAULT) {
            LOG_WRN("Modem set PSM failed, AT cmd failed!");
         } else {
            LOG_WRN("Modem set PSM failed, error: %d!", err);
         }
      } else {
         err = modem_at_cmd("AT+CPSMS?", buf, sizeof(buf), "+CPSMS: ");
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

#ifdef CONFIG_UDP_EDRX_ENABLE
      err = lte_lc_edrx_req(true);
      if (err) {
         if (err == -EFAULT) {
            LOG_WRN("Modem set eDRX failed, AT cmd failed!");
         } else {
            LOG_WRN("Modem set eDRX failed, error: %d!", err);
         }
      } else {
         err = modem_at_cmd("AT+CEDRXS?", buf, sizeof(buf), "+CEDRXS: ");
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

#if 0
      err = nrf_modem_at_printf("AT+CGDCONT=1,\"IPV4V6\",\"flolive.net\"");
      if (err) {
         LOG_WRN("Failed to set CGDCONT, err %d", err);
      }
#endif

#ifdef CONFIG_PDN
      pdn_default_ctx_cb_reg(pdn_handler);
      err = modem_at_cmd("AT+CGEREP=1", buf, sizeof(buf), "+CGEREP: ");
      if (err < 0) {
         LOG_WRN("Failed to enable CGEREP, err %d", err);
      } else if (err > 0) {
         LOG_INF("CGEREP: %s", buf);
      }
      err = modem_at_cmd("AT+CNEC=24", buf, sizeof(buf), "+CNEC: ");
      if (err < 0) {
         LOG_WRN("Failed to enable CNEC, err %d", err);
      } else if (err > 0) {
         LOG_INF("CNEC: %s", buf);
      }
#endif

      err = lte_lc_init();
      if (err) {
         if (err == -EFAULT) {
            LOG_WRN("Modem initialization failed, AT cmd failed!");
         } else if (err == -EALREADY) {
            LOG_WRN("Modem initialization failed, already initialized");
         } else {
            LOG_WRN("Modem initialization failed, error: %d", err);
         }
         return err;
      }
      initialized = true;

      LOG_INF("modem initialized");
   }
   if (initialized) {
      if (wakeup_handler) {
         s_wakeup_handler = wakeup_handler;
      }
      if (state_handler) {
         s_lte_state_change_handler = state_handler;
      }
   }
   return err;
}

int modem_wait_ready(const k_timeout_t timeout)
{
   int err = 0;
   k_timeout_t time = K_MSEC(0);
   k_timeout_t interval = K_MSEC(1500);
   int led_on = 1;
   while (!lte_ready_wait(interval)) {
      led_on = !led_on;
      if (led_on) {
         ui_led_op(LED_COLOR_BLUE, LED_SET);
         ui_led_op(LED_COLOR_RED, LED_SET);
      } else {
         ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
         ui_led_op(LED_COLOR_RED, LED_CLEAR);
      }
      time.ticks += interval.ticks;
      if ((time.ticks - timeout.ticks) > 0) {
         err = -1;
         break;
      }
   }
   return err;
}

int modem_start(const k_timeout_t timeout)
{
   int err = 0;
   int64_t time;
   char buf[32];

   if (lte_force_nb_iot) {
      LOG_INF("NB-IoT (config button 1)");
   } else if (lte_force_lte_m) {
      LOG_INF("LTE-M (call button)");
   } else {
#ifdef CONFIG_LTE_NETWORK_MODE_NBIOT
      LOG_INF("NB-IoT");
#elif CONFIG_LTE_NETWORK_MODE_LTE_M
      LOG_INF("LTE-M");
#elif CONFIG_LTE_MODE_PREFERENCE_LTE_M
      LOG_INF("LTE-M preference.");
#elif CONFIG_LTE_MODE_PREFERENCE_NBIOT
      LOG_INF("NB-IoT preference.");
#elif CONFIG_LTE_MODE_PREFERENCE_LTE_M_PLMN_PRIO
      LOG_INF("LTE-M PLMN preference.");
#elif CONFIG_LTE_MODE_PREFERENCE_NBIOT_PLMN_PRIO
      LOG_INF("NB-IoT PLMN preference.");
#endif
   }
   memset(&network_info, 0, sizeof(network_info));
   memset(&imsi, 0, sizeof(imsi));
   memset(&iccid, 0, sizeof(iccid));

#ifdef CONFIG_UDP_AS_RAI_ENABLE
   /** Release Assistance Indication  */
   err = modem_at_cmd("AT%%RAI=1", buf, sizeof(buf), "%RAI: ");
   if (err < 0) {
      LOG_WRN("Failed to enable access stratum RAI, err %d", err);
   }
#else
   err = modem_at_cmd("AT%%XRAI=0", buf, sizeof(buf), "%XRAI: ");
   if (err < 0) {
      LOG_WRN("Failed to disable control plane RAI, err %d", err);
   }
#endif

   ui_led_op(LED_COLOR_BLUE, LED_SET);
   ui_led_op(LED_COLOR_RED, LED_SET);

   err = modem_connect();
   if (!err) {
      time = k_uptime_get();
      err = modem_wait_ready(timeout);
      time = k_uptime_get() - time;
      if (!err) {
         LOG_INF("LTE attached in %ld [ms]", (long)time);

#if CONFIG_MODEM_SAVE_CONFIG_THRESHOLD > 0
#if CONFIG_MODEM_SAVE_CONFIG_THRESHOLD == 1
         if (1) {
#else
         if (time > CONFIG_MODEM_SAVE_CONFIG_THRESHOLD * MSEC_PER_SEC) {
#endif
            LOG_INF("Modem saving ...");
            lte_lc_power_off();
            lte_lc_normal();
            LOG_INF("Modem saved.");
            err = modem_wait_ready(timeout);
         } else {
            LOG_INF("Modem not saved.");
         }
#endif
      }
      ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
      ui_led_op(LED_COLOR_RED, LED_CLEAR);
#if 0
      {
         char buf[64];

         err = modem_at_cmd("AT%%PERIODICSEARCHCONF=1", buf, sizeof(buf), "%PERIODICSEARCHCONF: ");
         if (err < 0) {
            LOG_WRN("Failed to read PERIODICSEARCHCONF.");
         } else {
            LOG_INF("search-conf: '%s'", buf);
         }
      }
#endif
   }
   return err;
}

const char *modem_get_network_mode(void)
{
   return network_mode;
}

int modem_get_edrx_status(struct lte_lc_edrx_cfg *edrx)
{
   enum lte_lc_lte_mode mode;
   k_mutex_lock(&lte_mutex, K_FOREVER);
   mode = edrx_status.mode;
   if (edrx) {
      *edrx = edrx_status;
   }
   k_mutex_unlock(&lte_mutex);
#ifdef CONFIG_UDP_EDRX_ENABLE
   return 0;
#else
   return LTE_LC_LTE_MODE_NONE == mode ? -ENODATA : 0;
#endif
}

int modem_get_psm_status(struct lte_lc_psm_cfg *psm)
{
   int active_time;
   k_mutex_lock(&lte_mutex, K_FOREVER);
   active_time = psm_status.active_time;
   if (psm) {
      *psm = psm_status;
   }
   k_mutex_unlock(&lte_mutex);
#ifdef CONFIG_UDP_PSM_ENABLE
   return 0;
#else
   return active_time < 0 ? -ENODATA : 0;
#endif
}

int modem_get_release_time(void)
{
   return rai_time;
}

int modem_get_network_info(struct lte_network_info *info)
{
   if (info) {
      k_mutex_lock(&lte_mutex, K_FOREVER);
      *info = network_info;
      info->plmn_lock = lte_plmn_lock;
      k_mutex_unlock(&lte_mutex);
   }
   return 0;
}

int modem_get_imsi(char *buf, size_t len)
{
   int result = 0;
   k_mutex_lock(&lte_mutex, K_FOREVER);
   if (buf) {
      strncpy(buf, imsi, len);
   }
   result = strlen(imsi);
   k_mutex_unlock(&lte_mutex);
   return result;
}

int modem_get_imei(char *buf, size_t len)
{
   if (buf) {
      strncpy(buf, imei, len);
   }
   return strlen(imei);
}

int modem_get_iccid(char *buf, size_t len)
{
   int result = 0;
   k_mutex_lock(&lte_mutex, K_FOREVER);
   if (buf) {
      strncpy(buf, iccid, len);
   }
   result = strlen(iccid);
   k_mutex_unlock(&lte_mutex);
   return result;
}

void modem_set_transmission_time(void)
{
   int64_t now = k_uptime_get();
   k_mutex_lock(&lte_mutex, K_FOREVER);
   transmission_time = now;
   k_mutex_unlock(&lte_mutex);
}

int modem_read_network_info(struct lte_network_info *info)
{
   char buf[96];
   struct lte_network_info temp;
   long value;
   const char *cur = buf;
   char *t = NULL;

   if (!modem_at_cmd("AT%%XMONITOR", buf, sizeof(buf), "%XMONITOR: ")) {
      return -ENODATA;
   }
   LOG_INF("XMONITOR: %s", buf);

   memset(&temp, 0, sizeof(temp));

   switch ((int)strtol(cur, &t, 10)) {
      case 0:
         temp.reg_status = "not registered";
         break;
      case 1:
         temp.reg_status = "home";
         temp.registered = true;
         break;
      case 2:
         temp.reg_status = "searching";
         break;
      case 3:
         temp.reg_status = "denied";
         break;
      case 5:
         temp.reg_status = "roaming";
         temp.registered = true;
         break;
      case 91:
         temp.reg_status = "not registered (UICC)";
         break;
      default:
         temp.reg_status = "unknown";
         break;
   }
   if (temp.registered && *t == ',') {
      cur = t + 1;
      // skip 2 parameter "...", find start of 3th parameter.
      cur = parse_next_chars(cur, '"', 5);
      if (cur) {
         // copy 5 character plmn
         cur += parse_strncpy(temp.provider, cur, '"', sizeof(temp.provider));
         // skip  ," find start of next parameter
         cur = parse_next_chars(cur, '"', 1);
      }
      if (cur) {
         // 4 character tac
         value = strtol(cur, &t, 16);
         if (cur != t && 0 <= value && value < 0x10000) {
            temp.tac = (uint16_t)value;
         }
         // skip parameter by ,
         cur = parse_next_chars(t, ',', 2);
      }
      if (cur) {
         temp.band = (int)strtol(cur, &t, 10);
         if (cur == t) {
            temp.band = 0;
         }
      }
      if (cur && *t == ',') {
         // skip ,"
         cur = t + 2;
         // copy 8 character cell
         value = strtol(cur, &t, 16);
         if (cur != t) {
            temp.cell = (uint32_t)value;
         }
         // skip 3 parameter by ,
         cur = parse_next_chars(t, ',', 3);
         if (cur) {
            temp.rsrp = (int)strtol(cur, &t, 10) - 140;
            if (cur == t) {
               temp.rsrp = 0;
            }
         }
      }
   }

   k_mutex_lock(&lte_mutex, K_FOREVER);
   if (network_info.cell != temp.cell || network_info.tac != temp.tac) {
      lte_cell_updates++;
   }
   strncpy(temp.apn, network_info.apn, sizeof(temp.apn));
   strncpy(temp.local_ip, network_info.local_ip, sizeof(temp.local_ip));
   network_info = temp;
   k_mutex_unlock(&lte_mutex);
   if (info) {
      *info = temp;
   }

   return 0;
}

int modem_read_pdn_info(char *info, size_t len)
{
   char buf[64];
   char apn[16];
   char ip[16];
   const char *cur = buf;

   if (!modem_at_cmd("AT+CGDCONT?", buf, sizeof(buf), "+CGDCONT: ")) {
      LOG_INF("Failed to read CGDCONT.");
      return -ENODATA;
   } else {
      memset(&apn, 0, sizeof(apn));
      memset(&ip, 0, sizeof(ip));
      // CGDCONT: 0,"IP","iot.1nce.net","10.223.63.3",0,0
      LOG_INF("CGDCONT: %s", buf);
      if (info) {
         strncpy(info, buf, len);
      }
      // skip 1 parameter "...", find start of 3th parameter.
      cur = parse_next_chars(cur, '"', 3);
      if (cur) {
         // copy apn
         cur += parse_strncpy(apn, cur, '"', sizeof(apn));
      }
      // skip 1 ", find start of 4th parameter.
      cur = parse_next_chars(cur, '"', 1);
      if (cur) {
         // copy ip
         cur += parse_strncpy(ip, cur, '"', sizeof(ip));
      }
      k_mutex_lock(&lte_mutex, K_FOREVER);
      strncpy(network_info.apn, apn, sizeof(network_info.apn));
      strncpy(network_info.local_ip, ip, sizeof(network_info.local_ip));
      k_mutex_unlock(&lte_mutex);
      return strlen(buf);
   }
}

int modem_read_statistic(struct lte_network_statistic *statistic)
{
   int err;
   char buf[64];

   memset(statistic, 0, sizeof(struct lte_network_statistic));
   err = modem_at_cmd("AT%%XCONNSTAT?", buf, sizeof(buf), "%XCONNSTAT: ");
   if (err > 0) {
      LOG_INF("%s", buf);
      err = sscanf(buf, " %*u,%*u,%u,%u,%hu,%hu",
                   &statistic->transmitted,
                   &statistic->received,
                   &statistic->max_packet_size,
                   &statistic->average_packet_size);
      if (err == 4) {
         k_mutex_lock(&lte_mutex, K_FOREVER);
         statistic->searchs = lte_searchs;
         statistic->psm_delays = lte_psm_delays;
         statistic->cell_updates = lte_cell_updates;
         k_mutex_unlock(&lte_mutex);
      }
   }
   return err;
}

int modem_at_cmd(const char *cmd, char *buf, size_t max_len, const char *skip)
{
   int err;
   char at_buf[128];

   if (buf) {
      memset(buf, 0, max_len);
   }
   err = nrf_modem_at_cmd(at_buf, sizeof(at_buf), cmd);
   if (err < 0) {
      return err;
   } else if (err > 0) {
      int error = nrf_modem_at_err(err);
      const char *type = "AT ERROR";
      switch (nrf_modem_at_err_type(err)) {
         case NRF_MODEM_AT_CME_ERROR:
            type = "AT CME ERROR";
            break;
         case NRF_MODEM_AT_CMS_ERROR:
            type = "AT CMS ERROR";
            break;
         case NRF_MODEM_AT_ERROR:
         default:
            break;
      }
      LOG_WRN("%s: %d", type, error);
      return -error;
   }
   terminate_at_buffer(at_buf, sizeof(at_buf));
   if (buf) {
      if (skip && strncmp(at_buf, skip, strlen(skip)) == 0) {
         strncpy(buf, at_buf + strlen(skip), max_len - 1);
      } else {
         strncpy(buf, at_buf, max_len - 1);
      }
   } else {
      buf = at_buf;
   }
   return strlen(buf);
}

//#define USE_SO_RAI_NO_DATA

int modem_set_rai_mode(enum rai_mode mode, int socket)
{
   int err = 0;
#ifdef CONFIG_UDP_RAI_ENABLE
   if (rai_current_mode != mode) {
      /** Control Plane Release Assistance Indication  */
      if (mode == RAI_LAST) {
         err = modem_at_cmd("AT%%XRAI=0", NULL, 0, NULL);
         if (err < 0) {
            LOG_WRN("Disable RAI error: %d", err);
         } else {
            rai_current_mode = mode;
         }
      } else if (mode == RAI_ONE_RESPONSE) {
         err = modem_at_cmd("AT%%XRAI=3", NULL, 0, NULL);
         if (err < 0) {
            LOG_WRN("Enable RAI error: %d", err);
         } else {
            rai_current_mode = mode;
         }
      } else if (mode == RAI_LAST) {
         err = modem_at_cmd("AT%%XRAI=4", NULL, 0, NULL);
         if (err < 0) {
            LOG_WRN("Enable RAI error: %d", err);
         } else {
            rai_current_mode = mode;
         }
      }
   }
#elif CONFIG_UDP_AS_RAI_ENABLE
   int option = -1;
#ifdef USE_SO_RAI_NO_DATA
   switch (mode) {
      case RAI_NOW:
         option = SO_RAI_NO_DATA;
         break;
      case RAI_OFF:
         if (rai_current_mode != SO_RAI_ONGOING) {
            option = SO_RAI_ONGOING;
         }
         break;
      case RAI_LAST:
      case RAI_ONE_RESPONSE:
      default:
         break;
   }
#else
   switch (mode) {
      case RAI_NOW:
         break;
      case RAI_LAST:
         option = SO_RAI_LAST;
         break;
      case RAI_ONE_RESPONSE:
         option = SO_RAI_ONE_RESP;
         break;
      case RAI_OFF:
      default:
         if (rai_current_mode != SO_RAI_ONGOING) {
            option = SO_RAI_ONGOING;
         }
         break;
   }
#endif
   if (option >= 0) {
      err = setsockopt(socket, SOL_SOCKET, option, NULL, 0);
      if (err) {
         LOG_WRN("RAI sockopt %d, error %d", option, errno);
      } else {
         rai_current_mode = option;
      }
   }
#else
   (void)mode;
#endif
   return err;
}

int modem_set_offline(void)
{
   return lte_lc_offline();
}

int modem_set_normal(void)
{
   return lte_lc_normal();
}

int modem_set_lte_offline(void)
{
   return lte_lc_func_mode_set(LTE_LC_FUNC_MODE_DEACTIVATE_LTE) ? -EFAULT : 0;
}

int modem_power_off(void)
{
   return lte_lc_power_off();
}

#else

int modem_init(int config, wakeup_callback_handler_t wakeup_handler, lte_state_change_callback_handler_t state_handler)
{
   (void)config;
   (void)wakeup_handler;
   (void)connect_handler;
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

const char *modem_get_network_mode(void)
{
   return "n.a.";
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

int modem_get_imsi(char *buf, size_t len)
{
   (void)buf;
   (void)len;
   return 0;
}

int modem_get_imei(char *buf, size_t len)
{
   (void)buf;
   (void)len;
   return 0;
}

int modem_get_iccid(char *buf, size_t len)
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

int modem_get_release_time(void)
{
   return -1;
}

void modem_set_transmission_time(void)
{
}

int modem_at_cmd(const char *cmd, char *buf, size_t max_len, const char *skip)
{
   (void)cmd;
   (void)buf;
   (void)max_len;
   (void)skip;
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

#endif
