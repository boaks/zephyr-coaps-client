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
#include <string.h>
#include <zephyr/random/rand32.h>

/* auto generated header file during west build */
#include "ncs_version.h"

#include "coap_appl_client.h"
#include "dtls_client.h"
#include "dtls_debug.h"
#include "modem.h"
#include "modem_at.h"
#include "modem_desc.h"
#include "modem_sim.h"
#include "parse.h"
#include "power_manager.h"
#include "serialize.h"
#include "ui.h"

#include "appl_diagnose.h"
#include "appl_storage.h"
#include "appl_storage_config.h"
#include "appl_time.h"
#include "environment_sensor.h"

#include "sh_cmd.h"

#ifdef CONFIG_LOCATION_ENABLE
#include "location.h"
#endif

#ifdef CONFIG_COAP_UPDATE
#include "appl_update_coap.h"
#endif

#ifdef CONFIG_NAU7802_SCALE
#include "nau7802.h"
#endif

#define APP_COAP_LOG_PAYLOAD_SIZE 128

#define COAP_OPTION_NO_RESPONSE 0x102
#define COAP_NO_RESPONSE_IGNORE_ALL 0x1a

static COAP_CONTEXT(appl_context, 1280);

static const char *coap_client_id;

static unsigned int coap_client_request_counter = 0;

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

static void coap_appl_client_decode_payload(const uint8_t *payload, uint16_t len)
{
}

static void coap_appl_client_decode_text_payload(char *payload)
{
   while (*payload) {
      size_t pos = strcspn(payload, " :=\n\r");
      char *val = "";
      char *cur = payload;

      if (pos) {
         char sep = cur[pos];
         payload += pos;
         if (sep) {
            cur[pos] = 0;
            ++payload;
            if (strchr(" :=", sep)) {
               pos = strcspn(payload, "\n\r");
               if (pos) {
                  val = payload;
                  payload += pos;
                  if (*payload) {
                     *payload = 0;
                     ++payload;
                  }
               }
            }
            payload += strspn(payload, "\n\r");
         }
      } else {
         pos = strlen(payload);
         if (!pos) {
            break;
         }
         payload += pos;
      }
#ifdef CONFIG_SH_CMD
      if (!stricmp(cur, "cmd")) {
         long delay_ms = 1000;
         const char *cmd = parse_next_long(val, 10, &delay_ms);
         cmd += strspn(cmd, " \t");
         sh_cmd_append(cmd, K_MSEC(delay_ms));
         dtls_info("cmd %ld %s", delay_ms, cmd);
         continue;
      }
#endif
#ifdef CONFIG_COAP_UPDATE
      if (!stricmp(cur, "fw")) {
         /* deprecated use "cmd fota" instead */
         dtls_info("fw %s", val);
         appl_update_coap_cmd(val);
         continue;
      }
#endif
      dtls_info("%s %s", cur, val);
   }
}

int coap_appl_client_parse_data(uint8_t *data, size_t len)
{
   int err;
   int res;
   int format = -1;
   struct coap_packet reply;
   struct coap_option message_option;
   const uint8_t *payload;
   uint16_t payload_len;
   uint8_t code;

   err = coap_packet_parse(&reply, data, len, NULL, 0);
   if (err < 0) {
      dtls_debug("Malformed response received: %d\n", err);
      return err;
   }

   res = coap_client_match(&reply, appl_context.mid, appl_context.token);
   if (res < PARSE_RESPONSE) {
      return res;
   }

   code = coap_header_get_code(&reply);
   appl_context.message_len = 0;

   if (code == COAP_RESPONSE_CODE_CHANGED || code == COAP_RESPONSE_CODE_CONTENT) {

      err = coap_find_options(&reply, COAP_OPTION_CONTENT_FORMAT, &message_option, 1);
      if (err == 1) {
         format = coap_client_decode_content_format(&message_option);
      }

      payload = coap_packet_get_payload(&reply, &payload_len);

      if (payload_len > 0) {
         if (format == COAP_CONTENT_FORMAT_TEXT_PLAIN && payload_len < sizeof(appl_context.message_buf)) {
            memmove(appl_context.message_buf, payload, payload_len);
            appl_context.message_buf[payload_len] = 0;
            dtls_info("===== %u bytes", (unsigned int)payload_len);
            coap_appl_client_decode_text_payload(appl_context.message_buf);
            dtls_info("=====");
         } else {
            coap_appl_client_decode_payload(payload, payload_len);
            if (coap_client_printable_content_format(format)) {
               const char *more = "";
               if (payload_len > APP_COAP_LOG_PAYLOAD_SIZE) {
                  payload_len = APP_COAP_LOG_PAYLOAD_SIZE;
                  more = "...";
               }
               memmove(appl_context.message_buf, payload, payload_len);
               appl_context.message_buf[payload_len] = 0;
               dtls_info("  payload: '%s'%s", (const char *)appl_context.message_buf, more);
            }
         }
      }
   }
   if (PARSE_CON_RESPONSE == res) {
      res = coap_client_prepare_ack(&reply);
   }
   return res;
}

