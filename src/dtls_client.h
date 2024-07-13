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

#define DTLS_CLIENT_RETRY_STRATEGY_DTLS_HANDSHAKE 1
#define DTLS_CLIENT_RETRY_STRATEGY_OFFLINE 2
#define DTLS_CLIENT_RETRY_STRATEGY_OFF 4
#define DTLS_CLIENT_RETRY_STRATEGY_RESTARTS 8

extern unsigned int transmissions[COAP_MAX_RETRANSMISSION + 1];
extern unsigned int connect_time_ms;
extern unsigned int coap_rtt_ms;
extern unsigned int retransmissions;
extern unsigned int failures;
extern unsigned int sockets;
extern unsigned int dtls_handshakes;
extern volatile uint32_t send_interval;

void dtls_cmd_trigger(bool led, int mode, const uint8_t* data, size_t len);

#endif /* DTLS_CLIENT_H */
