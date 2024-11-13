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
#include <stdlib.h>
#include <string.h>

#include "parse.h"

void print_bin_groups(char *buf, size_t bits, size_t groups, int val)
{
   int index = 0;
   while (bits > 0) {
      if (index && (bits % groups) == 0) {
         buf[index++] = ' ';
      }
      --bits;
      if (val & (1 << bits)) {
         buf[index++] = '1';
      } else {
         buf[index++] = '0';
      }
   }
   buf[index] = 0;
}

void print_bin(char *buf, size_t bits, int val)
{
   print_bin_groups(buf, bits, bits, val);
}

const char *parse_next_char(const char *value, char sep)
{
   if (!value) {
      return "";
   }
   while (*value && *value != sep) {
      ++value;
   }
   if (*value == sep) {
      ++value;
   }
   return value;
}

const char *parse_next_chars(const char *value, char sep, int count)
{
   for (int index = 0; index < count; ++index) {
      value = parse_next_char(value, sep);
      if (*value == 0) {
         break;
      }
   }
   return value;
}

const char *parse_next_long(const char *value, int base, long *result)
{
   char *t = NULL;
   long l = strtol(value, &t, base);
   if (value != t && result) {
      *result = l;
   }
   return t;
}

const char *parse_next_long_text(const char *value, char sep, int base, long *result)
{
   const char *v = value;
   char *t = NULL;

   if (*v == sep) {
      ++v;
   }
   long l = strtol(v, &t, base);
   if (v != t) {
      if (result) {
         *result = l;
      }
      v = t;
      if (*v == sep) {
         ++v;
      }
   } else {
      v = value;
   }
   return v;
}

const char *parse_next_long_qtext(const char *value, char sep, int base, long *result)
{
   if (*value != sep) {
      if (result) {
         *result = 0;
      }
      return value;
   }
   return parse_next_long_text(value, sep, base, result);
}

const char *parse_next_text(const char *value, char sep, char *result, size_t len)
{
   if (result) {
      if (len > 0) {
         --len; // space for terminating 0
      } else {
         result = NULL;
      }
   }
   if (*value == sep) {
      ++value;
   }
   while (*value && *value != sep) {
      if (result && len) {
         *result++ = *value;
         --len;
      }
      ++value;
   }
   if (*value && *value == sep) {
      ++value;
   }
   if (result) {
      *result = 0;
   }
   return value;
}

const char *parse_next_qtext(const char *value, char sep, char *result, size_t len)
{
   if (*value != sep) {
      if (result && len > 0) {
         *result = 0;
      }
      return value;
   }
   return parse_next_text(value, sep, result, len);
}

int strstart(const char *value, const char *head, bool ignore_case)
{
   const char *cur = head;
   if (ignore_case) {
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
      // mismatch
      return 0;
   } else {
      return cur - head;
   }
}

int strend(const char *value, const char *tail, bool ignore_case)
{
   int offset = strlen(value) - strlen(tail);

   if (offset > 0) {
      value += offset;
   }
   return strstart(value, tail, ignore_case);
}

static inline const char *strichr_(const char *value1, int value2, bool ignore_case)
{
   if (ignore_case) {
      return strichr(value1, value2);
   } else {
      return strchr(value1, value2);
   }
}

int strstartsep(const char *value, const char *head, bool ignore_case, const char *separators)
{
   int index = strstart(value, head, ignore_case);
   if (index && separators) {
      char end = value[index];
      if (end) {
         if (strichr_(separators, end, ignore_case)) {
            ++index;
         } else {
            // separator mismatch
            index = 0;
         }
      }
   }
   return index;
}

int strsepend(const char *value, const char *tail, bool ignore_case, const char *separators)
{
   int index = strend(value, tail, ignore_case);
   if (index && separators) {
      int pos = strlen(value) - index - 1;
      if (pos >= 0) {
         char end = value[pos];
         if (end) {
            if (strichr_(separators, end, ignore_case)) {
               ++index;
            } else {
               // separator mismatch
               index = 0;
            }
         }
      }
   }
   return index;
}

const char *strichr(const char *value1, int value2)
{
   value2 = tolower(value2);
   while (*value1 && value2 != tolower(*value1)) {
      ++value1;
   }
   if (*value1) {
      return value1;
   } else {
      return NULL;
   }
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

int strtrim(const char *value, size_t *tail)
{
   unsigned char *uvalue = (unsigned char *)value;
   while (isspace(*uvalue)) {
      ++uvalue;
   }
   if (tail) {
      size_t end = strlen(uvalue);
      while (true) {
         if (!end) {
            *tail = end;
            break;
         }
         if (!isspace(uvalue[--end])) {
            *tail = end + 1;
            break;
         }
      }
   }
   return uvalue - (unsigned char *)value;
}

int strtrunc(char *value, char quote)
{
   return strtrunc2(value, quote, quote);
}

int strtrunc2(char *value, char quote1, char quote2)
{
   int index = 0;
   if (value[0] == quote1) {
      int index = strlen(value);
      if (value[index - 1] == quote2) {
         index -= 2;
         memmove(value, value + 1, index);
         value[index] = 0;
      }
   }
   return index;
}
