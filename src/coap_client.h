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

#ifndef COAP_CLIENT_H
#define COAP_CLIENT_H

#include <zephyr/net/coap.h>

#include "dtls_client.h"

typedef enum { PARSE_NONE = 0,
               PARSE_IGN,
               PARSE_RST,
               PARSE_ACK,
               PARSE_RESPONSE,
               PARSE_CON_RESPONSE } parse_result_t;

#define COAP_CONTEXT(N, S)    \
   struct N##_coap_context {  \
      uint32_t token;         \
      uint16_t mid;           \
      uint16_t message_len;   \
      uint8_t message_buf[S]; \
   } N = {0, 0, 0}

typedef int (*coap_client_get_message_t)(const uint8_t **buffer);
typedef int (*coap_client_parse_data_handler_t)(uint8_t *data, size_t len);

typedef struct coap_handler {
   coap_client_get_message_t get_message;
   coap_client_parse_data_handler_t parse_data;
} coap_handler_t;

int coap_client_decode_content_format(const struct coap_option *option);

int coap_client_decode_etag(const struct coap_option *option, uint8_t *etag);

bool coap_client_printable_content_format(int format);

void coap_client_dump_payload(char *buf, size_t size, const char *payload, size_t len);

int coap_client_match(const struct coap_packet *reply, uint16_t mid, uint32_t token);

int coap_client_prepare_ack(const struct coap_packet *reply);

int coap_client_message(const uint8_t **buffer);

long coap_client_next_token(void);

int coap_client_init(void);

#endif /* COAP_CLIENT_H */
