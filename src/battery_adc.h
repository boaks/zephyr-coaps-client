/*
 * Copyright (c) 2023 Achim Kraus CloudCoap.net
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

#ifndef BATTERY_ADC_H_
#define BATTERY_ADC_H_

/** Enable or disable measurement of the battery voltage.
 *
 * @param enable true to enable, false to disable
 *
 * @return zero on success, or a negative error code.
 */
int battery_measure_enable(bool enable);

/** Measure the battery voltage.
 *
 * @param voltage the battery voltage in millivolts
 * 
 * @return 0 on success, or a negative error code.
 */
int battery_sample(uint16_t* voltage);

#endif /* APPLICATION_BATTERY_H_ */
