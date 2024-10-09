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

#include "appl_diagnose.h"
#include "appl_settings.h"
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

#define CUSTOM_COAP_OPTION_TIME 0xfde8
#define CUSTOM_COAP_OPTION_READ_ETAG 0xfdec
#define CUSTOM_COAP_OPTION_READ_RESPONSE_CODE 0xfdf0
#define CUSTOM_COAP_OPTION_INTERVAL 0xfdf4
#define CUSTOM_COAP_OPTION_FORWARD_RESPONSE_CODE 0xfdf8

static COAP_CONTEXT(appl_context, 1280);

static uint8_t coap_read_etag[COAP_TOKEN_MAX_LEN + 1];

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

static int coap_appl_client_encode_time(struct coap_packet *request)
{
   int64_t time;
   uint8_t data[8];
   uint8_t index = 0;

   appl_get_now(&time);

   sys_put_be64(time, data);
   // skip leading 0s
   for (index = 0; index < sizeof(data); ++index) {
      if (data[index]) {
         break;
      }
   }
   // adjust negative numbers
   if (index > 0 && (data[index] & 0x80)) {
      --index;
   }
   int err = coap_packet_append_option(request, CUSTOM_COAP_OPTION_TIME, &data[index], sizeof(data) - index);
   if (err < 0) {
      dtls_warn("Failed to encode CoAP TIME option, %d", err);
   } else {
      dtls_info("Send CoAP TIME option %lld %llx (%d bytes)", time, time, sizeof(data) - index);
   }
   return err;
}

static void coap_appl_client_decode_time(const struct coap_option *option)
{
   uint8_t len = option->len;
   uint8_t index = len;
   uint64_t time = 0;

   if (len == 0) {
      dtls_info("Recv CoAP TIME option, empty");
      return;
   }

   for (index = 0; index < len; ++index) {
      time <<= 8;
      time |= (option->value[index] & 0xff);
   }

   dtls_info("Recv CoAP TIME option %lld %llx (%d bytes)", time, time, len);
   appl_set_now(time);
}

static void coap_appl_client_decode_read_etag(const struct coap_option *option)
{
   int len = coap_client_decode_etag(option, coap_read_etag);

   if (len == 0) {
      dtls_info("Recv CoAP etag option, empty");
   } else {
      dtls_info("Recv CoAP etag option (%d bytes)", len);
   }
}

static uint8_t coap_appl_client_decode_response_code(const char *description, const struct coap_option *option)
{
   int code = coap_option_value_to_int(option);
   dtls_info("Recv CoAP %s response code %d.%02d", description, (code >> 5) & 7, code & 0x1f);
   return (uint8_t)code;
}

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

   err = coap_find_options(&reply, CUSTOM_COAP_OPTION_TIME, &message_option, 1);
   if (err == 1) {
      coap_appl_client_decode_time(&message_option);
   }

   if (code == COAP_RESPONSE_CODE_CHANGED) {
      err = coap_find_options(&reply, CUSTOM_COAP_OPTION_READ_RESPONSE_CODE, &message_option, 1);
      if (err == 1) {
         code = coap_appl_client_decode_response_code("read", &message_option);
      }
      err = coap_find_options(&reply, CUSTOM_COAP_OPTION_READ_ETAG, &message_option, 1);
      if (err == 1) {
         coap_appl_client_decode_read_etag(&message_option);
      }
   }
   err = coap_find_options(&reply, CUSTOM_COAP_OPTION_FORWARD_RESPONSE_CODE, &message_option, 1);
   if (err == 1) {
      coap_appl_client_decode_response_code("forward", &message_option);
   }

   err = coap_find_options(&reply, COAP_OPTION_CONTENT_FORMAT, &message_option, 1);
   if (err == 1) {
      format = coap_client_decode_content_format(&message_option);
   }

   payload = coap_packet_get_payload(&reply, &payload_len);
   if (payload_len > 0) {
      if (code == COAP_RESPONSE_CODE_CONTENT) {
         if (format == COAP_CONTENT_FORMAT_TEXT_PLAIN && payload_len < sizeof(appl_context.message_buf)) {
            memmove(appl_context.message_buf, payload, payload_len);
            appl_context.message_buf[payload_len] = 0;
            dtls_info("===== %u bytes", (unsigned int)payload_len);
            coap_appl_client_decode_text_payload(appl_context.message_buf);
            dtls_info("=====");
         } else {
            coap_appl_client_decode_payload(payload, payload_len);
            if (coap_client_printable_content_format(format)) {
               coap_client_dump_payload(appl_context.message_buf, APP_COAP_LOG_PAYLOAD_SIZE + 1, payload, payload_len);
            }
         }
      } else if (coap_client_printable_content_format(format) ||
                 (code >= COAP_RESPONSE_CODE_BAD_REQUEST && format == -1)) {
         coap_client_dump_payload(appl_context.message_buf, APP_COAP_LOG_PAYLOAD_SIZE + 1, payload, payload_len);
      }
   }
   if (PARSE_CON_RESPONSE == res) {
      res = coap_client_prepare_ack(&reply);
   }
   return res;
}

