/*
 * Copyright (c) 2023 Achim Kraus CloudCoap.net
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
#include <stdio.h>
#include <stdlib.h>
#include <zephyr/logging/log.h>

#include "parse.h"

#include "sh_cmd.h"

LOG_MODULE_DECLARE(MODEM, CONFIG_MODEM_LOG_LEVEL);

#if defined(CONFIG_NRF_MODEM_LIB)

#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <modem/pdn.h>
#include <nrf_modem_at.h>

#include "io_job_queue.h"
#include "modem.h"
#include "modem_at.h"
#include "modem_desc.h"

static void modem_logging_switching_off_fn(struct k_work *work)
{
   LOG_INF("Modem switching off ...");
}

static K_WORK_DELAYABLE_DEFINE(modem_logging_switching_off_work, modem_logging_switching_off_fn);

int modem_reinit(void);

static bool modem_is_plmn(const char *value)
{
   int len = 0;
   while (isdigit(0xff & value[len])) {
      ++len;
   }
   return !value[len] && 5 <= len && len <= 6;
}

static int previous_mode = -1;

static int modem_off(void)
{
   enum lte_lc_func_mode mode;
   int res = lte_lc_func_mode_get(&mode);
   previous_mode = -1;
   if (!res) {
      res = mode;
      previous_mode = mode;
      if (mode != LTE_LC_FUNC_MODE_POWER_OFF) {
         work_reschedule_for_io_queue(&modem_logging_switching_off_work, K_MSEC(5000));
         lte_lc_func_mode_set(LTE_LC_FUNC_MODE_POWER_OFF);
         k_work_cancel_delayable(&modem_logging_switching_off_work);
      }
   }
   return res;
}

static int modem_restore(void)
{
   int res = 0;
   if (-1 < previous_mode && previous_mode != LTE_LC_FUNC_MODE_POWER_OFF) {
      res = lte_lc_func_mode_set(previous_mode);
   }
   previous_mode = -1;
   return res;
}

#define CFG_NB_IOT "nb"
#define CFG_LTE_M "m1"

static int modem_cmd_config(const char *config)
{
   int res;
   char buf[32];
   char value1[7];
   char value2[5];
   char value3[3];
   const char *cur = config;

   memset(value1, 0, sizeof(value1));
   memset(value2, 0, sizeof(value2));
   memset(value3, 0, sizeof(value3));

   cur = parse_next_text(cur, ' ', value1, sizeof(value1));
   cur = parse_next_text(cur, ' ', value2, sizeof(value2));
   cur = parse_next_text(cur, ' ', value3, sizeof(value3));
   if (!value1[0]) {
      char mode = 0;
      char type = 0;
      char net_mode = 0;
      char plmn[16];
      const char *desc = "\?\?\?";
      enum lte_lc_system_mode lte_mode = LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS;
      enum lte_lc_system_mode_preference lte_preference = CONFIG_LTE_MODE_PREFERENCE_VALUE;

      res = lte_lc_system_mode_get(&lte_mode, &lte_preference);
      if (res) {
         LOG_INF("Can't read current LTE mode!");
         return res;
      }
      memset(plmn, 0, sizeof(plmn));
      res = modem_at_cmd(buf, sizeof(buf), "+COPS: ", "AT+COPS?");
      if (res > 0) {
         res = sscanf(buf, " %c,%c,%15[^,],%c",
                      &mode, &type, plmn, &net_mode);
         strtrunc(plmn, '"');
      }
      if (mode == '0') {
         desc = "auto";
      } else if (mode == '1') {
         desc = plmn;
      }
      LOG_INF("cfg %s %s", desc, modem_get_system_mode_cfg(lte_mode, lte_preference));
      desc = "none";
      if (net_mode == '7') {
         desc = CFG_LTE_M;
      } else if (net_mode == '9') {
         desc = CFG_NB_IOT;
      }
      LOG_INF("currently %s %s", plmn, desc);
      return 0;
   }
   if (!stricmp("init", value1)) {
      if (value2[0]) {
         LOG_INF("cfg %s", config);
         LOG_INF("No arguments %s are supported for 'init'", value2);
         return -EINVAL;
      }
      LOG_INF(">> cfg init");
      modem_off();
      modem_reinit();
      modem_restore();
      LOG_INF(">> cfg init ready");
      return 1;
   }
   if (stricmp("auto", value1) && !modem_is_plmn(value1)) {
      LOG_INF("cfg %s", config);
      LOG_INF("plmn '%s' not supported! Either 'auto' or numerical plmn.", value1);
      return -EINVAL;
   }
   if (value2[0] && stricmp(CFG_NB_IOT, value2) && stricmp(CFG_LTE_M, value2) && stricmp("auto", value2)) {
      LOG_INF("cfg %s", config);
      LOG_INF("mode '%s' not supported!", value2);
      return -EINVAL;
   }
   if (value3[0] && stricmp(CFG_NB_IOT, value3) && stricmp(CFG_LTE_M, value3)) {
      LOG_INF("cfg %s", config);
      LOG_INF("mode '%s' not supported!", value3);
      return -EINVAL;
   }
   if (!stricmp("auto", value2) && value3[0]) {
      LOG_INF("cfg %s", config);
      LOG_INF("second mode '%s' not supported with 'auto'!", value3);
      return -EINVAL;
   }
   LOG_INF(">> cfg %s %s %s", value1, value2, value3);
   if (value2[0]) {
      enum lte_lc_system_mode lte_mode = LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS;
      enum lte_lc_system_mode_preference lte_preference = CONFIG_LTE_MODE_PREFERENCE_VALUE;
      enum lte_lc_system_mode lte_mode_new;
      enum lte_lc_system_mode_preference lte_preference_new;
      bool gps;
      res = lte_lc_system_mode_get(&lte_mode, &lte_preference);
      if (res) {
         LOG_INF("Can't read current LTE mode!");
         return res;
      }
      lte_mode_new = lte_mode;
      lte_preference_new = lte_preference;
      gps = lte_mode == LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS ||
            lte_mode == LTE_LC_SYSTEM_MODE_LTEM_GPS ||
            lte_mode == LTE_LC_SYSTEM_MODE_NBIOT_GPS;
      if (!stricmp("auto", value2)) {
         if (gps) {
            lte_mode_new = LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS;
         } else {
            lte_mode_new = LTE_LC_SYSTEM_MODE_LTEM_NBIOT;
         }
         lte_preference_new = LTE_LC_SYSTEM_MODE_PREFER_AUTO;
      } else if (!stricmp(CFG_NB_IOT, value2)) {
         if (!stricmp(CFG_LTE_M, value3)) {
            if (gps) {
               lte_mode_new = LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS;
            } else {
               lte_mode_new = LTE_LC_SYSTEM_MODE_LTEM_NBIOT;
            }
            lte_preference_new = LTE_LC_SYSTEM_MODE_PREFER_NBIOT;
         } else {
            if (gps) {
               lte_mode_new = LTE_LC_SYSTEM_MODE_NBIOT_GPS;
            } else {
               lte_mode_new = LTE_LC_SYSTEM_MODE_NBIOT;
            }
            lte_preference_new = LTE_LC_SYSTEM_MODE_PREFER_AUTO;
         }
      } else if (!stricmp(CFG_LTE_M, value2)) {
         if (!stricmp(CFG_NB_IOT, value3)) {
            if (gps) {
               lte_mode_new = LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS;
            } else {
               lte_mode_new = LTE_LC_SYSTEM_MODE_LTEM_NBIOT;
            }
            lte_preference_new = LTE_LC_SYSTEM_MODE_PREFER_LTEM;
         } else {
            if (gps) {
               lte_mode_new = LTE_LC_SYSTEM_MODE_LTEM_GPS;
            } else {
               lte_mode_new = LTE_LC_SYSTEM_MODE_LTEM;
            }
            lte_preference_new = LTE_LC_SYSTEM_MODE_PREFER_AUTO;
         }
      }
      if (lte_mode != lte_mode_new || lte_preference != lte_preference_new) {
         modem_off();
         res = lte_lc_system_mode_set(lte_mode_new, lte_preference_new);
         modem_set_preference(RESET_PREFERENCE);
         modem_restore();
         if (!res) {
            LOG_INF("Switched to %s", modem_get_system_mode_cfg(lte_mode_new, lte_preference_new));
         } else {
            LOG_INF("Switching LTE mode to %s failed!", modem_get_system_mode_cfg(lte_mode_new, lte_preference_new));
            return res < 0 ? res : -EINVAL;
         }
      } else {
         LOG_INF("Keep %s", modem_get_system_mode_cfg(lte_mode_new, lte_preference_new));
      }
   }
   if (!stricmp("auto", value1)) {
      res = modem_at_cmd(buf, sizeof(buf), "+COPS: ", "AT+COPS=0");
      if (res >= 0) {
         modem_lock_plmn(false);
      }
   } else {
      res = modem_at_cmdf(buf, sizeof(buf), "+COPS: ", "AT+COPS=1,2,\"%s\"", value1);
      if (res >= 0) {
         modem_lock_plmn(true);
      }
   }
   if (res < 0) {
      LOG_WRN("AT+COPS failed, err %d", res);
   } else {
      res = 1;
   }
   if (value3[0]) {
      LOG_INF(">> cfg %s %s %s ready", value1, value2, value3);
   } else if (value2[0]) {
      LOG_INF(">> cfg %s %s ready", value1, value2);
   } else {
      LOG_INF(">> cfg %s ready", value1);
   }

   return res;
}

static void modem_cmd_config_help(void)
{
   LOG_INF("> help cfg:");
   LOG_INF("  cfg         : read configuration.");
   LOG_INF("  cfg init    : reset configuration.");
   LOG_INF("  cfg <plmn> <modes>");
   LOG_INF("      <plmn>  : either auto or numerical plmn, e.g. 26202");
   LOG_INF("      <modes> : " CFG_NB_IOT ", " CFG_LTE_M ", " CFG_NB_IOT " " CFG_LTE_M ", " CFG_LTE_M " " CFG_NB_IOT ".");
   LOG_INF("              : " CFG_NB_IOT "    := NB-IoT");
   LOG_INF("              : " CFG_LTE_M "    := LTE-M");
   LOG_INF("              : " CFG_NB_IOT " " CFG_LTE_M " := NB-IoT/LTE-M");
   LOG_INF("              : " CFG_LTE_M " " CFG_NB_IOT " := LTE-M /NB-IoT");
}

static int modem_cmd_connect(const char *config)
{
   int res;
   char value1[7];
   char value2[3];
   const char *cur = config;

   memset(value1, 0, sizeof(value1));
   memset(value2, 0, sizeof(value2));
   cur = parse_next_text(cur, ' ', value1, sizeof(value1));
   cur = parse_next_text(cur, ' ', value2, sizeof(value2));
   if (!value1[0]) {
      char mode = 0;
      char type = 0;
      char net_mode = 0;
      char plmn[16];
      char buf[32];
      const char *desc = "none";

      memset(plmn, 0, sizeof(plmn));
      res = modem_at_cmd(buf, sizeof(buf), "+COPS: ", "AT+COPS?");
      if (res > 0) {
         res = sscanf(buf, " %c,%c,%15[^,],%c",
                      &mode, &type, plmn, &net_mode);
         strtrunc(plmn, '"');
      }
      if (net_mode == '7') {
         desc = "m1";
      } else if (net_mode == '9') {
         desc = "nb";
      }
      LOG_INF("con %s%s %s", mode == '0' ? "auto " : "", plmn, desc);
      return 0;
   }
   if (stricmp("auto", value1) == 0) {
      if (value2[0]) {
         LOG_INF("con auto %s", value2);
         LOG_INF("mode %s is not supported for 'auto'.", value2);
         return -EINVAL;
      }
      res = modem_at_cmd(NULL, 0, "+COPS: ", "AT+COPS=0");
      if (res < 0) {
         LOG_WRN("AT+COPS=0 failed, err %d", res);
      } else {
         res = 1;
         modem_lock_plmn(false);
      }
      LOG_INF(">> con auto ready");
      return res;
   } else if (!modem_is_plmn(value1)) {
      LOG_INF("con %s", config);
      LOG_INF("plmn '%s' not supported, only numerical plmn.", value1);
      return -EINVAL;
   }
   if (value2[0]) {
      if (stricmp(CFG_NB_IOT, value2) == 0) {
         cur = ",9";
      } else if (stricmp(CFG_LTE_M, value2) == 0) {
         cur = ",7";
      } else {
         LOG_INF("con %s", config);
         LOG_INF("mode '%s' not supported!", value2);
         return -EINVAL;
      }
   } else {
      cur = "";
   }
   res = modem_at_cmdf(NULL, 0, "+COPS: ", "AT+COPS=1,2,\"%s\"%s", value1, cur);
   if (res < 0) {
      LOG_WRN("AT+COPS failed, err %d", res);
   } else {
      res = 1;
      modem_lock_plmn(true);
   }
   if (value2[0]) {
      LOG_INF(">> con %s %s ready", value1, value2);
   } else {
      LOG_INF(">> con %s ready", value1);
   }

   return res;
}

static void modem_cmd_connect_help(void)
{
   LOG_INF("> help con:");
   LOG_INF("  con         : read connection information");
   LOG_INF("  con <plmn> [<mode>]");
   LOG_INF("      <plmn>  : numerical plmn, e.g. 26202");
   LOG_INF("      <mode>  : optional mode, " CFG_NB_IOT " or " CFG_LTE_M ".");
   LOG_INF("              : " CFG_NB_IOT " := NB-IoT");
   LOG_INF("              : " CFG_LTE_M " := LTE-M");
   LOG_INF("  con auto    : automatic network selection.");
}

static int modem_cmd_scan(const char *config)
{
   static struct lte_lc_ncellmeas_params params = {
       .search_type = LTE_LC_NEIGHBOR_SEARCH_TYPE_GCI_DEFAULT,
       .gci_count = 6};

   if (*config) {
      char *t = NULL;
      int type = (int)strtol(config, &t, 10);
      if (config == t) {
         return -EINVAL;
      }
      if (type < 0 || type > 5) {
         LOG_INF("Type %d out of range [0,5]", type);
         return -EINVAL;
      }
      params.search_type = type + 1;
      if (*t == ',' || *t == ' ') {
         int count = (int)strtol(++t, NULL, 10);
         if (count < 2 || count > 15) {
            LOG_INF("Count %d out of range [2,15]", count);
            return -EINVAL;
         }
         params.gci_count = count;
      }
   }

   if (params.search_type < LTE_LC_NEIGHBOR_SEARCH_TYPE_GCI_DEFAULT) {
      LOG_INF(">AT%%NCELLMEAS=%d", params.search_type - 1);
   } else {
      LOG_INF(">AT%%NCELLMEAS=%d,%d", params.search_type - 1, params.gci_count);
   }
   modem_set_scan_time();
   return lte_lc_neighbor_cell_measurement(&params);
}

static void modem_cmd_scan_help(void)
{
   LOG_INF("> help scan:");
   LOG_INF("  scan        : repeat previous network scan.");
   LOG_INF("  scan 0      : displays neighbor cell history");
   LOG_INF("  scan 1      : start neighbor cell search");
   LOG_INF("  scan 2      : start neighbor cell search, all bands");
   LOG_INF("  scan 3 <n>  : displays cell history");
   LOG_INF("  scan 4 <n>  : start cell search");
   LOG_INF("  scan 5 <n>  : start cell search, all bands");
   LOG_INF("  <n>         : maximum cells to list, values 2 to 15.");
}

#define ROUND_UP_TIME(T, D) (((T) + ((D)-1)) / (D))

static int modem_cmd_psm(const char *config)
{
   int res = 0;
   int active_time = 0;
   int tau_time = 0;
   char tau_unit = 's';

   const char *cur = config;
   char value[8];

   memset(value, 0, sizeof(value));
   cur = parse_next_text(cur, ' ', value, sizeof(value));

   if (!value[0]) {
      res = lte_lc_psm_get(&tau_time, &active_time);
      if (!res) {
         if (active_time < 0) {
            LOG_INF("PSM disabled");
         } else {
            LOG_INF("PSM enabled, act: %d s, tau: %d s", active_time, tau_time);
         }
      }
   } else if (!stricmp("normal", value)) {
      modem_lock_psm(false);
      res = modem_set_psm(CONFIG_UDP_PSM_CONNECT_RAT);
   } else {
      res = sscanf(config, "%u %u%c", &active_time, &tau_time, &tau_unit);
      if (res >= 2) {
         char rat[9] = "00000000";
         char tau[9] = "00000000";
         int rat_mul = 2;
         int tau_mul = 2;
         int tau_unit_id = 0x3;
         // requested active time
         // 2s
         active_time = ROUND_UP_TIME(active_time, 2);
         if (active_time > 31) {
            // 60s
            active_time = ROUND_UP_TIME(active_time, 30);
            rat_mul = 60;
            if (active_time > 31) {
               // 360s
               active_time = ROUND_UP_TIME(active_time, 6);
               rat_mul = 360;
               rat[1] = '1';
            } else {
               rat[2] = '1';
            }
         }
         print_bin(&rat[3], 5, active_time);
         // requested tracking aree update time
         // 2s
         if (tau_unit == 'h') {
            tau_time *= 3600;
         }
         tau_time = ROUND_UP_TIME(tau_time, 2);
         if (tau_time > 31) {
            // 30s
            tau_time = ROUND_UP_TIME(tau_time, 15);
            tau_mul = 30;
            tau_unit_id = 0x4;
            if (tau_time > 31) {
               // 60s
               tau_time = ROUND_UP_TIME(tau_time, 2);
               tau_mul = 60;
               tau_unit_id = 0x5;
               if (tau_time > 31) {
                  // 600s
                  tau_time = ROUND_UP_TIME(tau_time, 10);
                  tau_mul = 600;
                  tau_unit_id = 0;
                  if (tau_time > 31) {
                     // 3600s / 1h
                     tau_time = ROUND_UP_TIME(tau_time, 6);
                     tau_mul = 3600;
                     tau_unit_id = 1;
                     if (tau_time > 31) {
                        // 36000s / 10h
                        tau_time = ROUND_UP_TIME(tau_time, 10);
                        tau_mul = 36000;
                        tau_unit_id = 2;
                        if (tau_time > 31) {
                           // 320h
                           tau_time = ROUND_UP_TIME(tau_time, 32);
                           tau_mul = 36000 * 32;
                           tau_unit_id = 6;
                        }
                     }
                  }
               }
            }
         }
         print_bin(&tau[0], 3, tau_unit_id);
         print_bin(&tau[3], 5, tau_time);

         if (tau_unit == 'h') {
            LOG_INF("PSM enable, act: %d s, tau: %d h", active_time * rat_mul, (tau_time * tau_mul) / 3600);
         } else {
            LOG_INF("PSM enable, act: %d s, tau: %d s", active_time * rat_mul, tau_time * tau_mul);
         }
         modem_lock_psm(true);
         lte_lc_psm_param_set(tau, rat);
         res = lte_lc_psm_req(true);
      } else {
         res = -EINVAL;
      }
   }

   return res;
}

static void modem_cmd_psm_help(void)
{
   LOG_INF("> help psm:");
   LOG_INF("  psm <act-time> <tau-time>[h] : request PSM times.");
   LOG_INF("     <act-time>    : active time in s.");
   LOG_INF("     <tau-time>    : tracking area update time in s.");
   LOG_INF("     <tau-time>h   : tracking area update time in h.");
   LOG_INF("  psm normal       : PSM handled by application.");
   LOG_INF("  psm              : show current PSM status.");
}

static int modem_cmd_rai(const char *config)
{
   int res = 0;
   const char *cur = config;
   char value[5];

   memset(value, 0, sizeof(value));
   cur = parse_next_text(cur, ' ', value, sizeof(value));
   if (!value[0]) {
      enum lte_network_rai rai = LTE_NETWORK_RAI_UNKNOWN;
      res = modem_get_rai_status(&rai);
      if (!res) {
         LOG_INF("%s.", modem_get_rai_description(rai));
      }
   } else if (!stricmp("on", value)) {
      modem_lock_rai(false);
   } else if (!stricmp("off", value)) {
      modem_set_rai_mode(RAI_OFF, -1);
      modem_lock_rai(true);
   } else {
      res = -EINVAL;
   }

   return res;
}

static void modem_cmd_rai_help(void)
{
   LOG_INF("> help rai:");
   LOG_INF("  rai off|on : enable or disable RAI.");
   LOG_INF("  rai        : show current RAI status.");
}

static int modem_cmd_edrx(const char *config)
{
   int res = 0;
   unsigned int edrx_time = 0;
   const char *cur = config;
   char value[5];

   memset(value, 0, sizeof(value));
   cur = parse_next_text(cur, ' ', value, sizeof(value));
   if (!value[0]) {
      struct lte_lc_edrx_cfg edrx_cfg;
      res = lte_lc_edrx_get(&edrx_cfg);
      if (!res) {
         if (edrx_cfg.edrx < 1.0) {
            LOG_INF("eDRX disabled.");
         } else {
            LOG_INF("eDRX %.2fs, ptw %.2fs", edrx_cfg.edrx, edrx_cfg.ptw);
         }
      }
   } else if (!stricmp("off", value)) {
      res = modem_set_edrx(0);
   } else {
      res = sscanf(config, "%u", &edrx_time);
      if (res == 1) {
         res = modem_set_edrx(edrx_time);
      } else {
         res = -EINVAL;
      }
   }

   return res;
}

static void modem_cmd_edrx_help(void)
{
   LOG_INF("> help edrx:");
   LOG_INF("  edrx <edrx-time> : request eDRX time.");
   LOG_INF("     <edrx-time>   : eDRX time in s.");
   LOG_INF("                   : 0 to disable eDRX.");
   LOG_INF("  edrx off         : disable eDRX.");
   LOG_INF("  edrx             : show current eDRX status.");
}

static int modem_cmd_print_bands(const char *bands)
{
   int band;
   int pos = 0;
   int end = strlen(bands);
   char line[128];

   for (band = 1; band < end; ++band) {
      if (bands[end - band] == '1') {
         pos += snprintf(&line[pos], sizeof(line) - pos, "%d ", band);
      }
   }
   if (end > 0) {
      LOG_INF("BANDLOCK: %s", line);
   } else {
      LOG_INF("BANDLOCK: not used");
   }
   return pos;
}

static int modem_cmd_band(const char *config)
{
   int res;
   char buf[128];
   char value[4];
   const char *cur = config;

   cur = parse_next_text(cur, ' ', value, sizeof(value));
   if (!value[0]) {
      // show bands
      res = modem_at_cmd(buf, sizeof(buf), "%XBANDLOCK: ", "AT%XBANDLOCK?");
      if (res > 0) {
         LOG_DBG("BANDLOCK: %s", buf);
         parse_next_qtext(buf, '"', buf, sizeof(buf));
         if (!modem_cmd_print_bands(buf)) {
            res = modem_at_cmd(buf, sizeof(buf), "%XCBAND: ", "AT%XCBAND=?");
            if (res > 0) {
               strtrunc2(buf, '(', ')');
               LOG_INF("Supported BANDs: %s", buf);
            }
         }
      }
   } else if (!stricmp(value, "all")) {
      modem_off();
      res = modem_at_cmd(buf, sizeof(buf), "%XBANDLOCK: ", "AT%XBANDLOCK=0");
      if (res > 0) {
         LOG_INF("BANDLOCK: %s", buf);
      }
      modem_restore();
   } else {
      char bands[] = "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000";
      long band;
      while (parse_next_long(value, 10, &band) > value) {
         if (band > 0) {
            bands[sizeof(bands) - band - 1] = '1';
         }
         cur = parse_next_text(cur, ' ', value, sizeof(value));
      }
      modem_off();
      res = modem_at_cmdf(buf, sizeof(buf), "%XBANDLOCK: ", "AT%%XBANDLOCK=1,\"%s\"", bands);
      if (res >= 0) {
         LOG_INF("BANDLOCK: %s", buf);
      }
      modem_restore();
   }
   return 0;
}

static void modem_cmd_band_help(void)
{
   LOG_INF("> help band:");
   LOG_INF("  band               : show current bands.");
   LOG_INF("  band all           : activate all bands.");
   LOG_INF("  band <b1> <b2> ... : activate bands <b1> <b2> ... .");
}

static int modem_cmd_reduced_mobility(const char *config)
{
   unsigned int mode = 0;
   int res = sscanf(config, "%u", &mode);
   if (res == 1) {
      if (mode > 2) {
         LOG_INF("Mode %u is out of range [0..2].", mode);
         res = -EINVAL;
      } else {
         res = modem_set_reduced_mobility(mode);
      }
   } else if (res == EOF) {
      res = modem_get_reduced_mobility();
      switch (res) {
         case 0:
         case 2:
            LOG_INF("Reduced mobility disabled.");
            break;
         case 1:
            LOG_INF("Nordic specific reduced mobility.");
            break;
         default:
            break;
      }
   }
   return res;
}

static void modem_cmd_reduced_mobility_help(void)
{
   LOG_INF("> help remo:");
   LOG_INF("  remo   : show current reduced mobility mode.");
   LOG_INF("  remo 0 : no reduced mobility.");
   LOG_INF("  remo 1 : reduced mobility (nordic).");
   LOG_INF("  remo 2 : no reduced mobility.");
}

static int modem_cmd_power_level(const char *config)
{
   unsigned int level = 0;
   int res = sscanf(config, "%u", &level);
   if (res == 1) {
      if (level > 4) {
         LOG_INF("Level %u is out of range [0..4].", level);
         res = -EINVAL;
      } else {
         res = modem_set_power_level(level);
      }
   } else if (res == EOF) {
      res = modem_get_power_level();
      switch (res) {
         case 0:
            LOG_INF("Ultra-low power.");
            break;
         case 1:
            LOG_INF("Low power.");
            break;
         case 2:
            LOG_INF("Normal.");
            break;
         case 3:
            LOG_INF("Performance.");
            break;
         case 4:
            LOG_INF("High performance.");
            break;
         default:
            break;
      }
   }

   return res;
}

static void modem_cmd_power_level_help(void)
{
   LOG_INF("> help power:");
   LOG_INF("  power     : show current power level.");
   LOG_INF("  power <l> : set power level. Values 0 to 4.");
   LOG_INF("        0   : Ultra-low power");
   LOG_INF("        1   : Low power");
   LOG_INF("        2   : Normal");
   LOG_INF("        3   : Performance");
   LOG_INF("        4   : High performance");
}

#define CRSM_SUCCESS "144,0,\""

static int modem_cmd_read_imsi_sel(unsigned int *selected)
{
   char buf[64];

   int res = modem_at_cmd(buf, sizeof(buf), "+CRSM: ", "AT+CRSM=178,28616,1,4,13");
   if (res <= 0) {
      return res;
   }
   res = strstart(buf, CRSM_SUCCESS, false);
   if (!res) {
      LOG_DBG("IMSI read selection failed, %s", buf);
      return -ENOTSUP;
   }

   buf[res + 6] = 0;
   return sscanf(buf + res, "%x", selected);
}

static int modem_cmd_imsi_sel(const char *config)
{
   unsigned int select = 0;
   unsigned int selected = 0;
   const char *cur = config;
   char buf[64];

   int res = modem_cmd_read_imsi_sel(&selected);
   if (res == 1) {
      parse_next_text(cur, ' ', buf, sizeof(buf));
      if (!buf[0]) {
         // show selection
         if ((selected >> 8) == 0) {
            LOG_INF("IMSI auto select, %u selected", selected);
         } else if ((selected >> 8) == (selected & 0xff)) {
            LOG_INF("IMSI %u selected", selected >> 8);
         } else {
            LOG_INF("IMSI %u selection pending", selected >> 8);
         }
      } else {
         if (stricmp(buf, "auto")) {
            res = sscanf(config, "%d", &select);
         }
         /* if "auto" res is already 1 and select is 0 */
         if (res == 1) {
            if (select < 0 || select > 255) {
               LOG_INF("Selection %u is out of range [0..255].", select);
               return 0;
            }
            if (select == (selected >> 8)) {
               LOG_INF("IMSI %u already selected.", select);
            } else {
               res = modem_at_cmdf(buf, sizeof(buf), "+CRSM: ", "AT+CRSM=220,28616,1,4,13,\"%04xFFFFFFFFFFFFFFFFFFFFFF\"", select);
               if (res <= 0) {
                  return res;
               }
               res = strstart(buf, CRSM_SUCCESS, false);
               if (res > 0) {
                  LOG_INF("IMSI %u selected", select);
                  modem_off();
                  modem_restore();
                  res = modem_cmd_read_imsi_sel(&selected);
                  if (res != 1) {
                     return res;
                  }
                  if (select == 0) {
                     LOG_INF("IMSI auto select, %u selected.", selected);
                  } else if (select == (selected & 0xff)) {
                     LOG_INF("IMSI %u gets selected.", select);
                  } else {
                     LOG_INF("IMSI %u not selected.", select);
                  }
               } else {
                  LOG_INF("IMSI selection failed, %s", buf);
               }
            }
         } else {
            LOG_INF("imis %s invalid argument!", config);
            return -EINVAL;
         }
      }
   } else if (res == -ENOTSUP) {
      LOG_INF("IMSI selection not supported by SIM.");
   }
   res = modem_at_cmd(buf, sizeof(buf), NULL, "AT+CIMI");
   if (res > 0) {
      LOG_INF("IMSI: %s", buf);
   }
   return 0;
}

