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

#ifndef DTLS_CLIENT_H
#define DTLS_CLIENT_H

enum dtls_lte_connect_type {
	LTE_CONNECT_NETWORK,
	LTE_CONNECT_TRANSMISSION
};

void dtls_lte_connected(enum dtls_lte_connect_type type, int connected);

void dtls_trigger(void);

int dtls_loop(void);

#endif /* DTLS_CLIENT_ */
