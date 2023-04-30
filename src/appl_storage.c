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

#include <zephyr/device.h>
#include <zephyr/drivers/eeprom.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <stddef.h>
#include <stdio.h>

#include "appl_storage.h"
#include "appl_time.h"


#if defined(CONFIG_FLASH) || defined(CONFIG_EEPROM)

LOG_MODULE_REGISTER(STORAGE, CONFIG_STORAGE_LOG_LEVEL);

#define TIME_SIZE 6

#define MAX_ITEM_SIZE (TIME_SIZE + MAX_VALUE_SIZE)

enum storage_init_state {
   STORAGE_NOT_INITIALIZED,
   STORAGE_INITIALIZE_ERROR,
   STORAGE_INITIALIZED,
};

struct storage_setup {
   const char *desc;
   uint8_t header[8];
   size_t item_size;
   size_t value_size;
   off_t headers_offset;
   off_t current_offset;
   off_t end_offset;
};

static volatile enum storage_init_state storage_init_state = STORAGE_NOT_INITIALIZED;

static K_MUTEX_DEFINE(storage_mutex);

static size_t storage_size;

static size_t storage_setups_count;
static struct storage_setup storage_setups[3];

static inline off_t appl_storage_start_offset(const struct storage_setup *config)
{
   return config->headers_offset + config->item_size;
}

#if defined(CONFIG_FLASH)

#define DT_STORAGE_DEV DT_ALIAS(appl_storage_flash)

static const struct device *storage_dev = DEVICE_DT_GET_OR_NULL(DT_STORAGE_DEV);

int appl_storage_read_memory(off_t mem_addr, uint8_t *data, size_t num_bytes)
{
   if (storage_init_state != STORAGE_INITIALIZE_ERROR) {
      int rc = flash_read(storage_dev, mem_addr, data, num_bytes);
      if (rc) {
         LOG_DBG("Storage: reading %d@0x%lx failed, %d", num_bytes, mem_addr, rc);
      } else {
         LOG_DBG("Storage: read %d@0x%lx ", num_bytes, mem_addr);
      }
      return rc;
   } else {
      return -ENOTSUP;
   }
}

int appl_storage_write_memory(off_t mem_addr, const uint8_t *data, size_t num_bytes)
{
   if (storage_init_state != STORAGE_INITIALIZE_ERROR) {
      int rc = flash_write(storage_dev, mem_addr, data, num_bytes);
      if (rc) {
         LOG_DBG("Storage: writing %d@0x%lx failed, %d", num_bytes, mem_addr, rc);
         k_sleep(K_MSEC(1000));
      } else {
         LOG_DBG("Storage: written %d@0x%lx ", num_bytes, mem_addr);
      }
      return rc;
   } else {
      return -ENOTSUP;
   }
}

int appl_storage_erase_memory(off_t mem_addr, size_t num_bytes)
{
   if (storage_init_state != STORAGE_INITIALIZE_ERROR) {
      int rc = flash_erase(storage_dev, mem_addr, num_bytes);
      if (rc) {
         LOG_DBG("Storage: erasing %d@0x%lx failed, %d", num_bytes, mem_addr, rc);
      } else {
         LOG_DBG("Storage: erased %d@0x%lx ", num_bytes, mem_addr);
      }
      return rc;
   } else {
      return -ENOTSUP;
   }
}

static int appl_storage_get_page_info_by_offs(off_t mem_addr, struct flash_pages_info *info)
{
   if (storage_init_state != STORAGE_INITIALIZE_ERROR) {
      return flash_get_page_info_by_offs(storage_dev, mem_addr, info);
   } else {
      return -ENOTSUP;
   }
}

#elif defined(CONFIG_EEPROM)

#define DT_STORAGE_DEV DT_ALIAS(appl_storage_eeprom)

static const uint16_t storage_page_size = DT_PROP(DT_STORAGE_DEV, pagesize);
static const uint16_t storage_write_timeout = DT_PROP(DT_STORAGE_DEV, timeout);
static const struct device *storage_dev = DEVICE_DT_GET_OR_NULL(DT_STORAGE_DEV);

int appl_storage_read_memory(off_t mem_addr, uint8_t *data, size_t num_bytes)
{
   if (storage_init_state != STORAGE_INITIALIZE_ERROR) {
      int rc = eeprom_read(storage_dev, mem_addr, data, num_bytes);
      if (rc) {
         LOG_DBG("Storage: reading %d@0x%lx failed, %d", num_bytes, mem_addr, rc);
      } else {
         LOG_DBG("Storage: read %d@0x%lx ", num_bytes, mem_addr);
      }
      return rc;
   } else {
      return -ENOTSUP;
   }
}