static int coap_appl_client_add_uri_query(struct coap_packet *request, const char *query)
{
   if (query && strlen(query) > 0) {
      int err;

      err = coap_packet_append_option(request, COAP_OPTION_URI_QUERY,
                                      (uint8_t *)query,
                                      strlen(query));
      if (err < 0) {
         dtls_warn("Failed to encode CoAP URI-QUERY option '%s', %d", query, err);
         return err;
      }
   }
   return 0;
}

static int coap_appl_client_add_uri_query_param(struct coap_packet *request, const char *query, const char *value)
{
   if (query && strlen(query) > 0 && value && strlen(value) > 0) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%s=%s", query, value);
      return coap_appl_client_add_uri_query(request, buf);
   }
   return 0;
}

int coap_appl_client_serialize_modem_info(serializer_t *serializer, serialize_buffer_t *buffer, int flags)
{
   int64_t reboot_times[REBOOT_INFOS];
   uint16_t reboot_codes[REBOOT_INFOS];
   uint16_t battery_voltage = 0xffff;
   char buf[64];
   struct lte_modem_info modem_info;
   int64_t uptime;
   size_t mark = 0;
   size_t current = buffer->current;
   int err;

   uptime = k_uptime_get() / MSEC_PER_SEC;
   sb_mark(buffer);
   serializer->field(buffer, "app", true);
   serializer->start_map(buffer);
   serializer->field(buffer, "up", false);

   memset(buf, 0, sizeof(buf));
   if ((uptime / 60) < 5) {
      snprintf(buf, sizeof(buf), "%lu [s]", (unsigned long)uptime);
   } else {
      uint8_t secs = uptime % 60;
      uptime = uptime / 60;
      if (uptime < 60) {
         snprintf(buf, sizeof(buf), "%u:%02u [m:ss]", (uint8_t)uptime, secs);
      } else {
         uint8_t mins = uptime % 60;
         uptime = uptime / 60;
         if (uptime < 24) {
            snprintf(buf, sizeof(buf), "%u:%02u:%02u [h:mm:ss]", (uint8_t)uptime, mins, secs);
         } else {
            uint8_t hours = uptime % 24;
            uptime = uptime / 24;
            snprintf(buf, sizeof(buf), "%u-%02u:%02u:%02u [d-hh:mm:ss]", (uint8_t)uptime, hours, mins, secs);
         }
      }
   }
   serializer->text(buffer, buf);
   if (!(flags & COAP_SEND_FLAG_DYNAMIC_VALUES)) {
      serializer->field(buffer, "mode", true);
      serializer->text(buffer, CONFIG_APPL_MODEL_DESCRIPTION);
      serializer->field(buffer, "ver", true);
      serializer->text(buffer, appl_get_version());
   }
   serializer->field(buffer, "trans", true);
   serializer->start_map(buffer);

   for (int i = 0; i < COAP_MAX_RETRANSMISSION + 1; ++i) {
      snprintf(buf, sizeof(buf), "%d", i + 1);
      serializer->field(buffer, buf, false);
      serializer->number(buffer, transmissions[i], 0);
   }
   serializer->field(buffer, "failures", false);
   serializer->number(buffer, failures, 0);
   serializer->end_map(buffer);
   serializer->end_map(buffer);

   dtls_info("%s", sb_from_mark(buffer));

   if (!(flags & COAP_SEND_FLAG_MINIMAL) && !(flags & COAP_SEND_FLAG_DYNAMIC_VALUES)) {
      memset(&modem_info, 0, sizeof(modem_info));
      if (!modem_get_modem_info(&modem_info)) {
         serializer->next_item(buffer);
         sb_mark(buffer);
         serializer->field(buffer, "modem", false);
         serializer->start_map(buffer);
         serializer->field(buffer, "NCS", false);
         serializer->text(buffer, NCS_VERSION_STRING);
         serializer->field(buffer, "HW", false);
         serializer->text(buffer, modem_info.version);
         serializer->field(buffer, "MFW", false);
         serializer->text(buffer, modem_info.firmware);
         serializer->field(buffer, "IMEI", false);
         serializer->text(buffer, modem_info.imei);
         serializer->end_map(buffer);
         dtls_info("%s", sb_from_mark(buffer));
      }
#ifdef CONFIG_COAP_UPDATE
      mark = sb_mark(buffer);
      serializer->next_item(buffer);
      sb_mark(buffer);
      if (appl_update_coap_status_serialize(serializer, buffer)) {
         dtls_info("%s", sb_from_mark(buffer));
      } else {
         sb_reset_to(buffer, mark);
      }
#endif
   }

   mark = sb_mark(buffer);
   serializer->next_item(buffer);
   sb_mark(buffer);
   err = power_manager_status_serialize(serializer, buffer);
   if (err) {
      dtls_info("%s", sb_from_mark(buffer));
   } else {
      sb_reset_to(buffer, mark);
   }

   err = power_manager_voltage_ext(&battery_voltage);
   if (!err) {
      serializer->next_item(buffer);
      sb_mark(buffer);
      serializer->number_field(buffer, "Ext.Bat.", "mV", battery_voltage, 0);
      dtls_info("%s", sb_from_mark(buffer));
   }

   if (!(flags & COAP_SEND_FLAG_DYNAMIC_VALUES)) {
      memset(buf, 0, sizeof(buf));
      memset(reboot_times, 0, sizeof(reboot_times));
      memset(reboot_codes, 0, sizeof(reboot_codes));
      err = appl_storage_read_int_items(REBOOT_CODE_ID, 0, reboot_times, reboot_codes, REBOOT_INFOS);
      if (err > 0) {
         serializer->next_item(buffer);
         sb_mark(buffer);
         serializer->field(buffer, "reboot", false);
         serializer->start_map(buffer);
         serializer->field(buffer, "cause", true);
         serializer->text(buffer, appl_get_reboot_desciption(reboot_codes[0]));
         if (appl_format_time(reboot_times[0], buf, sizeof(buf))) {
            serializer->field(buffer, "date", true);
            serializer->text(buffer, buf);
         }
         serializer->end_map(buffer);
         dtls_info("%s", sb_from_mark(buffer));
         sb_mark(buffer);

         for (int i = 1; i < err; ++i) {
            buffer->seperator = false;
            serializer->field(buffer, "cause", true);
            serializer->text(buffer, appl_get_reboot_desciption(reboot_codes[i]));
            if (appl_format_time(reboot_times[i], buf, sizeof(buf))) {
               serializer->field(buffer, "date", true);
               serializer->text(buffer, buf);
            }
            dtls_info("%s", sb_from_mark(buffer));

            sb_reset(buffer);
         }
      }
      err = appl_reset_cause_description(buf, sizeof(buf));
      if (err > 0) {
         sb_mark(buffer);
         serializer->field(buffer, "restart", true);
         serializer->text(buffer, buf);
         dtls_info("%s", sb_from_mark(buffer));
      }
   }

   return buffer->current - current;
}

