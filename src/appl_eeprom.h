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

#ifndef APPL_EEPROM_H
#define APPL_EEPROM_H

int appl_eeprom_init(void);
int appl_eeprom_read_memory(uint16_t mem_addr, uint8_t *data, uint32_t num_bytes);
int appl_eeprom_write_memory(uint16_t mem_addr, const uint8_t *data, uint32_t num_bytes);

int appl_eeprom_write_code(int64_t time, uint16_t code);
int appl_eeprom_read_codes(int64_t* times, uint16_t* codes, size_t count);

#endif /* APPL_EEPROM_H */
