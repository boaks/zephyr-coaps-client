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

#ifndef ACCELEROMETER_SENSOR_H
#define ACCELEROMETER_SENSOR_H

#include <stdbool.h>

/** Number of accelerometer channels. */
#define ACCELEROMETER_CHANNELS 3

struct accelerometer_evt {
   /** x, y, z values. */
   double values[ACCELEROMETER_CHANNELS];
};

/** @brief accelerometer_handler_t asynchronous event handler.
 *
 *  @param[in] evt The event and accelerometer values (x, y, z).
 */
typedef void (*accelerometer_handler_t)(
    const struct accelerometer_evt *const evt);

int accelerometer_init(accelerometer_handler_t handler);

int accelerometer_enable(bool enable);

#endif /* ACCELEROMETER_SENSOR_H */
