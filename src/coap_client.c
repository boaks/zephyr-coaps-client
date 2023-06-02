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

#include <stdio.h>
#include <string.h>
#include <zephyr/net/coap.h>
#include <zephyr/random/rand32.h>

#include "coap_client.h"
#include "dtls_debug.h"
#include "modem.h"
#include "parse.h"
#include "power_manager.h"
#include "ui.h"

#include "appl_diagnose.h"
#include "appl_storage.h"
#include "appl_storage_config.h"
#include "appl_time.h"
#include "environment_sensor.h"

#ifdef CONFIG_LOCATION_ENABLE
#include "location.h"
#endif

#define APP_COAP_MAX_MSG_LEN 1280
#define APP_COAP_VERSION 1
#define APP_COAP_LOG_PAYLOAD_SIZE 128

#define COAP_OPTION_NO_RESPONSE 0x102
#define COAP_NO_RESPONSE_IGNORE_ALL 0x1a

#define CUSTOM_COAP_OPTION_INTERVAL 0xff1c
#define CUSTOM_COAP_OPTION_TIME 0xff3c
#define CUSTOM_COAP_OPTION_READ_ETAG 0xff5c
#define CUSTOM_COAP_OPTION_READ_RESPONSE_CODE 0xff7c

#ifdef CONFIG_COAP_NO_RESPONSE_ENABLE
#define COAP_MESSAGE_TYPE COAP_TYPE_NON_CON
#else
#define COAP_MESSAGE_TYPE COAP_TYPE_CON
#endif

static uint32_t coap_current_token;
static uint16_t coap_current_mid;
static uint16_t coap_message_len = 0;
static uint8_t coap_message_buf[APP_COAP_MAX_MSG_LEN];
static uint8_t coap_read_etag[9];
static const char *coap_client_id;

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

unsigned int transmissions[COAP_MAX_RETRANSMISSION + 2];

static int coap_client_encode_time(struct coap_packet *request)
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

static void coap_client_decode_time(const struct coap_option *option)
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

static void coap_client_decode_read_etag(const struct coap_option *option)
{
   uint8_t index = 0;
   uint8_t len = option->len;
   coap_read_etag[0] = len;

   if (len == 0) {
      dtls_info("Recv CoAP etag option, empty");
      return;
   }

   if (len > sizeof(coap_read_etag) - 1) {
      len = sizeof(coap_read_etag) - 1;
   }

   for (index = 0; index < len; ++index) {
      coap_read_etag[1 + index] = option->value[index];
   }

   dtls_info("Recv CoAP etag option (%d bytes)", len);
}

static void coap_client_decode_read_response(const struct coap_option *option)
{
   int code = coap_option_value_to_int(option);
   dtls_info("CoAP read response code %d.%02d", (code >> 5) & 7, code & 0x1f);
}

static void coap_client_decode_payload(const uint8_t *payload, uint16_t len)
{
}

