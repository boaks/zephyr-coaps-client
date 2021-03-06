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

#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <stddef.h>

typedef enum {
   POWER_UNKNOWN,
   FROM_BATTERY,
   CHARGING_TRICKLE,
   CHARGING_I,
   CHARGING_V,
   CHARGING_COMPLETED,
} power_manager_status_t;

int power_manager_init(void);

int power_manager_status(uint8_t *level, uint16_t *voltage, power_manager_status_t *status);

#endif /* POWER_MANAGER_H */