int coap_appl_client_prepare_modem_info(char *buf, size_t len, int flags)
{
   uint16_t battery_voltage = 0xffff;
   struct lte_modem_info modem_info;
   int64_t uptime;
   int index = 0;
   int start = 0;
   int err;

   uptime = k_uptime_get() / MSEC_PER_SEC;

   start = 0;
   if ((uptime / 60) < 5) {
      index = snprintf(buf, len, "%lu [s]", (unsigned long)uptime);
   } else {
      uint8_t secs = uptime % 60;
      uptime = uptime / 60;
      if (uptime < 60) {
         index = snprintf(buf, len, "%u:%02u [m:ss]", (uint8_t)uptime, secs);
      } else {
         uint8_t mins = uptime % 60;
         uptime = uptime / 60;
         if (uptime < 24) {
            index = snprintf(buf, len, "%u:%02u:%02u [h:mm:ss]", (uint8_t)uptime, mins, secs);
         } else {
            uint8_t hours = uptime % 24;
            uptime = uptime / 24;
            index = snprintf(buf, len, "%u-%02u:%02u:%02u [d-hh:mm:ss]", (uint8_t)uptime, hours, mins, secs);
         }
      }
   }

   index += snprintf(buf + index, len - index, ", %s %s, 0*%u, 1*%u, 2*%u, 3*%u, failures %u",
                     CONFIG_APPL_MODEL_DESCRIPTION, appl_get_version(), transmissions[0], transmissions[1], transmissions[2], transmissions[3], failures);
   dtls_info("%s", buf + start);

   buf[index++] = '\n';
   start = index;

   if ((flags & COAP_SEND_FLAG_INITIAL) || !(flags & COAP_SEND_FLAG_MINIMAL)) {

      memset(&modem_info, 0, sizeof(modem_info));
      if (!modem_get_modem_info(&modem_info)) {
         index += snprintf(buf + index, len - index, "NCS: %s, HW: %s, MFW: %s, IMEI: %s", NCS_VERSION_STRING, modem_info.version, modem_info.firmware, modem_info.imei);
         dtls_info("%s", buf + start);
         buf[index++] = '\n';
         start = index;
      }
   }

#ifdef CONFIG_COAP_UPDATE
   index += appl_update_coap_status(buf + index, len - index);
   if (index > start) {
      dtls_info("%s", buf + start);
      buf[index++] = '\n';
      start = index;
   }
#endif

   err = power_manager_status_desc(&buf[index + 1], len - index - 1);
   if (err) {
      buf[index++] = '!';
      index += err;
      dtls_info("%s", buf + start);
      buf[index++] = '\n';
      start = index;
   }

   err = power_manager_voltage_ext(&battery_voltage);
   if (!err) {
      index += snprintf(buf + index, len - index, "!Ext.Bat.: %u mV", battery_voltage);
      dtls_info("%s", buf + start);
      buf[index++] = '\n';
      start = index;
   }

   if ((flags & COAP_SEND_FLAG_INITIAL) || !(flags & COAP_SEND_FLAG_MINIMAL)) {
      err = appl_reboot_cause_description(0, 0, buf + index, len - index);
      if (err > 0) {
         dtls_info("%s", buf + start);
         index += err;
         buf[index++] = '\n';
         start = index;
      }

      index += snprintf(buf + index, len - index, "Restart: ");
      err = appl_reset_cause_description(buf + index, len - index);
      if (err > 0) {
         dtls_info("%s", buf + start);
         index += err;
         buf[index++] = '\n';
         start = index;
      } else {
         index = start;
      }
   }
   if (connect_time_ms > 0 || coap_rtt_ms > 0) {
      index += snprintf(buf + index, len - index, "!RETRANS: %u", retransmissions);
      if (coap_rtt_ms > 0) {
         index += snprintf(buf + index, len - index, ", RTT: %u ms", coap_rtt_ms);
      }
      if (connect_time_ms > 0) {
         index += snprintf(buf + index, len - index, ", CT: %u ms", connect_time_ms);
      }
      dtls_info("%s", buf + start);
      buf[index++] = '\n';
      start = index;
   }

   return index - 1;
}