int coap_appl_client_prepare_sim_info(char *buf, size_t len, int flags)
{
   struct lte_sim_info sim_info;
   int start = 0;
   int index = 0;
   memset(&sim_info, 0, sizeof(sim_info));
   if (modem_sim_get_info(&sim_info) >= 0 && sim_info.valid) {
      index += snprintf(buf, len, "ICCID: %s, eDRX cycle: %s",
                        sim_info.iccid, sim_info.edrx_cycle_support ? "on" : "off");
      if (sim_info.hpplmn_search_interval && sim_info.hpplmn[0]) {
         index += snprintf(buf + index, len - index, ", HPPLMN %s interval: %d [h]",
                           sim_info.hpplmn, sim_info.hpplmn_search_interval);
      } else if (sim_info.hpplmn_search_interval) {
         index += snprintf(buf + index, len - index, ", HPPLMN interval: %d [h]", sim_info.hpplmn_search_interval);
      } else if (sim_info.hpplmn[0]) {
         index += snprintf(buf + index, len - index, ", HPPLMN %s", sim_info.hpplmn);
      } else {
         index += snprintf(buf + index, len - index, ", no HPPLMN search");
      }
      dtls_info("%s", buf);
      start = index + 1;
      if (sim_info.imsi_select_support && sim_info.imsi_select != 0xffff) {
         if (sim_info.imsi_select) {
            index += snprintf(buf + index, len - index, "\nMulti-IMSI: %s (imsi %u)",
                              sim_info.imsi, sim_info.imsi_select & 0xff);
         } else {
            index += snprintf(buf + index, len - index, "\nMulti-IMSI: %s (imsi %u, auto %d s)",
                              sim_info.imsi, sim_info.imsi_select & 0xff, sim_info.imsi_interval);
         }
      } else if (sim_info.prev_imsi[0]) {
         index += snprintf(buf + index, len - index, "\nMulti-IMSI: %s, %s, %d s",
                           sim_info.imsi, sim_info.prev_imsi, sim_info.imsi_interval);
      } else {
         index += snprintf(buf + index, len - index, "\nIMSI: %s", sim_info.imsi);
      }
      dtls_info("%s", buf + start);
      if (sim_info.forbidden[0]) {
         start = index + 1;
         index += snprintf(buf + index, len - index, "\nForbidden: %s",
                           sim_info.forbidden);
         dtls_info("%s", buf + start);
      }
   }
   return index;
}

