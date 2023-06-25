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

#ifndef APPL_DIAGNOSE_H
#define APPL_DIAGNOSE_H

#include <stdbool.h>
#include <sys/types.h>

#define ERROR_CODE_SOCKET 0x0000
#define ERROR_CODE_OPEN_SOCKET 0x1000
#define ERROR_CODE_INIT_NO_LTE 0x2000
#define ERROR_CODE_INIT_SOCKET 0x3000
#define ERROR_CODE_INIT_NO_DTLS 0x4000
#define ERROR_CODE_TOO_MANY_FAILURES 0x5000
#define ERROR_CODE_NO_INITIAL_SUCCESS 0x6000
#define ERROR_CODE_MODEM_FAULT 0x7000
#define ERROR_CODE_CMD 0x9000
#define ERROR_CODE_MANUAL_TRIGGERED 0xa000

#define ERROR_CODE(BASE, ERR) ((BASE & 0xf000) | (ERR & 0xfff))

#define WATCHDOG_TIMEOUT_S (60 * 5)

void watchdog_feed(void);

void appl_reboot(int error, int delay);
bool appl_reboots(void);

uint32_t appl_reset_cause(int *flags);
int appl_reset_cause_description(char* buf, size_t len);

#endif /* APPL_DIAGNOSE_H */