int coap_appl_client_prepare_sim_info(char *buf, size_t len, int flags)
{
   struct lte_sim_info sim_info;
   int start = 0;
   int index = 0;
   memset(&sim_info, 0, sizeof(sim_info));
   if (modem_sim_get_info(&sim_info) >= 0 && sim_info.valid) {
      if ((flags & COAP_SEND_FLAG_INITIAL) || !(flags & COAP_SEND_FLAG_MINIMAL)) {

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
         buf[index++] = '\n';
         start = index;
      }
      if (sim_info.imsi_select_support && sim_info.imsi_select != 0xffff) {
         if (sim_info.imsi_select) {
            index += snprintf(buf + index, len - index, "Multi-IMSI: %s (imsi %u)",
                              sim_info.imsi, sim_info.imsi_select & 0xff);
         } else {
            index += snprintf(buf + index, len - index, "Multi-IMSI: %s (imsi %u, auto %d s)",
                              sim_info.imsi, sim_info.imsi_select & 0xff, sim_info.imsi_interval);
         }
      } else if (sim_info.prev_imsi[0]) {
         index += snprintf(buf + index, len - index, "Multi-IMSI: %s, %s, %d s",
                           sim_info.imsi, sim_info.prev_imsi, sim_info.imsi_interval);
      } else {
         index += snprintf(buf + index, len - index, "IMSI: %s", sim_info.imsi);
      }
      dtls_info("%s", buf + start);
      if (sim_info.forbidden[0]) {
         buf[index++] = '\n';
         start = index;
         index += snprintf(buf + index, len - index, "Forbidden: %s",
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

   if (!(flags & COAP_SEND_FLAG_MINIMAL)) {
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
         index += modem_print_edrx("", &params.edrx, buf + index, len - index);
         dtls_info("%s", buf + start);
      }
   }

   return index;
}

union lte_stats {
   struct lte_network_statistic network_statistic;
   struct lte_ce_info ce_info;
};

int coap_appl_client_prepare_net_stats(char *buf, size_t len, int flags)
{
   int start = 0;
   int index = 0;
   union lte_stats params;

   memset(&params, 0, sizeof(params));
   if (modem_get_coverage_enhancement_info(&params.ce_info) >= 0) {
      if (params.ce_info.ce_supported) {

         index = snprintf(buf, len, "!CE: down: %u, up: %u",
                          params.ce_info.downlink_repetition, params.ce_info.uplink_repetition);
         if (params.ce_info.rsrp < INVALID_SIGNAL_VALUE) {
            index += snprintf(buf + index, len - index, ", RSRP: %d dBm",
                              params.ce_info.rsrp);
         }
         if (params.ce_info.cinr < INVALID_SIGNAL_VALUE) {
            index += snprintf(buf + index, len - index, ", CINR: %d dB",
                              params.ce_info.cinr);
         }
         if (params.ce_info.snr < INVALID_SIGNAL_VALUE) {
            index += snprintf(buf + index, len - index, ", SNR: %d dB",
                              params.ce_info.snr);
         }
         dtls_info("%s", buf + start);
      }
   }

   if (!(flags & COAP_SEND_FLAG_MINIMAL)) {

      memset(&params, 0, sizeof(params));
      if (modem_read_statistic(&params.network_statistic) >= 0) {
         if (index) {
            buf[index++] = '\n';
         }
         start = index;
         index += snprintf(buf + index, len - index, "Stat: tx %u kB, rx %u kB, max %u B, avg %u B",
                           params.network_statistic.transmitted, params.network_statistic.received,
                           params.network_statistic.max_packet_size, params.network_statistic.average_packet_size);
         dtls_info("%s", buf + start);
         start = index + 1;
         index += snprintf(buf + index, len - index, "\nCell updates %u, Network searchs %u (%u s), PSM delays %u (%u s)",
                           params.network_statistic.cell_updates, params.network_statistic.searchs, params.network_statistic.search_time,
                           params.network_statistic.psm_delays, params.network_statistic.psm_delay_time);
         dtls_info("%s", buf + start);
         start = index + 1;
         index += snprintf(buf + index, len - index, "\nModem Restarts %u, Sockets %u, DTLS handshakes %u", params.network_statistic.restarts, sockets, dtls_handshakes);
         dtls_info("%s", buf + start);
         start = index + 1;
         index += snprintf(buf + index, len - index, "\nWakeups %u, %u s, connected %u s, asleep %u s",
                           params.network_statistic.wakeups, params.network_statistic.wakeup_time,
                           params.network_statistic.connected_time, params.network_statistic.asleep_time);
         dtls_info("%s", buf + start);
      }
   }

   return index;
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

int coap_appl_client_prepare_env_info(char *buf, size_t len, int flags)
{
   int index = 0;
   int res = 0;

#ifdef CONFIG_ENVIRONMENT_SENSOR
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
      buf[index++] = '!';
      index += coap_appl_client_prepare_env_history(values, res, 2, buf + index, len - index);
      index += snprintf(buf + index, len - index, " C");
      dtls_info("%s", buf);
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
      if (index) {
         buf[index++] = '\n';
      }
      start = index;
      buf[index++] = '!';
      index += coap_appl_client_prepare_env_history(values, res, 2, buf + index, len - index);
      index += snprintf(buf + index, len - index, " %%H");
      dtls_info("%s", buf + start);
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
      if (index) {
         buf[index++] = '\n';
      }
      start = index;
      buf[index++] = '!';
      index += coap_appl_client_prepare_env_history(values, res, 0, buf + index, len - index);
      index += snprintf(buf + index, len - index, " hPa");
      dtls_info("%s", buf + start);
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
   if (res <= 0 && environment_get_iaq(&int_value, &byte_value) == 0) {
      const char *desc = environment_get_iaq_description(int_value);
      if (index) {
         buf[index++] = '\n';
      }
      start = index;
      index += snprintf(buf + index, len - index, "!%d;%d Q (%s)", int_value, byte_value, desc);
      dtls_info("%s", buf + start);
   }
#else  /* CONFIG_ENVIRONMENT_SENSOR */

   buf[index++] = '!';
   res = modem_at_cmd(buf + index, len - index, "%XTEMP: ", "AT%XTEMP?");
   if (res > 0) {
      index += res;
      index += snprintf(buf + index, len - index, " C");
      dtls_info("%s", buf);
   } else {
      if (res < 0) {
         dtls_warn("Failed to read XTEMP.");
      }
      index = 0;
   }
#endif /* CONFIG_ENVIRONMENT_SENSOR */
   return index;
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
   int index = 0;
   int start = 0;
   bool read_etag = false;
   uint8_t *token = (uint8_t *)&appl_context.token;
   char value[MAX_SETTINGS_VALUE_LENGTH];
   struct coap_packet request;

   appl_context.message_len = 0;

   if (flags & COAP_SEND_FLAG_SET_PAYLOAD) {
      index = len;
   } else {
      if (flags & COAP_SEND_FLAG_MODEM_INFO) {
         err = coap_appl_client_prepare_modem_info(buf, len, flags);
         if (err > 0) {
            index = start + err;
         }
      }

      if (flags & COAP_SEND_FLAG_SIM_INFO) {
         buf[index] = '\n';
         start = index + 1;
         err = coap_appl_client_prepare_sim_info(buf + start, len - start, flags);
         if (err > 0) {
            index = start + err;
         }
      }

      if (flags & COAP_SEND_FLAG_NET_INFO) {
         buf[index] = '\n';
         start = index + 1;
         err = coap_appl_client_prepare_net_info(buf + start, len - start, flags);
         if (err > 0) {
            index = start + err;
         }
      }

      if (flags & COAP_SEND_FLAG_NET_STATS) {
         buf[index] = '\n';
         start = index + 1;
         err = coap_appl_client_prepare_net_stats(buf + start, len - start, flags);
         if (err > 0) {
            index = start + err;
         }
      }

#ifdef CONFIG_LOCATION_ENABLE
      if (flags & COAP_SEND_FLAG_LOCATION_INFO) {
         buf[index] = '\n';
         start = index + 1;
         err = coap_appl_client_prepare_location_info(buf + start, len - start, flags);
         if (err > 0) {
            index = start + err;
         }
      }
#endif /* CONFIG_LOCATION_ENABLE */

      if (flags & COAP_SEND_FLAG_ENV_INFO) {
         buf[index] = '\n';
         start = index + 1;
         err = coap_appl_client_prepare_env_info(buf + start, len - start, flags);
         if (err > 0) {
            index = start + err;
         }
      }

#ifdef CONFIG_ADC_SCALE
      if (flags & COAP_SEND_FLAG_SCALE_INFO) {
         buf[index] = '\n';
         start = index + 1;
         err = scale_sample_desc(buf + start, len - start, true);
         if (err > 0) {
            index = start + err;
         }
      }
#endif /* CONFIG_ADC_SCALE */

      if (flags & COAP_SEND_FLAG_NET_SCAN_INFO) {
         buf[index] = '\n';
         start = index + 1;
         err = modem_get_last_neighbor_cell_meas(buf + start, len - start);
         if (err > 0) {
            index = start + err;
         }
      }

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

   if (appl_settings_get_coap_path(value, sizeof(value))) {
      err = coap_packet_set_path(&request, value);
      if (err < 0) {
         dtls_warn("Failed to encode CoAP URI-PATH '%s' option, %d", value, err);
         return err;
      }
   }

   err = coap_append_option_int(&request, COAP_OPTION_CONTENT_FORMAT,
                                COAP_CONTENT_FORMAT_TEXT_PLAIN);
   if (err < 0) {
      dtls_warn("Failed to encode CoAP CONTENT_FORMAT option, %d", err);
      return err;
   }

   if (appl_settings_get_coap_query(value, sizeof(value))) {
      const char *read = NULL;
      err = coap_packet_set_path(&request, value);
      if (err < 0) {
         dtls_warn("Failed to encode CoAP URI-QUERY '%s' option, %d", value, err);
         return err;
      }
      read = strstr(value, "read");
      if (read > value) {
         char c = *(read - 1);
         if (c == '?' || c == '&') {
            c = *(read + 4);
            read_etag = c == 0 || c == '&' || c == '=';
         }
      }
   }

   if (flags & COAP_SEND_FLAG_NO_RESPONSE) {
      err = coap_append_option_int(&request, COAP_OPTION_NO_RESPONSE,
                                   COAP_NO_RESPONSE_IGNORE_ALL);
      if (err < 0) {
         dtls_warn("Failed to encode CoAP NO_RESPONSE option, %d", err);
         return err;
      }
   }

   err = coap_appl_client_encode_time(&request);
   if (err < 0) {
      return err;
   }

   if (read_etag) {
      if (coap_read_etag[0]) {
         err = coap_packet_append_option(&request, CUSTOM_COAP_OPTION_READ_ETAG,
                                         &coap_read_etag[1],
                                         coap_read_etag[0]);
         if (err < 0) {
            dtls_warn("Failed to encode CoAP read-etag option, %d", err);
            return err;
         } else {
            dtls_info("Send CoAP read-etag option (%u bytes)", coap_read_etag[0]);
         }
      } else {
         dtls_info("Send CoAP no read-etag option");
      }
   }

   err = get_send_interval();
   if (err > 0) {
      err = coap_append_option_int(&request, CUSTOM_COAP_OPTION_INTERVAL, err);
      if (err < 0) {
         dtls_warn("Failed to encode CoAP interval option, %d", err);
         return err;
      }
   }

   if (index > 0) {
      err = coap_packet_append_payload_marker(&request);
      if (err < 0) {
         dtls_warn("Failed to encode CoAP payload-marker, %d", err);
         return err;
      }

      err = coap_packet_append_payload(&request, buf, index);
      if (err < 0) {
         dtls_warn("Failed to encode %d bytes CoAP payload, %d", index, err);
         return err;
      }
   }
   appl_context.message_len = request.offset;
   dtls_info("CoAP request prepared, token 0x%02x%02x%02x%02x, %u bytes", token[0], token[1], token[2], token[3], request.offset);

   return appl_context.message_len;
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
            return DTLS_CLIENT_RETRY_STRATEGY_OFF;
         case 3:
            return DTLS_CLIENT_RETRY_STRATEGY_DTLS_HANDSHAKE;
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

coap_handler_t coap_appl_client_handler = {
    .get_message = coap_appl_client_message,
    .parse_data = coap_appl_client_parse_data,
};

#ifdef CONFIG_SH_CMD

static char cmd_buf[512];

static int sh_cmd_net(const char *parameter)
{
   (void)parameter;
   coap_appl_client_prepare_net_info(cmd_buf, sizeof(cmd_buf), 0);
   return coap_appl_client_prepare_net_stats(cmd_buf, sizeof(cmd_buf), 0);
}

static int sh_cmd_dev(const char *parameter)
{
   (void)parameter;
   return coap_appl_client_prepare_modem_info(cmd_buf, sizeof(cmd_buf), 0);
}

static int sh_cmd_env(const char *parameter)
{
   (void)parameter;
   return coap_appl_client_prepare_env_info(cmd_buf, sizeof(cmd_buf), 0);
}

SH_CMD(net, "", "read network info.", sh_cmd_net, NULL, 0);
SH_CMD(dev, NULL, "read device info.", sh_cmd_dev, NULL, 0);
SH_CMD(env, NULL, "read environment sensor.", sh_cmd_env, NULL, 0);
#endif /* CONFIG_SH_CMD */