union lte_params {
   struct lte_lc_psm_cfg psm;
   struct lte_lc_edrx_cfg edrx;
   struct lte_network_info network_info;
   enum lte_network_rai rai_info;
};

int coap_appl_client_prepare_net_info(char *buf, size_t len, int flags)
{
   int start = 0;
   int index = 0;
   int time = 0;
   union lte_params params;

   memset(&params, 0, sizeof(params));
   if (!modem_get_network_info(&params.network_info)) {
      index += snprintf(buf, len, "Network: %s",
                        modem_get_network_mode_description(params.network_info.mode));
      index += snprintf(buf + index, len - index, ",%s",
                        modem_get_registration_short_description(params.network_info.status));
      if (params.network_info.registered == LTE_NETWORK_STATE_ON) {
         index += snprintf(buf + index, len - index, ",Band %d", params.network_info.band);
         if (params.network_info.plmn_lock == LTE_NETWORK_STATE_ON) {
            index += snprintf(buf + index, len - index, ",#PLMN %s", params.network_info.provider);
         } else {
            index += snprintf(buf + index, len - index, ",PLMN %s", params.network_info.provider);
         }
         index += snprintf(buf + index, len - index, ",TAC %u", params.network_info.tac);
         index += snprintf(buf + index, len - index, ",Cell %u", params.network_info.cell);
         index += snprintf(buf + index, len - index, ",EARFCN %u", params.network_info.earfcn);
      }
   }
   dtls_info("%s", buf);

   if (params.network_info.registered == LTE_NETWORK_STATE_ON) {
      if (index) {
         buf[index++] = '\n';
      }
      start = index;
      index += snprintf(buf + index, len - index, "PDN: %s,%s",
                        params.network_info.apn, params.network_info.local_ip);
      if (params.network_info.rate_limit) {
         if (params.network_info.rate_limit_time) {
            index += snprintf(buf + index, len - index, ",rate-limit %u exceeded,%u s left",
                              params.network_info.rate_limit, params.network_info.rate_limit_time);
         } else {
            index += snprintf(buf + index, len - index, ",rate-limit %u,%u s",
                              params.network_info.rate_limit, params.network_info.rate_limit_period);
         }
      }
      dtls_info("%s", buf + start);
   }

   if (index) {
      buf[index++] = '\n';
   }
   start = index;

   memset(&params, 0, sizeof(params));
   if (modem_get_psm_status(&params.psm) == 0) {
      if (params.psm.active_time >= 0) {
         index += snprintf(buf + index, len - index, "PSM: TAU %d [s], Act %d [s]", params.psm.tau, params.psm.active_time);
      } else {
         index += snprintf(buf + index, len - index, "PSM: n.a.");
      }
   }
   time = modem_get_release_time();
   if (time >= 0) {
      if (index > start) {
         index += snprintf(buf + index, len - index, ", ");
      }
      memset(&params, 0, sizeof(params));
      if (modem_get_rai_status(&params.rai_info) == 0 && params.rai_info != LTE_NETWORK_RAI_UNKNOWN) {
         index += snprintf(buf + index, len - index, "%s, ", modem_get_rai_description(params.rai_info));
      }
      index += snprintf(buf + index, len - index, "Released: %d ms", time);
   }
   if (index > start) {
      dtls_info("%s", buf + start);
   } else {
      index = start - 1;
   }
   memset(&params, 0, sizeof(params));
   if (modem_get_edrx_status(&params.edrx) == 0) {
      if (index) {
         buf[index++] = '\n';
      }
      start = index;
      switch (params.edrx.mode) {
         case LTE_LC_LTE_MODE_NONE:
            index += snprintf(buf + index, len - index, "eDRX: n.a.");
            break;
         case LTE_LC_LTE_MODE_LTEM:
            index += snprintf(buf + index, len - index, "eDRX: LTE-M %0.2f [s], page %0.2f [s]", params.edrx.edrx, params.edrx.ptw);
            break;
         case LTE_LC_LTE_MODE_NBIOT:
            index += snprintf(buf + index, len - index, "eDRX: NB-IoT %0.2f [s], page %0.2f [s]", params.edrx.edrx, params.edrx.ptw);
            break;
         default:
            index += snprintf(buf + index, len - index, "eDRX: unknown");
            break;
      }
      dtls_info("%s", buf + start);
   }

   return index;
}