static void modem_cmd_imsi_sel_help(void)
{
   LOG_INF("> help imsi:");
   LOG_INF("  imsi      : show current IMSI selection.");
   LOG_INF("  imsi auto : automatic IMSI select. Switching IMSI on timeout (300s).");
   LOG_INF("  imsi <n>  : select IMSI (FloLive SIM card). Values 0 to 255.");
   LOG_INF("  imsi 0    : automatic IMSI select. Switching IMSI on timeout (300s).");
   LOG_INF("  imsi 1    : select IMSI profile 1.");
   LOG_INF("  imsi n    : select IMSI profile n. The largest value depends on the SIM card");
}

static int modem_cmd_switch_on(const char *parameter)
{
   (void)parameter;
   return modem_set_normal();
}

static int modem_cmd_state(const char *parameter)
{
   (void)parameter;
   return modem_read_network_info(NULL, true);
}

static int modem_cmd_rate_limit(const char *parameter)
{
   (void)parameter;
   uint32_t time = 0;
   int res = modem_read_rate_limit_time(&time);
   if (time) {
      LOG_INF(">> rate limit exceeded, %u s", time);
   }
   return res;
}

#ifdef CONFIG_SMS

#include <modem/sms.h>

static int modem_cmd_sms(const char *config)
{
   char destination[32];
   const char *cur = config;

   memset(destination, 0, sizeof(destination));
   cur = parse_next_text(cur, ' ', destination, sizeof(destination));
   modem_set_psm(120);
   if (destination[0]) {
      return sms_send_text(destination, cur);
   } else {
      return 0;
   }
}

