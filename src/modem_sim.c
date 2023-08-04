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

// #include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "modem.h"
#include "modem_at.h"
#include "modem_sim.h"
#include "parse.h"

LOG_MODULE_DECLARE(MODEM, CONFIG_MODEM_LOG_LEVEL);

#if defined(CONFIG_NRF_MODEM_LIB)

#define MSEC_TO_SEC(X) (((X) + (MSEC_PER_SEC / 2)) / MSEC_PER_SEC)

#define MULTI_IMSI_MINIMUM_TIMEOUT_MS (300 * MSEC_PER_SEC)

static K_MUTEX_DEFINE(sim_mutex);

static struct lte_sim_info sim_info;

static int64_t imsi_time = 0;

static const char *find_id(const char *buf, const char *id)
{
   char *pos = strstr(buf, id);
   while (pos > buf && *(pos - 1) != ',') {
      pos = strstr(pos + 1, id);
   }
   return pos;
}

/**
 * PLMN encoding:
 * 123  56 = 0x21 0xF3 0x65
 * 123 456 = 0x21 0x43 0x65
 * e.g. 262 02 = 0x62 0xF2 0x20
 */

// #define CONFIG_USER_PLMN_SELECTOR "62F2204000"
// #define CONFIG_FORBIDDEN_PLMN "62F240FFFFFFFFFFFFFFFFFF"

// #define CONFIG_USER_PLMN_SELECTOR "FFFFFF0000"
// #define CONFIG_FORBIDDEN_PLMN "FFFFFFFFFFFFFFFFFFFFFFFF"

#define CRSM_SUCCESS "144,0,\""

static size_t copy_plmn(const char *buf, char *plmn, size_t len, const char *mcc)
{
   size_t result = 0;
   if (mcc) {
      buf = find_id(buf, mcc);
      if (!buf) {
         return 0;
      }
   }
   while (len) {
      *plmn = *buf;
      if (*buf == ',') {
         *plmn = 0;
         return result;
      } else if (*buf == 0) {
         return result;
      }
      --len;
      if (!len) {
         *plmn = 0;
      } else {
         ++result;
         ++plmn;
         ++buf;
      }
   }
   return result;
}

static inline void append_plmn(char **plmn, char digit)
{
   if (digit != 'F') {
      **plmn = digit;
      (*plmn)++;
   }
}

static size_t get_plmn(const char *buf, size_t len, char *plmn)
{
   char *cur = plmn;

   if (*buf && *buf != '"' && len >= 6 && memcmp(buf, "FFFFFF", 6)) {
      // according to TS 24.008 [9].
      // For instance, using 246 for the MCC and 81 for the MNC
      // and if this is stored in PLMN 3 the contents is as follows:
      // Bytes 7 to 9: '42' 'F6' '18'.
      // If storage for fewer than n PLMNs is required,
      // the unused bytes shall be set to 'FF'.
      append_plmn(&cur, buf[1]);
      append_plmn(&cur, buf[0]);
      append_plmn(&cur, buf[3]);
      append_plmn(&cur, buf[2]);
      append_plmn(&cur, buf[5]);
      append_plmn(&cur, buf[4]);
      *cur = 0;
      return cur - plmn;
   }
   return 0;
}

static bool has_service(const char *service_table, size_t len, int service)
{
   char digit[3];
   int index;
   int flags;
   int bit;

   index = (service / 8) * 2;
   LOG_DBG("Service %d, idx %d", service, index);
   if (index + 1 > len) {
      return false;
   }
   digit[0] = service_table[index];
   digit[1] = service_table[index + 1];
   digit[2] = 0;
   flags = (int)strtol(digit, NULL, 16);
   bit = 1 << ((service - 1) % 8);
   LOG_DBG("Service %d, '%c%c' 0x%02x 0x%02x %savailable.", service, digit[0], digit[1], flags, bit, flags & bit ? "" : "not ");
   return flags & bit;
}

static uint8_t check_service(uint8_t service_mask, uint8_t bit, const char *service_table, size_t len, int service)
{
   if (service_mask & bit) {
      if (!has_service(service_table, len, service)) {
         service_mask &= ~bit;
      }
   }
   return service_mask;
}

#define MODEM_PLMN_SELECTOR_SIZE (MODEM_PLMN_SIZE + 4)

