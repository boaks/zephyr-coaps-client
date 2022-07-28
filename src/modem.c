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
#include <modem/modem_key_mgmt.h>
#include <modem/nrf_modem_lib.h>
#include <nrf_modem_at.h>
#include <stdio.h>
#include <string.h>
#include <zephyr.h>

#include "dtls_client.h"
#include "modem.h"
#include "ui.h"

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

#if defined(CONFIG_NRF_MODEM_LIB)

static K_MUTEX_DEFINE(lte_mutex);
static K_CONDVAR_DEFINE(lte_condvar);

static K_SEM_DEFINE(ptau_update, 0, 1);

static wakeup_callback_handler_t wakeup_handler = NULL;
static bool initialized = 0;
static bool start_connect = 0;

static volatile int lte_connected_state = 0;

static const char *volatile network_mode = "init";

static struct lte_lc_edrx_cfg edrx_status = {LTE_LC_LTE_MODE_NONE, 0.0, 0.0};
static struct lte_lc_psm_cfg psm_status = {0, 0};
static unsigned long transmission_time = 0;

static volatile int rai_time = -1;

static void lte_connection_set(int connected)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   lte_connected_state = connected;
   if (connected) {
      k_condvar_broadcast(&lte_condvar);
   }
   k_mutex_unlock(&lte_mutex);
}