int coap_client_parse_data(uint8_t *data, size_t len)
{
   int err;
   struct coap_packet reply;
   struct coap_option custom_option;
   const uint8_t *payload;
   uint16_t mid;
   uint16_t payload_len;
   uint8_t token[8];
   uint8_t token_len;
   uint8_t code;
   uint8_t type;

   err = coap_packet_parse(&reply, data, len, NULL, 0);
   if (err < 0) {
      dtls_debug("Malformed response received: %d\n", err);
      return err;
   }

   type = coap_header_get_type(&reply);
   code = coap_header_get_code(&reply);
   mid = coap_header_get_id(&reply);
   if (COAP_CODE_EMPTY == code) {
      if (COAP_TYPE_CON == type) {
         /* ping, ignore for now */
         return PARSE_IGN;
      }
      if (coap_current_mid == mid) {
         if (COAP_TYPE_ACK == type) {
            dtls_info("CoAP ACK %u received.", mid);
            return PARSE_ACK;
         } else if (COAP_TYPE_RESET == type) {
            dtls_debug("CoAP RST %u received.", mid);
            return PARSE_RST;
         } else if (COAP_TYPE_NON_CON == type) {
            dtls_debug("CoAP NON %u received.", mid);
            return PARSE_IGN;
         }
      } else {
         dtls_debug("CoAP msg %u received, mismatching %u.", mid, coap_current_mid);
         return PARSE_IGN;
      }
   }

   token_len = coap_header_get_token(&reply, token);
   if ((token_len != sizeof(coap_current_token)) ||
       (memcmp(&coap_current_token, token, token_len) != 0)) {
      dtls_debug("Invalid token received: 0x%02x%02x%02x%02x", token[0], token[1], token[2], token[3]);
      return PARSE_IGN;
   }

   coap_message_len = 0;

   err = coap_find_options(&reply, CUSTOM_COAP_OPTION_TIME, &custom_option, 1);
   if (err == 1) {
      coap_client_decode_time(&custom_option);
   }
   err = coap_find_options(&reply, CUSTOM_COAP_OPTION_READ_ETAG, &custom_option, 1);
   if (err == 1) {
      coap_client_decode_read_etag(&custom_option);
   }
   err = coap_find_options(&reply, CUSTOM_COAP_OPTION_READ_RESPONSE_CODE, &custom_option, 1);
   if (err == 1) {
      coap_client_decode_read_response(&custom_option);
   }

   payload = coap_packet_get_payload(&reply, &payload_len);

   if (COAP_TYPE_ACK == type) {
      dtls_info("CoAP ACK response received. code: %d.%02d, token 0x%02x%02x%02x%02x, %d bytes", (code >> 5) & 7, code & 0x1f, token[0], token[1], token[2], token[3], payload_len);
   } else if (COAP_TYPE_CON == type) {
      dtls_info("CoAP CON response received. code: %d.%02d, token 0x%02x%02x%02x%02x, %d bytes", (code >> 5) & 7, code & 0x1f, token[0], token[1], token[2], token[3], payload_len);
   } else if (COAP_TYPE_NON_CON == type) {
      dtls_info("CoAP NON response received. code: %d.%02d, token 0x%02x%02x%02x%02x, %d bytes", (code >> 5) & 7, code & 0x1f, token[0], token[1], token[2], token[3], payload_len);
   }
   if (payload_len > 0) {
      if (payload_len > APP_COAP_LOG_PAYLOAD_SIZE) {
         payload_len = APP_COAP_LOG_PAYLOAD_SIZE;
      }
      memcpy(coap_message_buf, payload, payload_len);
      coap_message_buf[payload_len] = 0;
      dtls_info("  payload: '%s'", (const char *)coap_message_buf);
   }
   coap_client_decode_payload(payload, payload_len);

   if (COAP_TYPE_CON == type) {
      struct coap_packet ack;
      err = coap_ack_init(&ack, &reply, coap_message_buf, sizeof(coap_message_buf), 0);
      if (err < 0) {
         dtls_warn("Failed to create CoAP ACK, %d", err);
      } else {
         dtls_info("Created CoAP ACK, mid %u", mid);
         coap_message_len = ack.offset;
         return PARSE_CON_RESPONSE;
      }
   }
   return PARSE_RESPONSE;
}

#ifdef ENVIRONMENT_SENSOR
#if (CONFIG_ENVIRONMENT_HISTORY_SIZE > 0)
static double s_temperatures[CONFIG_ENVIRONMENT_HISTORY_SIZE];
static uint16_t s_iaqs[CONFIG_ENVIRONMENT_HISTORY_SIZE];
#endif
#endif

static int coap_client_add_uri_query(struct coap_packet *request, const char *query)
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

static int coap_client_add_uri_query_param(struct coap_packet *request, const char *query, const char *value)
{
   if (query && strlen(query) > 0 && value && strlen(value) > 0) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%s=%s", query, value);
      return coap_client_add_uri_query(request, buf);
   }
   return 0;
}

static int coap_client_add_uri_query_param_opt(struct coap_packet *request, const char *query, const char *value)
{
   if (value && strlen(value) > 0) {
      return coap_client_add_uri_query_param(request, query, value);
   } else {
      return coap_client_add_uri_query(request, query);
   }
}

union lte_params {
   struct lte_lc_psm_cfg psm;
   struct lte_lc_edrx_cfg edrx;
   struct lte_network_info network_info;
   struct lte_network_statistic network_statistic;
   struct lte_ce_info ce_info;
   struct lte_sim_info sim_info;
};

