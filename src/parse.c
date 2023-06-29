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

#include <ctype.h>
#include <string.h>

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
   char cur = 0;
   int index;

   for (index = 0;; ++index) {
      cur = value[index];
      if (!cur || cur == end) {
         break;
      }
   }
   if (index < size) {
      strncpy(buf, value, index);
      buf[index] = 0;
   } else {
      strncpy(buf, value, size - 1);
      buf[size - 1] = 0;
   }
   while (cur && cur == end) {
      cur = value[++index];
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

int strstart(const char *value, const char *head, bool ignoreCase)
{
   const char *cur = head;
   if (ignoreCase) {
      while (*cur && tolower(*value) == tolower(*cur)) {
         ++value;
         ++cur;
      }
   } else {
      while (*cur && *value == *cur) {
         ++value;
         ++cur;
      }
   }
   if (*cur) {
      return 0;
   } else {
      return cur - head;
   }
}

int strend(const char *value, const char *tail, bool ignoreCase)
{
   int offset = strlen(value) - strlen(tail);

   if (offset > 0) {
      value += offset;
   }
   return strstart(value, tail, ignoreCase);
}

int stricmp(const char *value1, const char *value2)
{
   int res = tolower(*value1) - tolower(*value2);
   while (*value1 && res == 0) {
      ++value1;
      ++value2;
      res = tolower(*value1) - tolower(*value2);
   }
   return res;
}

int strtrunc(char *value, char quote)
{
   int index = 0;
   if (value[0] == quote) {
      int index = strlen(value);
      if (value[index - 1] == quote) {
         index -= 2;
         memmove(value, value + 1, index);
      }
      value[index] = 0;
   }
   return index;
}