static int lte_connection_wait(k_timeout_t timeout)
{
   int res = 0;
   k_mutex_lock(&lte_mutex, timeout);
   res = lte_connected_state;
   if (!res) {
      k_condvar_wait(&lte_condvar, &lte_mutex, timeout);
      res = lte_connected_state;
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

static int64_t get_transmission_time(void)
{
   int64_t time;
   k_mutex_lock(&lte_mutex, K_FOREVER);
   time = transmission_time;
   k_mutex_unlock(&lte_mutex);
   return time;
}

static void lte_registration(enum lte_lc_nw_reg_status reg_status)
{
   const char *description = "unknown";
   int connected = 0;

   switch (reg_status) {
      case LTE_LC_NW_REG_NOT_REGISTERED:
         description = "Not Connected";
         break;
      case LTE_LC_NW_REG_REGISTERED_HOME:
         description = "Connected - home network";
         connected = 1;
         break;
      case LTE_LC_NW_REG_SEARCHING:
         description = "Searching ...";
         break;
      case LTE_LC_NW_REG_REGISTRATION_DENIED:
         description = "Not Connected - denied";
         break;
      case LTE_LC_NW_REG_UNKNOWN:
         break;
      case LTE_LC_NW_REG_REGISTERED_ROAMING:
         description = "Connected - roaming network";
         connected = 1;
         break;
      case LTE_LC_NW_REG_REGISTERED_EMERGENCY:
         description = "Connected - emergency network";
         connected = 1;
         break;
      case LTE_LC_NW_REG_UICC_FAIL:
         description = "Not Connected - UICC fail";
         break;
   }

   lte_connection_set(connected);
   dtls_lte_connected(LTE_CONNECT_NETWORK, connected);

   LOG_INF("Network registration status: %s", description);
}

static void lte_handler(const struct lte_lc_evt *const evt)
{
   static int64_t connect_time = 0;
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
         if (evt->psm_cfg.active_time > 0) {
            lte_set_psm_status(&evt->psm_cfg);
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
            if (evt->edrx_cfg.mode != LTE_LC_LTE_MODE_NONE) {
               lte_set_edrx_status(&evt->edrx_cfg);
            }
            break;
         }
      case LTE_LC_EVT_RRC_UPDATE:
         {
            int64_t now = k_uptime_get();
            if (evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED) {
               connect_time = now;
               LOG_INF("RRC mode: Connected");
               dtls_lte_connected(LTE_CONNECT_TRANSMISSION, 1);
            } else {
               int64_t time = get_transmission_time();
               if ((time - connect_time) > 0) {
                  rai_time = (int)(now - time);
                  LOG_INF("RRC mode: Idle after %lld ms (%d ms inactivity)", now - connect_time, rai_time);
                  dtls_lte_connected(LTE_CONNECT_TRANSMISSION, 0);
               } else {
                  rai_time = -1;
                  LOG_INF("RRC mode: Idle after %lld ms", now - connect_time);
                  dtls_lte_connected(LTE_CONNECT_TRANSMISSION, 0);
               }
            }
            break;
         }
      case LTE_LC_EVT_TAU_PRE_WARNING:
         LOG_INF("LTE Tracking area Update");
         break;
      case LTE_LC_EVT_CELL_UPDATE:
         LOG_INF("LTE cell changed: Cell ID: %d, Tracking area: %d",
                 evt->cell.id, evt->cell.tac);
         break;
      case LTE_LC_EVT_MODEM_SLEEP_ENTER:
         LOG_INF("LTE modem sleeps");
         break;
      case LTE_LC_EVT_MODEM_SLEEP_EXIT:
         LOG_INF("LTE modem wakes up");
         if (wakeup_handler) {
            wakeup_handler();
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
   } else if (!start_connect) {
      err = lte_lc_connect_async(lte_handler);
      if (err) {
         LOG_WRN("Connecting to LTE network failed, error: %d",
                 err);
      } else {
         start_connect = true;
      }
   }
   return err;
}

int modem_init(wakeup_callback_handler_t handler)
{
   int err = 0;

   if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT)) {
      /* Do nothing, modem is already configured and LTE connected. */
   } else if (!initialized) {
      char buf[32];
      nrf_modem_lib_init(NORMAL_MODE);

      err = modem_at_cmd("AT%%HWVERSION", buf, sizeof(buf), "%HWVERSION: ");
      if (err > 0) {
         LOG_INF("hw: %s", buf);
      }
      err = modem_at_cmd("AT+CGMR", buf, sizeof(buf), NULL);
      if (err > 0) {
         LOG_INF("rev: %s", buf);
      }
#if 0
      err = modem_at_cmd("AT%%XFACTORYRESET=0", buf, sizeof(buf), NULL);
      if (err > 0) {
         LOG_INF("Factory reset: %s", buf);
      }
#endif

#if 0
      err = modem_at_cmd("AT%%REL14FEAT=0,1,0,0,0", buf, sizeof(buf), NULL);
      if (err > 0) {
         LOG_INF("rel14feat: %s", buf);
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
      wakeup_handler = handler;
      LOG_INF("modem initialized");
   }

   return err;
}

int modem_start(const k_timeout_t timeout)
{
   int err = 0;
   k_timeout_t interval = K_MSEC(1500);
   k_timeout_t save = K_SECONDS(60);
   k_timeout_t time = K_MSEC(0);

#ifdef CONFIG_LTE_MODE_PREFERENCE_LTE_M
   LOG_INF("LTE-M preference.");
#elif CONFIG_LTE_MODE_PREFERENCE_NBIOT
   LOG_INF("NB-IoT preference.");
#elif CONFIG_LTE_MODE_PREFERENCE_LTE_M_PLMN_PRIO
   LOG_INF("LTE-M PLMN preference.");
#elif CONFIG_LTE_MODE_PREFERENCE_NBIOT_PLMN_PRIO
   LOG_INF("NB-IoT PLMN preference.");
#endif

   ui_led_op(LED_COLOR_BLUE, LED_SET);
   ui_led_op(LED_COLOR_RED, LED_SET);

   /* Initialize the modem before calling configure_low_power(). This is
    * because the enabling of RAI is dependent on the
    * configured network mode which is set during modem initialization.
    */
   if (!err) {
      err = modem_connect();
   }
   if (!err) {
      int led_on = 1;
      while (!lte_connection_wait(interval)) {
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
            err = 1;
            break;
         }
      }
      if ((time.ticks - save.ticks) > 0) {
         LOG_INF("Modem save ...");
         modem_power_off();
         modem_set_normal();
      }
      ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
      ui_led_op(LED_COLOR_RED, LED_CLEAR);
   }
   return err;
}

const char *modem_get_network_mode(void)
{
   return network_mode;
}

