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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "appl_diagnose.h"
#include "io_job_queue.h"
#include "modem.h"
#include "modem_at.h"
#include "modem_sim.h"
#include "parse.h"
#include "sh_cmd.h"

LOG_MODULE_DECLARE(MODEM, CONFIG_MODEM_LOG_LEVEL);

#if defined(CONFIG_NRF_MODEM_LIB)

#define MSEC_TO_SEC(X) (((X) + (MSEC_PER_SEC / 2)) / MSEC_PER_SEC)

#define MULTI_IMSI_MINIMUM_TIMEOUT_MS (300 * MSEC_PER_SEC)

#define MAX_PLMNS 15
#define MAX_SIM_BYTES (MAX_PLMNS * 5)

static K_MUTEX_DEFINE(sim_mutex);

#define SIM_STATUS_SELECT_IMSI 0
#define SIM_STATUS_TEST_IMSI 1

static atomic_t sim_status = ATOMIC_INIT(0);
static atomic_t imsi_success = ATOMIC_INIT(-1);

static struct lte_sim_info sim_info;

static int64_t imsi_time = 0;

static const char *find_id(const char *buf, const char *id)
{
   size_t len = strlen(id);
   char *pos = strstr(buf, id);
   while (pos) {
      // start
      if (pos == buf || *(pos - 1) == ',') {
         // end
         if (pos[len] == ',' || !pos[len]) {
            return pos;
         }
      }
      pos = strstr(pos + 1, id);
   }
   return pos;
}

