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

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
// #include <zephyr/sys/atomic.h>

#include <nrf_modem_at.h>

#include "io_job_queue.h"
#include "modem_at.h"
#include "parse.h"

LOG_MODULE_DECLARE(MODEM, CONFIG_MODEM_LOG_LEVEL);

#if defined(CONFIG_NRF_MODEM_LIB)

#include <nrf_modem_at.h>

#define INTERNAL_BUF_SIZE 256

static K_MUTEX_DEFINE(lte_at_mutex);
static char lte_at_buf[INTERNAL_BUF_SIZE];

static volatile int lte_at_counter = 0;
static volatile bool lte_at_warn = true;

static const char *volatile lte_at_response_skip = NULL;
static atomic_ptr_t lte_at_response_handler = ATOMIC_PTR_INIT(NULL);

static size_t terminate_at_buffer(char *line, size_t len)
{
   char *current = line;
   while (*current != 0 && *current != '\n' && *current != '\r' && len > 1) {
      ++current;
      --len;
   }
   if (*current != 0 && len > 0) {
      *current = 0;
   }
   return current - line;
}

int modem_at_cmd(char *buf, size_t len, const char *skip, const char *cmd)
{
   int err;
   int at_len;

   LOG_DBG("%s", cmd);
   err = modem_at_lock(K_FOREVER);
   if (err) {
      LOG_INF("Modem busy");
      return err;
   }
   err = nrf_modem_at_cmd(lte_at_buf, sizeof(lte_at_buf), "%s", cmd);
   if (err < 0) {
      if (lte_at_warn) {
         LOG_WRN(">> %s:", cmd);
         LOG_WRN(">> %s: %d", strerror(-err), err);
      }
      modem_at_unlock();
      return err;
   } else if (err > 0) {
      int error = nrf_modem_at_err(err);
      if (lte_at_warn) {
         const char *type = "AT ERROR";
         switch (nrf_modem_at_err_type(err)) {
            case NRF_MODEM_AT_CME_ERROR:
               type = "AT CME ERROR";
               break;
            case NRF_MODEM_AT_CMS_ERROR:
               type = "AT CMS ERROR";
               break;
            case NRF_MODEM_AT_ERROR:
            default:
               break;
         }
         LOG_WRN(">> %s:", cmd);
         LOG_WRN(">> %s: %d (%d)", type, error, err);
      }
      modem_at_unlock();
      if (error) {
         return -error;
      } else {
         return -EBADMSG;
      }
   }
   at_len = terminate_at_buffer(lte_at_buf, sizeof(lte_at_buf));
   if (buf && len) {
      int skip_len = 0;
      if (skip) {
         skip_len = strstart(lte_at_buf, skip, true);
         at_len -= skip_len;
      }
      if (at_len > len - 1) {
         at_len = len - 1;
      }
      memmove(buf, lte_at_buf + skip_len, at_len);
      buf[at_len] = 0;
   }
   modem_at_unlock();
   return at_len;
}

int modem_at_lock(const k_timeout_t timeout)
{
   int res = k_mutex_lock(&lte_at_mutex, timeout);
   if (!res) {
      if (atomic_ptr_get(&lte_at_response_handler)) {
         k_mutex_unlock(&lte_at_mutex);
         return -EBUSY;
      }
      if (lte_at_counter) {
         lte_at_counter++;
      }
   }
   return res;
}

int modem_at_lock_no_warn(const k_timeout_t timeout)
{
   int res = k_mutex_lock(&lte_at_mutex, timeout);
   if (!res) {
      if (atomic_ptr_get(&lte_at_response_handler)) {
         k_mutex_unlock(&lte_at_mutex);
         return -EBUSY;
      }
      lte_at_counter++;
      lte_at_warn = false;
   }
   return res;
}

