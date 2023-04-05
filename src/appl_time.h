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

#ifndef APPL_TIME_H
#define APPL_TIME_H

#include <stddef.h>

void appl_get_now(int64_t* now);
void appl_set_now(int64_t now);

int appl_format_time(int64_t time_millis, char *buf, size_t len);

#endif /* APPL_TIME_H */