static void modem_cmd_sms_help(void)
{
   LOG_INF("> help sms:");
   LOG_INF("  sms                  : receive sms (120s).");
   LOG_INF("  sms <dest> <message> : send sms and receive sms (120s).");
   LOG_INF("  <dest>               : international IMSI");
   LOG_INF("  <message>            : message");
}

SH_CMD(sms, "", "send SMS.", modem_cmd_sms, modem_cmd_sms_help, 0);

#endif

SH_CMD(eval, "AT%CONEVAL", "evaluate connection.", NULL, NULL, 0);
SH_CMD(off, "AT+CFUN=0", "switch modem off.", NULL, NULL, 0);
SH_CMD(offline, "AT+CFUN=4", "switch modem offline.", NULL, NULL, 0);
SH_CMD(reset, "AT%XFACTORYRESET=0", "modem factory reset.", NULL, NULL, 0);
SH_CMD(search, "AT+COPS=?", "network search.", NULL, NULL, 0);

SH_CMD(limit, "", "read apn rate limit.", modem_cmd_rate_limit, NULL, 0);
SH_CMD(on, "", "switch modem on.", modem_cmd_switch_on, NULL, 0);
SH_CMD(state, "", "read modem state.", modem_cmd_state, NULL, 0);

SH_CMD(cfg, "", "configure modem.", modem_cmd_config, modem_cmd_config_help, 3);
SH_CMD(con, "", "connect modem.", modem_cmd_connect, modem_cmd_connect_help, 3);

SH_CMD(scan, "AT%NCELLMEAS", "network scan.", modem_cmd_scan, modem_cmd_scan_help, 0);

SH_CMD(band, "", "configure bands.", modem_cmd_band, modem_cmd_band_help, 0);
SH_CMD(edrx, "", "configure eDRX.", modem_cmd_edrx, modem_cmd_edrx_help, 0);
SH_CMD(psm, "", "configure PSM.", modem_cmd_psm, modem_cmd_psm_help, 0);
SH_CMD(rai, "", "configure RAI.", modem_cmd_rai, modem_cmd_rai_help, 0);

SH_CMD(remo, "", "reduced mobility.", modem_cmd_reduced_mobility, modem_cmd_reduced_mobility_help, 0);
SH_CMD(power, "", "configure power level.", modem_cmd_power_level, modem_cmd_power_level_help, 0);
SH_CMD(imsi, "", "select IMSI.", modem_cmd_imsi_sel, modem_cmd_imsi_sel_help, 0);

#endif
