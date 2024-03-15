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
#include <zephyr/random/random.h>

#include "coap_client.h"
#include "dtls_debug.h"
#include "modem.h"
#include "modem_at.h"
#include "modem_desc.h"
#include "modem_sim.h"
#include "parse.h"
#include "power_manager.h"
#include "ui.h"

static atomic_t token_factory = ATOMIC_INIT(0);

static COAP_CONTEXT(ack_context, 4);

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

int coap_client_decode_etag(const struct coap_option *option, uint8_t *etag)
{
   uint8_t index = 0;
   uint8_t len = option->len;

   memset(etag, 0, COAP_TOKEN_MAX_LEN + 1);

   if (len == 0) {
      return len;
   }

   if (len > COAP_TOKEN_MAX_LEN) {
      len = COAP_TOKEN_MAX_LEN;
   }

   etag[0] = len;
   for (index = 0; index < len; ++index) {
      etag[1 + index] = option->value[index];
   }

   return len;
}

int coap_client_decode_content_format(const struct coap_option *option)
{
   const char *desc = NULL;
   int format = coap_option_value_to_int(option);
   switch (format) {
      case COAP_CONTENT_FORMAT_TEXT_PLAIN:
         desc = "text/plain";
         break;
      case COAP_CONTENT_FORMAT_APP_LINK_FORMAT:
         desc = "appl/link-format";
         break;
      case COAP_CONTENT_FORMAT_APP_XML:
         desc = "appl/xml";
         break;
      case COAP_CONTENT_FORMAT_APP_OCTET_STREAM:
         desc = "appl/octetstream";
         break;
      case COAP_CONTENT_FORMAT_APP_EXI:
         desc = "appl/exi";
         break;
      case COAP_CONTENT_FORMAT_APP_JSON:
         desc = "appl/json";
         break;
      case COAP_CONTENT_FORMAT_APP_JSON_PATCH_JSON:
         desc = "appl/json-patch+json";
         break;
      case COAP_CONTENT_FORMAT_APP_MERGE_PATCH_JSON:
         desc = "appl/json-merge+json";
         break;
      case COAP_CONTENT_FORMAT_APP_CBOR:
         desc = "appl/cbor";
         break;
      default:
         break;
   }
   if (desc) {
      dtls_info("CoAP content format %s (%d)", desc, format);
   } else {
      dtls_info("CoAP content format %d", format);
   }
   return format;
}

bool coap_client_printable_content_format(int format) {
   bool res = false;
   switch (format) {
      case COAP_CONTENT_FORMAT_TEXT_PLAIN:
      case COAP_CONTENT_FORMAT_APP_LINK_FORMAT:
      case COAP_CONTENT_FORMAT_APP_XML:
      case COAP_CONTENT_FORMAT_APP_EXI:
      case COAP_CONTENT_FORMAT_APP_JSON:
      case COAP_CONTENT_FORMAT_APP_JSON_PATCH_JSON:
      case COAP_CONTENT_FORMAT_APP_MERGE_PATCH_JSON:
         res = true;
         break;
      default:
         break;
   }
   return res;
}

int coap_client_match(const struct coap_packet *reply, uint16_t expected_mid, uint32_t expected_token)
{
   uint16_t mid;
   uint16_t payload_len;
   uint8_t token[8];
   uint8_t token_len;
   uint8_t code;
   uint8_t type;

   type = coap_header_get_type(reply);
   code = coap_header_get_code(reply);
   mid = coap_header_get_id(reply);
   if (COAP_CODE_EMPTY == code) {
      if (COAP_TYPE_CON == type) {
         /* ping, ignore for now */
         return PARSE_IGN;
      }
      if (expected_mid == mid) {
         if (COAP_TYPE_ACK == type) {
            dtls_info("CoAP ACK %u received.", mid);
            return PARSE_ACK;
         } else if (COAP_TYPE_RESET == type) {
            dtls_debug("CoAP RST %u received.", mid);
            return PARSE_RST;
         } else if (COAP_TYPE_NON_CON == type) {
            dtls_debug("CoAP empty NON %u received.", mid);
            return PARSE_IGN;
         }
      } else {
         dtls_debug("CoAP msg %u received, mismatching %u.", mid, expected_mid);
         return PARSE_NONE;
      }
   }

   token_len = coap_header_get_token(reply, token);
   if ((token_len != sizeof(expected_token)) ||
       (memcmp(&expected_token, token, token_len) != 0)) {
      dtls_debug("Invalid token received: 0x%02x%02x%02x%02x", token[0], token[1], token[2], token[3]);
      return PARSE_NONE;
   }

   coap_packet_get_payload(reply, &payload_len);

   if (COAP_TYPE_ACK == type) {
      dtls_info("CoAP ACK response received. code: %d.%02d, token 0x%02x%02x%02x%02x, %d bytes", (code >> 5) & 7, code & 0x1f, token[0], token[1], token[2], token[3], payload_len);
   } else if (COAP_TYPE_CON == type) {
      dtls_info("CoAP CON response received. code: %d.%02d, token 0x%02x%02x%02x%02x, %d bytes", (code >> 5) & 7, code & 0x1f, token[0], token[1], token[2], token[3], payload_len);
   } else if (COAP_TYPE_NON_CON == type) {
      dtls_info("CoAP NON response received. code: %d.%02d, token 0x%02x%02x%02x%02x, %d bytes", (code >> 5) & 7, code & 0x1f, token[0], token[1], token[2], token[3], payload_len);
   }

   if (COAP_TYPE_CON == type) {
      return PARSE_CON_RESPONSE;
   }
   return PARSE_RESPONSE;
}

int coap_client_prepare_ack(const struct coap_packet *reply)
{
   struct coap_packet ack;
   int err = coap_ack_init(&ack, reply, ack_context.message_buf, sizeof(ack_context.message_buf), 0);
   if (err < 0) {
      dtls_warn("Failed to create CoAP ACK, %d", err);
      ack_context.message_len = 0;
      return PARSE_RESPONSE;
   } else {
      dtls_info("Created CoAP ACK, mid %u", coap_header_get_id(reply));
      ack_context.message_len = ack.offset;
      return PARSE_CON_RESPONSE;
   }
}

long coap_client_next_token(void)
{
   return atomic_inc(&token_factory);
}

int coap_client_message(const uint8_t **buffer)
{
   if (buffer) {
      *buffer = ack_context.message_buf;
   }
   return ack_context.message_len;
}

int coap_client_init(void)
{
   atomic_set(&token_factory, sys_rand32_get());
   return 0;
}