int coap_client_prepare_post(void)
{
#ifdef ENVIRONMENT_SENSOR
   double value = 0.0;
   int32_t int_value = 0;
   uint8_t byte_value = 0;
#endif
#ifdef CONFIG_LOCATION_ENABLE
   static uint32_t max_execution_time = 0;
   static uint32_t max_satellites_time = 0;
   struct modem_gnss_state result;
   bool pending;
#endif
#if defined(ENVIRONMENT_SENSOR) || defined(CONFIG_LOCATION_ENABLE)
   const char *p;
#endif

   power_manager_status_t battery_status = POWER_UNKNOWN;
   uint16_t battery_voltage = 0xffff;
   int16_t battery_forecast = -1;
   uint8_t battery_level = 0xff;
   int64_t reboot_times[REBOOT_INFOS];
   uint16_t reboot_codes[REBOOT_INFOS];

   char buf[800];
   int err;
   int index;
   int start;
   int64_t uptime;

   uint8_t *token = (uint8_t *)&coap_current_token;
   struct coap_packet request;

#ifdef CONFIG_COAP_QUERY_DELAY_ENABLE
   static int query_delay = 0;
   char query[30];
#endif

#if defined(CONFIG_COAP_SEND_NETWORK_INFO) || defined(CONFIG_COAP_SEND_SIM_INFO) || defined(CONFIG_COAP_SEND_STATISTIC_INFO)
   union lte_params params;

   memset(&params, 0, sizeof(params));
#endif

   coap_message_len = 0;

   uptime = k_uptime_get() / MSEC_PER_SEC;

   power_manager_status(&battery_level, &battery_voltage, &battery_status, &battery_forecast);

   start = 0;
   if ((uptime / 60) < 5) {
      index = snprintf(buf, sizeof(buf), "%lu [s]", (unsigned long)uptime);
   } else {
      uint8_t secs = uptime % 60;
      uptime = uptime / 60;
      if (uptime < 60) {
         index = snprintf(buf, sizeof(buf), "%u:%02u [m:ss]", (uint8_t)uptime, secs);
      } else {
         uint8_t mins = uptime % 60;
         uptime = uptime / 60;
         if (uptime < 24) {
            index = snprintf(buf, sizeof(buf), "%u:%02u:%02u [h:mm:ss]", (uint8_t)uptime, mins, secs);
         } else {
            uint8_t hours = uptime % 24;
            uptime = uptime / 24;
            index = snprintf(buf, sizeof(buf), "%u-%02u:%02u:%02u [d-hh:mm:ss]", (uint8_t)uptime, hours, mins, secs);
         }
      }
   }

   index += snprintf(buf + index, sizeof(buf) - index, ", Thingy:91 %s, 0*%u, 1*%u, 2*%u, 3*%u, failures %u",
                     CLIENT_VERSION, transmissions[0], transmissions[1], transmissions[2], transmissions[3], transmissions[4]);
   dtls_info("%s", buf + start);

   if (battery_voltage < 0xffff) {
      start = index + 1;
      index += snprintf(buf + index, sizeof(buf) - index, "\n!%u mV", battery_voltage);
      if (battery_level < 0xff) {
         index += snprintf(buf + index, sizeof(buf) - index, " %u%%", battery_level);
      }
      if (battery_forecast > 1 || battery_forecast == 0) {
         index += snprintf(buf + index, sizeof(buf) - index, " (%u days left)", battery_forecast);
      } else if (battery_forecast == 1) {
         index += snprintf(buf + index, sizeof(buf) - index, " (1 day left)");
      }
      const char *msg = "";
      switch (battery_status) {
         case FROM_BATTERY:
            msg = "battery";
            break;
         case CHARGING_TRICKLE:
            msg = "charging (trickle)";
            break;
         case CHARGING_I:
            msg = "charging (I)";
            break;
         case CHARGING_V:
            msg = "charging (V)";
            break;
         case CHARGING_COMPLETED:
            msg = "full";
            break;
         default:
            break;
      }
      if (strlen(msg)) {
         index += snprintf(buf + index, sizeof(buf) - index, " %s", msg);
      }
      dtls_info("%s", buf + start);
   }

   memset(reboot_times, 0, sizeof(reboot_times));
   memset(reboot_codes, 0, sizeof(reboot_codes));
   err = appl_storage_read_int_items(REBOOT_CODE_ID, 0, reboot_times, reboot_codes, REBOOT_INFOS);
   if (err > 0) {
      start = index + 1;
      index += snprintf(buf + index, sizeof(buf) - index, "\nLast code: ");
      index += appl_format_time(reboot_times[0], buf + index, sizeof(buf) - index);
      index += snprintf(buf + index, sizeof(buf) - index, " 0x%04x", reboot_codes[0]);
      for (int i = 1; i < err; ++i) {
         index += snprintf(buf + index, sizeof(buf) - index, ", 0x%04x", reboot_codes[i]);
      }
      dtls_info("%s", buf + start);
   }

   start = index + 1;
   index += snprintf(buf + index, sizeof(buf) - index, "\nRestart: ");
   err = appl_reset_cause_description(buf + index, sizeof(buf) - index);
   if (err > 0) {
      dtls_info("%s", buf + start);
      index += err;
   } else {
      index = start - 1;
   }
#if 0
   err = modem_at_cmd("AT%%CONEVAL", buf + index, sizeof(buf) - index, "%CONEVAL: ");
   if (err < 0) {
      dtls_warn("Failed to read CONEVAL.");
   } else {
      dtls_info("CONEVAL: %s", buf + index);
   }
#endif

#ifdef CONFIG_COAP_SEND_SIM_INFO
   memset(&params, 0, sizeof(params));
   if (modem_get_sim_info(&params.sim_info) >= 0 && params.sim_info.valid) {
      start = index + 1;
      index += snprintf(buf + index, sizeof(buf) - index, "\nICCID: %s, eDRX cycle: %s",
                        params.sim_info.iccid, params.sim_info.edrx_cycle_support ? "on" : "off");
      if (params.sim_info.hpplmn_search_interval && params.sim_info.hpplmn[0]) {
         index += snprintf(buf + index, sizeof(buf) - index, ", HPPLMN %s interval: %d [h]",
                           params.sim_info.hpplmn, params.sim_info.hpplmn_search_interval);
      } else if (params.sim_info.hpplmn_search_interval) {
         index += snprintf(buf + index, sizeof(buf) - index, ", HPPLMN interval: %d [h]",
                           params.sim_info.hpplmn_search_interval);
      }
      dtls_info("%s", buf + start);
      start = index + 1;
      if (params.sim_info.prev_imsi[0]) {
         index += snprintf(buf + index, sizeof(buf) - index, "\nMulti-IMSI: %s, %s, %d s",
                           params.sim_info.imsi, params.sim_info.prev_imsi, params.sim_info.imsi_interval);
      } else {
         index += snprintf(buf + index, sizeof(buf) - index, "\nIMSI: %s", params.sim_info.imsi);
      }
      dtls_info("%s", buf + start);
      if (params.sim_info.forbidden[0]) {
         start = index + 1;
         index += snprintf(buf + index, sizeof(buf) - index, "\nForbidden: %s",
                           params.sim_info.forbidden);
         dtls_info("%s", buf + start);
      }
   }
#endif /* CONFIG_COAP_SEND_SIM_INFO */

#ifdef CONFIG_COAP_SEND_NETWORK_INFO

   memset(&params, 0, sizeof(params));
   if (!modem_get_network_info(&params.network_info)) {
      start = index + 1;
      index += snprintf(buf + index, sizeof(buf) - index, "\nNetwork: %s",
                        modem_get_network_mode_description(params.network_info.mode));
      index += snprintf(buf + index, sizeof(buf) - index, ",%s",
                        modem_get_registration_description(params.network_info.status));
      if (params.network_info.registered) {
         index += snprintf(buf + index, sizeof(buf) - index, ",Band %d", params.network_info.band);
         if (params.network_info.plmn_lock) {
            index += snprintf(buf + index, sizeof(buf) - index, ",#PLMN %s", params.network_info.provider);
         } else {
            index += snprintf(buf + index, sizeof(buf) - index, ",PLMN %s", params.network_info.provider);
         }
         index += snprintf(buf + index, sizeof(buf) - index, ",TAC %u", params.network_info.tac);
         index += snprintf(buf + index, sizeof(buf) - index, ",Cell %u", params.network_info.cell);
      }
   }
   dtls_info("%s", buf + start);

   if (params.network_info.registered) {
      start = index + 1;
      index += snprintf(buf + index, sizeof(buf) - index, "\nPDN: %s,%s", params.network_info.apn, params.network_info.local_ip);
      dtls_info("%s", buf + start);
   }

   index += snprintf(buf + index, sizeof(buf) - index, "\n");
   start = index;

   memset(&params, 0, sizeof(params));
   if (modem_get_psm_status(&params.psm) == 0) {
      if (params.psm.active_time >= 0) {
         index += snprintf(buf + index, sizeof(buf) - index, "PSM: TAU %d [s], Act %d [s]", params.psm.tau, params.psm.active_time);
      } else {
         index += snprintf(buf + index, sizeof(buf) - index, "PSM: n.a.");
      }
   }
   err = modem_get_release_time();
   if (err > 0) {
      if (index > start) {
         index += snprintf(buf + index, sizeof(buf) - index, ", ");
      }
      index += snprintf(buf + index, sizeof(buf) - index, "Released: %d ms", err);
   }
   if (index > start) {
      dtls_info("%s", buf + start);
   } else {
      index = start - 1;
   }
   memset(&params, 0, sizeof(params));
   if (modem_get_edrx_status(&params.edrx) == 0) {
      start = index + 1;
      switch (params.edrx.mode) {
         case LTE_LC_LTE_MODE_NONE:
            index += snprintf(buf + index, sizeof(buf) - index, "\neDRX: n.a.");
            break;
         case LTE_LC_LTE_MODE_LTEM:
            index += snprintf(buf + index, sizeof(buf) - index, "\neDRX: LTE-M %0.2f [s], page %0.2f [s]", params.edrx.edrx, params.edrx.ptw);
            break;
         case LTE_LC_LTE_MODE_NBIOT:
            index += snprintf(buf + index, sizeof(buf) - index, "\neDRX: NB-IoT %0.2f [s], page %0.2f [s]", params.edrx.edrx, params.edrx.ptw);
            break;
      }
      dtls_info("%s", buf + start);
      if (params.edrx.mode == LTE_LC_LTE_MODE_NONE) {
         index = start - 1;
      }
   }
#endif /* CONFIG_COAP_SEND_NETWORK_INFO */

#ifdef CONFIG_COAP_SEND_STATISTIC_INFO
   memset(&params, 0, sizeof(params));
   if (modem_get_coverage_enhancement_info(&params.ce_info) >= 0) {
      if (params.ce_info.ce_supported) {
         start = index + 1;
         index += snprintf(buf + index, sizeof(buf) - index, "\n!CE: down: %u, up: %u",
                           params.ce_info.downlink_repetition, params.ce_info.uplink_repetition);
         if (params.ce_info.rsrp < INVALID_SIGNAL_VALUE) {
            index += snprintf(buf + index, sizeof(buf) - index, ", RSRP: %d dBm",
                              params.ce_info.rsrp);
         }
         if (params.ce_info.cinr < INVALID_SIGNAL_VALUE) {
            index += snprintf(buf + index, sizeof(buf) - index, ", CINR: %d dB",
                              params.ce_info.cinr);
         }
         if (params.ce_info.snr < INVALID_SIGNAL_VALUE) {
            index += snprintf(buf + index, sizeof(buf) - index, ", SNR: %d dB",
                              params.ce_info.snr);
         }
         dtls_info("%s", buf + start);
      }
   }

   memset(&params, 0, sizeof(params));
   if (modem_read_statistic(&params.network_statistic) >= 0) {
      start = index + 1;
      index += snprintf(buf + index, sizeof(buf) - index, "\nStat: tx %u kB, rx %u kB, max %u B, avg %u B",
                        params.network_statistic.transmitted, params.network_statistic.received,
                        params.network_statistic.max_packet_size, params.network_statistic.average_packet_size);
      dtls_info("%s", buf + start);
      start = index + 1;
      index += snprintf(buf + index, sizeof(buf) - index, "\nCell updates %u, Network searchs %u (%u [s]), PSM delays %u (%u [s]), Restarts %u",
                        params.network_statistic.cell_updates, params.network_statistic.searchs, params.network_statistic.search_time,
                        params.network_statistic.psm_delays, params.network_statistic.psm_delay_time, params.network_statistic.restarts);
      dtls_info("%s", buf + start);
   }

#endif /* CONFIG_COAP_SEND_STATISTIC_INFO */

#ifdef CONFIG_LOCATION_ENABLE
   err = 1;
   start = index + 1;
   p = "???";
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
         err = 0;
         break;
      default:
         break;
   }

   if (max_satellites_time < result.satellites_time) {
      max_satellites_time = result.satellites_time;
   }

   if (result.valid) {
      index += snprintf(buf + index, sizeof(buf) - index, "\nGNSS.1=%s%s,%u-sats,%us-vis,%us-vis-max",
                        p, pending ? ",pending" : "", result.max_satellites, result.satellites_time / 1000, max_satellites_time / 1000);
      dtls_info("%s", buf + start);
#ifdef GNSS_VISIBILITY
      start = index + 1;
#else
      index = start - 1;
#endif
      if (!err) {
         if (!max_execution_time) {
            /* skip the first */
            max_execution_time = 1;
            index += snprintf(buf + index, sizeof(buf) - index, "\nGNSS.2=%us-pos",
                              result.execution_time / 1000);
         } else {
            if (max_execution_time < result.execution_time) {
               max_execution_time = result.execution_time;
            }
            index += snprintf(buf + index, sizeof(buf) - index, "\nGNSS.2=%us-pos,%us-pos-max",
                              result.execution_time / 1000, max_execution_time / 1000);
         }
      } else if (max_execution_time > 1) {
         index += snprintf(buf + index, sizeof(buf) - index, "\nGNSS.2=%us-pos-max",
                           max_execution_time / 1000);
      }
      if (index > start) {
         dtls_info("%s", buf + start);
#ifdef GNSS_EXECUTION_TIMES
         start = index + 1;
#else
         index = start - 1;
#endif
      }
      index += snprintf(buf + index, sizeof(buf) - index, "\n%sGNSS.3=%.06f,%.06f,%.01f,%.02f,%.01f",
                        err ? "*" : "",
                        result.position.latitude, result.position.longitude, result.position.accuracy,
                        result.position.altitude, result.position.altitude_accuracy);
      index += snprintf(buf + index, sizeof(buf) - index, ",%04d-%02d-%02dT%02d:%02d:%02dZ",
                        result.position.datetime.year, result.position.datetime.month, result.position.datetime.day,
                        result.position.datetime.hour, result.position.datetime.minute, result.position.datetime.seconds);
      dtls_info("%s", buf + start);
   } else {
      index += snprintf(buf + index, sizeof(buf) - index, "\nGNSS.1=%s%s", p, pending ? ",pending" : "");
      dtls_info("%s", buf + start);
   }