int modem_get_edrx_status(struct lte_lc_edrx_cfg *edrx)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   *edrx = edrx_status;
   k_mutex_unlock(&lte_mutex);
   return (edrx->mode != LTE_LC_LTE_MODE_NONE) ? 0 : -ENODATA;
}

int modem_get_psm_status(struct lte_lc_psm_cfg *psm)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   *psm = psm_status;
   k_mutex_unlock(&lte_mutex);
   return (psm->tau > 0) ? 0 : -ENODATA;
}

int modem_get_release_time(void)
{
   return rai_time;
}

void modem_set_transmission_time(void)
{
   int64_t now = k_uptime_get();
   k_mutex_lock(&lte_mutex, K_FOREVER);
   transmission_time = now;
   k_mutex_unlock(&lte_mutex);
}

int modem_at_cmd(const char *cmd, char *buf, size_t max_len, const char *skip)
{
   int err;
   char at_buf[128];
   memset(buf, 0, max_len);
   err = nrf_modem_at_cmd(at_buf, sizeof(at_buf), cmd);
   if (err) {
      return err;
   }
   terminate_at_buffer(at_buf, sizeof(at_buf));
   if (skip && strncmp(at_buf, skip, strlen(skip)) == 0) {
      strncpy(buf, at_buf + strlen(skip), max_len - 1);
   } else {
      strncpy(buf, at_buf, max_len - 1);
   }
   return strlen(buf);
}

int modem_set_power_modes(int enable)
{
   int err = 0;
   char buf[128];

   if (enable) {
#if defined(CONFIG_UDP_PSM_ENABLE)
      /** Power Saving Mode */
      err = lte_lc_psm_req(true);
      if (err) {
         LOG_WRN("lte_lc_psm_req, error: %d", err);
      } else {
         if (k_sem_take(&ptau_update, K_SECONDS(10)) == 0) {
#if 1
            if (modem_at_cmd("AT+CPSMS?", buf, sizeof(buf), "+CPSMS:") > 0) {
               LOG_INF("CPSMS >> %s", buf);
            }
#endif
            if (modem_at_cmd("AT%%XMONITOR", buf, sizeof(buf), "%XMONITOR:") > 0) {
               LOG_INF("MON >> %s", buf);
            }
#if 1
            if (modem_at_cmd("AT+CEREG?", buf, sizeof(buf), "+CEREG:") > 0) {
               LOG_INF("CEREG >> %s", buf);
            }
#endif
#if 1
            if (modem_at_cmd("AT+CPSMS=0", buf, sizeof(buf), NULL) > 0) {
               LOG_INF("CPSMS=0 >> %s", buf);
            }
            if (modem_at_cmd("AT+CPSMS=1,,,\"00100100\",\"00000010\"", buf, sizeof(buf), NULL) > 0) {
               LOG_INF("CPSMS=1 >> %s", buf);
            }
#endif
         }
         LOG_INF("lte_lc_psm_req, enabled. %d s", psm_status.tau);
      }
#endif
#if defined(CONFIG_UDP_RAI_ENABLE)
      /** Release Assistance Indication  */
      if (modem_at_cmd("AT%%XRAI=" CONFIG_LTE_RAI_REQ_VALUE, buf, sizeof(buf), NULL) > 0) {
         LOG_INF(">> RAI: %s", buf);
      }
#endif
   } else {
      err = lte_lc_psm_req(false);
      if (err) {
         LOG_WRN("lte_lc_psm_req, error: %d", err);
      } else {
         LOG_INF("lte_lc_psm_req, disabled.");
      }
      err = lte_lc_rai_req(false);
      if (err) {
         LOG_WRN("lte_lc_rai_req, error: %d", err);
      } else {
         LOG_INF("lte_lc_rai_req, disabled.");
      }
   }
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

int modem_power_off(void)
{
   return lte_lc_power_off();
}

#else

int modem_init(wakeup_callback_handler_t handler)
{
   return 0;
}

int modem_start(int init)
{
   (void)init;
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

int modem_get_psm_status(struct lte_lc_psm_cfg *psm)
{
   (void)psm;
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

int modem_set_power_modes(int enable)
{
   (void)enable;
   return 0;
}

int modem_set_offline(void)
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
