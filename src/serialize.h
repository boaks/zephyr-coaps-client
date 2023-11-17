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

#ifndef SERIALIZE_H
#define SERIALIZE_H

#include <stdbool.h>

typedef struct serialize_buffer {
   char *buffer;
   size_t length;
   size_t current;
   size_t mark;
   uint8_t level;
   bool seperator;
} serialize_buffer_t;

typedef struct serializer {
   int (*start_array)(serialize_buffer_t *buffer);
   int (*end_array)(serialize_buffer_t *buffer);
   int (*start_map)(serialize_buffer_t *buffer);
   int (*end_map)(serialize_buffer_t *buffer);
   int (*next_item)(serialize_buffer_t *buffer);
   int (*field)(serialize_buffer_t *buffer, const char *name, bool opt);
   int (*text)(serialize_buffer_t *buffer, const char *value);
   int (*number)(serialize_buffer_t *buffer, long value, int hex);
   int (*number_float)(serialize_buffer_t *buffer, double value, int dec);
   int (*number_field)(serialize_buffer_t *buffer, const char *name, const char *unit, double value, int dec);
} serializer_t;

int sb_printf(serialize_buffer_t *buffer, const char *fmt, ...);
int sb_append(serialize_buffer_t *buffer, const char *append);
int sb_append_char(serialize_buffer_t *buffer, char append);
void sb_reset(serialize_buffer_t *buffer);

inline const char *sb_from_mark(serialize_buffer_t *buffer)
{
   return buffer->buffer + buffer->mark;
}

inline size_t sb_mark(serialize_buffer_t *buffer)
{
   buffer->mark = buffer->current;
   return buffer->current;
}

inline void sb_reset_to(serialize_buffer_t *buffer, size_t mark)
{
   buffer->current = mark;
   buffer->buffer[buffer->current] = 0;
}

inline void sb_reinit(serialize_buffer_t *buffer)
{
   buffer->current = 0;
   buffer->mark = 0;
   buffer->level = 0;
   buffer->seperator = false;
}

inline void sb_init(serialize_buffer_t *buffer, char *buf, size_t len)
{
   buffer->buffer = buf;
   buffer->length = len;
   sb_reinit(buffer);
}

extern serializer_t plain;
extern serializer_t json;

#endif /* SERIALIZE_H */
