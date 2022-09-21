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

#include <stddef.h>

#include "parse.h"

const char *parse_next_char(const char *value, char sep)
{
   if (value) {
      while (*value && *value != sep) {
         ++value;
      }
      if (*value == sep) {
         ++value;
         if (*value) {
            return value;
         }
      }
   }
   return NULL;
}

const char *parse_next_chars(const char *value, char sep, int count)
{
   for (int index = 0; index < count; ++index) {
      value = parse_next_char(value, sep);
   }
   return value;
}

int parse_strncpy(char *buf, const char *value, char end, int size)
{
   int index;
   for (index = 0; index < size; ++index) {
      char cur = *value;
      if (!cur || cur == end) {
         break;
      }
      buf[index] = cur;
      value++;
   }
   return index;
}

const uint8_t *parse_next_byte(const uint8_t *value, uint16_t len, uint8_t sep)
{
   if (value) {
      while (len > 0 && *value != sep) {
         ++value;
         --len;
      }
      if (*value == sep) {
         ++value;
         --len;
         if (len > 0) {
            return value;
         }
      }
   }
   return NULL;
}

int parse_memncpy(uint8_t *buf, const uint8_t *value, uint16_t len, uint8_t end, uint16_t size)
{
   int index;
   for (index = 0; index < size; ++index) {
      char cur = *value;
      if (index >= len || cur == end) {
         break;
      }
      buf[index] = cur;
      value++;
   }
   return index;
}
