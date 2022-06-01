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

#include <logging/log.h>
#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <nrf_modem_at.h>
#include <stdio.h>
#include <string.h>
#include <zephyr.h>

#include "dtls_client.h"
#include "modem.h"
#include "ui.h"

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

K_SEM_DEFINE(lte_connected, 0, 1);

#if defined(CONFIG_NRF_MODEM_LIB)

static char at_buf[128];
static volatile int lte_connected_state = 0;

static void lte_handler(const struct lte_lc_evt *const evt)
{
   static unsigned long connect_time = 0;
   switch (evt->type) {
      case LTE_LC_EVT_NW_REG_STATUS:
         if ((evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_HOME) &&
             (evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_ROAMING)) {
            if (evt->nw_reg_status == LTE_LC_NW_REG_SEARCHING) {
               LOG_INF("Network registration status: searching ...");
            }
            lte_connected_state = 0;
            k_sem_take(&lte_connected, K_NO_WAIT);
            dtls_lte_connected(LTE_CONNECT_NETWORK, 0);
            break;
         }

         LOG_INF("Network registration status: %s",
                 evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ? "Connected - home network" : "Connected - roaming");
         lte_connected_state = 1;
         dtls_lte_connected(LTE_CONNECT_NETWORK, 1);
         k_sem_give(&lte_connected);
         break;
      case LTE_LC_EVT_LTE_MODE_UPDATE:
         if (evt->lte_mode == LTE_LC_LTE_MODE_NONE) {
            LOG_INF("LTE Mode: none");
         } else if (evt->lte_mode == LTE_LC_LTE_MODE_LTEM) {
            LOG_INF("LTE Mode: CAT-M1");
         } else if (evt->lte_mode == LTE_LC_LTE_MODE_NBIOT) {
            LOG_INF("LTE Mode: CAT-NB");
         }
         break;
      case LTE_LC_EVT_PSM_UPDATE:
         LOG_INF("PSM parameter update: TAU: %d, Active time: %d",
                 evt->psm_cfg.tau, evt->psm_cfg.active_time);
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
            break;
         }
      case LTE_LC_EVT_RRC_UPDATE:
         {
            unsigned long now = (unsigned long)k_uptime_get();
            if (evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED) {
               connect_time = now;
               LOG_INF("RRC mode: Connected");
               dtls_lte_connected(LTE_CONNECT_TRANSMISSION, 1);
            } else {
               LOG_INF("RRC mode: Idle after %ld ms", now - connect_time);
               dtls_lte_connected(LTE_CONNECT_TRANSMISSION, 0);
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
         break;
      case LTE_LC_EVT_MODEM_EVENT:
         if (evt->modem_evt == LTE_LC_MODEM_EVT_BATTERY_LOW) {
            LOG_INF("LTE modem Battery Low!");
         } else if (evt->modem_evt == LTE_LC_MODEM_EVT_OVERHEATED) {
            LOG_INF("LTE modem Overheated!");
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

static int modem_init(void)
{
   int err = 0;
   static const char header[] = "%HWVERSION: ";

   if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT)) {
      /* Do nothing, modem is already configured and LTE connected. */
   } else {
      nrf_modem_lib_init(NORMAL_MODE);

      err = modem_at_cmd("AT%%HWVERSION", at_buf, sizeof(at_buf));
      if (err > 0) {
         if (strncmp(at_buf, header, sizeof(header) - 1) == 0) {
            LOG_INF("hw: %s", at_buf + sizeof(header) - 1);
         } else {
            LOG_INF("hw: %s", at_buf);
         }
      }
      err = modem_at_cmd("AT+CGMR", at_buf, sizeof(at_buf));
      if (err > 0) {
         LOG_INF("R: %s", at_buf);
      }

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
      LOG_INF("modem initialized");
   }

   return err;
}

static int modem_connect(void)
{
   int err = 0;

   if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT)) {
      /* Do nothing, modem is already configured and LTE connected. */
   } else {
      err = lte_lc_connect_async(lte_handler);
      if (err) {
         LOG_WRN("Connecting to LTE network failed, error: %d",
                 err);
      }
   }
   return err;
}

int modem_start(int init)
{
   int err = 0;
   int count = 0;

   ui_led_op(LED_COLOR_BLUE, LED_SET);
   ui_led_op(LED_COLOR_RED, LED_SET);

   /* Initialize the modem before calling configure_low_power(). This is
    * because the enabling of RAI is dependent on the
    * configured network mode which is set during modem initialization.
    */
   if (init & 1) {
      err = modem_init();
   }

   if (!err && init & 2) {
      err = modem_connect();
   }

   if (!err && !lte_connected_state) {

      while (k_sem_take(&lte_connected, K_MSEC(1500))) {
         ui_led_op(LED_COLOR_BLUE, LED_TOGGLE);
         ui_led_op(LED_COLOR_RED, LED_TOGGLE);
         ++count;
         if (count > 100) {
            err = 1;
            break;
         }
      }

      ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
      ui_led_op(LED_COLOR_RED, LED_CLEAR);
   }
   return err;
}

void modem_set_power_modes(int enable)
{
   int err = 0;

   if (enable) {
#if defined(CONFIG_UDP_PSM_ENABLE)
      /** Power Saving Mode */
      err = lte_lc_psm_req(true);
      if (err) {
         LOG_WRN("lte_lc_psm_req, error: %d", err);
      } else {
         LOG_INF("lte_lc_psm_req, enabled.");
      }
#endif
#if defined(CONFIG_UDP_RAI_ENABLE)
      /** Release Assistance Indication  */
      err = lte_lc_rai_req(true);
      if (err) {
         LOG_WRN("lte_lc_rai_req, error: %d", err);
      } else {
         LOG_INF("lte_lc_rai_req, enabled.");
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
}

int modem_at_cmd(const char *cmd, char *buf, size_t max_len)
{
   int err;
   char at_buf[128];
   memset(buf, 0, max_len);
   err = nrf_modem_at_cmd(at_buf, sizeof(at_buf), cmd);
   if (err) {
      return err;
   }
   terminate_at_buffer(at_buf, sizeof(at_buf));
   strncpy(buf, at_buf, max_len - 1);
   return strlen(buf);
}

#else

int modem_start(int init)
{
   (void)init;
   return 0;
}

void modem_set_power_modes(int enable)
{
   (void)enable;
}

int modem_at_cmd(const char *cmd, char *buf, size_t max_len)
{
   (void)cmd;
   (void)buf;
   (void)max_len;
   return 0;
}

#endif
