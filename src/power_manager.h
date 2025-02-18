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
#include <zephyr/sys_clock.h>

typedef enum {
   POWER_UNKNOWN,
   FROM_BATTERY,
   CHARGING_TRICKLE,
   CHARGING_I,
   CHARGING_V,
   CHARGING_COMPLETED,
   FROM_EXTERNAL,
} power_manager_status_t;

#define PM_INVALID_VOLTAGE 0xffff
#define PM_INVALID_POWER 0xffff
#define PM_INVALID_CURRENT 0x7fff
#define PM_INVALID_LEVEL 0xff

int power_manager_init(void);

int power_manager_add_device(const struct device *dev);

int power_manager_suspend_device(const struct device *dev);

int power_manager_suspend(bool enable);

int power_manager_pulse(k_timeout_t time);

int power_manager_3v3(bool enable);

int power_manager_1v8(bool enable);

int power_manager_voltage(uint16_t *voltage);

int power_manager_voltage_ext(uint16_t *voltage);

int power_manager_ext(uint16_t *voltage, int16_t *current, uint16_t *power);

int power_manager_status(uint8_t *level, uint16_t *voltage, power_manager_status_t *status, int16_t *forecast);

int power_manager_status_desc(char* buf, size_t len);

#endif /* POWER_MANAGER_H */
