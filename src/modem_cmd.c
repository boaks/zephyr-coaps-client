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

#include "modem.h"
#include "modem_at.h"
#include "modem_cmd.h"
#include "modem_desc.h"
#include "parse.h"

LOG_MODULE_DECLARE(MODEM, CONFIG_MODEM_LOG_LEVEL);

#if defined(CONFIG_NRF_MODEM_LIB)

#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <modem/pdn.h>
#include <nrf_modem_at.h>

int modem_reinit(void);

static bool modem_is_plmn(const char *value)
{
   int len = 0;
   while (isdigit(0xff & value[len])) {
      ++len;
   }
   return !value[len] && 5 <= len && len <= 6;
}

#define CFG_NB_IOT "nb"
#define CFG_LTE_M "m1"

int modem_cmd_config(const char *config)
{
   enum lte_lc_func_mode func_mode;
   int err = lte_lc_func_mode_get(&func_mode);

   if (!err) {
      char buf[32];
      char value1[7];
      char value2[3];
      char value3[3];
      const char *cur = config;

      memset(value1, 0, sizeof(value1));
      memset(value2, 0, sizeof(value2));
      memset(value3, 0, sizeof(value3));
      while (*cur == ' ') {
         ++cur;
      }
      cur = parse_next_text(cur, ' ', value1, sizeof(value1));
      cur = parse_next_text(cur, ' ', value2, sizeof(value2));
      cur = parse_next_text(cur, ' ', value3, sizeof(value3));
      if (!value1[0]) {
         char mode = 0;
         char type = 0;
         char net_mode = 0;
         char plmn[16];
         const char *desc = "\?\?\?";
         enum lte_lc_system_mode lte_mode = LTE_LC_SYSTEM_MODE_NONE;
         enum lte_lc_system_mode_preference lte_preference = CONFIG_LTE_MODE_PREFERENCE;

         lte_lc_system_mode_get(&lte_mode, &lte_preference);
         memset(plmn, 0, sizeof(plmn));
         err = modem_at_cmd(buf, sizeof(buf), "+COPS: ", "AT+COPS?");
         if (err > 0) {
            err = sscanf(buf, " %c,%c,%15[^,],%c",
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
            desc = "m1";
         } else if (net_mode == '9') {
            desc = "nb";
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
         if (func_mode != LTE_LC_FUNC_MODE_POWER_OFF) {
            lte_lc_func_mode_set(LTE_LC_FUNC_MODE_POWER_OFF);
         }
         modem_reinit();
         if (func_mode != LTE_LC_FUNC_MODE_POWER_OFF) {
            lte_lc_func_mode_set(func_mode);
         }
         LOG_INF(">> cfg init ready");
         return 1;
      }
      if (stricmp("auto", value1) && !modem_is_plmn(value1)) {
         LOG_INF("cfg %s", config);
         LOG_INF("plmn '%s' not supported! Either 'auto' or numerical plmn.", value1);
         return -EINVAL;
      }
      if (value2[0] && stricmp(CFG_NB_IOT, value2) && stricmp(CFG_LTE_M, value2)) {
         LOG_INF("cfg %s", config);
         LOG_INF("mode '%s' not supported!", value2);
         return -EINVAL;
      }
      if (value3[0] && stricmp(CFG_NB_IOT, value3) && stricmp(CFG_LTE_M, value3)) {
         LOG_INF("cfg %s", config);
         LOG_INF("mode '%s' not supported!", value3);
         return -EINVAL;
      }
      LOG_INF(">> cfg %s %s %s", value1, value2, value3);
      if (value2[0]) {
         enum lte_lc_system_mode lte_mode = LTE_LC_SYSTEM_MODE_NONE;
         enum lte_lc_system_mode_preference lte_preference = CONFIG_LTE_MODE_PREFERENCE;
         enum lte_lc_system_mode lte_mode_new;
         enum lte_lc_system_mode_preference lte_preference_new;
         bool gps;
         lte_lc_system_mode_get(&lte_mode, &lte_preference);
         lte_mode_new = lte_mode;
         lte_preference_new = lte_preference;
         gps = lte_mode == LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS ||
               lte_mode == LTE_LC_SYSTEM_MODE_LTEM_GPS ||
               lte_mode == LTE_LC_SYSTEM_MODE_NBIOT_GPS;
         if (!stricmp(CFG_NB_IOT, value2)) {
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
            if (func_mode != LTE_LC_FUNC_MODE_POWER_OFF) {
               lte_lc_func_mode_set(LTE_LC_FUNC_MODE_POWER_OFF);
            }
            err = lte_lc_system_mode_set(lte_mode_new, lte_preference_new);
            modem_set_preference(RESET_PREFERENCE);
            if (func_mode != LTE_LC_FUNC_MODE_POWER_OFF) {
               lte_lc_func_mode_set(func_mode);
            }
            if (!err) {
               LOG_INF("Switched to %s", modem_get_system_mode_cfg(lte_mode_new, lte_preference_new));
            } else {
               LOG_INF("Switching LTE mode to %s failed!", modem_get_system_mode_cfg(lte_mode_new, lte_preference_new));
               return err < 0 ? err : -EINVAL;
            }
         } else {
            LOG_INF("Keep %s", modem_get_system_mode_cfg(lte_mode_new, lte_preference_new));
         }
      }
      if (!stricmp("auto", value1)) {
         err = modem_at_cmd(buf, sizeof(buf), "+COPS: ", "AT+COPS=0");
         if (err >= 0) {
            modem_lock_plmn(false);
         }
      } else {
         err = modem_at_cmdf(buf, sizeof(buf), "+COPS: ", "AT+COPS=1,2,\"%s\"", value1);
         if (err >= 0) {
            modem_lock_plmn(true);
         }
      }
      if (err < 0) {
         LOG_WRN("AT+COPS failed, err %d", err);
      } else {
         err = 1;
      }
      if (value3[0]) {
         LOG_INF(">> cfg %s %s %s ready", value1, value2, value3);
      } else if (value2[0]) {
         LOG_INF(">> cfg %s %s ready", value1, value2);
      } else {
         LOG_INF(">> cfg %s ready", value1);
      }
   }
   return err;
}

void modem_cmd_config_help(void)
{
   LOG_INF("> help cfg:");
   LOG_INF("  cfg         : reset configuration.");
   LOG_INF("  cfg <plmn> <modes>");
   LOG_INF("      <plmn>  : either auto or numerical plmn, e.g. 26202");
   LOG_INF("      <modes> : " CFG_NB_IOT ", " CFG_LTE_M ", " CFG_NB_IOT " " CFG_LTE_M ", " CFG_LTE_M " " CFG_NB_IOT ".");
   LOG_INF("              : " CFG_NB_IOT "    := NB-IoT");
   LOG_INF("              : " CFG_LTE_M "    := LTE-M");
   LOG_INF("              : " CFG_NB_IOT " " CFG_LTE_M " := NB-IoT/LTE-M");
   LOG_INF("              : " CFG_LTE_M " " CFG_NB_IOT " := LTE-M /NB-IoT");
}

int modem_cmd_connect(const char *config)
{
   int err;
   char value1[7];
   char value2[3];
   const char *cur = config;

   memset(value1, 0, sizeof(value1));
   memset(value2, 0, sizeof(value2));
   while (*cur == ' ') {
      ++cur;
   }
   cur = parse_next_text(cur, ' ', value1, sizeof(value1));
   cur = parse_next_text(cur, ' ', value2, sizeof(value2));
   if (stricmp("auto", value1) == 0) {
      if (value2[0]) {
         LOG_INF("con auto %s", value2);
         LOG_INF("mode %s is not supported for 'auto'.", value2);
         return -EINVAL;
      }
      err = modem_at_cmd(NULL, 0, "+COPS: ", "AT+COPS=0");
      if (err < 0) {
         LOG_WRN("AT+COPS=0 failed, err %d", err);
      } else {
         err = 1;
         modem_lock_plmn(false);
      }
      LOG_INF(">> con auto ready");
      return err;
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
   err = modem_at_cmdf(NULL, 0, "+COPS: ", "AT+COPS=1,2,\"%s\"%s", value1, cur);
   if (err < 0) {
      LOG_WRN("AT+COPS failed, err %d", err);
   } else {
      err = 1;
      modem_lock_plmn(true);
   }
   if (value2[0]) {
      LOG_INF(">> con %s %s ready", value1, value2);
   } else {
      LOG_INF(">> con %s ready", value1);
   }

   return err;
}

void modem_cmd_connect_help(void)
{
   LOG_INF("> help con:");
   LOG_INF("  con <plmn> [<mode>]");
   LOG_INF("      <plmn>  : numerical plmn, e.g. 26202");
   LOG_INF("      <mode>  : optional mode, " CFG_NB_IOT " or " CFG_LTE_M ".");
   LOG_INF("              : " CFG_NB_IOT " := NB-IoT");
   LOG_INF("              : " CFG_LTE_M " := LTE-M");
   LOG_INF("  con auto    : automatic network selection.");
}

int modem_cmd_scan(const char *config)
{
   static struct lte_lc_ncellmeas_params params = {
       .search_type = LTE_LC_NEIGHBOR_SEARCH_TYPE_GCI_DEFAULT,
       .gci_count = 6};

   if (*config == '=' || *config == ' ') {
      char *t = NULL;
      int type = (int)strtol(++config, &t, 10);
      if (config != t) {
         params.search_type = type + 1;
         if (*t == ',' || *t == ' ') {
            params.gci_count = (int)strtol(++t, NULL, 10);
         }
      }
   } else if (*config) {
      LOG_INF("ignore > %s", config);
      return -EINVAL;
   }

   if (params.search_type < LTE_LC_NEIGHBOR_SEARCH_TYPE_GCI_DEFAULT) {
      LOG_INF(">AT%%NCELLMEAS=%d", params.search_type - 1);
   } else {
      LOG_INF(">AT%%NCELLMEAS=%d,%d", params.search_type - 1, params.gci_count);
   }
   return lte_lc_neighbor_cell_measurement(&params);
}

void modem_cmd_scan_help(void)
{
   LOG_INF("> help scan:");
   LOG_INF("  scan        : repeat previous network scan.");
   LOG_INF("  scan 0      : displays neighbor cell history");
   LOG_INF("  scan 1      : start neighbor cell search");
   LOG_INF("  scan 2      : start neighbor cell search, all bands");
   LOG_INF("  scan 3 <n>  : displays cell history");
   LOG_INF("  scan 4 <n>  : start cell search");
   LOG_INF("  scan 5 <n>  : start cell search, all bands");
   LOG_INF("  <n>         : maximum cells to list");
}

#ifdef CONFIG_SMS

#include <modem/sms.h>

int modem_cmd_sms(const char *config)
{
   char destination[32];
   const char *cur = config;

   memset(destination, 0, sizeof(destination));
   while (*cur == ' ') {
      ++cur;
   }
   cur = parse_next_text(cur, ' ', destination, sizeof(destination));
   modem_set_psm(120);
   if (destination[0]) {
      return sms_send_text(destination, cur);
   } else {
      return -EINVAL;
   }
}

void modem_cmd_sms_help(void)
{
   LOG_INF("> help sms:");
   LOG_INF("  sms                  : receive sms (120s).");
   LOG_INF("  sms <dest> <message> : send sms and receive sms (120s).");
   LOG_INF("  <dest>               : international IMSI");
   LOG_INF("  <message>            : message");
}

#endif

#define ROUND_UP_TIME(T, D) (((T) + ((D)-1)) / (D))

int modem_cmd_psm(const char *config)
{
   unsigned int active_time = 0;
   unsigned int tau_time = 0;
   char tau_unit = 's';
   int err = sscanf(config, "%u %u%c", &active_time, &tau_time, &tau_unit);
   if (err >= 2) {
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
      return lte_lc_psm_req(true);
   } else {
      char value[8];
      const char *cur = config;

      memset(value, 0, sizeof(value));
      while (*cur == ' ') {
         ++cur;
      }
      cur = parse_next_text(cur, ' ', value, sizeof(value));
      if (!stricmp("normal", value)) {
         modem_lock_psm(false);
         return modem_set_psm(CONFIG_UDP_PSM_CONNECT_RAT);
      }
   }
   return -EINVAL;
}

void modem_cmd_psm_help(void)
{
   LOG_INF("> help psm:");
   LOG_INF("  psm <act-time> <tau-time>[h] : request PSM times.");
   LOG_INF("     <act-time>    : active time in s.");
   LOG_INF("     <tau-time>    : tracking area update time in s.");
   LOG_INF("     <tau-time>h   : tracking area update time in h.");
   LOG_INF("  psm normal       : PSM handled by application.");
}

int modem_cmd_rai(const char *config)
{
   char value[5];
   const char *cur = config;

   memset(value, 0, sizeof(value));
   while (*cur == ' ') {
      ++cur;
   }
   cur = parse_next_text(cur, ' ', value, sizeof(value));
   if (!stricmp("on", value)) {
      modem_lock_rai(false);
      return 0;
   } else if (!stricmp("off", value)) {
      modem_set_rai_mode(RAI_OFF, -1);
      modem_lock_rai(true);
      return 0;
   } else {
      return -EINVAL;
   }
}

void modem_cmd_rai_help(void)
{
   LOG_INF("> help rai:");
   LOG_INF("  rai off|on : enable or disable RAI.");
}

int modem_cmd_edrx(const char *config)
{
   int edrx_time = 0;
   int err = sscanf(config, "%d", &edrx_time);
   if (err == 1) {
      return modem_set_edrx(edrx_time);
   } else {
      char value[5];
      const char *cur = config;

      memset(value, 0, sizeof(value));
      while (*cur == ' ') {
         ++cur;
      }
      cur = parse_next_text(cur, ' ', value, sizeof(value));
      if (!stricmp("off", value)) {
         return modem_set_edrx(0);
      }
   }
   return -EINVAL;
}

void modem_cmd_edrx_help(void)
{
   LOG_INF("> help edrx:");
   LOG_INF("  edrx <edrx-time> : request eDRX time.");
   LOG_INF("     <edrx-time>   : eDRX time in s.");
   LOG_INF("                   : 0 to disable eDRX.");
   LOG_INF("  edrx off         : disable eDRX.");
}

#else

int modem_config(const char *config)
{
   (void)config;
   return 0;
}

void modem_cmd_config_help(void)
{
   LOG_WRN("> 'cfg' not supported!");
}

int modem_cmd_connect(const char *config)
{
   (void)config;
   return 0;
}

void modem_cmd_connect_help(void)
{
   LOG_WRN("> 'con' not supported!");
}

int modem_cmd_scan(const char *config)
{
   (void)config;
   return 0;
}

void modem_cmd_scan_help(void)
{
   LOG_WRN("> 'scan' not supported!");
}

int modem_cmd_sms(const char *config)
{
   (void)config;
   return 0;
}

void modem_cmd_sms_help(void)
{
   LOG_WRN("> 'sms' not supported!");
}

int modem_cmd_psm(const char *config)
{
   (void)config;
   return 0;
}

void modem_cmd_psm_help(void)
{
   LOG_WRN("> 'psm' not supported!");
}

int modem_cmd_edrx(const char *config)
{
   (void)config;
   return 0;
}

void modem_cmd_edrx_help(void)
{
   LOG_WRN("> 'edrx' not supported!");
}

#endif
