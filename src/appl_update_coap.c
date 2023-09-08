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
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include "appl_diagnose.h"
#include "appl_update.h"
#include "appl_update_coap.h"
#include "coap_client.h"
#include "io_job_queue.h"
#include "parse.h"

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

#define APP_COAP_MAX_RES_PATH_LEN 64

#define APP_COAP_FIRMWARE_PATH "fw"

#define APP_COAP_MODEM_PATH CONFIG_APPL_MODEL

#define APP_COAP_MAX_UPDATE_SIZE (0x70000)

static K_SEM_DEFINE(appl_update_coap_ready, 0, 1);
static K_MUTEX_DEFINE(appl_update_coap_mutex);

static COAP_CONTEXT(update_context, 128);

static uint8_t coap_resource_path[APP_COAP_MAX_RES_PATH_LEN];
static uint8_t coap_etag[COAP_TOKEN_MAX_LEN + 1];

static bool coap_download = false;
static bool coap_download_request = false;
static bool coap_download_canceled = false;
static bool coap_download_ready = false;
static bool coap_apply_update = false;
static struct coap_block_context coap_block_context;

static bool appl_update_coap_cancel_download(bool cancel);
static int appl_update_coap_start(const char *resource);

static void appl_update_coap_erase_fn(struct k_work *work)
{
   LOG_INF("Download, erase flash ...");
   k_sleep(K_MSEC(200));

   if (!appl_update_erase()) {
      LOG_INF("Download, erase flash done.");
      k_mutex_lock(&appl_update_coap_mutex, K_FOREVER);
      if (coap_download) {
         coap_block_transfer_init(&coap_block_context, COAP_BLOCK_1024, 0);
         coap_download_request = true;
      }
      k_mutex_unlock(&appl_update_coap_mutex);
   } else {
      appl_update_coap_cancel_download(true);
      appl_update_cmd("erase");
   }
}

static K_WORK_DELAYABLE_DEFINE(appl_update_coap_erase_work, appl_update_coap_erase_fn);

static bool appl_update_coap_cancel_download(bool cancel)
{
   bool canceled;
   k_work_cancel_delayable(&appl_update_coap_erase_work);
   k_mutex_lock(&appl_update_coap_mutex, K_FOREVER);
   canceled = coap_download;
   coap_download = false;
   coap_download_request = false;
   coap_download_canceled = cancel;
   coap_download_ready = !cancel;
   memset(coap_etag, 0, sizeof(coap_etag));
   k_mutex_unlock(&appl_update_coap_mutex);
   return canceled;
}

static int appl_update_coap_normalize(const char *resource, char *normalized, size_t len)
{
   int l = len;
   char term;
   const char *cur;

   memset(normalized, 0, len);
   while (*resource == ' ') {
      ++resource;
   }
   cur = resource;
   term = *cur;
   if (term != '"' && term != '\'') {
      term = ' ';
   } else {
      ++cur;
   }
   while (l > 0 && *cur && *cur != term) {
      *normalized++ = *cur++;
      --l;
   }
   if (*cur && *cur != term) {
      LOG_INF("Resource path %s too long, max. %d bytes supported.", resource, len - 1);
      return -ENOMEM;
   }
   return 0;
}

static int appl_update_coap_verify_version(void)
{
   char download_version[32];

   int err = appl_update_get_pending_version(download_version, sizeof(download_version));
   if (!err) {
      if (stricmp(coap_resource_path, download_version)) {
         LOG_INF("CoAP download version %s doesn't match %s!", download_version, coap_resource_path);
         err = -EINVAL;
      } else {
         LOG_INF("CoAP downloaded version %s.", download_version);
      }
   }

   return err;
}

bool appl_update_coap_pending(void)
{
   bool download;

   k_mutex_lock(&appl_update_coap_mutex, K_FOREVER);
   download = coap_download;
   k_mutex_unlock(&appl_update_coap_mutex);

   return download;
}

bool appl_update_coap_reboot(void)
{
   bool reboot;

   k_mutex_lock(&appl_update_coap_mutex, K_FOREVER);
   reboot = coap_download_ready && coap_apply_update;
   k_mutex_unlock(&appl_update_coap_mutex);

   return reboot;
}