#endif

#ifdef ENVIRONMENT_SENSOR
   p = "";
   int_value = 0;
#if (CONFIG_ENVIRONMENT_HISTORY_SIZE > 0)
   int_value = environment_get_temperature_history(s_temperatures, CONFIG_ENVIRONMENT_HISTORY_SIZE);
   if (int_value > 0) {
      int history_index;
      index += snprintf(buf + index, sizeof(buf) - index, "\n!");
      start = index - 1;
      for (history_index = 0; history_index < int_value; ++history_index) {
         index += snprintf(buf + index, sizeof(buf) - index, "%.2f,", s_temperatures[history_index]);
      }
      --index;
      index += snprintf(buf + index, sizeof(buf) - index, " C");
      dtls_info("%s", buf + start);
   }
#endif
   if (!int_value && environment_get_temperature(&value) == 0) {
      start = index + 1;
      index += snprintf(buf + index, sizeof(buf) - index, "\n!%.2f C", value);
      dtls_info("%s", buf + start);
      p = "!";
   }
   if (environment_get_humidity(&value) == 0) {
      start = index + 1;
      index += snprintf(buf + index, sizeof(buf) - index, "\n%s%.2f %%H", p, value);
      dtls_info("%s", buf + start);
   }
   if (environment_get_pressure(&value) == 0) {
      start = index + 1;
      index += snprintf(buf + index, sizeof(buf) - index, "\n%s%.1f hPa", p, value);
      dtls_info("%s", buf + start);
   }