union lte_stats {
   struct lte_network_statistic network_statistic;
   struct lte_ce_info ce_info;
};

int coap_appl_client_serialize_net_stats(serializer_t *serializer, serialize_buffer_t *buffer, int flags)
{
   size_t current = buffer->current;
   union lte_stats params;

   serializer->field(buffer, "netstat", true);
   serializer->start_map(buffer);

   memset(&params, 0, sizeof(params));
   if (modem_get_coverage_enhancement_info(&params.ce_info) >= 0) {
      if (params.ce_info.ce_supported) {
         sb_mark(buffer);
         serializer->field(buffer, "CE", false);
         serializer->start_map(buffer);
         serializer->field(buffer, "downlink", false);
         serializer->number(buffer, params.ce_info.downlink_repetition, 0);
         serializer->field(buffer, "uplink", false);
         serializer->number(buffer, params.ce_info.uplink_repetition, 0);
         serializer->number_field(buffer, "RSRP", "dBm", params.ce_info.rsrp, 0);
         serializer->number_field(buffer, "CINR", "dB", params.ce_info.cinr, 0);
         serializer->number_field(buffer, "SNR", "dB", params.ce_info.snr, 0);
         serializer->end_map(buffer);
         dtls_info("%s", sb_from_mark(buffer));
      }
   }

   memset(&params, 0, sizeof(params));
   if (modem_read_statistic(&params.network_statistic) >= 0) {
      sb_mark(buffer);
      serializer->field(buffer, "stat", false);
      serializer->start_map(buffer);
      serializer->number_field(buffer, "tx", "kB", params.network_statistic.transmitted, 0);
      serializer->number_field(buffer, "rx", "kB", params.network_statistic.received, 0);
      serializer->number_field(buffer, "max", "B", params.network_statistic.max_packet_size, 0);
      serializer->number_field(buffer, "avg", "B", params.network_statistic.average_packet_size, 0);
      serializer->end_map(buffer);
      dtls_info("%s", sb_from_mark(buffer));

      if (!(flags & COAP_SEND_FLAG_MINIMAL)) {
         serializer->field(buffer, "misc", false);
         serializer->start_map(buffer);
         sb_mark(buffer);
         serializer->field(buffer, "Cell updates", false);
         serializer->number(buffer, params.network_statistic.cell_updates, 0);
         serializer->field(buffer, "Network searchs", false);
         serializer->number(buffer, params.network_statistic.searchs, 0);
         serializer->number_field(buffer, "Network searchtime", "s", params.network_statistic.search_time, 0);
         serializer->field(buffer, "PSM delays", false);
         serializer->number(buffer, params.network_statistic.psm_delays, 0);
         serializer->number_field(buffer, "PSM delaystime", "s", params.network_statistic.psm_delay_time, 0);
         dtls_info("%s", sb_from_mark(buffer));

         sb_mark(buffer);
         serializer->field(buffer, "Modem restarts", false);
         serializer->number(buffer, params.network_statistic.restarts, 0);
         serializer->field(buffer, "Sockets", false);
         serializer->number(buffer, sockets, 0);
         serializer->field(buffer, "DTLS handshakes", false);
         serializer->number(buffer, dtls_handshakes, 0);
         dtls_info("%s", sb_from_mark(buffer));
#if 0
         sb_mark(buffer);
         serializer->field(buffer, "Wakeups", false);
         serializer->number(buffer, params.network_statistic.wakeups, 0);
         serializer->number_field(buffer, "Wakeuptime", "s", params.network_statistic.wakeup_time, 0);
         serializer->number_field(buffer, "Connected", "s", params.network_statistic.connected_time, 0);
         serializer->number_field(buffer, "Asleep", "s", params.network_statistic.asleep_time, 0);
         dtls_info("%s", sb_from_mark(buffer));
#endif
         serializer->end_map(buffer);
      }
   }
   serializer->end_map(buffer);

   return buffer->current - current;
}

#if defined(CONFIG_ENVIRONMENT_SENSOR)
static int coap_appl_client_prepare_env_history(const double *values, size_t size, int prec, char *buf, size_t len)
{
   int index = 0;
   int history_index = 0;
   for (; history_index < size; ++history_index) {
      index += snprintf(buf + index, len - index, "%.*f,", prec, values[history_index]);
   }
   --index;
   return index;
}
#endif

