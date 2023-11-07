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
#include "ui.h"

#include "appl_diagnose.h"
#include "appl_storage.h"
#include "appl_storage_config.h"
#include "appl_time.h"
#include "environment_sensor.h"

#include "uart_cmd.h"

#ifdef CONFIG_LOCATION_ENABLE
#include "location.h"
#endif

#ifdef CONFIG_COAP_UPDATE
#include "appl_update_coap.h"
#endif

#ifdef CONFIG_NAU7802_SCALE
#include "nau7802.h"
#endif

#ifdef CONFIG_EXT_BATTERY_ADC
#include "battery_adc.h"
#endif

#define APP_COAP_LOG_PAYLOAD_SIZE 128

#define COAP_OPTION_NO_RESPONSE 0x102
#define COAP_NO_RESPONSE_IGNORE_ALL 0x1a

#define CUSTOM_COAP_OPTION_TIME 0xfde8
#define CUSTOM_COAP_OPTION_READ_ETAG 0xfdec
#define CUSTOM_COAP_OPTION_READ_RESPONSE_CODE 0xfdf0
#define CUSTOM_COAP_OPTION_INTERVAL 0xfdf4

static COAP_CONTEXT(appl_context, 1280);

static uint8_t coap_read_etag[COAP_TOKEN_MAX_LEN + 1];
static const char *coap_client_id;

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

static uint8_t coap_appl_client_decode_read_response(const struct coap_option *option)
{
   int code = coap_option_value_to_int(option);
   dtls_info("Recv CoAP read response code %d.%02d", (code >> 5) & 7, code & 0x1f);
   return (uint8_t)code;
}

static void coap_appl_client_decode_payload(const uint8_t *payload, uint16_t len)
{
}

