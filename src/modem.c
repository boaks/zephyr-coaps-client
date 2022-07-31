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
#include <nrf_modem_at.h>
#include <stdio.h>
#include <string.h>
#include <zephyr.h>

#include "modem.h"
#include "ui.h"

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

#if defined(CONFIG_NRF_MODEM_LIB)

static K_MUTEX_DEFINE(lte_mutex);
static K_CONDVAR_DEFINE(lte_condvar);

static K_SEM_DEFINE(ptau_update, 0, 1);

static wakeup_callback_handler_t s_wakeup_handler = NULL;
static connect_callback_handler_t s_connect_handler = NULL;
static bool initialized = 0;

static volatile int lte_connected_state = 0;

static const char *volatile network_mode = "init";

static struct lte_lc_edrx_cfg edrx_status = {LTE_LC_LTE_MODE_NONE, 0.0, 0.0};
static struct lte_lc_psm_cfg psm_status = {0, 0};
static uint32_t psm_delays = 0;
static bool plmn_lock = false;
static char provider[12] = "00,00000(*)";
static int64_t transmission_time = 0;

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

static void lte_inc_psm_delays(void)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   ++psm_delays;
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

static const char *modem_next_char(const char *value, char sep)
{
   if (value) {
      while (*value && *value != sep) {
         ++value;
      }
      if (*value == sep) {
         ++value;
         if (*value) {
            return value;
         }
      }
   }
   return NULL;
}

static const char *modem_next_chars(const char *value, char sep, int count)
{
   for (int index = 0; index < count; ++index) {
      value = modem_next_char(value, sep);
   }
   return value;
}

static int modem_strncpy(char *buf, const char *value, char end, int size)
{
   int index;
   for (index = 0; index < size; ++index) {
      char cur = *value;
      if (!cur || cur == end) {
         break;
      }
      buf[index] = cur;
      value++;
   }
   return index;
}

static void modem_read_operator_info()
{
   int index;
   char buf[96];
   char temp[12];
   const char *cur = buf;
   char *t = temp;

   if (!modem_at_cmd("AT%%XMONITOR", buf, sizeof(buf), "%XMONITOR: ")) {
      return;
   }

   memset(temp, 0, sizeof(temp));

   index = modem_strncpy(t, cur, ',', 3);
   t += index;
   cur += index;
   if (*cur == ',') {
      *t++ = *cur++;
      // skip 2 parameter "...", find start of 3th parameter.
      cur = modem_next_chars(cur, '"', 5);
      if (cur) {
         t += modem_strncpy(t, cur, '"', 5);
      }
   }
   if (plmn_lock) {
      strcpy(t, "(#)");
   } else {
      strcpy(t, "(*)");
   }
   k_mutex_lock(&lte_mutex, K_FOREVER);
   strncpy(provider, temp, sizeof(temp));
   k_mutex_unlock(&lte_mutex);
   LOG_INF("PLMN: %s", provider);
}

static void lte_registration(enum lte_lc_nw_reg_status reg_status)
{
   const char *description = "unknown";
   bool connected = false;

   switch (reg_status) {
      case LTE_LC_NW_REG_NOT_REGISTERED:
         description = "Not Connected";
         break;
      case LTE_LC_NW_REG_REGISTERED_HOME:
         description = "Connected - home network";
         connected = true;
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
         connected = true;
         break;
      case LTE_LC_NW_REG_REGISTERED_EMERGENCY:
         description = "Connected - emergency network";
         connected = true;
         break;
      case LTE_LC_NW_REG_UICC_FAIL:
         description = "Not Connected - UICC fail";
         break;
   }

   lte_connection_set(connected);
   if (s_connect_handler) {
      s_connect_handler(LTE_CONNECT_NETWORK, connected);
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
         if (evt->psm_cfg.active_time >= 0) {
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
               if (s_connect_handler) {
                  s_connect_handler(LTE_CONNECT_TRANSMISSION, true);
               }
            } else {
               int64_t time = get_transmission_time();
               idle_time = now;
               if ((time - connect_time) > 0) {
                  rai_time = (int)(now - time);
                  LOG_INF("RRC mode: Idle after %lld ms (%d ms inactivity)", now - connect_time, rai_time);
               } else {
                  rai_time = -1;
                  LOG_INF("RRC mode: Idle after %lld ms", now - connect_time);
               }
               if (s_connect_handler) {
                  s_connect_handler(LTE_CONNECT_TRANSMISSION, false);
               }
            }
            break;
         }
      case LTE_LC_EVT_TAU_PRE_WARNING:
         LOG_INF("LTE Tracking area Update");
         break;
      case LTE_LC_EVT_CELL_UPDATE:
         LOG_INF("LTE cell changed: Cell ID: %d, Tracking area: %d", evt->cell.id, evt->cell.tac);
         break;
      case LTE_LC_EVT_MODEM_SLEEP_ENTER:
         if (idle_time) {
            int64_t time = (k_uptime_get() - idle_time);
            bool delayed = active_time >= 0 && ((time / MSEC_PER_SEC) > (active_time + 5));
            if (delayed) {
               lte_inc_psm_delays();
            }
            LOG_INF("LTE modem sleeps after %lld ms idle%s", time, delayed ? ", delaye" : "");
         } else {
            LOG_INF("LTE modem sleeps");
         }
         break;
      case LTE_LC_EVT_MODEM_SLEEP_EXIT:
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

int modem_init(wakeup_callback_handler_t wakeup_handler, connect_callback_handler_t connect_handler)
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
      LOG_INF("Factory reset: %s", buf);
      k_sleep(K_SECONDS(10));
#endif
#if 0
      err = modem_at_cmd("AT%%REL14FEAT=0,1,0,0,0", buf, sizeof(buf), NULL);
      if (err > 0) {
         LOG_INF("rel14feat: %s", buf);
      }
#endif
      int err = modem_at_cmd("AT%%XCONNSTAT=1", buf, sizeof(buf), NULL);
      if (err > 0) {
         LOG_INF("stat: %s", buf);
      }

#ifdef CONFIG_UDP_PSM_ENABLE
      err = lte_lc_psm_req(true);
      if (err) {
         if (err == -EFAULT) {
            LOG_WRN("Modem set power mode failed, AT cmd failed!");
         } else {
            LOG_WRN("Modem set power mode failed, error: %d!", err);
         }
      } else {
         err = modem_at_cmd("AT+CPSMS?", buf, sizeof(buf), "+CPSMS: ");
         if (err > 0) {
            LOG_INF("psm: %s", buf);
         }
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

      s_wakeup_handler = wakeup_handler;
      s_connect_handler = connect_handler;
      LOG_INF("modem initialized");
   }
   if (initialized) {
      if (wakeup_handler) {
         s_wakeup_handler = wakeup_handler;
      }
      if (connect_handler) {
         s_connect_handler = connect_handler;
      }
   }
   return err;
}