static int flip_digits(char *buf, size_t len)
{
   int res = 0;
   if (2 <= len) {
      len -= 2;
      while (buf[res] && res <= len) {
         char t = buf[res];
         buf[res] = buf[res + 1];
         ++res;
         buf[res] = t;
         ++res;
      }
   }
   return res;
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
#define CRSM_HEADER_SIZE 25

static void modem_sim_log_imsi_sel(unsigned int selected)
{
   unsigned int select = selected >> 8;
   selected &= 0xff;
   if (select == 0) {
      LOG_INF("SIM auto select, imsi %u selected", selected);
   } else if (select == selected) {
      LOG_INF("SIM imsi %u selected", selected);
   } else {
      LOG_INF("SIM imsi %u pending", select);
   }
}

static int modem_sim_get_imsi_sel(unsigned int selected)
{
   unsigned int select = selected >> 8;
   selected &= 0xff;
   if (select == 0) {
      return 0;
   } else if (select == selected) {
      return select == 0xff ? 0 : select;
   } else {
      return -EINVAL;
   }
}

static int modem_sim_read_imsi_sel(unsigned int *selected)
{
   char buf[64];

   int res = modem_at_cmd(buf, sizeof(buf), "+CRSM: ", "AT+CRSM=178,28616,1,4,13");
   if (res <= 0) {
      return res;
   }
   res = strstart(buf, CRSM_SUCCESS, false);
   if (!res) {
      LOG_DBG("SIM read imsi ID failed, %s", buf);
      return -ENOTSUP;
   }
   // SSSSUU 4 digits select, 2 digits used
   buf[res + 6] = 0;
   return sscanf(buf + res, "%x", selected);
}

static int modem_sim_write_imsi_sel(unsigned int select, bool restart, const char *action)
{
   char buf[64];
   unsigned int selected = 0;
   int res = modem_at_cmdf(buf, sizeof(buf), "+CRSM: ", "AT+CRSM=220,28616,1,4,13,\"%04xFFFFFFFFFFFFFFFFFFFFFF\"", select);
   if (res <= 0) {
      return res;
   }
   res = strstart(buf, CRSM_SUCCESS, false);
   if (res > 0) {
      atomic_set_bit(&sim_status, SIM_STATUS_SELECT_IMSI);
      if (restart) {
         modem_at_push_off(false);
         modem_at_restore();
         res = modem_sim_read_imsi_sel(&selected);
         if (res == 1) {
            if (select == 0) {
               LOG_INF("SIM %s auto select, imsi %u selected.", action, selected);
            } else if (select == (selected & 0xff)) {
               LOG_INF("SIM %s imsi %u gets selected.", action, select);
            } else {
               LOG_INF("SIM %s imsi %u not selected.", action, select);
            }
         }
      } else {
         LOG_INF("SIM %s imsi %u written", action, select);
      }
   } else {
      LOG_INF("SIM %s writing imsi %u failed, %s", action, select, buf);
   }
   return res;
}

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

static size_t encode_plmn(const char *buf, char *encoded_plmn, size_t len)
{
   int l = strspn(buf, "0123456789ABCDEF");

   if ((5 == l || 6 == l) && len > 6) {
      // according to TS 24.008 [9].
      // For instance, using 246 for the MCC and 81 for the MNC
      // and if this is stored in PLMN 3 the contents is as follows:
      // Bytes 7 to 9: '42' 'F6' '18'.
      encoded_plmn[0] = buf[1];
      encoded_plmn[1] = buf[0];
      encoded_plmn[3] = buf[2];
      if (6 == l) {
         encoded_plmn[2] = buf[3];
         encoded_plmn[4] = buf[5];
         encoded_plmn[5] = buf[4];
      } else {
         encoded_plmn[2] = 'F';
         encoded_plmn[4] = buf[4];
         encoded_plmn[5] = buf[3];
      }
      encoded_plmn[6] = 0;
      return 6;
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

static int modem_sim_read_forbidden_list(char *buf, size_t buf_len, char *plmns, size_t plmns_len)
{
   int res = 0;
   int plmn_bytes = MAX_PLMNS * 3;

   if (CRSM_HEADER_SIZE + plmn_bytes * 2 > buf_len) {
      plmn_bytes = (buf_len - CRSM_HEADER_SIZE) / 6;
   }

   memset(buf, 0, buf_len);
   res = modem_at_cmdf(buf, buf_len, "+CRSM: ", "AT+CRSM=176,28539,0,0,%d", plmn_bytes);
   if (res < 0) {
      LOG_INF("Failed to read CRSM forbidden plmn.");
   } else {
      LOG_DBG("CRSM forbidden plmn: %s", buf);
      if (plmns) {
         memset(plmns, 0, plmns_len);
         res = get_plmns(buf, res, plmns, plmns_len);
         if (res) {
            LOG_INF("CRSM forbidden plmn: %s", plmns);
         } else {
            LOG_INF("CRSM no forbidden plmn");
         }
      } else {
         int skip = strstart(buf, CRSM_SUCCESS, false);
         if (skip > 0) {
            res -= (skip + 1);
            memmove(buf, &buf[skip], res);
            buf[res] = 0;
         } else {
            res = -EINVAL;
         }
      }
   }
   return res;
}

#define MAX_SIM_RETRIES 5
#define SIM_READ_RETRY_MILLIS 300

#define SERVICE_20_BIT 1
#define SERVICE_42_BIT 2
#define SERVICE_43_BIT 4
#define SERVICE_71_BIT 8
#define SERVICE_96_BIT 16

static int modem_sim_read_with_retry(int retries, char *buf, size_t len, const char *skip, const char *cmd)
{
   int res = modem_at_cmd(buf, len, skip, cmd);
   if (res == -EBUSY) {
      return res;
   }
   while (res < 0 && retries > 0) {
      --retries;
      k_sleep(K_MSEC(SIM_READ_RETRY_MILLIS));
      res = modem_at_cmd(buf, len, skip, cmd);
   }
   return res;
}

static int modem_sim_read_locked_with_retry(int retries, char *buf, size_t len, const char *skip, const char *cmd)
{
   int res = modem_at_lock_no_warn(K_FOREVER);
   if (!res) {
      res = modem_sim_read_with_retry(retries - 1, buf, len, skip, cmd);
      modem_at_unlock();
      if (res) {
         res = modem_sim_read_with_retry(0, buf, len, skip, cmd);
      }
   }
   return res;
}

static int modem_cmd_read_iccid(bool init, char *buf, size_t len)
{
   int res = 0;

   memset(buf, 0, len);
   if (init) {
      res = modem_sim_read_locked_with_retry(MAX_SIM_RETRIES, buf, len, "+CRSM: ", "AT+CRSM=176,12258,0,0,12");
   } else {
      res = modem_sim_read_with_retry(MAX_SIM_RETRIES, buf, len, "+CRSM: ", "AT+CRSM=176,12258,0,0,12");
   }
   if (res > 0) {
      LOG_DBG("SIM ICCID: %s", buf);
      int skip = strstart(buf, CRSM_SUCCESS, false);
      if (skip > 0) {
         res -= (skip + 1);
         memmove(buf, &buf[skip], res);
         buf[res] = 0;
         flip_digits(buf, res);
         LOG_DBG("Read ICCID: %s", buf);
      } else {
         LOG_DBG("Read ICCID failed: %s", buf);
         res = -EINVAL;
      }
   }

   return res;
}

static void modem_sim_read(bool init)
{
   static uint8_t service = 0xff;

   bool imsi_select = false;
   char buf[CRSM_HEADER_SIZE + MAX_SIM_BYTES * 2];
   char temp[MAX_PLMNS * MODEM_PLMN_SIZE];
   char plmn[MODEM_PLMN_SIZE];
   char c_plmn[MODEM_PLMN_SIZE];
   char mcc[4];
   int res = 0;
   int start = 0;

   res = modem_cmd_read_iccid(init, buf, sizeof(buf));
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
#if defined(CONFIG_MODEM_ICCID_IMSI_SELECT)
         if (sim_info.iccid[0]) {
            char iccid[6];

            memset(&iccid, 0, sizeof(iccid));
            memcpy(iccid, sim_info.iccid, sizeof(iccid) - 1);

            if (find_id(CONFIG_MODEM_ICCID_IMSI_SELECT, iccid)) {
               LOG_INF("Found ICCID %s in IMSI select support list.", iccid);
               sim_info.imsi_select_support = true;
               imsi_select = true;
            }
         }
#endif
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
      res = modem_sim_read_locked_with_retry(MAX_SIM_RETRIES, buf, sizeof(buf), NULL, "AT+CIMI");
   } else {
      res = modem_sim_read_with_retry(MAX_SIM_RETRIES, buf, sizeof(buf), NULL, "AT+CIMI");
   }
   if (res < 0) {
      LOG_INF("Failed to read IMSI.");
      return;
   } else {
      int64_t now = k_uptime_get();
      bool selected = false;
      int imsi = 0;
      k_mutex_lock(&sim_mutex, K_FOREVER);
      if (strcmp(sim_info.imsi, buf)) {
         selected = atomic_test_and_clear_bit(&sim_status, SIM_STATUS_SELECT_IMSI);
         if (sim_info.imsi[0]) {
            strncpy(sim_info.prev_imsi, sim_info.imsi, sizeof(sim_info.prev_imsi) - 1);
            if (!selected) {
               imsi_time = MSEC_TO_SEC(now - imsi_time);
               sim_info.imsi_interval = MIN(imsi_time, 30000);
            }
            sim_info.imsi_counter++;
         }
         strncpy(sim_info.imsi, buf, sizeof(sim_info.imsi) - 1);
         imsi_time = now;
      } else if (init) {
         imsi_time = now;
      }
      imsi = modem_sim_get_imsi_sel(sim_info.imsi_select);
      k_mutex_unlock(&sim_mutex);
      if (modem_sim_automatic_multi_imsi()) {
         LOG_INF("multi-imsi: %s (%s, %d seconds)", buf, sim_info.prev_imsi, sim_info.imsi_interval);
      } else if (sim_info.imsi_select_support && imsi >= 0) {
         LOG_INF("multi-imsi: %s (%d imsi)", buf, imsi);
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
         res -= start;
         /* user controlled PLMN selector */
         service = check_service(service, SERVICE_20_BIT, table, res, 20);
         /* operator controlled PLMN selector */
         service = check_service(service, SERVICE_42_BIT, table, res, 42);
         /* Home PLMN selector */
         service = check_service(service, SERVICE_43_BIT, table, res, 43);
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
   res = modem_sim_read_forbidden_list(buf, sizeof(buf), temp, sizeof(temp));
   if (res >= 0) {
      memset(plmn, 0, sizeof(plmn));
      if (!copy_plmn(temp, plmn, sizeof(plmn), mcc)) {
         copy_plmn(temp, plmn, sizeof(plmn), NULL);
      }
      k_mutex_lock(&sim_mutex, K_FOREVER);
      strcpy(sim_info.forbidden, plmn);
      k_mutex_unlock(&sim_mutex);
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

   if (imsi_select) {
      unsigned int selected = 0;
      if (modem_sim_read_imsi_sel(&selected) == 1) {
         k_mutex_lock(&sim_mutex, K_FOREVER);
         sim_info.imsi_select = selected;
         k_mutex_unlock(&sim_mutex);
         modem_sim_log_imsi_sel(selected);
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

void modem_sim_network(bool registered)
{
   static bool network_registered = false;

   if (network_registered != registered) {
      network_registered = registered;
      if (!registered) {
         k_mutex_lock(&sim_mutex, K_FOREVER);
         imsi_time = k_uptime_get();
         k_mutex_unlock(&sim_mutex);
      }
   }
}

bool modem_sim_automatic_multi_imsi(void)
{
   bool multi = false;

   k_mutex_lock(&sim_mutex, K_FOREVER);
   if (sim_info.prev_imsi[0]) {
      multi = !sim_info.imsi_select_support || !modem_sim_get_imsi_sel(sim_info.imsi_select);
   }
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
   if (modem_sim_automatic_multi_imsi()) {
      res = 1;
   }
   k_mutex_unlock(&sim_mutex);
   return res;
}

int modem_sim_read_info(struct lte_sim_info *info, bool init)
{
   modem_sim_read(init);
   return modem_sim_get_info(info);
}

static void modem_cmd_sim_reset_fn(struct k_work *work)
{
   ARG_UNUSED(work);
   modem_sim_reset(true);
}

static K_WORK_DELAYABLE_DEFINE(modem_cmd_sim_reset_work, modem_cmd_sim_reset_fn);

int modem_sim_ready(void)
{
   int sim_info_select = -1;

   k_mutex_lock(&sim_mutex, K_FOREVER);
   if (sim_info.imsi_select_support) {
      sim_info_select = sim_info.imsi_select;
   }
   k_mutex_unlock(&sim_mutex);
   if (0 <= sim_info_select) {
      unsigned int select = 0;
      if (modem_sim_read_imsi_sel(&select) == 1) {
         int imsi = modem_sim_get_imsi_sel(select);
         int sim_info_imsi = modem_sim_get_imsi_sel(sim_info_select);
         if (0 <= imsi && imsi == sim_info_imsi) {
            atomic_set(&imsi_success, imsi);
            atomic_clear_bit(&sim_status, SIM_STATUS_TEST_IMSI);
            k_work_cancel_delayable(&modem_cmd_sim_reset_work);
            LOG_INF("SIM imsi %u successful registered.", imsi);
         } else {
            LOG_INF("SIM imsi %u changed while register, was %u.", imsi, sim_info_imsi);
         }
      } else {
         LOG_INF("SIM read imsi ID failed on register.");
      }
   }
   return 0;
}

int modem_sim_reset(bool restart)
{
   if (atomic_test_and_clear_bit(&sim_status, SIM_STATUS_TEST_IMSI)) {
      int imsi = atomic_clear(&imsi_success);
      atomic_set(&imsi_success, -1);
      k_work_cancel_delayable(&modem_cmd_sim_reset_work);
      if (0 <= imsi) {
         modem_sim_write_imsi_sel(imsi, restart, "restore");
      } else {
         LOG_INF("SIM no imsi to restore.");
      }
   } else {
      LOG_INF("SIM no imsi-test pending.");
   }
   return 0;
}

#if defined(CONFIG_LTE_LINK_CONTROL)

LTE_LC_ON_CFUN(modem_sim_on_cfun_hook, modem_sim_on_cfun, NULL);

static void modem_sim_on_cfun(enum lte_lc_func_mode mode, void *ctx)
{
   if (mode == LTE_LC_FUNC_MODE_NORMAL ||
       mode == LTE_LC_FUNC_MODE_ACTIVATE_LTE) {
      k_mutex_lock(&sim_mutex, K_FOREVER);
      imsi_time = k_uptime_get();
      k_mutex_unlock(&sim_mutex);
   }
}
#endif /* CONFIG_LTE_LINK_CONTROL */

#ifdef CONFIG_SH_CMD

static int modem_cmd_sim(const char *parameter)
{
   (void)parameter;
   modem_sim_read_info(NULL, true);
   return 0;
}

static int modem_cmd_iccid(const char *parameter)
{
   (void)parameter;
   int res = 0;
   char buf[64];

   res = modem_cmd_read_iccid(false, buf, sizeof(buf));
   if (res > 0) {
      LOG_INF("iccid: %s", buf);
   } else {
      LOG_INF("SIM failed to read ICCID.");
   }

   return 0;
}

static int modem_cmd_imsi_sel(const char *parameter)
{
   unsigned int select = 0;
   unsigned int selected = 0;
   const char *cur = parameter;
   char buf[64];

   int res = modem_sim_read_imsi_sel(&selected);
   if (res == 1) {
      int imsi = atomic_get(&imsi_success);
      cur = parse_next_text(cur, ' ', buf, sizeof(buf));
      if (!buf[0]) {
         // show selection
         modem_sim_log_imsi_sel(selected);
         if (atomic_test_bit(&sim_status, SIM_STATUS_TEST_IMSI)) {
            if (0 <= imsi) {
               LOG_INF("(SIM imsi %u for restore.)", imsi);
            }
         }
      } else {
         bool force = false;
         if (stricmp(buf, "force") == 0) {
            force = true;
            cur = parse_next_text(cur, ' ', buf, sizeof(buf));
            if (!buf[0]) {
               LOG_INF("imsi %s 'force' requires select <n>!", parameter);
               return -EINVAL;
            }
         }
         if (stricmp(buf, "auto")) {
            res = sscanf(buf, "%u", &select);
         }
         /* if "auto" res is already 1 and select is 0 */
         if (res == 1) {
            if (select > 255) {
               LOG_INF("imsi select %u is out of range [0..255].", select);
               return -EINVAL;
            } else if (select == (selected >> 8)) {
               LOG_INF("SIM imsi %u already selected.", select);
            } else {
               res = modem_sim_write_imsi_sel(select, true, force ? "force" : "test");
               if (!force && res == 1 && 0 < select && select < 255 && 0 <= imsi) {
                  atomic_set_bit(&sim_status, SIM_STATUS_TEST_IMSI);
                  LOG_INF("SIM remember imsi %d to restore.", imsi);
                  work_reschedule_for_io_queue(&modem_cmd_sim_reset_work, K_MINUTES(CONFIG_MODEM_SEARCH_TIMEOUT_IMSI));
               }
            }
         } else {
            LOG_INF("imsi %s invalid argument!", parameter);
            return -EINVAL;
         }
      }
   } else if (res == -ENOTSUP) {
      LOG_INF("SIM imsi selection not supported.");
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
   LOG_INF("  imsi           : show current IMSI selection.");
   LOG_INF("  imsi auto      : select IMSI automatically. Switching IMSI on timeout (300s).");
   LOG_INF("  imsi <n>       : select IMSI. Values 0 to 255.");
   LOG_INF("  imsi 0         : select IMSI automatically. Switching IMSI on timeout (300s).");
   LOG_INF("  imsi 1         : select IMSI 1. Fallback to latest successful IMSI.");
   LOG_INF("  imsi n         : select IMSI. The largest value depends on the SIM card");
   LOG_INF("  imsi force <n> : select IMSI. No fallback!");
}

static int modem_cmd_banclr(const char *parameter)
{
   (void)parameter;
   char buf[25 + (MAX_PLMNS * 6)];
   size_t len = MAX_PLMNS * 6;

   int res = modem_sim_read_forbidden_list(buf, sizeof(buf), NULL, 0);
   if (0 < res) {
      int l = strspn(buf, "F");
      if (l == res) {
         LOG_INF("Forbidden PLMNs already cleared.");
         res = 0;
      }
      len = res;
   }
   if (len) {
      memset(buf, 'F', len);
      buf[len] = 0;
      res = modem_at_cmdf(buf, sizeof(buf), "+CRSM: ", "AT+CRSM=214,28539,0,0,%d,\"%s\"", len / 2, buf);
      if (res >= 0) {
         if (strstart(buf, CRSM_SUCCESS, false)) {
            LOG_INF("Forbidden PLMNs cleared (%d bytes).", len);
         } else {
            LOG_WRN("Forbidden PLMNs not cleared (%d bytes).", len);
         }
         res = 0;
      }
   }
   return res;
}

static int modem_cmd_ban(const char *parameter)
{
   const char *cur = parameter;
   char plmn[7];
   char buf[25 + (MAX_PLMNS * 6)];
   int res = 0;

   memset(buf, 0, sizeof(buf));
   memset(plmn, 0, sizeof(plmn));
   cur = parse_next_text(cur, ' ', plmn, sizeof(plmn));

   if (plmn[0]) {
      size_t len = 0;
      while (plmn[0]) {
         len += encode_plmn(plmn, &buf[len], sizeof(buf) - len);
         memset(plmn, 0, sizeof(plmn));
         cur = parse_next_text(cur, ' ', plmn, sizeof(plmn));
      }
      if (len) {
         res = modem_at_cmdf(buf, sizeof(buf), "+CRSM: ", "AT+CRSM=214,28539,0,0,%d,\"%s\"", len / 2, buf);
         if (res >= 0) {
            if (strstart(buf, CRSM_SUCCESS, false)) {
               LOG_INF("Forbidden PLMN written (%d bytes).", len);
            } else {
               LOG_WRN("Forbidden PLMN not written (%d bytes).", len);
            }
            res = 0;
         }
      }
   } else {
      char temp[MAX_PLMNS * MODEM_PLMN_SIZE];
      res = modem_sim_read_forbidden_list(buf, sizeof(buf), temp, sizeof(temp));
      if (res > 0) {
         res = 0;
      }
   }
   return res;
}

static void modem_cmd_ban_help(void)
{
   LOG_INF("> help ban:");
   LOG_INF("  ban                       : show ban-list.");
   LOG_INF("  ban <plmn> [<plmn-2> ...] : set plmn(s) as ban-list.");
}

SH_CMD(sim, "", "read SIM-card info.", modem_cmd_sim, NULL, 0);
SH_CMD(imsi, "", "select IMSI.", modem_cmd_imsi_sel, modem_cmd_imsi_sel_help, 0);
SH_CMD(iccid, "", "read ICCID.", modem_cmd_iccid, NULL, 0);
SH_CMD(banclr, "", "clear forbidden PLMN list (SIM-card).", modem_cmd_banclr, NULL, 0);
SH_CMD(ban, "", "add PLMN to forbidden list (SIM-card).", modem_cmd_ban, modem_cmd_ban_help, 0);

#endif /* CONFIG_SH_CMD */
#endif /* CONFIG_NRF_MODEM_LIB */
