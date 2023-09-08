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

#ifndef APPL_UPDATE_COAP_H
#define APPL_UPDATE_COAP_H

#include <stddef.h>
#include <stdint.h>

bool appl_update_coap_pending(void);
bool appl_update_coap_reboot(void);
int appl_update_coap_status(uint8_t *data, size_t len);
int appl_update_coap_cmd(const char* config);
int appl_update_coap_cancel(void);
int appl_update_coap_parse_data(uint8_t *data, size_t len);
bool appl_update_coap_pending_next(void);
int appl_update_coap_next(void);
int appl_update_coap_message(const uint8_t** buffer);

#endif /* APPL_UPDATE_COAP_H */
