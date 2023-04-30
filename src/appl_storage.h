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

#ifndef APPL_STORAGE_H
#define APPL_STORAGE_H

#include <sys/types.h>

#define MAX_VALUE_SIZE 10

struct storage_config {
    const char* desc;
    int magic;
    int version;
    size_t pages;
    size_t value_size;
};

extern const struct storage_config storage_configs[];
extern const size_t storage_config_count;

int appl_storage_read_memory(off_t mem_addr, uint8_t *data, size_t num_bytes);
int appl_storage_write_memory(off_t mem_addr, const uint8_t *data, size_t num_bytes);
int appl_storage_erase_memory(off_t mem_addr, size_t num_bytes);

int appl_storage_write_int_item(size_t id, uint16_t code);
int appl_storage_read_int_items(size_t id, size_t index, int64_t* times, uint16_t* codes, size_t count);

int appl_storage_write_bytes_item(size_t id, uint8_t* data, size_t data_size);
int appl_storage_read_bytes_item(size_t id, size_t index, int64_t* time, uint8_t* data, size_t data_size);

#endif /* APPL_STORAGE_H */
