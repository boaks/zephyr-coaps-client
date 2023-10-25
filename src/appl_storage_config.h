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

#ifndef APPL_STORAGE_CONFIG_H
#define APPL_STORAGE_CONFIG_H

#include <sys/types.h>

#define REBOOT_CODE_ID 1

#if defined(CONFIG_NAU7802_SCALE)
#define CALIBRATE_VALUE_SIZE 10
#define CALIBRATION_A_ID 2
#define CALIBRATION_B_ID 3
#endif /* CONFIG_NAU7802_SCALE */

struct device;

struct storage_config {
    const struct device *storage_device;
    const char* desc;
    bool is_flash_device;
    int id;
    int magic;
    int version;
    size_t pages;
    size_t value_size;
};

extern const struct storage_config storage_configs[];
extern const size_t storage_config_count;

#endif /* APPL_STORAGE_CONFIG_H */
