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
#include <stdbool.h>

typedef enum {
   POWER_UNKNOWN,
   FROM_BATTERY,
   CHARGING_TRICKLE,
   CHARGING_I,
   CHARGING_V,
   CHARGING_COMPLETED,
} power_manager_status_t;

int power_manager_init(void);

int power_manager_add_device(const struct device *dev);

int power_manager_suspend_device(const struct device *dev);

int power_manager_suspend(bool enable);

int power_manager_3v3(bool enable);

int power_manager_1v8(bool enable);

int power_manager_voltage(uint16_t *voltage);

int power_manager_status(uint8_t *level, uint16_t *voltage, power_manager_status_t *status, int16_t *forecast);

int power_manager_status_desc(char* buf, size_t len);

#endif /* POWER_MANAGER_H */