#if (CONFIG_ENVIRONMENT_HISTORY_SIZE > 0)
   int_value = environment_get_iaq_history(s_iaqs, CONFIG_ENVIRONMENT_HISTORY_SIZE);
   if (int_value > 0) {
      int history_index;
      const char *desc = environment_get_iaq_description(IAQ_VALUE(s_iaqs[0]));
      index += snprintf(buf + index, sizeof(buf) - index, "\n!");
      start = index - 1;
      for (history_index = 0; history_index < int_value; ++history_index) {
         index += snprintf(buf + index, sizeof(buf) - index, "%d;%d,", IAQ_VALUE(s_iaqs[history_index]), IAQ_ACCURANCY(s_iaqs[history_index]));
      }
      --index;
      index += snprintf(buf + index, sizeof(buf) - index, " Q (%s)", desc);
      dtls_info("%s", buf + start);
   }
#endif
   if (!int_value && environment_get_iaq(&int_value, &byte_value) == 0) {
      const char *desc = environment_get_iaq_description(int_value);
      start = index + 1;
      index += snprintf(buf + index, sizeof(buf) - index, "\n%s%d;%d Q (%s)", p, int_value, byte_value, desc);
      dtls_info("%s", buf + start);
   }
#else  /* ENVIRONMENT_SENSOR */
   start = index;
   index += snprintf(buf + index, sizeof(buf) - index, "\n!");
   err = modem_at_cmd("AT%%XTEMP?", buf + index, sizeof(buf) - index, "%XTEMP: ");
   if (err > 0) {
      index += err;
      index += snprintf(buf + index, sizeof(buf) - index, " C");
      dtls_info("%s", buf + start + 1);
   } else {
      if (err < 0) {
         dtls_warn("Failed to read XTEMP.");
      }
      index = start;
   }