static size_t find_plmns(const char *list, size_t len, char *plmn, size_t plmn_size)
{
   size_t result = 0;
   int select = 0;
   char access[MODEM_PLMN_SELECTOR_SIZE];

   int success = strstart(list, CRSM_SUCCESS, false);
   if (!success) {
      return 0;
   }

   list += success;
   len -= success;
   access[MODEM_PLMN_SELECTOR_SIZE - 1] = 0;
   while (*list && *list != '"' &&
          len >= (MODEM_PLMN_SELECTOR_SIZE - 1) &&
          plmn_size >= MODEM_PLMN_SIZE) {
      memcpy(access, list, MODEM_PLMN_SELECTOR_SIZE - 1);
      LOG_DBG("Check selector %s", access);
      select = (int)strtol(&access[MODEM_PLMN_SIZE - 1], NULL, 16);
      if (select == 0 || select & 0x4000) {
         if (memcmp(access, "FFFFFF", MODEM_PLMN_SIZE - 1)) {
            select = get_plmn(access, MODEM_PLMN_SIZE - 1, plmn);
            if (select) {
               plmn += select;
               *plmn++ = ',';
               *plmn = 0;
               ++select;
               plmn_size -= select;
               result += select;
            }
         }
      }
      list += 10;
      len -= 10;
   }
   if (result) {
      plmn--;
      *plmn = 0;
   }
   return result;
}

static size_t get_plmns(const char *list, size_t len, char *plmn, size_t plmn_size)
{
   size_t result = 0;
   int err = 0;

   int success = strstart(list, CRSM_SUCCESS, false);
   if (!success) {
      return 0;
   }

   list += success;
   len -= success;
   while (*list && *list != '"' &&
          len >= (MODEM_PLMN_SIZE - 1) &&
          plmn_size >= MODEM_PLMN_SIZE) {
      if (memcmp(list, "FFFFFF", MODEM_PLMN_SIZE - 1)) {
         err = get_plmn(list, MODEM_PLMN_SIZE - 1, plmn);
         if (err) {
            plmn += err;
            *plmn++ = ',';
            *plmn = 0;
            ++err;
            plmn_size -= err;
            result += err;
         }
      }
      list += (MODEM_PLMN_SIZE - 1);
      len -= (MODEM_PLMN_SIZE - 1);
   }
   if (result) {
      plmn--;
      *plmn = 0;
   }
   return result;
}

#define MAX_SIM_RETRIES 5
#define SIM_READ_RETRY_MILLIS 300
#define MAX_PLMNS 15
#define MAX_SIM_BYTES (MAX_PLMNS * 5)

#define SERVICE_20_BIT 1
#define SERVICE_42_BIT 2
#define SERVICE_43_BIT 4
#define SERVICE_47_BIT 8
#define SERVICE_71_BIT 16
#define SERVICE_96_BIT 32

