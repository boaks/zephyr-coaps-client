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

#ifndef APPL_UPDATE_XMODEM_H
#define APPL_UPDATE_XMODEM_H

#include <stddef.h>
#include <stdint.h>

#define XMODEM_SOH 0x01
#define XMODEM_STX 0x02
#define XMODEM_EOT 0x04
#define XMODEM_ACK 0x06
#define XMODEM_NAK 0x15
#define XMODEM_CRC 'C'

enum xmodem_state {
   XMODEM_NONE,
   XMODEM_NOT_OK,
   XMODEM_BLOCK_READY,
   XMODEM_READY,
   XMODEM_DUPLICATE,
};

int appl_update_xmodem_start(uint8_t *buffer, size_t size, bool crc);
int appl_update_xmodem_append(const uint8_t *data, size_t len);
void appl_update_xmodem_retry(void);
int appl_update_xmodem_write_block(void);

#endif /* APPL_UPDATE_XMODEM_H */
