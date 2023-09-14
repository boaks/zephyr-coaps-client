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

#ifndef APPL_UPDATE_H
#define APPL_UPDATE_H

#include <stddef.h>
#include <stdint.h>

int appl_update_cmd(const char *config);
void appl_update_cmd_help(void);

bool appl_update_pending(void);
int appl_update_start(void);
size_t appl_update_written(void);
int appl_update_erase(void);
int appl_update_write(const uint8_t *data, size_t len);
int appl_update_finish(void);
int appl_update_cancel(void);
int64_t appl_update_time(void);

int appl_update_get_pending_version(char *buf, size_t len);
int appl_update_dump_pending_image(void);
int appl_update_request_upgrade(void);
int appl_update_image_verified(void);

#endif /* APPL_UPDATE_H */
