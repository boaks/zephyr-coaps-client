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

#define CLIENT_VERSION "v0.5.99"

#define COAP_MAX_RETRANSMISSION 3
#define BAT_LEVEL_SLOTS 10

extern unsigned int transmissions[COAP_MAX_RETRANSMISSION + 2];
extern unsigned int bat_level[BAT_LEVEL_SLOTS];

typedef enum { PARSE_IGN = 0, PARSE_RST, PARSE_ACK, PARSE_RESPONSE } parse_result_t;
int coap_client_parse_data(uint8_t *data, size_t len);

int coap_client_prepare_post(void);
int coap_client_send_post(struct dtls_context_t *ctx, session_t *dst);

int coap_client_init(void);

#endif /* COAP_CLIENT_H */