static void modem_sim_read(bool init)
{
   static uint8_t service = 0xff;

   char buf[25 + MAX_SIM_BYTES * 2];
   char temp[MAX_PLMNS * MODEM_PLMN_SIZE];
   char plmn[MODEM_PLMN_SIZE];
   char c_plmn[MODEM_PLMN_SIZE];
   char mcc[4];
   int retries = 0;
   int res = 0;
   int start = 0;
   bool locked = false;

   if (init) {
      res = modem_at_lock_no_warn(K_FOREVER);
      if (res == -EBUSY) {
         return;
      }
      locked = true;
   }
   res = modem_at_cmd(buf, sizeof(buf), "%XICCID: ", "AT%XICCID");
   if (res == -EBUSY) {
      if (locked) {
         modem_at_unlock();
      }
      return;
   }
   while (res < 0 && retries < MAX_SIM_RETRIES) {
      ++retries;
      k_sleep(K_MSEC(SIM_READ_RETRY_MILLIS));
      if (locked && retries == MAX_SIM_RETRIES) {
         modem_at_unlock();
         locked = false;
      }
      res = modem_at_cmd(buf, sizeof(buf), "%XICCID: ", "AT%XICCID");
   }
   if (locked) {
      modem_at_unlock();
      locked = false;
   }
   if (res < 0) {
      LOG_INF("Failed to read ICCID.");
      return;
   } else {
      bool changed = false;
      memset(mcc, 0, sizeof(mcc));
      modem_get_mcc(mcc);
      k_mutex_lock(&sim_mutex, K_FOREVER);
      if (strcmp(sim_info.iccid, buf)) {
         // SIM card changed, clear infos
         memset(&sim_info, 0, sizeof(sim_info));
         strncpy(sim_info.iccid, buf, sizeof(sim_info.iccid) - 1);
         changed = true;
      }
      k_mutex_unlock(&sim_mutex);
      if (changed) {
         LOG_INF("iccid: %s (new)", buf);
         service = 0xff;
      } else {
         LOG_INF("iccid: %s", buf);
      }
   }

   if (init) {
      res = modem_at_lock_no_warn(K_FOREVER);
      if (res == -EBUSY) {
         return;
      }
      locked = true;
   }
   res = modem_at_cmdf(buf, sizeof(buf), NULL, "AT+CIMI");
   if (res == -EBUSY) {
      if (locked) {
         modem_at_unlock();
      }
      return;
   }
   while (res < 0 && retries < MAX_SIM_RETRIES) {
      ++retries;
      k_sleep(K_MSEC(SIM_READ_RETRY_MILLIS));
      if (locked && retries == MAX_SIM_RETRIES) {
         modem_at_unlock();
         locked = false;
      }
      res = modem_at_cmdf(buf, sizeof(buf), NULL, "AT+CIMI");
   }
   if (locked) {
      modem_at_unlock();
      locked = false;
   }

   if (res < 0) {
      LOG_INF("Failed to read IMSI.");
      return;
   } else {
      int64_t now = k_uptime_get();
      k_mutex_lock(&sim_mutex, K_FOREVER);
      if (strcmp(sim_info.imsi, buf)) {
         if (sim_info.imsi[0]) {
            strncpy(sim_info.prev_imsi, sim_info.imsi, sizeof(sim_info.prev_imsi) - 1);
            sim_info.imsi_interval = MSEC_TO_SEC((now - imsi_time));
            sim_info.imsi_counter++;
         }
         strncpy(sim_info.imsi, buf, sizeof(sim_info.imsi) - 1);
         imsi_time = now;
      } else if (init) {
         imsi_time = now;
      }
      k_mutex_unlock(&sim_mutex);
      if (sim_info.prev_imsi[0]) {
         LOG_INF("multi-imsi: %s (%s, %d seconds)", buf,
                 sim_info.prev_imsi, sim_info.imsi_interval);
      } else {
         LOG_INF("imsi: %s", buf);
      }
   }

   /* 0x6FAD, check for eDRX SIM suspend support*/
   res = modem_at_cmd(buf, sizeof(buf), "+CRSM: ", "AT+CRSM=176,28589,0,0,0");
   if (res < 0) {
      LOG_INF("Failed to read CRSM eDRX.");
      return;
   } else {
      LOG_DBG("CRSM eDRX: %s", buf);
      res = strstart(buf, CRSM_SUCCESS, false);
      if (res) {
         // successful read
         char n = buf[res + 6]; // byte 3, low nibble
         if (n > '7') {
            LOG_INF("eDRX cycle supported.");
         } else {
            LOG_INF("eDRX cycle not supported.");
         }
         k_mutex_lock(&sim_mutex, K_FOREVER);
         sim_info.edrx_cycle_support = (n > '7');
         k_mutex_unlock(&sim_mutex);
      }
   }

   /* 0x6F31, Higher Priority PLMN search period */
   res = modem_at_cmd(buf, sizeof(buf), "+CRSM: ", "AT+CRSM=176,28465,0,0,0");
   if (res < 0) {
      LOG_INF("Failed to read CRSM HPPLMN period.");
      return;
   } else {
      LOG_DBG("CRSM hpplmn: %s", buf);
      res = strstart(buf, CRSM_SUCCESS, false);
      if (res) {
         // successful read
         int interval = (int)strtol(&buf[res], NULL, 16);
         interval *= 2;
         if (interval > 80) {
            interval *= 2;
            if (interval > 240) {
               interval = 240;
            }
         }
         LOG_INF("HPPLMN search interval: %d [h]", interval);
         k_mutex_lock(&sim_mutex, K_FOREVER);
         sim_info.hpplmn_search_interval = (int16_t)interval;
         k_mutex_unlock(&sim_mutex);
      }
   }

   /* 0x6F38, Service table */
   res = modem_at_cmd(buf, sizeof(buf), "+CRSM: ", "AT+CRSM=176,28472,0,0,40");
   if (res < 0) {
      LOG_INF("Failed to read CRSM service table.");
      return;
   } else {
      LOG_DBG("CRSM service table: %s", buf);
      start = strstart(buf, CRSM_SUCCESS, false);
      if (start) {
         char *table = &buf[start];
         /* user controlled PLMN selector */
         res -= start;
         service = check_service(service, SERVICE_20_BIT, table, res, 20);
         /* operator controlled PLMN selector */
         service = check_service(service, SERVICE_42_BIT, table, res, 42);
         /* Home PLMN selector */
         service = check_service(service, SERVICE_43_BIT, table, res, 43);
         /* Mailbox Dialling Numbers */
         service = check_service(service, SERVICE_47_BIT, table, res, 47);
         /* Equivalent Home PLMN */
         service = check_service(service, SERVICE_71_BIT, table, res, 71);
         /* Non Access Stratum Configuration */
         service = check_service(service, SERVICE_96_BIT, table, res, 96);
      }
   }

   if (service & SERVICE_71_BIT) {
      /* 0x6FD9, Serv. 71, equivalent H(ome)PLMN, 15*3 */
      res = modem_at_cmdf(buf, sizeof(buf), "+CRSM: ", "AT+CRSM=176,28633,0,0,%d", MAX_PLMNS * 3);
      if (res < 0) {
         LOG_INF("Failed to read CRSM eq. home plmn.");
         return;
      } else {
         LOG_DBG("CRSM eq. home plmn: %s", buf);
         res = get_plmns(buf, res, temp, sizeof(temp));
         if (res) {
            LOG_INF("CRSM eq. home plmn: %s", temp);
         } else {
            LOG_INF("CRSM no eq. home plmn");
         }
      }
   }

   memset(plmn, 0, sizeof(plmn));
   memset(c_plmn, 0, sizeof(c_plmn));

   if (service & SERVICE_43_BIT) {
      /*
       * 0x6F62, Serv. 43, H(ome)PLMN selector, 15*5,
       * only used to determine access technology for (Equivalent)H(ome)PLMN
       */
      res = modem_at_cmdf(buf, sizeof(buf), "+CRSM: ", "AT+CRSM=176,28514,0,0,%d", MAX_PLMNS * 5);
      if (res < 0) {
         LOG_INF("Failed to read CRSM home plmn sel.");
         return;
      } else {
         LOG_DBG("CRSM home plmn sel: %s", buf);
         res = find_plmns(buf, res, temp, sizeof(temp));
         if (res) {
            LOG_INF("CRSM home plmn sel: %s", temp);
            if (!plmn[0]) {
               copy_plmn(temp, plmn, sizeof(plmn), NULL);
            }
            if (!c_plmn[0] && mcc[0]) {
               copy_plmn(temp, c_plmn, sizeof(c_plmn), mcc);
            }
         } else {
            LOG_INF("CRSM no home plmn sel");
         }
      }
   }
   if (service & SERVICE_20_BIT) {
      /* 0x6F60, Serv. 20, User controlled PLMN selector, 15*5 */
      res = modem_at_cmdf(buf, sizeof(buf), "+CRSM: ", "AT+CRSM=176,28512,0,0,%d", MAX_PLMNS * 5);
      if (res < 0) {
         LOG_INF("Failed to read CRSM user plmn sel.");
         return;
      } else {
         LOG_DBG("CRSM user plmn sel: %s", buf);
#ifdef CONFIG_USER_PLMN_SELECTOR
         start = strstart(buf, CRSM_SUCCESS, false);
         if (start) {
            int len = strlen(CONFIG_USER_PLMN_SELECTOR);
            int comp_len = res - start - 1;
            if (len < comp_len) {
               comp_len = len;
            }
            if (strncmp(buf + start, CONFIG_USER_PLMN_SELECTOR, comp_len) != 0) {
               res = modem_at_cmdf(buf, sizeof(buf), "+CRSM: ", "AT+CRSM=214,28512,0,0,%d,\"%s\"", len / 2, CONFIG_USER_PLMN_SELECTOR);
               if (res >= 0) {
                  if (strstart(buf, CRSM_SUCCESS, false)) {
                     LOG_INF("CRSM user plmn sel written.");
                  } else {
                     LOG_WRN("CRSM user plmn sel not written.");
                  }
               }
               res = modem_at_cmdf(buf, sizeof(buf), "+CRSM: ", "AT+CRSM=176,28512,0,0,%d", MAX_PLMNS * 5);
            }
         }
#endif
         res = find_plmns(buf, res, temp, sizeof(temp));
         if (res) {
            LOG_INF("CRSM user plmn sel: %s", temp);
            if (!plmn[0]) {
               copy_plmn(temp, plmn, sizeof(plmn), NULL);
            }
            if (!c_plmn[0] && mcc[0]) {
               copy_plmn(temp, c_plmn, sizeof(c_plmn), mcc);
            }
         } else {
            LOG_INF("CRSM no user plmn sel");
         }
      }
   }

   if (service & SERVICE_42_BIT) {
      /* 0x6F61, Serv. 42, Operator controlled PLMN selector, 15*5 */
      res = modem_at_cmdf(buf, sizeof(buf), "+CRSM: ", "AT+CRSM=176,28513,0,0,%d", MAX_PLMNS * 5);
      if (res < 0) {
         LOG_INF("Failed to read CRSM operator plmn sel.");
         return;
      } else {
         LOG_DBG("CRSM operator plmn sel: %s", buf);
         res = find_plmns(buf, res, temp, sizeof(temp));
         if (res) {
            LOG_INF("CRSM operator plmn sel: %s", temp);
            if (!plmn[0]) {
               copy_plmn(temp, plmn, sizeof(plmn), NULL);
            }
            if (!c_plmn[0] && mcc[0]) {
               copy_plmn(temp, c_plmn, sizeof(c_plmn), mcc);
            }
         } else {
            LOG_INF("CRSM no operator plmn sel");
         }
      }
   }
   k_mutex_lock(&sim_mutex, K_FOREVER);
   if (c_plmn[0]) {
      strcpy(sim_info.hpplmn, c_plmn);
   } else if (plmn[0]) {
      strcpy(sim_info.hpplmn, plmn);
   } else {
      sim_info.hpplmn[0] = 0;
   }
   sim_info.valid = true;
   k_mutex_unlock(&sim_mutex);
   if (plmn[0] || c_plmn[0]) {
      if (mcc[0]) {
         if (c_plmn[0]) {
            LOG_INF("HPPLMN %s/%s/%s", mcc, c_plmn, plmn);
         } else {
            LOG_INF("HPPLMN %s/-/%s", mcc, plmn);
         }
      } else {
         LOG_INF("HPPLMN %s", plmn);
      }
   } else {
      LOG_INF("No HPPLMN configured");
   }

   /* 0x6F7B, Forbidden PLMNs, 15*3 */
   res = modem_at_cmdf(buf, sizeof(buf), "+CRSM: ", "AT+CRSM=176,28539,0,0,%d", MAX_PLMNS * 3);
   if (res < 0) {
      LOG_INF("Failed to read CRSM forbidden plmn.");
      return;
   } else {
      LOG_DBG("CRSM forbidden plmn: %s", buf);
#ifdef CONFIG_FORBIDDEN_PLMN
      start = strstart(buf, CRSM_SUCCESS, false);
      if (start) {
         int len = strlen(CONFIG_FORBIDDEN_PLMN);
         int comp_len = res - start - 1;
         if (len < comp_len) {
            comp_len = len;
         }
         if (strncmp(buf + start, CONFIG_FORBIDDEN_PLMN, comp_len) != 0) {
            res = modem_at_cmdf(buf, sizeof(buf), "+CRSM: ", "AT+CRSM=214,28539,0,0,%d,\"%s\"", len / 2, CONFIG_FORBIDDEN_PLMN);
            if (res >= 0) {
               if (strstart(buf, CRSM_SUCCESS, false)) {
                  LOG_INF("Forbidden PLMN written (%d bytes).", len);
               } else {
                  LOG_WRN("Forbidden PLMN not written (%d bytes).", len);
               }
            }
            res = modem_at_cmdf(buf, sizeof(buf), "+CRSM: ", "AT+CRSM=176,28539,0,0,%d", MAX_PLMNS * 3);
            if (res > 0) {
               LOG_INF("CRSM forbidden plmn: %s", buf);
            }
         }
      }
#endif
      memset(plmn, 0, sizeof(plmn));
      res = get_plmns(buf, res, temp, sizeof(temp));
      if (res) {
         LOG_INF("CRSM forbidden plmn: %s", temp);
         if (!copy_plmn(temp, plmn, sizeof(plmn), mcc)) {
            copy_plmn(temp, plmn, sizeof(plmn), NULL);
         }
      } else {
         LOG_INF("CRSM no forbidden plmn");
      }
      k_mutex_lock(&sim_mutex, K_FOREVER);
      strcpy(sim_info.forbidden, plmn);
      k_mutex_unlock(&sim_mutex);
   }

   if (service & SERVICE_47_BIT) {
      /* 0x6FC8, Serv. 47, Mailbox Dialling Numbers */
      res = modem_at_cmd(buf, sizeof(buf), "+CRSM: ", "AT+CRSM=178,28616,1,4,13");
      if (res < 0) {
         LOG_INF("Failed to read CRSM MBDN/EXT6.");
         return;
      } else {
         start = strstart(buf, CRSM_SUCCESS, false);
         if (start) {
            LOG_INF("CRSM MBDN/EXT6: %s", buf);
         } else {
            service &= ~SERVICE_47_BIT;
         }
      }
   }

   if (service & SERVICE_96_BIT) {
      /* 0x6FE8, Serv. 96, NAS Config */
      res = modem_at_cmdf(buf, sizeof(buf), "+CRSM: ", "AT+CRSM=176,28648,0,0,%d", MAX_SIM_BYTES);
      if (res < 0) {
         LOG_INF("Failed to read CRSM NAS config.");
         return;
      } else {
         LOG_INF("CRSM NAS config: %s", buf);
      }
   }
}