int coap_appl_client_serialize_env_info(serializer_t *serializer, serialize_buffer_t *buffer, int flags)
{
   size_t current = buffer->current;
   int res = 0;

   serializer->field(buffer, "env", false);
   serializer->start_map(buffer);
   {
#ifdef CONFIG_ENVIRONMENT_SENSOR
      int index = 0;
      int start = 0;
      int32_t int_value = 0;
      uint8_t byte_value = 0;

#if (CONFIG_ENVIRONMENT_HISTORY_SIZE > 0)
      double values[CONFIG_ENVIRONMENT_HISTORY_SIZE];
      uint16_t iaqs[CONFIG_ENVIRONMENT_HISTORY_SIZE];
#else
      double values[1];
#endif

#if (CONFIG_ENVIRONMENT_HISTORY_SIZE > 0)
      res = environment_get_temperature_history(values, CONFIG_ENVIRONMENT_HISTORY_SIZE);
#endif
      if (res <= 0 && environment_get_temperature(&values[0]) == 0) {
         res = 1;
      }
      if (res > 0) {
         sb_mark(buffer);
         serializer->number_field(buffer, "temperature", "°C", values[0], 2);
         dtls_info("%s", sb_from_mark(buffer));
      }

#if (CONFIG_ENVIRONMENT_HISTORY_SIZE > 0)
      res = environment_get_humidity_history(values, CONFIG_ENVIRONMENT_HISTORY_SIZE);
#else
      res = 0;
#endif
      if (res <= 0 && environment_get_humidity(&values[0]) == 0) {
         res = 1;
      }
      if (res > 0) {
         sb_mark(buffer);
         serializer->number_field(buffer, "humidity", "%H", values[0], 2);
         dtls_info("%s", sb_from_mark(buffer));
      }

#if (CONFIG_ENVIRONMENT_HISTORY_SIZE > 0)
      res = environment_get_pressure_history(values, CONFIG_ENVIRONMENT_HISTORY_SIZE);
#else
      res = 0;
#endif
      if (res <= 0 && environment_get_pressure(&values[0]) == 0) {
         res = 1;
      }
      if (res > 0) {
         sb_mark(buffer);
         serializer->number_field(buffer, "pressure", "hPa", values[0], 0);
         dtls_info("%s", sb_from_mark(buffer));
      }

#if (CONFIG_ENVIRONMENT_HISTORY_SIZE > 0)
      res = environment_get_iaq_history(iaqs, CONFIG_ENVIRONMENT_HISTORY_SIZE);
      if (res > 0) {
         int history_index;
         const char *desc = environment_get_iaq_description(IAQ_VALUE(iaqs[0]));
         if (index) {
            buf[index++] = '\n';
         }
         start = index;
         buf[index++] = '!';
         for (history_index = 0; history_index < int_value; ++history_index) {
            index += snprintf(buf + index, len - index, "%d;%d,", IAQ_VALUE(iaqs[history_index]), IAQ_ACCURANCY(iaqs[history_index]));
         }
         --index;
         index += snprintf(buf + index, len - index, " Q (%s)", desc);
         dtls_info("%s", buf + start);
      }
#endif
#if 0
      if (res <= 0 && environment_get_iaq(&int_value, &byte_value) == 0) {
         const char *desc = environment_get_iaq_description(int_value);
         if (index) {
            buf[index++] = '\n';
         }
         start = index;
         index += snprintf(buf + index, len - index, "!%d;%d Q (%s)", int_value, byte_value, desc);
         dtls_info("%s", buf + start);
      }
#endif
#else  /* CONFIG_ENVIRONMENT_SENSOR */
      char buf[64];
      res = modem_at_cmd(buf, sizeof(buf), "%XTEMP: ", "AT%XTEMP?");
      if (res > 0) {
         int temperature = 0;
         res = sscanf(buf, "%d", &temperature);
         if (res == 1) {
            sb_mark(buffer);
            serializer->number_field(buffer, "temperature", "°C", (double)temperature, 0);
            dtls_info("%s", sb_from_mark(buffer));
         }
      } else {
         if (res < 0) {
            dtls_warn("Failed to read XTEMP.");
         }
      }
#endif /* CONFIG_ENVIRONMENT_SENSOR */
   }
   serializer->end_map(buffer);

   return buffer->current - current;
}

