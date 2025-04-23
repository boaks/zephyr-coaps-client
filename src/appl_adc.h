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

#ifndef APPL_ADC_H_
#define APPL_ADC_H_

#include <stdint.h>
#include <stddef.h>

/** Measure the battery voltage.
 *
 * @param voltage the battery voltage in millivolts
 * 
 * @return 0 on success, or a negative error code.
 */
int appl_adc_sample(int channel, int max_sample, int max_dither, uint16_t* voltage);
int appl_adc_sample_desc(char* buf, size_t len);

#endif /* APPL_ADC_H_ */
