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

#ifndef TCP_CLIENT_H
#define TCP_CLIENT_H

#include "session.h"

int tls_cert_provision(void);

int http_head(session_t *dst, bool tls, bool keep_connection, unsigned long *connected_time, uint8_t *buffer, size_t len);

int coap_post(session_t *dst, bool tls, bool keep_connection, unsigned long *connected_time, uint8_t *buffer, size_t len);

#endif /* TCP_CLIENT_H */