#endif /* ENVIRONMENT_SENSOR */

   coap_current_token++;
   coap_current_mid = coap_next_id();

   err = coap_packet_init(&request, coap_message_buf, sizeof(coap_message_buf),
                          APP_COAP_VERSION, COAP_MESSAGE_TYPE,
                          sizeof(coap_current_token), token,
                          COAP_METHOD_POST, coap_current_mid);

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
   err = coap_client_add_uri_query_param(&request, "rlen", CONFIG_COAP_QUERY_RESPONSE_LENGTH);
   if (err < 0) {
      return err;
   }
#endif /* CONFIG_COAP_QUERY_RESPONSE_LENGTH */
#ifdef CONFIG_COAP_QUERY_KEEP_ENABLE
   err = coap_client_add_uri_query(&request, "keep");
   if (err < 0) {
      return err;
   }
   err = coap_client_add_uri_query_param(&request, "id", coap_client_id);
   if (err < 0) {
      return err;
   }
#endif
#if CONFIG_COAP_QUERY_ACK_ENABLE
   err = coap_client_add_uri_query(&request, "ack");
   if (err < 0) {
      return err;
   }
#endif
#ifdef CONFIG_COAP_QUERY_SERIES_ENABLE
   err = coap_client_add_uri_query(&request, "series");
   if (err < 0) {
      return err;
   }