int coap_appl_client_prepare_location_info(char *buf, size_t len, int flags)
{
   int index = 0;

#ifdef CONFIG_LOCATION_ENABLE
   static uint32_t max_execution_time = 0;
   static uint32_t max_satellites_time = 0;
   struct modem_gnss_state result;
   bool pending;
   const char *p = "???";
   int res = 1;
   int start = 0;

   switch (location_get(&result, &pending)) {
      case MODEM_GNSS_NOT_AVAILABLE:
         p = "n.a.";
         break;
      case MODEM_GNSS_TIMEOUT:
         p = "timeout";
         break;
      case MODEM_GNSS_ERROR:
         p = "error";
         break;
      case MODEM_GNSS_INVISIBLE:
         p = "invisible";
         break;
      case MODEM_GNSS_POSITION:
         p = "valid";
         res = 0;
         break;
      default:
         break;
   }

   if (max_satellites_time < result.satellites_time) {
      max_satellites_time = result.satellites_time;
   }

   if (result.valid) {
      index += snprintf(buf, len, "GNSS.1=%s%s,%u-sats,%us-vis,%us-vis-max",
                        p, pending ? ",pending" : "", result.max_satellites, result.satellites_time / 1000, max_satellites_time / 1000);
      dtls_info("%s", buf);
#ifdef GNSS_VISIBILITY
      if (index) {
         buf[index++] = '\n';
      }
      start = index;
#else
      index = 0;
#endif
      if (!res) {
         if (!max_execution_time) {
            /* skip the first */
            max_execution_time = 1;
            index += snprintf(buf + index, len - index, "GNSS.2=%us-pos",
                              result.execution_time / 1000);
         } else {
            if (max_execution_time < result.execution_time) {
               max_execution_time = result.execution_time;
            }
            index += snprintf(buf + index, len - index, "GNSS.2=%us-pos,%us-pos-max",
                              result.execution_time / 1000, max_execution_time / 1000);
         }
      } else if (max_execution_time > 1) {
         index += snprintf(buf + index, len - index, "GNSS.2=%us-pos-max",
                           max_execution_time / 1000);
      }
      if (index > start) {
         dtls_info("%s", buf + start);
#ifdef GNSS_EXECUTION_TIMES
         buf[index++] = '\n';
         start = index;
#else
         if (start) {
            index = start - 1;
         } else {
            index = 0;
         }
#endif
      }
      index += snprintf(buf + index, len - index, "%s!GNSS.3=%.06f,%.06f,%.01f,%.02f,%.01f",
                        res ? "*" : "",
                        result.position.latitude, result.position.longitude, result.position.accuracy,
                        result.position.altitude, result.position.altitude_accuracy);
      index += snprintf(buf + index, len - index, ",%04d-%02d-%02dT%02d:%02d:%02dZ",
                        result.position.datetime.year, result.position.datetime.month, result.position.datetime.day,
                        result.position.datetime.hour, result.position.datetime.minute, result.position.datetime.seconds);
      dtls_info("%s", buf + start);
   } else {
      index += snprintf(buf, len, "GNSS.1=%s%s", p, pending ? ",pending" : "");
      dtls_info("%s", buf);
   }
#endif
   return index;
}

