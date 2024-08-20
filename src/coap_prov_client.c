/*
 * Copyright (c) 2024 Achim Kraus CloudCoap.net
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

#include "appl_settings.h"
#include "coap_prov_client.h"
#include "dtls_client.h"
#include "dtls_debug.h"
#include "parse.h"

#include "sh_cmd.h"

#define APP_COAP_LOG_PAYLOAD_SIZE 128

static COAP_CONTEXT(appl_context, 512);

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

int coap_prov_client_parse_data(uint8_t *data, size_t len)
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

   err = coap_find_options(&reply, COAP_OPTION_CONTENT_FORMAT, &message_option, 1);
   if (err == 1) {
      format = coap_client_decode_content_format(&message_option);
   }

   payload = coap_packet_get_payload(&reply, &payload_len);
   if (payload_len > 0) {
      if (code == COAP_RESPONSE_CODE_CHANGED || code == COAP_RESPONSE_CODE_CONTENT) {
         if (coap_client_printable_content_format(format)) {
            coap_client_dump_payload(appl_context.message_buf, APP_COAP_LOG_PAYLOAD_SIZE + 1, payload, payload_len);
         }
         appl_settings_provisioning_done();
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

int coap_prov_client_prepare_post(char *buf, size_t len)
{
   int err;
   int index = 0;
   uint8_t *token = (uint8_t *)&appl_context.token;
   struct coap_packet request;

   appl_context.message_len = 0;

   index = appl_settings_get_provisioning(buf, len);

   if (index > 0) {
      appl_context.token = coap_client_next_token();
      appl_context.mid = coap_next_id();

      err = coap_packet_init(&request, appl_context.message_buf, sizeof(appl_context.message_buf),
                             COAP_VERSION_1,
                             COAP_TYPE_CON,
                             sizeof(appl_context.token), token,
                             COAP_METHOD_POST, appl_context.mid);

      if (err < 0) {
         dtls_warn("Failed to create CoAP request, %d", err);
         return err;
      }

      err = coap_packet_set_path(&request, "prov");
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
   }

   return appl_context.message_len;
}

int coap_prov_client_message(const uint8_t **buffer)
{
   if (buffer) {
      *buffer = appl_context.message_buf;
   }
   return appl_context.message_len;
}

coap_handler_t coap_prov_client_handler = {
   .get_message = coap_prov_client_message,
   .parse_data = coap_prov_client_parse_data,
};
