/*
 * Copyright (c) 2023 Achim Kraus CloudCoap.net
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

#ifndef DTLS_CLIENT_H
#define DTLS_CLIENT_H

#include <stddef.h>
#include <stdbool.h>

#define COAP_MAX_RETRANSMISSION 3

enum dtls_retry_strategy {
	RETRY_NONE,
	RETRY_DTLS,
	RETRY_OFFLINE,
	RETRY_OFF,
	RETRY_RESTART,
};

extern unsigned int transmissions[COAP_MAX_RETRANSMISSION + 1];
extern unsigned int failures;
extern unsigned int sockets;
extern unsigned int dtls_handshakes;

void dtls_cmd_trigger(bool led, int mode, const uint8_t* data, size_t len);

#endif /* DTLS_CLIENT_H */