void modem_sim_init(void)
{
   k_mutex_lock(&sim_mutex, K_FOREVER);
   memset(&sim_info, 0, sizeof(sim_info));
   imsi_time = 0;
   sim_info.hpplmn_search_interval = -ENODATA;
   k_mutex_unlock(&sim_mutex);
}

bool modem_sim_multi_imsi(void)
{
   bool multi;
   k_mutex_lock(&sim_mutex, K_FOREVER);
   multi = sim_info.prev_imsi[0];
   k_mutex_unlock(&sim_mutex);
   return multi;
}

bool modem_sim_apply_iccid_preference(void)
{
#if defined(CONFIG_MODEM_ICCID_LTE_M_PREFERENCE) || defined(CONFIG_MODEM_ICCID_NBIOT_PREFERENCE)
   char iccid[6];

   memset(&iccid, 0, sizeof(iccid));
   k_mutex_lock(&sim_mutex, K_FOREVER);
   memcpy(iccid, sim_info.iccid, sizeof(iccid) - 1);
   k_mutex_unlock(&sim_mutex);

   if (iccid[0]) {
#ifdef CONFIG_MODEM_ICCID_LTE_M_PREFERENCE
      if (find_id(CONFIG_MODEM_ICCID_LTE_M_PREFERENCE, iccid)) {
         LOG_INF("Found ICCID %s in LTE-M preference list.", iccid);
         modem_set_preference(LTE_M_PREFERENCE);
         return true;
      }
#endif
#ifdef CONFIG_MODEM_ICCID_NBIOT_PREFERENCE
      if (find_id(CONFIG_MODEM_ICCID_NBIOT_PREFERENCE, iccid)) {
         LOG_INF("Found ICCID %s in NB-IoT preference list.", iccid);
         modem_set_preference(NBIOT_PREFERENCE);
         return true;
      }
#endif
   }
#endif
   return false;
}

int modem_sim_get_info(struct lte_sim_info *info)
{
   int res = 0;
   k_mutex_lock(&sim_mutex, K_FOREVER);
   if (info) {
      *info = sim_info;
   }
   if (sim_info.prev_imsi[0]) {
      res = 1;
   }
   k_mutex_unlock(&sim_mutex);
   return res;
}

int modem_sim_read_info(struct lte_sim_info *info, bool init)
{
   int res = 0;
   modem_sim_read(init);
   k_mutex_lock(&sim_mutex, K_FOREVER);
   if (info) {
      *info = sim_info;
   }
   if (sim_info.prev_imsi[0]) {
      res = 1;
   }
   k_mutex_unlock(&sim_mutex);
   return res;
}

#endif
