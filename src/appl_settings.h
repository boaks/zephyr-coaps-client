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

#ifndef APPL_SETTINGS_H
#define APPL_SETTINGS_H

#include <stdbool.h>

#include "dtls.h"

void dtls_settings_init(const char *imei, dtls_handler_t* handler);

const char* dtls_get_psk_identity(void);
const char* dtls_get_destination(void);
uint16_t dtls_get_destination_port(bool secure);
int dtls_get_provisioning(char *buf, size_t len);
bool dtls_is_provisioning(void);
void dtls_provisioning_done(void);

#endif /* APPL_SETTINGS_H */
