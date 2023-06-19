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

#ifndef PARSE_H
#define PARSE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

const char *parse_next_char(const char *value, char sep);

const char *parse_next_chars(const char *value, char sep, int count);

int parse_strncpy(char *buf, const char *value, char end, int size);

const uint8_t *parse_next_byte(const uint8_t *value, uint16_t len, uint8_t sep);

int parse_memncpy(uint8_t *buf, const uint8_t *value, uint16_t len, uint8_t end, uint16_t size);

int strstart(const char *value, const char *head, bool ignoreCase);

#endif /* PARSE_H */
