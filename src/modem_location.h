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

#ifndef MODEM_LOCATION_H
#define MODEM_LOCATION_H

typedef enum {
   NO_LOCATION,
   CURRENT_LOCATION,
   PREVIOUS_LOCATION
} location_state_t;

typedef void (*location_callback_handler_t)(void);

int modem_location_init(location_callback_handler_t handler);

int modem_location_start(int interval, int timeout);

location_state_t modem_location_get(int timeout, double *latitude, double *longitude);

#endif /* MODEM_LOCATION_H */
