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

#include "dtls.h"

#define CLIENT_VERSION "v0.6.99"

#define FLAG_TLS 1
#define FLAG_KEEP_CONNECTION 2

#define COAP_MAX_RETRANSMISSION 3

#define REBOOT_INFOS 4

extern unsigned int transmissions[COAP_MAX_RETRANSMISSION + 2];

typedef enum { PARSE_IGN = 0, PARSE_RST, PARSE_ACK, PARSE_RESPONSE, PARSE_CON_RESPONSE } parse_result_t;

int coap_client_parse_data(uint8_t *data, size_t len);

int coap_client_prepare_post(void);

int coap_client_message(const uint8_t** buffer);

int coap_client_set_id(const char* id);

int coap_client_init(void);

#endif /* COAP_CLIENT_H */
