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

#ifndef COAP_PROV_CLIENT_H
#define COAP_PROC_CLIENT_H

#include "coap_client.h"

int coap_prov_client_parse_data(uint8_t *data, size_t len);

int coap_prov_client_prepare_post(char *buf, size_t len);

int coap_prov_client_message(const uint8_t** buffer);

extern coap_handler_t coap_prov_client_handler;

#endif /* COAP_PROV_CLIENT_H */