int modem_at_unlock(void)
{
   int res = k_mutex_unlock(&lte_at_mutex);
   if (!res && lte_at_counter) {
      --lte_at_counter;
      if (!lte_at_counter) {
         lte_at_warn = true;
      }
   }
   return res;
}

int modem_at_cmdf(char *buf, size_t len, const char *skip, const char *cmd, ...)
{
   va_list ap;
   int err = modem_at_lock(K_FOREVER);
   if (err) {
      LOG_INF("Modem busy");
      return err;
   }

   va_start(ap, cmd);
   vsnprintf(lte_at_buf, sizeof(lte_at_buf), cmd, ap);
   va_end(ap);

   err = modem_at_cmd(buf, len, skip, lte_at_buf);
   modem_at_unlock();

   return err;
}

static void modem_at_cmd_async_response_handler(const char *response)
{
   modem_at_response_handler_t handler = atomic_ptr_get(&lte_at_response_handler);
   if (handler) {
      int skip_len = 0;
      size_t len = strlen(response);
      if (lte_at_response_skip) {
         skip_len = strstart(response, lte_at_response_skip, true);
         len -= skip_len;
         response += skip_len;
      }
      handler(response);
      lte_at_response_skip = NULL;
      atomic_ptr_clear(&lte_at_response_handler);
   }
}

int modem_at_cmdf_async(modem_at_response_handler_t handler, const char *skip, const char *cmd, ...)
{
   va_list ap;
   int res = -EBUSY;

   k_mutex_lock(&lte_at_mutex, K_FOREVER);
   if (atomic_ptr_cas(&lte_at_response_handler, NULL, handler)) {
      lte_at_response_skip = skip;
      va_start(ap, cmd);
      vsnprintf(lte_at_buf, sizeof(lte_at_buf), cmd, ap);
      va_end(ap);

      res = nrf_modem_at_cmd_async(modem_at_cmd_async_response_handler, "%s", lte_at_buf);
      if (res) {
         lte_at_response_skip = NULL;
         atomic_ptr_clear(&lte_at_response_handler);
      }
   } else {
      LOG_INF("Modem busy");
   }
   k_mutex_unlock(&lte_at_mutex);

   return res;
}

int modem_at_cmd_async(modem_at_response_handler_t handler, const char *skip, const char *cmd)
{
   int res = -EBUSY;

   k_mutex_lock(&lte_at_mutex, K_FOREVER);
   if (atomic_ptr_cas(&lte_at_response_handler, NULL, handler)) {
      lte_at_response_skip = skip;
      res = nrf_modem_at_cmd_async(modem_at_cmd_async_response_handler, "%s", cmd);
      if (res) {
         lte_at_response_skip = NULL;
         atomic_ptr_clear(&lte_at_response_handler);
      }
   } else {
      LOG_INF("Modem busy");
   }
   k_mutex_unlock(&lte_at_mutex);

   return res;
}

#else

int modem_at_lock(const k_timeout_t timeout)
{
   (void)timeout;
   return 0;
}

int modem_at_lock_no_warn(const k_timeout_t timeout)
{
   (void)timeout;
   return 0;
}

int modem_at_unlock(void)
{
   return 0;
}

int modem_at_cmdf(char *buf, size_t max_len, const char *skip, const char *cmd, ...)
{
   (void)buf;
   (void)max_len;
   (void)skip;
   (void)cmd;
   return 0;
}

int modem_at_cmd(char *buf, size_t max_len, const char *skip, const char *cmd, ...)
{
   (void)buf;
   (void)max_len;
   (void)skip;
   (void)cmd;
   return 0;
}
int modem_at_cmdf_async(modem_at_response_handler_t handler, const char *skip, const char *cmd, ...)
{
   (void)handler;
   (void)skip;
   (void)cmd;
   return 0;
}

int modem_at_cmd_async(modem_at_response_handler_t handler, const char *skip, const char *cmd)
{
   (void)handler;
   (void)skip;
   (void)cmd;
   return 0;
}

#endif