static int appl_storage_write_block(off_t mem_addr, const uint8_t *data, size_t num_bytes)
{
   int rc = eeprom_write(storage_dev, mem_addr, data, num_bytes);
   if (rc) {
      LOG_DBG("Storage: writing %d@0x%lx failed, %d", num_bytes, mem_addr, rc);
      k_sleep(K_MSEC(1000));
   } else {
      LOG_DBG("Storage: written %d@0x%lx ", num_bytes, mem_addr);
      k_sleep(K_MSEC(storage_write_timeout));
   }
   return rc;
}

int appl_storage_write_memory(off_t mem_addr, const uint8_t *data, size_t num_bytes)
{
   if (storage_init_state != STORAGE_INITIALIZE_ERROR) {
      int rc = 0;
      off_t start = mem_addr % storage_page_size;

      if (start) {
         /* first chunk */
         size_t head = storage_page_size - start;
         head = MIN(head, num_bytes);
         rc = appl_storage_write_block(mem_addr, data, head);
         num_bytes -= head;
         mem_addr += head;
         data += head;
      }
      while (num_bytes > storage_page_size && !rc) {
         /* full blocks */
         rc = appl_storage_write_block(mem_addr, data, storage_page_size);
         num_bytes -= storage_page_size;
         mem_addr += storage_page_size;
      }
      if (num_bytes) {
         /* last chunk */
         rc = appl_storage_write_block(mem_addr, data, num_bytes);
      }
      return rc;
   } else {
      return -ENOTSUP;
   }
}

int appl_storage_erase_memory(off_t mem_addr, size_t num_bytes)
{
   if (storage_init_state != STORAGE_INITIALIZE_ERROR) {
      int rc = 0;
      uint8_t data[storage_page_size];
      off_t start = mem_addr % storage_page_size;

      memset(data, 0xff, sizeof(data));

      if (start) {
         /* first chunk */
         size_t head = storage_page_size - start;
         head = MIN(head, num_bytes);
         rc = appl_storage_write_block(mem_addr, data, head);
         num_bytes -= head;
         mem_addr += head;
      }
      while (num_bytes > storage_page_size && !rc) {
         /* full blocks */
         rc = appl_storage_write_block(mem_addr, data, storage_page_size);
         num_bytes -= storage_page_size;
         mem_addr += storage_page_size;
      }
      if (num_bytes) {
         /* last chunk */
         rc = appl_storage_write_block(mem_addr, data, num_bytes);
      }
      return 0;
   } else {
      return -ENOTSUP;
   }
}

#define EEPROM_BLOCK_SIZE 128

static int appl_storage_get_page_info_by_offs(off_t mem_addr, struct flash_pages_info *info)
{
   if (storage_init_state != STORAGE_INITIALIZE_ERROR) {
      info->index = mem_addr / EEPROM_BLOCK_SIZE;
      info->size = EEPROM_BLOCK_SIZE;
      info->start_offset = info->index * EEPROM_BLOCK_SIZE;
      return 0;
   } else {
      return -ENOTSUP;
   }
}

#endif

static bool only_ff(const uint8_t *data, size_t len)
{
   for (int index = 0; index < len; ++index) {
      if (data[index] != 0xff) {
         return false;
      }
   }
   return true;
}

static void appl_storage_init_headers(struct storage_setup *config)
{
   appl_storage_write_memory(config->headers_offset, config->header, sizeof(config->header));
}

static void appl_storage_init_offset(struct storage_setup *config)
{
   uint8_t data[MAX_ITEM_SIZE];

   k_mutex_lock(&storage_mutex, K_FOREVER);
   if (!appl_storage_read_memory(config->headers_offset, data, sizeof(config->header))) {
      if (memcmp(data, config->header, sizeof(config->header)) != 0) {
         LOG_INF("Storage: format %s 0x%lx.", config->desc, config->headers_offset);
         LOG_HEXDUMP_DBG(data, sizeof(config->header), "Storage: header read");
         LOG_HEXDUMP_DBG(config->header, sizeof(config->header), "Storage: header expected");
         appl_storage_erase_memory(config->headers_offset, config->end_offset - config->headers_offset);
         appl_storage_init_headers(config);
         config->current_offset = appl_storage_start_offset(config);
         LOG_INF("Storage: format %s ready.", config->desc);
      } else {
         for (int addr = appl_storage_start_offset(config); addr < config->end_offset; addr += sizeof(data)) {
            if (!appl_storage_read_memory(addr, data, sizeof(data))) {
               for (int index = 0;
                    index < sizeof(data) && (addr + index) < config->end_offset;
                    index += config->item_size) {
                  if (only_ff(&data[index], TIME_SIZE)) {
                     config->current_offset = addr + index;
                     k_mutex_unlock(&storage_mutex);
                     return;
                  }
               }
            }
         }
      }
   }
   k_mutex_unlock(&storage_mutex);
}

