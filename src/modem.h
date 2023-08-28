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
#include <zephyr/sys_clock.h>

#include <modem/lte_lc.h>

#define MODEM_ID_SIZE 24
#define MODEM_PLMN_SIZE 7

typedef struct lte_modem_info {
   char version[MODEM_ID_SIZE];
   char firmware[MODEM_ID_SIZE];
   char imei[MODEM_ID_SIZE];
} lte_modem_info_t;

typedef struct lte_sim_info {
   bool valid;
   bool edrx_cycle_support;
   int16_t hpplmn_search_interval;
   int16_t imsi_interval;
   uint16_t imsi_counter;
   char hpplmn[MODEM_PLMN_SIZE];
   char forbidden[MODEM_PLMN_SIZE];
   char iccid[MODEM_ID_SIZE];
   char imsi[MODEM_ID_SIZE];
   char prev_imsi[MODEM_ID_SIZE];
} lte_sim_info_t;

enum lte_network_state_type {
	LTE_NETWORK_STATE_INIT = 0,
	LTE_NETWORK_STATE_OFF = 1,
	LTE_NETWORK_STATE_ON = 3
};

enum lte_network_rai {
	LTE_NETWORK_RAI_UNKNOWN = 0,
	LTE_NETWORK_NO_RAI = 1,
	LTE_NETWORK_CP_RAI = 2,
	LTE_NETWORK_AS_RAI = 3
};


typedef struct lte_network_info {
   uint16_t registered:2;
   uint16_t pdn_active:2;
   uint16_t rrc_active:2;
   uint16_t sleeping:2;
   uint16_t plmn_lock:2;
   enum lte_lc_nw_reg_status status;
   enum lte_lc_lte_mode mode;
   uint8_t band;
   char provider[MODEM_PLMN_SIZE];
   uint16_t tac;
   uint32_t cell;
   uint32_t earfcn;
   char apn[16];
   char local_ip[16];
} lte_network_info_t;

#define INVALID_SIGNAL_VALUE 0x7fff
#define NONE_SIGNAL_VALUE 0x7ffe

typedef struct lte_ce_info {
   uint8_t ce_supported;
   char state;
   uint8_t downlink_repetition;
   uint8_t uplink_repetition;
   int16_t rsrp;
   int16_t cinr;
   int16_t snr;
} lte_ce_info_t;

typedef struct lte_network_statistic {
   uint32_t searchs;
   uint32_t psm_delays;
   uint32_t restarts;
   uint32_t cell_updates;
   uint32_t wakeups;
   uint32_t search_time;
   uint32_t psm_delay_time;
   uint32_t wakeup_time;
   uint32_t connected_time;
   uint32_t asleep_time;
   uint32_t transmitted;
   uint32_t received;
   uint16_t max_packet_size;
   uint16_t average_packet_size;
} lte_network_statistic_t;

enum lte_state_type {
	LTE_STATE_REGISTRATION,
	LTE_STATE_READY,
	LTE_STATE_CONNECTED,
	LTE_STATE_SLEEPING
};

enum rai_mode {
	RAI_OFF,
	RAI_NOW, /* intended to uses a extra dummy message! */
	RAI_LAST,
	RAI_ONE_RESPONSE
};

enum preference_mode {
   RESET_PREFERENCE,
   SWAP_PREFERENCE,
   NBIOT_PREFERENCE,
   LTE_M_PREFERENCE,
};

#ifdef CONFIG_UDP_AS_RAI_ENABLE
#define CONFIG_UDP_USE_CONNECT 1
#undef CONFIG_UDP_RAI_ENABLE
#endif

typedef void (*lte_state_change_callback_handler_t)(enum lte_state_type type, bool active);

int modem_init(int config, lte_state_change_callback_handler_t state_handler);

int modem_start(const k_timeout_t timeout, bool save);

int modem_start_search(void);

int modem_wait_ready(const k_timeout_t timeout);

bool modem_set_preference(enum preference_mode mode);

int modem_get_edrx_status(struct lte_lc_edrx_cfg *edrx);

int modem_get_psm_status(struct lte_lc_psm_cfg *psm);

int modem_get_rai_status(enum lte_network_rai *rai);

int modem_get_network_info(struct lte_network_info* info);

int modem_get_mcc(char* mcc);

int modem_get_coverage_enhancement_info(struct lte_ce_info* info);

int modem_get_sim_info(struct lte_sim_info* info);

int modem_get_modem_info(struct lte_modem_info* info);

int modem_get_imei(char* buf, size_t len);

int modem_get_release_time(void);

void modem_set_transmission_time(void);

void modem_set_scan_time(void);

int modem_read_network_info(struct lte_network_info* info, bool callbacks);

int modem_read_statistic(struct lte_network_statistic* statistic);

int modem_read_coverage_enhancement_info(struct lte_ce_info* info);

int modem_set_psm(int16_t active_time_s);

int modem_set_rai_mode(enum rai_mode mode, int socket);

int modem_set_edrx(int16_t edrx_time_s);

void modem_lock_psm(bool on);

void modem_lock_rai(bool on);

void modem_lock_plmn(bool on);

int modem_set_offline(void);

int modem_set_lte_offline(void);

int modem_set_normal(void);

int modem_power_off(void);

int modem_factory_reset(void);

#endif /* MODEM_H */
