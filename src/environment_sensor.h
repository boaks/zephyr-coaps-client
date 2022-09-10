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

#if (defined CONFIG_BME680_BSEC) || (defined CONFIG_BME680) || defined(CONFIG_SHT3XD) || (defined CONFIG_SHT21)

#define ENVIRONMENT_SENSOR

#include <stdbool.h>

int environment_init(void);

int environment_get_temperature(double *value);

int environment_get_humidity(double *value);

int environment_get_pressure(double *value);

int environment_get_gas(int32_t *value);

int environment_get_iaq(int32_t *value);

#if (CONFIG_ENVIRONMENT_HISTORY_SIZE > 0)

int environment_get_temperature_history(double *values, uint8_t size);

void environment_add_temperature_history(double value, bool force);

void environment_init_temperature_history(void);
#else

#define environment_get_temperature_history(X,S) -1
#define environment_add_temperature_history(X,S)
#define environment_init_temperature_history()

#endif /* CONFIG_ENVIRONMENT_HISTORY_SIZE > 0 */

#endif /* CONFIG_BME680_BSEC || CONFIG_BME680 || CONFIG_SHT3XD || CONFIG_SHT21 */

#endif /* ENVIRONMENT_SENSOR_H */
