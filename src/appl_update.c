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
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>

#include "appl_update.h"
#include "parse.h"

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

static struct flash_img_context dfu_context;
static volatile int dfu_flash_area_id = -1;
static volatile int64_t dfu_time = -1;

static int appl_update_init_context(void)
{
   int rc;

   dfu_flash_area_id = -1;
   memset(&dfu_context, 0, sizeof(dfu_context));
   rc = flash_img_init(&dfu_context);
   if (!rc) {
      dfu_flash_area_id = dfu_context.flash_area->fa_id;
   }
   return rc;
}

static int appl_update_swap_type(bool info)
{
   const char *desc = NULL;

   int swap_type = mcuboot_swap_type();
   switch (swap_type) {
      case BOOT_SWAP_TYPE_NONE:
         desc = "no update pending";
         break;
      case BOOT_SWAP_TYPE_TEST:
         desc = "test update";
         break;
      case BOOT_SWAP_TYPE_PERM:
         desc = "permanent update";
         break;
      case BOOT_SWAP_TYPE_REVERT:
         desc = "revert update";
         break;
      case BOOT_SWAP_TYPE_FAIL:
         desc = "fail update";
         break;
   }
   if (desc) {
      if (info) {
         LOG_INF("%s", desc);
      } else {
         LOG_DBG("%s", desc);
      }
   }
   return swap_type;
}

static int appl_update_dump_header(bool warn)
{
   struct mcuboot_img_header header;
   int rc;

   memset(&header, 0, sizeof(header));
   rc = boot_read_bank_header((uint8_t)dfu_flash_area_id, &header, sizeof(header));
   if (rc < 0) {
      if (warn) {
         LOG_WRN("Update failed, header not available: %d", rc);
      }
      return rc;
   }

   if (header.mcuboot_version == 1) {
      struct mcuboot_img_sem_ver *sem_ver = &header.h.v1.sem_ver;
      if (dfu_context.flash_area) {
         LOG_INF("Update %d bytes, %d.%d.%d-%d ongoing.",
                 header.h.v1.image_size, sem_ver->major, sem_ver->minor,
                 sem_ver->revision, sem_ver->build_num);
      } else if (dfu_time > -1) {
         LOG_INF("Update %d bytes, %d.%d.%d-%d ready after %d s.",
                 header.h.v1.image_size, sem_ver->major, sem_ver->minor,
                 sem_ver->revision, sem_ver->build_num, (int)(dfu_time / MSEC_PER_SEC));
      } else {
         LOG_INF("Update %d bytes, %d.%d.%d-%d ready.",
                 header.h.v1.image_size, sem_ver->major, sem_ver->minor,
                 sem_ver->revision, sem_ver->build_num);
      }
   } else {
      LOG_WRN("Update failed, unknown mcuboot version %u", header.mcuboot_version);
      return -EINVAL;
   }

   return rc;
}

int appl_update_cmd(const char *config)
{
   int rc = 0;
   bool close = false;
   const char *cur = config;
   char value[7];

   memset(value, 0, sizeof(value));
   while (*cur == ' ') {
      ++cur;
   }
   cur = parse_next_text(cur, ' ', value, sizeof(value));

   if (stricmp("info", value) && stricmp("erase", value) && stricmp("revert", value)) {
      LOG_INF("update '%s' not supported!", config);
      return -EINVAL;
   }

   if (!dfu_context.flash_area) {
      close = true;
      rc = appl_update_init_context();
      if (rc) {
         return rc;
      }
   }

   if (!stricmp("info", value)) {
      size_t written = written = flash_img_bytes_written(&dfu_context);
      if (written) {
         LOG_INF("Update %u bytes written.", written);
      }
      memset(&dfu_context, 0, sizeof(dfu_context));
      rc = appl_update_dump_header(false);
      if (!rc) {
         appl_update_swap_type(true);
      }
   } else if (!stricmp("erase", value)) {
      rc = boot_erase_img_bank((uint8_t)dfu_flash_area_id);
   } else if (!stricmp("revert", value)) {
      rc = boot_request_upgrade(BOOT_UPGRADE_TEST);
   }

   if (close) {
      memset(&dfu_context, 0, sizeof(dfu_context));
      dfu_flash_area_id = -1;
   }
   return rc;
}

void appl_update_cmd_help(void)
{
   LOG_INF("> help update:");
   LOG_INF("  update        : start update.");
   LOG_INF("  update info   : display current update info.");
   LOG_INF("  update erase  : erase current update.");
   LOG_INF("  update revert : revert last update.");
}

int appl_update_start(void)
{
   int rc = -EINVAL;

   if (dfu_context.flash_area) {
      return -EINPROGRESS;
   }
   rc = appl_update_init_context();
   if (!rc) {
      dfu_time = k_uptime_get();
   }
   return rc;
}

size_t appl_update_written(void)
{
   if (dfu_context.flash_area) {
      return flash_img_bytes_written(&dfu_context);
   } else {
      return 0;
   }
}

int appl_update_erase(void)
{
   if (dfu_flash_area_id >= 0) {
      return boot_erase_img_bank((uint8_t)dfu_flash_area_id);
      dfu_time = -1;
   }
   return -EINVAL;
}

int appl_update_write(const uint8_t *data, size_t len)
{
   return flash_img_buffered_write(&dfu_context, data, len, false);
}

int appl_update_finish(void)
{
   size_t written = flash_img_bytes_written(&dfu_context);
   int rc = flash_img_buffered_write(&dfu_context, NULL, 0, true);
   if (!rc) {
      if (dfu_time > -1) {
         dfu_time = k_uptime_get() - dfu_time;
         LOG_INF("Transfered %u bytes in %d s.", written, (int)(dfu_time / MSEC_PER_SEC));
         dfu_time = -1;
      } else {
         LOG_INF("Transfered %u bytes.", written);
      }
   }
   return rc;
}

int appl_update_cancel(void)
{
   int rc = flash_img_buffered_write(&dfu_context, NULL, 0, true);
   if (!rc) {
      if (dfu_time > -1) {
         dfu_time = k_uptime_get() - dfu_time;
         LOG_INF("Transfer canceled after %d s.", (int)(dfu_time / MSEC_PER_SEC));
         dfu_time = -1;
      } else {
         LOG_INF("Transfer canceled");
      }
      dfu_flash_area_id = -1;
   }
   return rc;
}

int appl_update_dump_pending_image(void)
{
   if (dfu_flash_area_id < 0) {
      return -EINVAL;
   }
   return appl_update_dump_header(true);
}

int appl_update_request_upgrade(void)
{
   int rc = boot_request_upgrade(BOOT_UPGRADE_TEST);
   if (!rc) {
      appl_update_swap_type(false);
   }
   return rc;
}

int appl_update_image_verified(void)
{
   if (boot_is_img_confirmed()) {
      return 1;
   } else {
      LOG_INF("Update confirm image.");
      return boot_write_img_confirmed();
   }
}

static int appl_update_init(void)
{
   dfu_flash_area_id = -1;
   dfu_time = -1;
   memset(&dfu_context, 0, sizeof(dfu_context));
   return 0;
}

SYS_INIT(appl_update_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);