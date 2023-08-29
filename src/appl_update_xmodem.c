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

#include <errno.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include "appl_update.h"
#include "appl_update_xmodem.h"

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

#define XMODEM_HEADER_LEN 3
#define XMODEM_TYPE 0
#define XMODEM_BLOCK 1

static volatile bool xmodem_crc = false;
static volatile size_t xmodem_len = 0;
static volatile uint8_t xmodem_block = 0;
static size_t xmodem_buffer_size = 0;
static uint8_t *xmodem_buffer = NULL;

int appl_update_xmodem_start(uint8_t *buffer, size_t size, bool crc)
{
   xmodem_len = 0;
   xmodem_block = 1;
   xmodem_buffer = buffer;
   xmodem_buffer_size = size;
   xmodem_crc = crc;
   return 0;
}

int appl_update_xmodem_append(const uint8_t *data, size_t len)
{
   if (xmodem_len + len < xmodem_buffer_size) {
      uint8_t s;
      memmove(&xmodem_buffer[xmodem_len], data, len);
      xmodem_len += len;
      s = xmodem_buffer[XMODEM_TYPE];
      if (s == XMODEM_SOH || s == XMODEM_STX) {
         if (xmodem_len < XMODEM_HEADER_LEN) {
            return XMODEM_NONE;
         }
         uint8_t b = xmodem_buffer[XMODEM_BLOCK];
         uint8_t c = xmodem_buffer[XMODEM_BLOCK + 1] ^ ~b;
         if (c == 0) {
            int crc_len = xmodem_crc ? 2 : 1;
            int block_len = s == XMODEM_SOH ? 128 : 1024;
            if (xmodem_len < (XMODEM_HEADER_LEN + block_len + crc_len)) {
               return XMODEM_NONE;
            }
            LOG_DBG("Block %d %u ready", block_len, b);
            return XMODEM_BLOCK_READY;
         } else {
            LOG_INF("Block # failure, %u != %u!", b, c);
         }
      } else if (s == XMODEM_EOT && xmodem_len == 1) {
         LOG_INF("Transfer ready.");
         return XMODEM_READY;
      }
   }
   return XMODEM_NOT_OK;
}

void appl_update_xmodem_retry(void)
{
   xmodem_len = 0;
}

static bool appl_update_xmodem_crc(const uint8_t *data, int block_len)
{
   uint8_t b = data[XMODEM_BLOCK];
   uint16_t crc_calc = crc16_itu_t(0, &data[XMODEM_HEADER_LEN], block_len);
   uint16_t crc_buffer = sys_get_be16(&data[block_len + XMODEM_HEADER_LEN]);
   if (crc_calc == crc_buffer) {
      LOG_INF("Block %d/crc %u verified", block_len, b);
      return true;
   } else {
      LOG_INF("Block %d/crc %u crc error 0x%x != 0x%x", block_len, b, crc_calc, crc_buffer);
      return false;
   }
}

int appl_update_xmodem_write_block(void)
{
   uint8_t s = xmodem_buffer[XMODEM_TYPE];
   uint8_t b = xmodem_buffer[XMODEM_BLOCK];
   int rc = -EINVAL;
   int crc_len = xmodem_crc ? 2 : 1;
   int block_len;

   if (s == XMODEM_SOH) {
      block_len = 128;
   } else if (s == XMODEM_STX) {
      block_len = 1024;
   } else {
      LOG_INF("Invalid type 0x%02x", s);
      return -EINVAL;
   }
   if (xmodem_len < XMODEM_HEADER_LEN + block_len + crc_len) {
      LOG_INF("Invalid length %d, expected %d", xmodem_len,
              (XMODEM_HEADER_LEN + block_len + crc_len));
      return -EINVAL;
   }
   if (xmodem_crc) {
      if (!appl_update_xmodem_crc(xmodem_buffer, block_len)) {
         return -EBADMSG;
      }
   } else {
      int sum = 0;
      const uint8_t *cur = &xmodem_buffer[XMODEM_HEADER_LEN];

      for (int i = 0; i < block_len; ++i) {
         sum += *cur++;
      }
      if (*cur == (uint8_t)sum) {
         LOG_INF("Block 128 %u verified", b);
      } else {
         LOG_INF("Block 128 %u checksum error %u != %u", b, (uint8_t)sum, *cur);
         return -EBADMSG;
      }
   }

   if (xmodem_block == b) {
      rc = appl_update_write(&xmodem_buffer[XMODEM_HEADER_LEN], block_len);
      ++xmodem_block;
   } else {
      ++b;
      if (xmodem_block == b) {
         // b already processed, next block expected
         rc = XMODEM_DUPLICATE;
      } else {
         return -EBADMSG;
      }
   }
   xmodem_len = 0;
   return rc;
}
