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

/*
 * With NCS-2.1.0 / v3.1.99-ncs1 it seems, that the KConfig of
 * sensors in the Device Tree are automatically set to 'y'.
 * That causes CONFIG_BME680 to be switched to 'y' by default
 * for the Thingy:91. To use other sensors or the CONFIG_BME680_BSEC
 * please switch CONFIG_BME680 or disable it in an overlay configuration.
 */

#if (defined CONFIG_ENVIRONMENT_SENSOR) || (defined CONFIG_SHT21)

#include <stdbool.h>

int environment_init(void);

int environment_sensor_fetch(bool force);

int environment_get_temperature(double *value);

int environment_get_humidity(double *value);

int environment_get_pressure(double *value);

int environment_get_gas(int32_t *value);

int environment_get_iaq(int32_t *value, uint8_t *accurancy);

const char *environment_get_iaq_description(int32_t value);

#define IAQ_VALUE(X) ((X)&0x3fff)
#define IAQ_ACCURANCY(X) ((X) >> 14)
#define IAQ_ACCURANCY_HIST(X) ((X & 3) << 14)


#if (CONFIG_ENVIRONMENT_HISTORY_SIZE > 0)

int environment_get_temperature_history(double *values, uint8_t size);

void environment_add_temperature_history(double value, bool force);

int environment_get_iaq_history(uint16_t *values, uint8_t size);

void environment_add_iaq_history(uint16_t value, bool force);

void environment_init_history(void);

#ifdef CONFIG_BME680_BSEC
#define NO_ENVIRONMENT_HISTORY_WORKER
#endif

#else

#define environment_get_temperature_history(X, S) -1
#define environment_add_temperature_history(X, S)
#define environment_get_iaq_history(X, S) -1
#define environment_add_iaq_history(X, S)
#define environment_init_history()

#endif /* CONFIG_ENVIRONMENT_HISTORY_SIZE > 0 */

#endif /* CONFIG_ENVIRONMENT_SENSOR || CONFIG_SHT21 */

#endif /* ENVIRONMENT_SENSOR_H */
