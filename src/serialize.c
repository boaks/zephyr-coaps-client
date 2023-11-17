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

#include <stdarg.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "serialize.h"

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

int sb_printf(serialize_buffer_t *buffer, const char *fmt, ...)
{
   int rc;
   va_list ap;
   va_start(ap, fmt);
   rc = vsnprintf(buffer->buffer + buffer->current, buffer->length - buffer->current, fmt, ap);
   va_end(ap);
   if (rc > 0) {
      rc += buffer->current;
      if (rc > buffer->length) {
         rc = buffer->length;
         buffer->buffer[rc - 1] = 0;
      }
      buffer->current = rc;
   }
   return rc;
}

int sb_append(serialize_buffer_t *buffer, const char *append)
{
   int rc = 0;
   while (*append && buffer->current < buffer->length) {
      buffer->buffer[buffer->current++] = *append++;
      ++rc;
   }
   if (buffer->current < buffer->length) {
      buffer->buffer[buffer->current] = 0;
   } else {
      buffer->buffer[buffer->length - 1] = 0;
   }
   return rc;
}

int sb_append_char(serialize_buffer_t *buffer, char append)
{
   int rc = 0;
   if (buffer->current < buffer->length - 1) {
      buffer->buffer[buffer->current++] = append;
      buffer->buffer[buffer->current] = 0;
      ++rc;
   }
   return rc;
}

void sb_reset(serialize_buffer_t *buffer)
{
   sb_reset_to(buffer, buffer->mark);
}

static int serialize_number(serialize_buffer_t *buffer, long value, int hex)
{
   if (hex) {
      return sb_printf(buffer, "%0*x", hex, value);
   } else {
      return sb_printf(buffer, "%ld", value);
   }
}

static int serialize_number_float(serialize_buffer_t *buffer, double value, int dec)
{
   return sb_printf(buffer, "%.*f", dec, value);
}

static int serialize_plain_start(serialize_buffer_t *buffer)
{
   buffer->level++;
   buffer->seperator = false;
   return 0;
}

static int serialize_plain_end(serialize_buffer_t *buffer)
{
   __ASSERT(buffer->level, "serializer level failure!");
   buffer->seperator = true;
   buffer->level--;
   if (!buffer->level) {
      return sb_append_char(buffer, '\n');
   }
   return 0;
}

static int serialize_plain_next_item(serialize_buffer_t *buffer)
{
   buffer->seperator = false;
   return sb_append_char(buffer, ',');
}

static int serialize_plain_field(serialize_buffer_t *buffer, const char *name, bool opt)
{
   int res = 0;
   if (buffer->seperator) {
      res = serialize_plain_next_item(buffer);
   }
   buffer->seperator = true;
   return opt ? res : res + sb_printf(buffer, "%s:", name);
}

static int serialize_plain_text(serialize_buffer_t *buffer, const char *value)
{
   return sb_append(buffer, value);
}

static int serialize_plain_number_field(serialize_buffer_t *buffer, const char *name, const char *unit, double value, int dec)
{
   int res = serialize_plain_field(buffer, name, false);
   res += serialize_number_float(buffer, value, dec);
   if (strlen(unit) > 1) {
      res += sb_append_char(buffer, ' ');
   }
   res += serialize_plain_text(buffer, unit);
   return res;
}

static int serialize_json_start_array(serialize_buffer_t *buffer)
{
   buffer->seperator = false;
   buffer->level++;
   return sb_append_char(buffer, '[');
}

static int serialize_json_end_array(serialize_buffer_t *buffer)
{
   __ASSERT(buffer->level, "serializer level failure!");
   buffer->seperator = true;
   buffer->level--;
   if (buffer->level <= 1) {
      return sb_append(buffer, "]\n");
   } else {
      return sb_append_char(buffer, ']');
   }
}

static int serialize_json_start_map(serialize_buffer_t *buffer)
{
   buffer->seperator = false;
   buffer->level++;
   return sb_append_char(buffer, '{');
}

static int serialize_json_end_map(serialize_buffer_t *buffer)
{
   __ASSERT(buffer->level, "serializer level failure!");
   buffer->seperator = true;
   buffer->level--;
   if (buffer->level <= 1) {
      return sb_append(buffer, "}\n");
   } else {
      return sb_append_char(buffer, '}');
   }
}

static int serialize_json_next_item(serialize_buffer_t *buffer)
{
   buffer->seperator = false;
   return sb_append_char(buffer, ',');
}

static int serialize_json_field(serialize_buffer_t *buffer, const char *name, bool opt)
{
   int res = 0;
   if (buffer->seperator) {
      res = serialize_json_next_item(buffer);
   }
   buffer->seperator = true;
   return res + sb_printf(buffer, "\"%s\":", name);
}

static int serialize_json_text(serialize_buffer_t *buffer, const char *value)
{
   return sb_printf(buffer, "\"%s\"", value);
}

static int serialize_json_number_field(serialize_buffer_t *buffer, const char *name, const char *unit, double value, int dec)
{
   int res = serialize_json_field(buffer, name, false);
   if (unit) {
      res += serialize_json_start_map(buffer);
      res += serialize_json_field(buffer, "value", true);
   }
   res += serialize_number_float(buffer, value, dec);
   if (unit) {
      res += serialize_json_field(buffer, "unit", true);
      res += serialize_json_text(buffer, unit);
      res += serialize_json_end_map(buffer);
   }
   return res;
}

serializer_t plain = {
    .start_array = serialize_plain_start,
    .end_array = serialize_plain_end,
    .start_map = serialize_plain_start,
    .end_map = serialize_plain_end,
    .next_item = serialize_plain_next_item,
    .field = serialize_plain_field,
    .text = serialize_plain_text,
    .number = serialize_number,
    .number_float = serialize_number_float,
    .number_field = serialize_plain_number_field,
};

serializer_t json = {
    .start_array = serialize_json_start_array,
    .end_array = serialize_json_end_array,
    .start_map = serialize_json_start_map,
    .end_map = serialize_json_end_map,
    .next_item = serialize_json_next_item,
    .field = serialize_json_field,
    .text = serialize_json_text,
    .number = serialize_number,
    .number_float = serialize_number_float,
    .number_field = serialize_json_number_field,
};