int appl_update_coap_status(uint8_t *buf, size_t len)
{
   int index = 0;
   k_mutex_lock(&appl_update_coap_mutex, K_FOREVER);
   if (coap_download) {
      index = snprintf(buf, len, "Downloading %s", coap_resource_path);
      if (coap_block_context.total_size > 0) {
         index += snprintf(buf + index, len - index, ", %d%%",
                           (coap_block_context.current) * 100 / coap_block_context.total_size);
      }
   } else if (coap_download_ready) {
      index = snprintf(buf, len, "Downloaded %s", coap_resource_path);
      if (coap_apply_update) {
         index += snprintf(buf + index, len - index, " reboot.");
      }
   } else if (coap_download_canceled) {
      index = snprintf(buf, len, "Canceled %s", coap_resource_path);
   }
   k_mutex_unlock(&appl_update_coap_mutex);

   return index;
}

int appl_update_coap_cmd(const char *config)
{
   int rc = 0;
   char cmd[10];
   char version[32];
   const char *cur = config;

   if (appl_reboots()) {
      return -ESHUTDOWN;
   }

   memset(cmd, 0, sizeof(cmd));
   cur = parse_next_text(cur, ' ', cmd, sizeof(cmd));
   rc = appl_update_coap_normalize(cur, version, sizeof(version));
   if (rc < 0) {
      return rc;
   }
   if (!stricmp(cmd, "download")) {
      coap_apply_update = false;
      rc = appl_update_coap_start(version);
   } else if (!stricmp(cmd, "update")) {
      coap_apply_update = true;
      rc = appl_update_coap_start(version);
   } else if (!stricmp(cmd, "apply")) {
      k_mutex_lock(&appl_update_coap_mutex, K_FOREVER);
      if (!coap_resource_path[0]) {
         LOG_INF("No CoAP download!");
         k_mutex_unlock(&appl_update_coap_mutex);
         rc = -EINVAL;
      } else if (stricmp(version, coap_resource_path)) {
         LOG_INF("CoAP download version %s doesn't match %s!", version, coap_resource_path);
         k_mutex_unlock(&appl_update_coap_mutex);
         rc = -EINVAL;
      } else if (!coap_download_ready) {
         LOG_INF("CoAP download not ready!");
         k_mutex_unlock(&appl_update_coap_mutex);
         rc = -EINVAL;
      } else if (!appl_update_coap_verify_version()) {
         k_mutex_unlock(&appl_update_coap_mutex);
         rc = -EINVAL;
      } else {
         appl_update_cmd("reboot");
      }
   } else if (!stricmp(cmd, "cancel")) {
      if (!coap_resource_path[0]) {
         LOG_INF("No CoAP download!");
         rc = -EINVAL;
      } else if (stricmp(version, coap_resource_path)) {
         LOG_INF("CoAP download version %s doesn't match %s!", version, coap_resource_path);
         rc = -EINVAL;
      } else {
         appl_update_coap_cancel_download(true);
         work_reschedule_for_cmd_queue(&appl_update_coap_erase_work, K_MSEC(100));
      }
   } else {
      rc = -EINVAL;
   }

   return rc;
}

static int appl_update_coap_set_resource(const char *resource)
{
   int rc = 0;

   k_mutex_lock(&appl_update_coap_mutex, K_FOREVER);
   if (coap_download) {
      rc = -EBUSY;
   } else {
      if (!stricmp(resource, CONFIG_MCUBOOT_IMAGE_VERSION)) {
         LOG_INF("Version '%s' already available.", resource);
         rc = -EEXIST;
      } else {
         LOG_INF("Update '%s' to '%s'.", CONFIG_MCUBOOT_IMAGE_VERSION, resource);
         memset(coap_resource_path, 0, sizeof(coap_resource_path));
         strncpy(coap_resource_path, resource, sizeof(coap_resource_path) - 1);
         coap_download = true;
         coap_download_request = false;
         coap_download_canceled = false;
         coap_download_ready = false;
      }
   }
   k_mutex_unlock(&appl_update_coap_mutex);

   return rc;
}

static int appl_update_coap_start(const char *resource)
{
   int rc = 0;

   rc = appl_update_coap_set_resource(resource);

   if (rc) {
      return rc;
   }

   memset(coap_etag, 0, sizeof(coap_etag));
   rc = appl_update_start();
   if (!rc) {
      LOG_INF("Start downloading %s.", coap_resource_path);
      work_reschedule_for_cmd_queue(&appl_update_coap_erase_work, K_MSEC(1000));
   }
   return rc;
}

