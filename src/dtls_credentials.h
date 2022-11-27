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

#ifndef DTLS_CREDENTIALS_H
#define DTLS_CREDENTIALS_H

#include "dtls.h"

void dtls_credentials_init_psk(const char *imei);

void dtls_credentials_init_handler(dtls_handler_t* handler);

const char* dtls_credentials_get_psk_identity(void);

#endif /* DTLS_CREDENTIALS_H */
