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

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

static struct flash_img_context dfu_context;
static volatile int dfu_flash_area_id = -1;
static volatile int64_t dfu_time = -1;

int appl_update_start(void)
{
   int rc = -EINVAL;

   if (dfu_context.flash_area) {
      return -EINPROGRESS;
   }
   dfu_flash_area_id = -1;
   memset(&dfu_context, 0, sizeof(dfu_context));
   rc = flash_img_init(&dfu_context);
   if (!rc) {
      dfu_flash_area_id = dfu_context.flash_area->fa_id;
      dfu_time = k_uptime_get();
   }
   return rc;
}

size_t appl_update_written(void)
{
   return flash_img_bytes_written(&dfu_context);
}

int appl_update_erase(void)
{
   if (dfu_flash_area_id >= 0) {
      return boot_erase_img_bank((uint8_t)dfu_flash_area_id);
   }
   return -EINVAL;
}

int appl_update_write(const uint8_t *data, size_t len)
{
   return flash_img_buffered_write(&dfu_context, data, len, false);
}

int appl_update_finish(void)
{
   int rc = flash_img_buffered_write(&dfu_context, NULL, 0, true);
   if (!rc) {
      if (dfu_time > -1) {
         dfu_time = k_uptime_get() - dfu_time;
         LOG_INF("Transfer %d s.", (int)(dfu_time / MSEC_PER_SEC));
         dfu_time = -1;
      } else {
         LOG_INF("Transfer finished");
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
   struct mcuboot_img_header header;
   int rc;
   if (dfu_flash_area_id < 0) {
      return -EINVAL;
   }

   memset(&header, 0, sizeof(header));
   rc = boot_read_bank_header((uint8_t)dfu_flash_area_id, &header, sizeof(header));
   if (rc < 0) {
      LOG_WRN("Update failed, header not available: %d", rc);
      return rc;
   }

   if (header.mcuboot_version == 1) {
      struct mcuboot_img_sem_ver *sem_ver = &header.h.v1.sem_ver;
      if (dfu_context.flash_area) {
         LOG_INF("Update %d bytes, %d.%d.%d-%d ongoing.",
                 header.h.v1.image_size, sem_ver->major, sem_ver->minor,
                 sem_ver->revision, sem_ver->build_num);
      } else {
         LOG_INF("Update %d bytes, %d.%d.%d-%d ready after %d s.",
                 header.h.v1.image_size, sem_ver->major, sem_ver->minor,
                 sem_ver->revision, sem_ver->build_num, (int)(dfu_time / MSEC_PER_SEC));
      }
   } else {
      LOG_WRN("Update failed, unknown mcuboot version %u", header.mcuboot_version);
      return -EINVAL;
   }

   return rc;
}

int appl_update_request_upgrade(void)
{
   int rc = boot_request_upgrade(BOOT_UPGRADE_TEST);
   if (!rc) {
      int swap_type = mcuboot_swap_type();
      switch (swap_type) {
         case BOOT_SWAP_TYPE_NONE:
            LOG_DBG("> no update");
            break;
         case BOOT_SWAP_TYPE_TEST:
            LOG_DBG("> test update");
            break;
         case BOOT_SWAP_TYPE_PERM:
            LOG_DBG("> permanent update");
            break;
         case BOOT_SWAP_TYPE_REVERT:
            LOG_DBG("> revert update");
            break;
         case BOOT_SWAP_TYPE_FAIL:
            LOG_DBG("> fail update");
            break;
      }
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
