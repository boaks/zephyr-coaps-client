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

void print_bin(char *buf, size_t bits, int val);
void print_bin_groups(char *buf, size_t bits, size_t groups, int val);

const char *parse_next_char(const char *value, char sep);

const char *parse_next_chars(const char *value, char sep, int count);

const char *parse_next_long(const char *value, int base, long* result);

const char *parse_next_long_text(const char *value, char sep, int base, long *result);

const char *parse_next_long_qtext(const char *value, char sep, int base, long *result);

const char *parse_next_text(const char *value, char sep, char* result, size_t len);

const char *parse_next_qtext(const char *value, char sep, char* result, size_t len);

int strstart(const char *value, const char *head, bool ignore_case);

int strend(const char *value, const char *tail, bool ignore_case);

int strstartsep(const char *value, const char *head, bool ignore_case, const char *separators);

int strsepend(const char *value, const char *tail, bool ignore_case, const char *separators);

const char *strichr(const char *value1, int value2);

int stricmp(const char *value1, const char *value2);

int strtrunc(char *value, char quote);

int strtrunc2(char *value, char quote1, char quote2);

#endif /* PARSE_H */
