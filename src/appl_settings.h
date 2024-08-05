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

#define MAX_SETTINGS_VALUE_LENGTH 64

void appl_settings_init(const char *imei, dtls_handler_t* handler);

int appl_settings_get_apn(char* buf, size_t len);
int appl_settings_get_device_identity(char* buf, size_t len);
int appl_settings_get_scheme(char* buf, size_t len);
int appl_settings_get_destination(char* buf, size_t len);
int appl_settings_get_coap_path(char* buf, size_t len);
int appl_settings_get_coap_query(char* buf, size_t len);

uint16_t appl_settings_get_destination_port(bool secure);

int appl_settings_get_battery_profile(void);

int appl_settings_get_provisioning(char *buf, size_t len);
bool appl_settings_is_provisioning(void);
void appl_settings_provisioning_done(void);

bool appl_settings_unlock(const char* value);

#endif /* APPL_SETTINGS_H */