static int appl_storage_init(const struct device *arg)
{
   const struct storage_config *configs = storage_configs;

   if (storage_init_state == STORAGE_NOT_INITIALIZED) {
      int rc = 0;
      size_t index = 0;
      off_t end = 0;
      uint8_t data[16];
      struct flash_pages_info info;

      LOG_INF("Storage init");
      if (storage_dev == NULL) {
         LOG_WRN("Storage: could not get I2C/SPI driver");
         goto init_error;
      }

      if (!device_is_ready(storage_dev)) {
         LOG_ERR("Storage: %s device is not ready", storage_dev->name);
         goto init_error;
      }

      while (index < storage_config_count && !rc) {
         rc = appl_storage_get_page_info_by_offs(end, &info);
         if (!rc) {
            struct storage_setup *setup = &storage_setups[index++];
            setup->desc = configs->desc;
            setup->value_size = configs->value_size;
            setup->item_size = (configs->value_size + TIME_SIZE);
            setup->headers_offset = end;
            setup->current_offset = appl_storage_start_offset(setup);
            setup->end_offset = setup->headers_offset + configs->pages * info.size;
            sys_put_be32(configs->magic, setup->header);
            sys_put_be32(configs->version, &(setup->header[4]));
            end = setup->end_offset;

            LOG_INF("%s: size 0x%x, off 0x%lx, index 0x%x", setup->desc, info.size, info.start_offset, info.index);
            LOG_INF("%s: 0x%lx-0x%lx", setup->desc, setup->headers_offset, setup->end_offset);
            ++configs;
         }
      }
      if (rc) {
         goto init_error;
      }
#if defined(CONFIG_FLASH)
      storage_size = flash_get_page_count(storage_dev) * info.size;
#else
      storage_size = DT_PROP(DT_STORAGE_DEV, size);
#endif
      storage_init_state = STORAGE_INITIALIZED;
      index = 0;
      while (index < storage_config_count) {
         appl_storage_init_offset(&storage_setups[index++]);
      }
      if (rc) {
         goto init_error;
      }
      storage_setups_count = storage_config_count;
      for (off_t addr = 0x0; addr < end; addr += sizeof(data)) {
         rc = appl_storage_read_memory(addr, data, sizeof(data));
         if (rc) {
            goto init_error;
         } else if (!only_ff(data, sizeof(data))) {
            char label[24];
            snprintf(label, sizeof(label), "Storage: @0x%lx", addr);
            LOG_HEXDUMP_INF(data, sizeof(data), label);
         }
      }
   }
   if (storage_init_state == STORAGE_INITIALIZED) {
      return 0;
   }
init_error:
   LOG_ERR("Storage: init failed");
   storage_init_state = STORAGE_INITIALIZE_ERROR;
   return -ENOTSUP;
}