static int modem_connection_wait(const k_timeout_t timeout)
{
   int err = 0;
   k_timeout_t time = K_MSEC(0);
   k_timeout_t interval = K_MSEC(1500);
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
   return err;
}

int modem_start(const k_timeout_t timeout)
{
   int err = 0;
   int64_t time;

#ifdef CONFIG_LTE_MODE_PREFERENCE_LTE_M
   LOG_INF("LTE-M preference.");
#elif CONFIG_LTE_MODE_PREFERENCE_NBIOT
   LOG_INF("NB-IoT preference.");
#elif CONFIG_LTE_MODE_PREFERENCE_LTE_M_PLMN_PRIO
   LOG_INF("LTE-M PLMN preference.");
#elif CONFIG_LTE_MODE_PREFERENCE_NBIOT_PLMN_PRIO
   LOG_INF("NB-IoT PLMN preference.");
#endif

#ifdef CONFIG_LTE_LOCK_PLMN
   LOG_INF("Lock PLMN %s", CONFIG_LTE_LOCK_PLMN_STRING);
   plmn_lock = true;
#else
   LOG_INF("All PLMN");
   plmn_lock = false;
#endif

   ui_led_op(LED_COLOR_BLUE, LED_SET);
   ui_led_op(LED_COLOR_RED, LED_SET);

   err = modem_connect();
   if (!err) {
      time = k_uptime_get();
      err = modem_connection_wait(timeout);
      time = k_uptime_get() - time;
      if (!err) {
         LOG_INF("LTE connected in %ld [ms]", (long)time);
      }
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
         err = modem_connection_wait(timeout);
      } else {
         LOG_INF("Modem not saved.");
      }
#endif
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

int modem_get_psm_status(struct lte_lc_psm_cfg *psm, uint32_t *delays)
{
   int tau;
   k_mutex_lock(&lte_mutex, K_FOREVER);
   tau = psm_status.tau;
   if (psm) {
      *psm = psm_status;
   }
   if (delays) {
      *delays = psm_delays;
   }
   k_mutex_unlock(&lte_mutex);
   return (tau >= 0) ? 0 : -ENODATA;
}

int modem_get_release_time(void)
{
   return rai_time;
}

int modem_get_provider(char *buf, size_t len)
{
   k_mutex_lock(&lte_mutex, K_FOREVER);
   strncpy(buf, provider, len);
   k_mutex_unlock(&lte_mutex);
   return strlen(buf);
}

void modem_set_transmission_time(void)
{
   int64_t now = k_uptime_get();
   k_mutex_lock(&lte_mutex, K_FOREVER);
   transmission_time = now;
   k_mutex_unlock(&lte_mutex);
}

int modem_read_provider(char *buf, size_t len)
{
   modem_read_operator_info();
   return modem_get_provider(buf, len);
}

int modem_read_statistic(char *data, size_t len)
{
   int err;
   char buf[92];

   memset(data, 0, len);
   err = modem_at_cmd("AT%%XCONNSTAT?", buf, sizeof(buf), "%XCONNSTAT: ");
   if (err > 0) {
      const char *cur = modem_next_chars(buf, ',', 2);
      if (cur) {
         strncpy(data, cur, len);
      }
   }
   return strlen(data);
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

int modem_set_rai(int enable)
{
   int err = 0;

#ifdef CONFIG_UDP_RAI_ENABLE
   if (enable) {
      /** Release Assistance Indication  */
      err = lte_lc_rai_req(true);
      if (err) {
         LOG_WRN("lte_lc_rai_req, error: %d", err);
      } else {
         LOG_INF("lte_lc_rai_req, enabled.");
      }
   } else {
      err = lte_lc_rai_req(false);
      if (err) {
         LOG_WRN("lte_lc_rai_req disable, error: %d", err);
      } else {
         LOG_INF("lte_lc_rai_req, disabled.");
      }
   }
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

int modem_power_off(void)
{
   return lte_lc_power_off();
}

#else

int modem_init(wakeup_callback_handler_t wakeup_handler, connect_callback_handler_t connect_handler)
{
   (void)wakeup_handler;
   (void)connect_handler;
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

int modem_set_rai(int enable)
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
