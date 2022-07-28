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

#ifndef MODEM_H
#define MODEM_H

#include <stddef.h>
#include <sys_clock.h>

#include <modem/lte_lc.h>

typedef void (*wakeup_callback_handler_t)(void);

int modem_init(wakeup_callback_handler_t handler);

int modem_start(const k_timeout_t timeout);

const char* modem_get_network_mode(void);

int modem_get_edrx_status(struct lte_lc_edrx_cfg *edrx);

int modem_get_psm_status(struct lte_lc_psm_cfg *psm);

int modem_get_release_time(void);

void modem_set_transmission_time(void);

int modem_at_cmd(const char* cmd, char* buf, size_t max_len, const char *skip);

int modem_set_power_modes(int enable);

int modem_set_offline(void);

int modem_set_normal(void);

int modem_power_off(void);

#endif /* MODEM_H */
