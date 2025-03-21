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

#ifndef COAP_APPL_CLIENT_H
#define COAP_APPL_CLIENT_H

#include "coap_client.h"

#define COAP_SEND_FLAG_NO_RESPONSE 1

#define COAP_SEND_FLAG_SET_PAYLOAD 0x10000

#define COAP_SEND_FLAG_INITIAL 2
#define COAP_SEND_FLAG_MINIMAL 4
#define COAP_SEND_FLAG_DYNAMIC_VALUES 8

#define COAP_SEND_FLAG_MODEM_INFO 16
#define COAP_SEND_FLAG_SIM_INFO 32
#define COAP_SEND_FLAG_NET_INFO 64
#define COAP_SEND_FLAG_NET_STATS 128
#define COAP_SEND_FLAG_LOCATION_INFO 256
#define COAP_SEND_FLAG_ENV_INFO 512
#define COAP_SEND_FLAG_SCALE_INFO 1024
#define COAP_SEND_FLAG_NET_SCAN_INFO 2048

#ifdef CONFIG_COAP_SEND_MODEM_INFO
#define COAP_SEND_FLAG_MODEM_INFO_ COAP_SEND_FLAG_MODEM_INFO
#else
#define COAP_SEND_FLAG_MODEM_INFO_ 0
#endif

#ifdef CONFIG_COAP_SEND_SIM_INFO
#define COAP_SEND_FLAG_SIM_INFO_ COAP_SEND_FLAG_SIM_INFO
#else
#define COAP_SEND_FLAG_SIM_INFO_ 0
#endif

#ifdef CONFIG_COAP_SEND_NETWORK_INFO
#define COAP_SEND_FLAG_NET_INFO_ COAP_SEND_FLAG_NET_INFO
#else
#define COAP_SEND_FLAG_NET_INFO_ 0
#endif

#ifdef CONFIG_COAP_SEND_STATISTIC_INFO
#define COAP_SEND_FLAG_NET_STATS_ COAP_SEND_FLAG_NET_STATS
#else
#define COAP_SEND_FLAG_NET_STATS_ 0
#endif

#ifdef CONFIG_LOCATION_ENABLE
#define COAP_SEND_FLAG_LOCATION_INFO_ COAP_SEND_FLAG_LOCATION_INFO
#else
#define COAP_SEND_FLAG_LOCATION_INFO_ 0
#endif

#ifdef CONFIG_ADC_SCALE
#define COAP_SEND_FLAG_SCALE_INFO_ COAP_SEND_FLAG_SCALE_INFO
#else
#define COAP_SEND_FLAG_SCALE_INFO_ 0
#endif

#define COAP_SEND_FLAGS_ALL (COAP_SEND_FLAG_MODEM_INFO_ | COAP_SEND_FLAG_SIM_INFO_ |    \
                             COAP_SEND_FLAG_NET_INFO_ | COAP_SEND_FLAG_NET_STATS_ |     \
                             COAP_SEND_FLAG_LOCATION_INFO_ | COAP_SEND_FLAG_ENV_INFO | \
                             COAP_SEND_FLAG_SCALE_INFO_)

#ifdef CONFIG_COAP_SEND_MINIMAL
#define COAP_SEND_FLAGS (COAP_SEND_FLAG_MINIMAL | COAP_SEND_FLAGS_ALL)
#else
#define COAP_SEND_FLAGS COAP_SEND_FLAGS_ALL
#endif

int coap_appl_client_parse_data(uint8_t *data, size_t len);

int coap_appl_client_prepare_modem_info(char *buf, size_t len, int flags, const char *trigger);

int coap_appl_client_prepare_sim_info(char *buf, size_t len, int flags);

int coap_appl_client_prepare_net_info(char *buf, size_t len, int flags);

int coap_appl_client_prepare_net_stats(char *buf, size_t len, int flags);

int coap_appl_client_prepare_env_info(char *buf, size_t len, int flags);

int coap_appl_client_prepare_post(char *buf, size_t len, int flags, const char* trigger);

int coap_appl_client_message(const uint8_t **buffer);

int coap_appl_client_retry_strategy(int counter, bool dtls);

extern coap_handler_t coap_appl_client_handler;

#endif /* COAP_APPL_CLIENT_H */