int coap_appl_client_prepare_post(char *buf, size_t len, int flags)
{
   int err;
   uint8_t *token = (uint8_t *)&appl_context.token;
   const char *path;
   struct coap_packet request;

   serialize_buffer_t buffer;
   sb_init(&buffer, buf, len);

   if (coap_client_request_counter == 0) {
      path = CONFIG_COAP_RESOURCE;
   } else {
      path = CONFIG_COAP_FOLLOW_UP_RESOURCE;
      flags |= COAP_SEND_FLAG_DYNAMIC_VALUES;
   }
   coap_client_request_counter++;
   appl_context.message_len = 0;

   if (len && buf[0] == 0) {
      json.start_map(&buffer);
#ifdef CONFIG_COAP_SEND_MODEM_INFO
      coap_appl_client_serialize_modem_info(&json, &buffer, flags);
#endif

#if 0

#ifdef CONFIG_COAP_SEND_SIM_INFO
      buf[index] = '\n';
      start = index + 1;
      err = coap_appl_client_prepare_sim_info(buf + start, len - start, flags);
      if (err > 0) {
         index = start + err;
      }
#endif /* CONFIG_COAP_SEND_SIM_INFO */

#if defined(CONFIG_COAP_SEND_NETWORK_INFO)
      buf[index] = '\n';
      start = index + 1;
      err = coap_appl_client_prepare_net_info(buf + start, len - start, flags);
      if (err > 0) {
         index = start + err;
      }
#endif /* CONFIG_COAP_SEND_NETWORK_INFO */
#endif

#if defined(CONFIG_COAP_SEND_STATISTIC_INFO)
      coap_appl_client_serialize_net_stats(&json, &buffer, flags);
#endif /* CONFIG_COAP_SEND_STATISTIC_INFO */

#if 0

#ifdef CONFIG_LOCATION_ENABLE
      buf[index] = '\n';
      start = index + 1;
      err = coap_appl_client_prepare_location_info(buf + start, len - start, flags);
      if (err > 0) {
         index = start + err;
      }
#endif /* CONFIG_LOCATION_ENABLE */

#ifdef CONFIG_ADC_SCALE
      buf[index] = '\n';
      start = index + 1;
      err = scale_sample_desc(buf + start, len - start, true);
      if (err > 0) {
         index = start + err;
      }
#endif
#endif
      coap_appl_client_serialize_env_info(&json, &buffer, flags);
      json.end_map(&buffer);
      dtls_info("%d/%d", buffer.current, buffer.length);
   } else {
      buffer.current = len;
   }
   appl_context.token = coap_client_next_token();
   appl_context.mid = coap_next_id();

   err = coap_packet_init(&request, appl_context.message_buf, sizeof(appl_context.message_buf),
                          COAP_VERSION_1,
                          flags & COAP_SEND_FLAG_NO_RESPONSE ? COAP_TYPE_NON_CON : COAP_TYPE_CON,
                          sizeof(appl_context.token), token,
                          COAP_METHOD_POST, appl_context.mid);

   if (err < 0) {
      dtls_warn("Failed to create CoAP request, %d", err);
      return err;
   }

   err = coap_packet_set_path(&request, path);
   if (err < 0) {
      dtls_warn("Failed to encode CoAP URI-PATH option, %d", err);
      return err;
   }

   err = coap_append_option_int(&request, COAP_OPTION_CONTENT_FORMAT,
                                COAP_CONTENT_FORMAT_APP_JSON);
   if (err < 0) {
      dtls_warn("Failed to encode CoAP CONTENT_FORMAT option, %d", err);
      return err;
   }

   if (flags & COAP_SEND_FLAG_NO_RESPONSE) {
      err = coap_append_option_int(&request, COAP_OPTION_NO_RESPONSE,
                                   COAP_NO_RESPONSE_IGNORE_ALL);
      if (err < 0) {
         dtls_warn("Failed to encode CoAP NO_RESPONSE option, %d", err);
         return err;
      }
   }

   err = coap_packet_append_payload_marker(&request);
   if (err < 0) {
      dtls_warn("Failed to encode CoAP payload-marker, %d", err);
      return err;
   }

   err = coap_packet_append_payload(&request, buffer.buffer, buffer.current);
   if (err < 0) {
      dtls_warn("Failed to encode CoAP payload, %d", err);
      return err;
   }

   appl_context.message_len = request.offset;
   dtls_info("CoAP request prepared, token 0x%02x%02x%02x%02x, %u bytes", token[0], token[1], token[2], token[3], request.offset);

   return request.offset;
}

int coap_appl_client_message(const uint8_t **buffer)
{
   if (buffer) {
      *buffer = appl_context.message_buf;
   }
   return appl_context.message_len;
}

int coap_appl_client_retry_strategy(int counter, bool dtls)
{
   if (dtls) {
      switch (counter) {
         case 1:
            return 0;
         case 2:
            return DTLS_CLIENT_RETRY_STRATEGY_OFF | DTLS_CLIENT_RETRY_STRATEGY_DTLS_HANDSHAKE;
      }
   } else {
      switch (counter) {
         case 1:
            return 0;
         case 2:
            return DTLS_CLIENT_RETRY_STRATEGY_OFF;
      }
   }
   return DTLS_CLIENT_RETRY_STRATEGY_RESTARTS;
}

int coap_appl_client_init(const char *id)
{
   coap_client_id = id;
   return id ? strlen(id) : 0;
}

static char cmd_buf[512];

static int sh_cmd_net(const char *parameter)
{
   (void)parameter;
   serialize_buffer_t buffer;
   sb_init(&buffer, cmd_buf, sizeof(cmd_buf));
   return coap_appl_client_serialize_net_stats(&plain, &buffer, 0);
}

static int sh_cmd_dev(const char *parameter)
{
   (void)parameter;
   serialize_buffer_t buffer;
   sb_init(&buffer, cmd_buf, sizeof(cmd_buf));
   return coap_appl_client_serialize_modem_info(&plain, &buffer, 0);
}

static int sh_cmd_env(const char *parameter)
{
   (void)parameter;
   serialize_buffer_t buffer;
   sb_init(&buffer, cmd_buf, sizeof(cmd_buf));
   return coap_appl_client_serialize_env_info(&plain, &buffer, 0);
}

SH_CMD(net, "", "read network info.", sh_cmd_net, NULL, 0);
#ifdef CONFIG_BATTERY_VOLTAGE_SOURCE_MODEM
SH_CMD(dev, "", "read device info.", sh_cmd_dev, NULL, 0);
#else
SH_CMD(dev, NULL, "read device info.", sh_cmd_dev, NULL, 0);
#endif
SH_CMD(env, NULL, "read environment sensor.", sh_cmd_env, NULL, 0);
