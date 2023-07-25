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

#ifndef MODEM_CMD_H
#define MODEM_CMD_H

#include <stddef.h>
#include <stdbool.h>

int modem_cmd_config(const char *config);
void modem_cmd_config_help(void);

int modem_cmd_connect(const char *config);
void modem_cmd_connect_help(void);

int modem_cmd_scan(const char *config);
void modem_cmd_scan_help(void);

int modem_cmd_sms(const char *config);
void modem_cmd_sms_help(void);

int modem_cmd_psm(const char *config);
void modem_cmd_psm_help(void);

int modem_cmd_rai(const char *config);
void modem_cmd_rai_help(void);

int modem_cmd_edrx(const char *config);
void modem_cmd_edrx_help(void);

#endif /* MODEM_CMD_H */
