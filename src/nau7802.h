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

#ifndef NAU7802_H_
#define NAU7802_H_

#include <stddef.h>

/**
 * @brief read sample from nau7802
 * 
 * @param channel channel 
 * @param value value out parameter
 * @return int 0 on success, negative on failure
 */
int scale_sample(double *valueA, double *valueB, double *temperatureA, double *temperatureB);

int scale_sample_desc(char* buf, size_t len, bool series);

enum calibrate_phase {
	CALIBRATE_NONE,
	CALIBRATE_START,
	CALIBRATE_ZERO,
	CALIBRATE_CHA_10KG,
	CALIBRATE_CHB_10KG,
	CALIBRATE_DONE,
};

int scale_calibrate(enum calibrate_phase phase);

bool scale_calibrate_setup(void);

#endif /* NAU7802_H_ */
