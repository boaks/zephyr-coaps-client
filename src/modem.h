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
#include <stdbool.h>
#include <sys_clock.h>

#include <modem/lte_lc.h>

typedef struct lte_network_info {
   const char* reg_status;
   bool registered;
   bool plmn_lock;
   int16_t rsrp;
   uint8_t band;
   char provider[6];
   char tac[5];
   char cell[9];
   char apn[16];
   char local_ip[16];
} lte_network_info_t;

typedef struct lte_network_statistic {
   uint32_t searchs;
   uint32_t psm_delays;
   uint32_t transmitted;
   uint32_t received;
   uint16_t max_packet_size;
   uint16_t average_packet_size;
} lte_network_statistic_t;

enum lte_state_type {
	LTE_STATE_REGISTER_NETWORK,
	LTE_STATE_CONNECT_NETWORK
};

typedef void (*wakeup_callback_handler_t)(void);
typedef void (*lte_state_change_callback_handler_t)(enum lte_state_type type, bool connected);

int modem_init(int config, wakeup_callback_handler_t wakeup_handler, lte_state_change_callback_handler_t state_handler);

int modem_start(const k_timeout_t timeout);

const char* modem_get_network_mode(void);

int modem_get_edrx_status(struct lte_lc_edrx_cfg *edrx);

int modem_get_psm_status(struct lte_lc_psm_cfg *psm);

int modem_get_network_info(struct lte_network_info* info);

int modem_get_imsi(char* buf, size_t len);

int modem_get_iccid(char* buf, size_t len);

int modem_get_release_time(void);

void modem_set_transmission_time(void);

int modem_read_network_info(struct lte_network_info* info);

int modem_read_pdn_info(char* buf, size_t len);

int modem_read_statistic(struct lte_network_statistic* statistic);

int modem_at_cmd(const char* cmd, char* buf, size_t max_len, const char *skip);

int modem_set_rai(int enable);

int modem_set_offline(void);

int modem_set_lte_offline(void);

int modem_set_normal(void);

int modem_power_off(void);

#endif /* MODEM_H */