#endif

#ifdef CONFIG_COAP_QUERY_READ_SUBRESOURCE_ENABLE
   err = coap_client_add_uri_query_param_opt(&request, "read", CONFIG_COAP_QUERY_READ_SUBRESOURCE);
   if (err < 0) {
      return err;
   }
#endif

#ifdef CONFIG_COAP_QUERY_WRITE_SUBRESOURCE_ENABLE
   err = coap_client_add_uri_query_param_opt(&request, "write", CONFIG_COAP_QUERY_WRITE_SUBRESOURCE);
   if (err < 0) {
      return err;
   }
#endif

#ifdef CONFIG_COAP_NO_RESPONSE_ENABLE
   err = coap_append_option_int(&request, COAP_OPTION_NO_RESPONSE,
                                COAP_NO_RESPONSE_IGNORE_ALL);
   if (err < 0) {
      dtls_warn("Failed to encode CoAP NO_RESPONSE option, %d", err);
      return err;
   }
#endif

#if CONFIG_COAP_SEND_INTERVAL > 0
   err = coap_append_option_int(&request, CUSTOM_COAP_OPTION_INTERVAL,
                                CONFIG_COAP_SEND_INTERVAL);
   if (err < 0) {
      dtls_warn("Failed to encode CoAP interval option, %d", err);
      return err;
   }
#endif

   err = coap_client_encode_time(&request);
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
#endif

   err = coap_packet_append_payload_marker(&request);
   if (err < 0) {
      dtls_warn("Failed to encode CoAP payload-marker, %d", err);
      return err;
   }

   err = coap_packet_append_payload(&request, buf, strlen(buf));
   if (err < 0) {
      dtls_warn("Failed to encode CoAP payload, %d", err);
      return err;
   }

   coap_message_len = request.offset;
   dtls_info("CoAP request prepared, token 0x%02x%02x%02x%02x, %u bytes", token[0], token[1], token[2], token[3], coap_message_len);

   return coap_message_len;
}

int coap_client_message(const uint8_t **buffer)
{
   if (buffer) {
      *buffer = coap_message_buf;
   }
   return coap_message_len;
}

int coap_client_init(const char *id)
{
   coap_current_token = sys_rand32_get();
   coap_client_id = id;
   return id ? strlen(id) : 0;
}
