/*
 * Copyright (c) 2025 Achim Kraus CloudCoap.net
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

#ifndef SOLAR_CHARGER_H
#define SOLAR_CHARGER_H

int solar_power_is_good(void);
int solar_is_charging(void);
int solar_is_enabled(void);

#endif /* SOLAR_CHARGER_H */