int appl_update_coap_cancel(void)
{
   int rc = 0;
   if (appl_reboots()) {
      return -ESHUTDOWN;
   }
   if (appl_update_coap_cancel_download(true)) {
      rc = appl_update_cancel();
   }
   return rc;
}

int appl_update_coap_parse_data(uint8_t *data, size_t len)
{
   int res;
   int err;
   int format = -1;
   int block2;
   bool ready;
   bool cancel = true;
   size_t current;
   struct coap_packet reply;
   struct coap_option message_option;
   struct coap_block_context block_context;
   uint8_t etag[COAP_TOKEN_MAX_LEN + 1];
   const uint8_t *payload;
   uint16_t payload_len;
   uint16_t block2_bytes;
   uint8_t code;

   if (appl_reboots()) {
      return -ESHUTDOWN;
   }

   k_mutex_lock(&appl_update_coap_mutex, K_FOREVER);
   ready = !coap_download;
   if (coap_download) {
      block_context = coap_block_context;
      current = coap_block_context.current;
   }
   k_mutex_unlock(&appl_update_coap_mutex);

   if (ready) {
      return PARSE_NONE;
   }

   res = coap_packet_parse(&reply, data, len, NULL, 0);
   if (res < 0) {
      LOG_DBG("Malformed response received: %d", res);
      return res;
   }

   res = coap_client_match(&reply, update_context.mid, update_context.token);
   if (res < PARSE_RESPONSE) {
      LOG_INF("No download response");
      return res;
   }

   code = coap_header_get_code(&reply);
   update_context.message_len = 0;
   ready = true;
   if (code == COAP_RESPONSE_CODE_CONTENT) {

      err = coap_find_options(&reply, COAP_OPTION_CONTENT_FORMAT, &message_option, 1);
      if (err == 1) {
         format = coap_client_decode_content_format(&message_option);
      }

      err = coap_find_options(&reply, COAP_OPTION_ETAG, &message_option, 1);
      if (err == 1) {
         coap_client_decode_etag(&message_option, etag);
         if (current) {
            if (memcmp(coap_etag, etag, sizeof(coap_etag))) {
               LOG_INF("Download content changed, new etag!");
               LOG_HEXDUMP_INF(&etag[1], etag[0], "new etag");
               LOG_HEXDUMP_INF(&coap_etag[1], coap_etag[0], "previous etag");
               appl_update_coap_cancel_download(true);
               return -EINVAL;
            }
         } else {
            memcpy(coap_etag, etag, sizeof(coap_etag));
         }
      }
      block2 = coap_get_option_int(&reply, COAP_OPTION_BLOCK2);
      payload = coap_packet_get_payload(&reply, &payload_len);
      if (block2 == -ENOENT) {
         if (current != 0) {
            LOG_INF("Download without block2 mismatch, current pos 0x%x", current);
            appl_update_coap_cancel_download(cancel);
            return -EINVAL;
         }
      } else {
         ready = !GET_MORE(block2);
         err = coap_update_from_block(&reply, &block_context);
         if (err < 0) {
            LOG_INF("Download update block failed, %d", err);
            appl_update_coap_cancel_download(cancel);
            return err;
         }
         if (block_context.total_size > APP_COAP_MAX_UPDATE_SIZE) {
            LOG_INF("Download size 0x%x exceeds max. 0x%x.", block_context.total_size, APP_COAP_MAX_UPDATE_SIZE);
            appl_update_coap_cancel_download(cancel);
            return -ENOMEM;
         }
         if (current != block_context.current) {
            LOG_INF("Download block 0x%x mismatch 0x%x", current, block_context.current);
            appl_update_coap_cancel_download(cancel);
            return -EINVAL;
         }
         block2_bytes = coap_block_size_to_bytes(block_context.block_size);
         if (payload_len > block2_bytes) {
            LOG_INF("Download block size exceeded, %d > %d", payload_len, block2_bytes);
            appl_update_coap_cancel_download(cancel);
            return -EINVAL;
         }
         if (payload_len < block2_bytes && !ready) {
            LOG_INF("Download block size too small, %d < %d", payload_len, block2_bytes);
            appl_update_coap_cancel_download(cancel);
            return -EINVAL;
         }
      }
      if (payload_len > 0) {
         appl_update_write(payload, payload_len);
      }
      if (ready) {
         err = appl_update_finish();
         if (!err) {
            err = appl_update_coap_verify_version();
         }
         if (!err) {
            err = appl_update_request_upgrade();
         }
         if (err) {
            LOG_INF("CoAP transfer failed. %d", err);
         } else {
            cancel = false;
            LOG_INF("CoAP transfer succeeded.");
            if (coap_apply_update) {
               LOG_INF("Reboot to apply update.");
            } else {
               LOG_INF("Reboot required to apply update.");
            }
         }
      } else {
         k_mutex_lock(&appl_update_coap_mutex, K_FOREVER);
         if (current == coap_block_context.current) {
            coap_block_context = block_context;
            coap_block_context.current += block2_bytes;
            coap_download_request = true;
            ready = false;
         }
         k_mutex_unlock(&appl_update_coap_mutex);
      }
   }
   if (ready) {
      appl_update_coap_cancel_download(cancel);
   }
   if (PARSE_CON_RESPONSE == res) {
      res = coap_client_prepare_ack(&reply);
   }
   return res;
}

