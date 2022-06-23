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
#include "ui.h"

#ifdef CONFIG_EXTERNAL_SENSORS
#include "ext_sensors.h"
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

#ifdef CONFIG_EXTERNAL_SENSORS
static void ext_sensor_handler(const struct ext_sensor_evt *const evt)
{
   switch (evt->type) {
      case EXT_SENSOR_EVT_ACCELEROMETER_ERROR:
         dtls_info("accelerometer error!");
         break;
      case EXT_SENSOR_EVT_TEMPERATURE_ERROR:
         dtls_info("temperature error!");
         break;
      case EXT_SENSOR_EVT_HUMIDITY_ERROR:
         dtls_info("humidity error!");
         break;
      case EXT_SENSOR_EVT_PRESSURE_ERROR:
         dtls_info("accelerometer trigger");
         break;
      case EXT_SENSOR_EVT_ACCELEROMETER_TRIGGER:
         dtls_info("accelerometer trigger");
         break;
   }
}
#endif

int coap_client_parse_data(uint8 *data, size_t len)
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
   char buf[192], *p;
   int err;
   int index;
   unsigned int uptime;
#ifdef CONFIG_EXTERNAL_SENSORS
   double value = 0.0;
#endif
   uint8_t *token = (uint8_t *)&coap_current_token;
   struct coap_packet request;
#ifdef CONFIG_COAP_QUERY_DELAY_ENABLE
   static int query_delay = 0;
   char query[30];
#endif

   coap_message_len = 0;

   for (index = BAT_LEVEL_SLOTS - 1; index > 0; --index) {
      bat_level[index] = bat_level[index - 1];
   }

   uptime = (unsigned int)(k_uptime_get() / 1000);

   err = modem_at_cmd("AT%%XVBAT", coap_message_buf, sizeof(coap_message_buf), "%XVBAT: ");
   if (err < 0) {
      dtls_warn("Failed to read battery level!");
   }
   if (err > 0) {
      bat_level[0] = atoi(coap_message_buf);
   } else {
      bat_level[0] = 1;
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
   index = snprintf(buf, sizeof(buf), "%u s, Thingy:91 %s, 0*%u, 1*%u, 2*%u, 3*%u, failures %u",
                    uptime, CLIENT_VERSION, transmissions[0], transmissions[1], transmissions[2], transmissions[3], transmissions[4]);
   if (bat_level[0] > 1) {
      index += snprintf(buf + index, sizeof(buf) - index, "\n%u mV", bat_level[0]);
      dtls_info("%u mV", bat_level[0]);
   }
   if (p) {
      index += snprintf(buf + index, sizeof(buf) - index, "\nRSSI: %s", p);
      dtls_info("RSSI q,p: %s", p);
   }
#ifdef CONFIG_EXTERNAL_SENSORS
   if (ext_sensors_temperature_get(&value) == 0) {
      index += snprintf(buf + index, sizeof(buf) - index, "\n%.2f C", value);
      dtls_info("%.2f C", value);
   }
   if (ext_sensors_humidity_get(&value) == 0) {
      index += snprintf(buf + index, sizeof(buf) - index, "\n%.2f %%H", value);
      dtls_info("%.2f %%H", value);
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
   int err = 0;
#ifdef CONFIG_EXTERNAL_SENSORS
   err = ext_sensors_init(ext_sensor_handler);
   if (err < 0) {
   }
#endif
   coap_current_token = sys_rand32_get();
   return err;
}
