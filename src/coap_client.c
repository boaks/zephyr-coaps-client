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

#include <net/coap.h>
#include <random/rand32.h>
#include <stdio.h>
#include <string.h>

#include "coap_client.h"
#include "dtls_debug.h"
#include "modem.h"
#include "power_manager.h"
#include "ui.h"

#include "environment_sensor.h"

#ifdef CONFIG_LOCATION_ENABLE
#include "modem_location.h"
#endif

#define APP_COAP_MAX_MSG_LEN 1280
#define APP_COAP_VERSION 1
#define APP_COAP_LOG_PAYLOAD_SIZE 128

static uint32_t coap_current_token;
static uint16_t coap_current_mid;
static uint16_t coap_message_len = 0;
static uint8_t coap_message_buf[APP_COAP_MAX_MSG_LEN];

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

unsigned int transmissions[COAP_MAX_RETRANSMISSION + 2];
unsigned int bat_level[BAT_LEVEL_SLOTS] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

int coap_client_parse_data(uint8_t *data, size_t len)
{
   int err;
   struct coap_packet reply;
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
            dtls_debug("CoAP ACK %u received.", mid);
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

   payload = coap_packet_get_payload(&reply, &payload_len);

   dtls_info("CoAP response received. code: %d.%02d, token 0x%02x%02x%02x%02x, %d bytes", (code >> 5) & 7, code & 0x1f, token[0], token[1], token[2], token[3], payload_len);
   if (payload_len > 0) {
      if (payload_len > APP_COAP_LOG_PAYLOAD_SIZE) {
         payload_len = APP_COAP_LOG_PAYLOAD_SIZE;
      }
      memcpy(coap_message_buf, payload, payload_len);
      coap_message_buf[payload_len] = 0;
      dtls_info("  payload: %s", coap_message_buf);
   }

   return PARSE_RESPONSE;
}