SYS_INIT(appl_storage_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static int appl_storage_write_item(struct storage_setup *config, int64_t time, const uint8_t *value, size_t value_size)
{
   int rc;
   int next;
   int data_size = config->item_size + 1;
   uint8_t data[MAX_ITEM_SIZE + 1];

   memset(data, 0, sizeof(data));
   k_mutex_lock(&storage_mutex, K_FOREVER);
   if (config->current_offset + data_size <= config->end_offset) {
      rc = appl_storage_read_memory(config->current_offset, data, data_size);
      next = config->current_offset + config->item_size;
   } else {
      rc = appl_storage_read_memory(config->current_offset, data, config->item_size);
      next = appl_storage_start_offset(config);
      rc = rc || appl_storage_read_memory(next, &data[config->item_size], 1);
   }
   if (!rc) {
      if (data[config->item_size] != 0xff) {
#ifdef CONFIG_FLASH
         struct flash_pages_info info;
         rc = appl_storage_get_page_info_by_offs(next, &info);
         rc = rc || appl_storage_erase_memory(info.start_offset, info.size);
         if (!rc && info.start_offset == config->headers_offset) {
            appl_storage_init_headers(config);
         }
#else
         rc = appl_storage_erase_memory(next, 1);
#endif
      }
      time = (time / MSEC_PER_SEC) & 0x7fffffffffffL;
      sys_put_be48(time, data);
      value_size = MIN(value_size, config->value_size);
      memmove(&data[TIME_SIZE], value, value_size);
      rc = rc || appl_storage_write_memory(config->current_offset, data, config->item_size);
      if (!rc) {
         config->current_offset = next;
      }
   }
   k_mutex_unlock(&storage_mutex);

   return rc;
}

static int appl_storage_read_item(struct storage_setup *config, off_t *current, int64_t *times, uint8_t *value, size_t value_size)
{
   int rc = 0;
   off_t offset;
   uint8_t data[MAX_ITEM_SIZE];

   if (current) {
      offset = *current;
   } else {
      k_mutex_lock(&storage_mutex, K_FOREVER);
      offset = config->current_offset;
      k_mutex_unlock(&storage_mutex);
   }

   if (offset == appl_storage_start_offset(config)) {
      offset = config->end_offset;
   }
   offset -= config->item_size;
   rc = appl_storage_read_memory(offset, data, config->item_size);
   if (!rc) {
      if (only_ff(data, TIME_SIZE)) {
         rc = 0;
      } else {
         rc = config->value_size;
         if (times) {
            *times = sys_get_be48(data) * MSEC_PER_SEC;
         }
         if (value) {
            rc = MIN(value_size, rc);
            memmove(value, &data[TIME_SIZE], rc);
         }
      }
   }
   if (current) {
      *current = offset;
   }
   return rc;
}

int appl_storage_write_int_item(size_t id, uint16_t code)
{
   uint8_t data[sizeof(code)];

   sys_put_be16(code, data);
   return appl_storage_write_bytes_item(id, data, sizeof(data));
}

int appl_storage_read_int_items(size_t id, size_t index, int64_t *times, uint16_t *codes, size_t count)
{
   int rc = -ENOTSUP;
   if (storage_init_state == STORAGE_INITIALIZED) {
      if (id < storage_setups_count) {
         size_t read = 0;
         off_t current;
         uint8_t data[sizeof(uint16_t)];
         struct storage_setup *setup = &storage_setups[id];

         k_mutex_lock(&storage_mutex, K_FOREVER);
         current = setup->current_offset;
         k_mutex_unlock(&storage_mutex);
         while (read < count) {
            LOG_INF("Read %s %d/%d", setup->desc, read, count);
            rc = appl_storage_read_item(setup, &current, times, data, sizeof(data));
            if (rc <= 0) {
               break;
            }
            if (index > 0) {
               --index;
            } else {
               if (times) {
                  ++times;
               }
               if (codes) {
                  *codes = sys_get_be16(data);
                  ++codes;
               }
               ++read;
            }
         }
         if (read) {
            return read;
         }
      } else {
         return -EINVAL;
      }
   }
   return rc;
}

int appl_storage_write_bytes_item(size_t id, uint8_t *data, size_t data_size)
{
   if (storage_init_state == STORAGE_INITIALIZED) {
      if (id < storage_setups_count) {
         int64_t now = 0;
         appl_get_now(&now);
         return appl_storage_write_item(&storage_setups[id], now, data, data_size);
      } else {
         return -EINVAL;
      }
   } else {
      return -ENOTSUP;
   }
}

int appl_storage_read_bytes_item(size_t id, size_t index, int64_t *time, uint8_t *data, size_t data_size)
{
   int rc = -ENOTSUP;
   if (storage_init_state == STORAGE_INITIALIZED) {
      if (id < storage_setups_count) {
         off_t current;
         struct storage_setup *setup = &storage_setups[id];

         k_mutex_lock(&storage_mutex, K_FOREVER);
         current = setup->current_offset;
         k_mutex_unlock(&storage_mutex);
         rc = 1;
         while (index--) {
            rc = appl_storage_read_item(setup, &current, NULL, NULL, 0);
            if (rc <= 0) {
               break;
            }
         }
         if (rc > 0) {
            rc = appl_storage_read_item(setup, &current, time, data, data_size);
            LOG_INF("Read %s: %d", setup->desc, rc);
         }
      } else {
         return -EINVAL;
      }
   }
   return rc;
}

#else /* CONFIG_FLASH */

int appl_storage_read_memory(off_t mem_addr, uint8_t *data, size_t num_bytes)
{
   (void)mem_addr;
   (void)data;
   (void)num_bytes;
   return -ENOTSUP;
}

int appl_storage_write_memory(off_t mem_addr, const uint8_t *data, size_t num_bytes)
{
   (void)mem_addr;
   (void)data;
   (void)num_bytes;
   return -ENOTSUP;
}

int appl_storage_erase_memory(off_t mem_addr, size_t num_bytes)
{
   (void)mem_addr;
   (void)num_bytes;
   return -ENOTSUP;
}

int appl_storage_write_int_item(size_t id, uint16_t code)
{
   (void)code;
   return -ENOTSUP;
}

int appl_storage_read_int_items(size_t id, size_t index, int64_t *times, uint16_t *codes, size_t count)
{
   (void)times;
   (void)codes;
   (void)count;
   return -ENOTSUP;
}

int appl_storage_write_bytes_item(size_t id, uint8_t *data, size_t data_size)
{
   (void)data;
   (void)data_size;
   return -ENOTSUP;
}

int appl_storage_read_bytes_item(size_t id, size_t index, int64_t *time, uint8_t *data, size_t data_size)
{
   (void)time;
   (void)data;
   (void)data_size;
   return -ENOTSUP;
}

#endif /* CONFIG_FLASH || CONFIG_EEPROM */