bool appl_update_coap_pending_next(void)
{
   bool request = false;

   if (!appl_reboots()) {
      k_mutex_lock(&appl_update_coap_mutex, K_FOREVER);
      if (coap_download) {
         request = coap_download_request;
      }
      k_mutex_unlock(&appl_update_coap_mutex);
   }

   return request;
}

int appl_update_coap_next(void)
{
   int rc = 0;
   bool request = false;
   struct coap_block_context block_context;

   if (appl_reboots()) {
      return -ESHUTDOWN;
   }

   k_mutex_lock(&appl_update_coap_mutex, K_FOREVER);
   if (coap_download) {
      request = coap_download_request;
      if (request) {
         coap_download_request = false;
         block_context = coap_block_context;
      }
   } else {
      rc = -EINVAL;
   }
   k_mutex_unlock(&appl_update_coap_mutex);

   if (request) {
      uint8_t *token = (uint8_t *)&update_context.token;
      struct coap_packet request;

      update_context.token = coap_client_next_token();
      update_context.mid = coap_next_id();

      rc = coap_packet_init(&request, update_context.message_buf, sizeof(update_context.message_buf),
                            COAP_VERSION_1, COAP_TYPE_CON,
                            sizeof(update_context.token), token,
                            COAP_METHOD_GET, update_context.mid);
      if (rc < 0) {
         LOG_WRN("Failed to create CoAP request, %d", rc);
         return rc;
      }

      rc = coap_packet_append_option(&request, COAP_OPTION_URI_PATH,
                                     APP_COAP_FIRMWARE_PATH,
                                     strlen(APP_COAP_FIRMWARE_PATH));
      if (rc < 0) {
         LOG_WRN("Failed to encode CoAP URI-PATH prefix, %d", rc);
         return rc;
      }

      rc = coap_packet_append_option(&request, COAP_OPTION_URI_PATH,
                                     APP_COAP_MODEM_PATH,
                                     strlen(APP_COAP_MODEM_PATH));
      if (rc < 0) {
         LOG_WRN("Failed to encode CoAP URI-PATH model, %d", rc);
         return rc;
      }

      rc = coap_packet_append_option(&request, COAP_OPTION_URI_PATH,
                                     coap_resource_path,
                                     strlen(coap_resource_path));
      if (rc < 0) {
         LOG_WRN("Failed to encode CoAP URI-PATH resource, %d", rc);
         return rc;
      }
      rc = coap_append_block2_option(&request, &block_context);
      update_context.message_len = request.offset;
      rc = request.offset;
      if (rc) {
         int block2 = coap_get_option_int(&request, COAP_OPTION_BLOCK2);
         LOG_INF("Download block %d, pos 0x%x", GET_BLOCK_NUM(block2), block_context.current);
      }
   }
   return rc;
}

int appl_update_coap_message(const uint8_t **buffer)
{
   if (buffer) {
      *buffer = update_context.message_buf;
   }
   return update_context.message_len;
}