int coap_client_prepare_post(void)
{
#ifdef ENVIRONMENT_SENSOR
   double value = 0.0;
   int32_t int_value = 0;
#endif
#ifdef CONFIG_LOCATION_ENABLE
   struct location_data location;
#endif

   power_manager_status_t battery_status = POWER_UNKNOWN;
   uint16_t battery_voltage = 0xffff;
   uint8_t battery_level = 0xff;

   const char *p;
   char buf[256];
   int err;
   int index;
   int start;
   unsigned int uptime;

   uint8_t *token = (uint8_t *)&coap_current_token;
   struct coap_packet request;
   struct lte_lc_psm_cfg psm;

#ifdef CONFIG_COAP_QUERY_DELAY_ENABLE
   static int query_delay = 0;
   char query[30];
#endif

   coap_message_len = 0;

   for (index = BAT_LEVEL_SLOTS - 1; index > 0; --index) {
      bat_level[index] = bat_level[index - 1];
   }
   bat_level[0] = 1;

   uptime = (unsigned int)(k_uptime_get() / 1000);

   if (!power_manager_status(&battery_level, &battery_voltage, &battery_status)) {
      if (battery_voltage != 0xffff) {
         bat_level[0] = battery_voltage;
      }
   }

   err = modem_at_cmd("AT+CESQ", coap_message_buf, sizeof(coap_message_buf), "+CESQ: ");
   if (err < 0) {
      dtls_warn("Failed to read signal level!");
   }
   if (err > 0) {
      p = coap_message_buf;
      index = 0;
      while (*p && index < 4) {
         if (*p++ == ',') {
            ++index;
         }
      }
   } else {
      p = NULL;
   }
   start = 0;
   index = snprintf(buf, sizeof(buf), "%u s, Thingy:91 %s, 0*%u, 1*%u, 2*%u, 3*%u, failures %u",
                    uptime, CLIENT_VERSION, transmissions[0], transmissions[1], transmissions[2], transmissions[3], transmissions[4]);
   dtls_info("%s", buf + start);

   if (bat_level[0] > 1) {
      start = index + 1;
      index += snprintf(buf + index, sizeof(buf) - index, "\n%u mV", bat_level[0]);
      if (battery_level < 0xff) {
         index += snprintf(buf + index, sizeof(buf) - index, " %u%%", battery_level);
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
      index += snprintf(buf + index, sizeof(buf) - index, " %s", msg);
      dtls_info("%s", buf + start);
   }

   if (p) {
      start = index + 1;
      index += snprintf(buf + index, sizeof(buf) - index, "\nRSSI q,p: %s", p);
      dtls_info("%s", buf + start);
   }
   p = modem_get_network_mode();
   start = index + 1;
   index += snprintf(buf + index, sizeof(buf) - index, "\nNetwork: %s", p);
   dtls_info("%s", buf + start);

   index += snprintf(buf + index, sizeof(buf) - index, "\n");
   start = index;
   if (modem_get_psm_status(&psm) == 0) {
      index += snprintf(buf + index, sizeof(buf) - index, "PSM: %d s", psm.tau);
   }
   err = modem_get_release_time();
   if (err > 0) {
      if (index > start) {
         index += snprintf(buf + index, sizeof(buf) - index, ", ");
      }
      index += snprintf(buf + index, sizeof(buf) - index, "Released: %d ms", err);
   }
   dtls_info("%s", buf + start);

#ifdef CONFIG_LOCATION_ENABLE
   err = 0;
   switch (modem_location_get(0, &location)) {
      case NO_LOCATION:
         index += snprintf(buf + index, sizeof(buf) - index, "\n*POS=n.a.");
         dtls_info("No location");
         break;
      case PENDING_LOCATION:
         index += snprintf(buf + index, sizeof(buf) - index, "\n*POS=pending");
         dtls_info("No location - pending");
         break;
      case TIMEOUT_LOCATION:
         index += snprintf(buf + index, sizeof(buf) - index, "\n*POS=timeout");
         dtls_info("No location - timeout");
         break;
      case PREVIOUS_LOCATION:
         err = 1;
      case CURRENT_LOCATION:
         if (location.datetime.valid) {
            index += snprintf(buf + index, sizeof(buf) - index, "\n%sPOS=%.06f,%.06f,%.01f,%04d-%02d-%02dT%02d:%02d:%02dZ",
                              err ? "*" : "",
                              location.latitude, location.longitude, location.accuracy, location.datetime.year, location.datetime.month, location.datetime.day, location.datetime.hour, location.datetime.minute, location.datetime.second);
         } else {
            index += snprintf(buf + index, sizeof(buf) - index, "\n%sPOS=%.06f,%.06f,%.01f",
                              err ? "*" : "",
                              location.latitude, location.longitude, location.accuracy);
         }
         dtls_info("URL: https://maps.google.com/?q=%.06f,%.06f,%.01f",
                   location.latitude, location.longitude, location.accuracy);
         break;
      default:
         break;
   }
#endif

#ifdef ENVIRONMENT_SENSOR
   if (environment_get_temperature(&value) == 0) {
      start = index + 1;
      index += snprintf(buf + index, sizeof(buf) - index, "\n%.2f C", value);
      dtls_info("%s", buf + start);
   }
   if (environment_get_humidity(&value) == 0) {
      start = index + 1;
      index += snprintf(buf + index, sizeof(buf) - index, "\n%.2f %%H", value);
      dtls_info("%s", buf + start);
   }
   if (environment_get_pressure(&value) == 0) {
      start = index + 1;
      index += snprintf(buf + index, sizeof(buf) - index, "\n%.1f hPa", value);
      dtls_info("%s", buf + start);
   }
   if (environment_get_iaq(&int_value) == 0) {
      start = index + 1;
      const char *desc = "???";
      switch (int_value > 0 ? (int_value - 1) / 50 : 0) {
         case 0:
            desc = "excellent";
            break;
         case 1:
            desc = "good";
            break;
         case 2:
            desc = "lightly polluted";
            break;
         case 3:
            desc = "moderately polluted";
            break;
         case 4:
            desc = "heavily polluted";
            break;
         case 5:
         case 6:
            desc = "severely polluted";
            break;
         default:
            desc = "extremely polluted";
            break;
      }
      index += snprintf(buf + index, sizeof(buf) - index, "\n%d Q (%s)", int_value, desc);
      dtls_info("%s", buf + start);
   }
#endif
   coap_current_token++;
   coap_current_mid = coap_next_id();

   err = coap_packet_init(&request, coap_message_buf, sizeof(coap_message_buf),
                          APP_COAP_VERSION, COAP_TYPE_NON_CON,
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
   dtls_info("CoAP request, delay %d", query_delay);

   err = coap_packet_append_option(&request, COAP_OPTION_URI_QUERY,
                                   (uint8_t *)query,
                                   strlen(query));
   if (err < 0) {
      dtls_warn("Failed to encode CoAP URI-QUERY option 'delay=%d', %d", query_delay, err);
      return err;
   }
   if (query_delay > 30000) {
      query_delay = 0;
   } else {
      query_delay += 2000;
   }
#endif /* CONFIG_COAP_QUERY_DELAY_ENABLE */
#ifdef CONFIG_COAP_QUERY_KEEP_ENABLE
   err = coap_packet_append_option(&request, COAP_OPTION_URI_QUERY,
                                   (uint8_t *)"keep",
                                   strlen("keep"));
   if (err < 0) {
      dtls_warn("Failed to encode CoAP URI-QUERY option 'keep', %d", err);
      return err;
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

   return err;
}

int coap_client_send_post(struct dtls_context_t *ctx, session_t *dst)
{
   int result = dtls_write(ctx, dst, coap_message_buf, coap_message_len);
   if (result < 0) {
      dtls_warn("Failed to send CoAP request, %d", errno);
   }
   return result;
}

int coap_client_init(void)
{
   power_manager_init();
   coap_current_token = sys_rand32_get();
   return 0;
}
