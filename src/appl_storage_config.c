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

#include <zephyr/device.h>

#include "appl_storage_config.h"

#if defined(CONFIG_EEPROM) && (DT_NODE_HAS_STATUS(DT_ALIAS(appl_storage_eeprom), okay))
#define STORAGE_FLASH_DEVICE false
#define DT_STORAGE_DEV DT_ALIAS(appl_storage_eeprom)
#elif defined(CONFIG_FLASH) && (DT_NODE_HAS_STATUS(DT_ALIAS(appl_storage_flash), okay))
#define STORAGE_FLASH_DEVICE true
#define DT_STORAGE_DEV DT_ALIAS(appl_storage_flash)
#endif

const struct storage_config storage_configs[] = {
#ifdef DT_STORAGE_DEV
    {
     .storage_device = DEVICE_DT_GET_OR_NULL(DT_STORAGE_DEV),
     .desc = "boot-code",
     .is_flash_device = STORAGE_FLASH_DEVICE,
     .id = REBOOT_CODE_ID,
     .magic = 0x01200340,
     .version = 2,
     .value_size = sizeof(uint16_t),
     .pages = 4
    }
#endif     
};

const size_t storage_config_count = sizeof(storage_configs) / sizeof(struct storage_config);