static void coap_appl_client_decode_text_payload(char *payload)
{
   while (*payload) {
      int pos = strcspn(payload, " :=\n\r");
      if (pos) {
         char *val = "";
         char *cur = payload;
         char sep = cur[pos];
         cur[pos] = 0;
         payload += pos + 1;
         if (strchr(" :=", sep)) {
            pos = strcspn(payload, "\n\r");
            if (pos) {
               val = payload;
               val[pos] = 0;
               payload += pos + 1;
               payload += strspn(payload, "\n\r");
            }
         }
         if (!stricmp(cur, "cmd")) {
            dtls_info("cmd %s", val);
         } else if (!stricmp(cur, "fw")) {
            dtls_info("fw %s", val);
#ifdef CONFIG_COAP_UPDATE
            appl_update_coap_cmd(val);
#endif
         }
      }
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
         code = coap_appl_client_decode_read_response(&message_option);
      }
      err = coap_find_options(&reply, CUSTOM_COAP_OPTION_READ_ETAG, &message_option, 1);
      if (err == 1) {
         coap_appl_client_decode_read_etag(&message_option);
      }
   }

   if (code == COAP_RESPONSE_CODE_CONTENT) {

      err = coap_find_options(&reply, COAP_OPTION_CONTENT_FORMAT, &message_option, 1);
      if (err == 1) {
         format = coap_client_decode_content_format(&message_option);
      }

      payload = coap_packet_get_payload(&reply, &payload_len);

      if (payload_len > 0) {
         const char *more = "";
         if (format == COAP_CONTENT_FORMAT_TEXT_PLAIN && payload_len < sizeof(appl_context.message_buf)) {
            memmove(appl_context.message_buf, payload, payload_len);
            appl_context.message_buf[payload_len] = 0;
            coap_appl_client_decode_text_payload(appl_context.message_buf);
         } else {
            coap_appl_client_decode_payload(payload, payload_len);
         }
         if (payload_len > APP_COAP_LOG_PAYLOAD_SIZE) {
            payload_len = APP_COAP_LOG_PAYLOAD_SIZE;
            more = "...";
         }
         memmove(appl_context.message_buf, payload, payload_len);
         appl_context.message_buf[payload_len] = 0;
         dtls_info("  payload: '%s'%s", (const char *)appl_context.message_buf, more);
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

#if defined(CONFIG_COAP_QUERY_READ_SUBRESOURCE_ENABLE) || defined(CONFIG_COAP_QUERY_WRITE_SUBRESOURCE_ENABLE)
static int coap_appl_client_add_uri_query_param_opt(struct coap_packet *request, const char *query, const char *value)
{
   if (value && strlen(value) > 0) {
      return coap_appl_client_add_uri_query_param(request, query, value);
   } else {
      return coap_appl_client_add_uri_query(request, query);
   }
}
#endif

int coap_appl_client_prepare_modem_info(char *buf, size_t len, int flags)
{
   int64_t reboot_times[REBOOT_INFOS];
   uint16_t reboot_codes[REBOOT_INFOS];
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
                     CONFIG_APPL_MODEL_DESCRIPTION, appl_get_version(), transmissions[0], transmissions[1], transmissions[2], transmissions[3], transmissions[4]);
   dtls_info("%s", buf + start);

   buf[index++] = '\n';
   start = index;

   if (!(flags & COAP_SEND_FLAG_MINIMAL)) {
      memset(&modem_info, 0, sizeof(modem_info));
      if (!modem_get_modem_info(&modem_info)) {
         index += snprintf(buf + index, len - index, "NCS: %s, HW: %s, MFW: %s, IMEI: %s", NCS_VERSION_STRING, modem_info.version, modem_info.firmware, modem_info.imei);
         dtls_info("%s", buf + start);
         buf[index++] = '\n';
         start = index;
      }
#ifdef CONFIG_COAP_UPDATE
      index += appl_update_coap_status(buf + index, len - index);
      if (index > start) {
         dtls_info("%s", buf + start);
         buf[index++] = '\n';
         start = index;
      }
#endif
   }

   err = power_manager_status_desc(&buf[index + 1], len - index - 1);
   if (err) {
      buf[index++] = '!';
      index += err;
      dtls_info("%s", buf + start);
      buf[index++] = '\n';
      start = index;
   }

#ifdef CONFIG_EXT_BATTERY_ADC
   {
      uint16_t battery_voltage = 0xffff;
      err = battery2_sample(&battery_voltage);
      if (!err) {
         index += snprintf(buf + index, len - index, "!Ext.Bat.: %u mV", battery_voltage);
         dtls_info("%s", buf + start);
         buf[index++] = '\n';
         start = index;
      }
   }
#endif

   memset(reboot_times, 0, sizeof(reboot_times));
   memset(reboot_codes, 0, sizeof(reboot_codes));
   err = appl_storage_read_int_items(REBOOT_CODE_ID, 0, reboot_times, reboot_codes, REBOOT_INFOS);
   if (err > 0) {
      index += snprintf(buf + index, len - index, "Last code: ");
      index += appl_format_time(reboot_times[0], buf + index, len - index);
      index += snprintf(buf + index, len - index, " %s", appl_get_reboot_desciption(reboot_codes[0]));
      dtls_info("%s", buf + start);
      buf[index++] = '\n';
      start = index;
      for (int i = 1; i < err; ++i) {
         index += appl_format_time(reboot_times[i], buf + index, len - index);
         index += snprintf(buf + index, len - index, " %s", appl_get_reboot_desciption(reboot_codes[i]));
         dtls_info("%s", buf + start);
         index = start;
      }
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
      if (sim_info.prev_imsi[0]) {
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
      if (!(flags & COAP_SEND_FLAG_MINIMAL)) {
         start = index + 1;
         index += snprintf(buf + index, len - index, "\nCell updates %u, Network searchs %u (%u s), PSM delays %u (%u s), Restarts %u",
                           params.network_statistic.cell_updates, params.network_statistic.searchs, params.network_statistic.search_time,
                           params.network_statistic.psm_delays, params.network_statistic.psm_delay_time, params.network_statistic.restarts);
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

int coap_appl_client_prepare_scale_info(char *buf, size_t len, int flags)
{
   int index = 0;

#ifdef CONFIG_NAU7802_SCALE
   double scaleA = 0;
   double scaleB = 0;
   double temperatureA = 0;
   double temperatureB = 0;
   int64_t time = 0;
   int start = 0;
   int res = 0;
   int err = 0;

   res = scale_sample(&scaleA, &scaleB, &temperatureA, &temperatureB);
   if (0 < res) {

      index += snprintf(buf, len, "Last calibration: ");
      start = index;
      err = appl_storage_read_bytes_item(CALIBRATION_A_ID, 0, &time, NULL, 0);
      if (err > 0) {
         index += snprintf(buf + index, len - index, "A ");
         index += appl_format_time(time, buf + index, len - index);
      }
      err = appl_storage_read_bytes_item(CALIBRATION_B_ID, 0, &time, NULL, 0);
      if (err > 0) {
         if (index > start) {
            index += snprintf(buf + index, len - index, ", ");
         }
         index += snprintf(buf + index, len - index, "B ");
         index += appl_format_time(time, buf + index, len - index);
      }
      if (index == start) {
         index = 0;
         start = 0;
      } else {
         dtls_info("%s", buf);
      }

      if (index) {
         start = index + 1;
         index += snprintf(buf + index, len - index, "\n");
      }
      index += snprintf(buf + index, len - index, "!");
      if (res & 1) {
         index += snprintf(buf + index, len - index, "CHA %.2f kg, %.1f°C", scaleA, temperatureA);
         if (res & 2) {
            index += snprintf(buf + index, len - index, ",");
         }
      }
      if (res & 2) {
         index += snprintf(buf + index, len - index, "CHB %.2f kg, %.1f°C", scaleB, temperatureB);
      }
      dtls_info("%s", buf + start);
   }
#endif
   return index;
}

int coap_appl_client_prepare_post(char *buf, size_t len, int flags)
{
   int err;
   int index = 0;
   int start = 0;
   uint8_t *token = (uint8_t *)&appl_context.token;
   struct coap_packet request;

#ifdef CONFIG_COAP_QUERY_DELAY_ENABLE
   static int query_delay = 0;
   char query[30];
#endif

   appl_context.message_len = 0;

   if (len && buf[0] == 0) {
#ifdef CONFIG_COAP_SEND_MODEM_INFO
      err = coap_appl_client_prepare_modem_info(buf, len, flags);
      if (err > 0) {
         index = start + err;
      }
#endif

      start = index + 1;
      index += snprintf(buf + index, len - index, "\nRestart: ");
      err = appl_reset_cause_description(buf + index, sizeof(buf) - index);
      if (err > 0) {
         dtls_info("%s", buf + start);
         index += err;
      } else {
         index = start - 1;
      }

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

#if defined(CONFIG_COAP_SEND_STATISTIC_INFO)
      buf[index] = '\n';
      start = index + 1;
      err = coap_appl_client_prepare_net_stats(buf + start, len - start, flags);
      if (err > 0) {
         index = start + err;
      }
#endif /* CONFIG_COAP_SEND_STATISTIC_INFO */

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
      err = coap_appl_client_prepare_scale_info(buf + start, len - start, flags);
      if (err > 0) {
         index = start + err;
      }
#endif

      buf[index] = '\n';
      start = index + 1;
      err = coap_appl_client_prepare_env_info(buf + start, len - start, flags);
      if (err > 0) {
         index = start + err;
      }
   } else {
      index = len;
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

   err = coap_packet_append_option(&request, COAP_OPTION_URI_PATH,
                                   (uint8_t *)CONFIG_COAP_RESOURCE,
                                   strlen(CONFIG_COAP_RESOURCE));
   if (err < 0) {
      dtls_warn("Failed to encode CoAP URI-PATH option, %d", err);
      return err;
   }

   err = coap_append_option_int(&request, COAP_OPTION_CONTENT_FORMAT,
                                COAP_CONTENT_FORMAT_TEXT_PLAIN);
   if (err < 0) {
      dtls_warn("Failed to encode CoAP CONTENT_FORMAT option, %d", err);
      return err;
   }

#ifdef CONFIG_COAP_QUERY_DELAY_ENABLE
   err = snprintf(query, sizeof(query), "delay=%d", query_delay);
   dtls_info("CoAP request, %s", query);

   err = coap_packet_append_option(&request, COAP_OPTION_URI_QUERY,
                                   (uint8_t *)query,
                                   strlen(query));
   if (err < 0) {
      dtls_warn("Failed to encode CoAP URI-QUERY option '%s', %d", query, err);
      return err;
   }
   if (query_delay > 30000) {
      query_delay = 0;
   } else {
      query_delay += 2000;
   }
#endif /* CONFIG_COAP_QUERY_DELAY_ENABLE */

#ifdef CONFIG_COAP_QUERY_RESPONSE_LENGTH
   err = coap_appl_client_add_uri_query_param(&request, "rlen", CONFIG_COAP_QUERY_RESPONSE_LENGTH);
   if (err < 0) {
      return err;
   }
#endif /* CONFIG_COAP_QUERY_RESPONSE_LENGTH */
#ifdef CONFIG_COAP_QUERY_KEEP_ENABLE
   err = coap_appl_client_add_uri_query(&request, "keep");
   if (err < 0) {
      return err;
   }
   err = coap_appl_client_add_uri_query_param(&request, "id", coap_client_id);
   if (err < 0) {
      return err;
   }
#endif /* CONFIG_COAP_QUERY_KEEP_ENABLE */
#if CONFIG_COAP_QUERY_ACK_ENABLE
   err = coap_appl_client_add_uri_query(&request, "ack");
   if (err < 0) {
      return err;
   }
#endif /* CONFIG_COAP_QUERY_ACK_ENABLE */
#ifdef CONFIG_COAP_QUERY_SERIES_ENABLE
   err = coap_appl_client_add_uri_query(&request, "series");
   if (err < 0) {
      return err;
   }
#endif /* CONFIG_COAP_QUERY_SERIES_ENABLE */

#ifdef CONFIG_COAP_QUERY_READ_SUBRESOURCE_ENABLE
   err = coap_appl_client_add_uri_query_param_opt(&request, "read", CONFIG_COAP_QUERY_READ_SUBRESOURCE);
   if (err < 0) {
      return err;
   }
#endif /* CONFIG_COAP_QUERY_READ_SUBRESOURCE_ENABLE */

#ifdef CONFIG_COAP_QUERY_WRITE_SUBRESOURCE_ENABLE
   err = coap_appl_client_add_uri_query_param_opt(&request, "write", CONFIG_COAP_QUERY_WRITE_SUBRESOURCE);
   if (err < 0) {
      return err;
   }
#endif /* CONFIG_COAP_QUERY_WRITE_SUBRESOURCE_ENABLE */

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

#ifdef CONFIG_COAP_QUERY_READ_SUBRESOURCE_ENABLE
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
#endif /* CONFIG_COAP_QUERY_READ_SUBRESOURCE_ENABLE */

#if CONFIG_COAP_SEND_INTERVAL > 0
   err = coap_append_option_int(&request, CUSTOM_COAP_OPTION_INTERVAL,
                                CONFIG_COAP_SEND_INTERVAL);
   if (err < 0) {
      dtls_warn("Failed to encode CoAP interval option, %d", err);
      return err;
   }
#endif /* CONFIG_COAP_SEND_INTERVAL */

   err = coap_packet_append_payload_marker(&request);
   if (err < 0) {
      dtls_warn("Failed to encode CoAP payload-marker, %d", err);
      return err;
   }

   err = coap_packet_append_payload(&request, buf, index);
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

int coap_appl_client_init(const char *id)
{
   coap_client_id = id;
   return id ? strlen(id) : 0;
}

static char cmd_buf[512];

static int at_cmd_net(const char *parameter)
{
   (void)parameter;
   coap_appl_client_prepare_net_info(cmd_buf, sizeof(cmd_buf), 0);
   return coap_appl_client_prepare_net_stats(cmd_buf, sizeof(cmd_buf), 0);
}

static int at_cmd_dev(const char *parameter)
{
   (void)parameter;
   return coap_appl_client_prepare_modem_info(cmd_buf, sizeof(cmd_buf), 0);
}

static int at_cmd_env(const char *parameter)
{
   (void)parameter;
   return coap_appl_client_prepare_env_info(cmd_buf, sizeof(cmd_buf), 0);
}

UART_CMD(net, "", "read network info.", at_cmd_net, NULL, 0);
#ifdef CONFIG_BATTERY_VOLTAGE_SOURCE_MODEM
UART_CMD(dev, NULL, "read device info.", at_cmd_dev, NULL, 0);
#else
UART_CMD(dev, "", "read device info.", at_cmd_dev, NULL, 0);
#endif
UART_CMD(env, NULL, "read environment sensor.", at_cmd_env, NULL, 0);
