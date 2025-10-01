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

#ifndef MODEM_AT_H
#define MODEM_AT_H

#include <stddef.h>
#include <stdbool.h>

typedef void (*modem_at_response_handler_t)(const char *resp);

int modem_at_lock(const k_timeout_t timeout);

int modem_at_lock_no_warn(const k_timeout_t timeout);

int modem_at_unlock(void);

int modem_at_cmdf(char *buf, size_t len, const char *skip, const char *cmd, ...);

int modem_at_cmd(char* buf, size_t len, const char *skip, const char* cmd);

int modem_at_cmdf_async(modem_at_response_handler_t handler, const char *skip, const char *cmd, ...);

int modem_at_cmd_async(modem_at_response_handler_t handler, const char *skip, const char* cmd);

bool modem_at_async_pending(void);

int modem_at_is_on(void);

int modem_at_push_off(bool force);

int modem_at_restore(void);

#endif /* MODEM_AT_H */
