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

#include "appl_storage.h"
#include "appl_storage_config.h"

#if defined(CONFIG_FLASH) || defined(CONFIG_EEPROM)

const struct storage_config storage_configs[] = {
    {.desc = "boot-code",
     .magic = 0x01200340,
     .version = 2,
     .value_size = sizeof(uint16_t),
     .pages = 4},
};

const size_t storage_config_count = sizeof(storage_configs) / sizeof(struct storage_config);

#endif /* CONFIG_FLASH || CONFIG_EEPROM */