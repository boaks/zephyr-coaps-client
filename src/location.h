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

#ifndef LOCATION_H
#define LOCATION_H

#include <stdbool.h>
#include <stdint.h>
#include <nrf_modem_gnss.h>

typedef enum modem_gnss_result {
	MODEM_GNSS_NOT_AVAILABLE,
	MODEM_GNSS_TIMEOUT,
	MODEM_GNSS_ERROR,
	MODEM_GNSS_INVISIBLE,
	MODEM_GNSS_POSITION
} modem_gnss_result_t;

struct modem_gnss_state {
	enum modem_gnss_result result;
	uint32_t execution_time;
	uint32_t satellites_time;	
	uint8_t max_satellites;	
	bool valid;
	struct nrf_modem_gnss_pvt_data_frame position;
};

typedef void (*location_callback_handler_t)(void);

int location_init(location_callback_handler_t handler);

bool location_enabled(void);

void location_start(bool force);

void location_stop(void);

modem_gnss_result_t location_get(struct modem_gnss_state *location, bool* running);

#endif /* LOCATION_H */
