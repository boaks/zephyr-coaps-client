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

#ifndef ENVIRONMENT_SENSOR_H
#define ENVIRONMENT_SENSOR_H

#if (defined CONFIG_BME680_BSEC) || (defined CONFIG_BME680)

#define ENVIRONMENT_SENSOR

#include <stdbool.h>

int environment_init(void);

int environment_get_temperature(double *value);

int environment_get_humidity(double *value);

int environment_get_pressure(double *value);

int environment_get_gas(int32_t *value);

int environment_get_iaq(int32_t *value);

#endif /* CONFIG_BME680_BSEC || CONFIG_BME680 */

#endif /* ENVIRONMENT_SENSOR_H */
