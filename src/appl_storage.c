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
#include "appl_storage_config.h"
#include "appl_time.h"

#ifndef CONFIG_EEPROM
#undef CONFIG_EEPROM_APPL_STORAGE
#endif

#ifndef CONFIG_FLASH
#undef CONFIG_FLASH_APPL_STORAGE
#endif

#if defined(CONFIG_FLASH_APPL_STORAGE) || defined(CONFIG_EEPROM_APPL_STORAGE)

LOG_MODULE_REGISTER(STORAGE, CONFIG_STORAGE_LOG_LEVEL);

#define MAX_STORAGE_SETUPS 6

#define TIME_SIZE 6

#define MAX_ITEM_SIZE (TIME_SIZE + MAX_VALUE_SIZE)

enum storage_init_state {
   STORAGE_NOT_INITIALIZED,
   STORAGE_INITIALIZED,
   STORAGE_INITIALIZE_ERROR,
};

struct storage_setup {
   const struct storage_config *config;
   uint8_t header[8];
   enum storage_init_state init_state;
   size_t item_size;
   off_t headers_offset;
   off_t current_offset;
   off_t end_offset;
};

static size_t storage_setups_count = 0;
static struct storage_setup storage_setups[MAX_STORAGE_SETUPS];

static K_MUTEX_DEFINE(storage_mutex);

static inline off_t appl_storage_start_offset(const struct storage_setup *config)
{
   return config->headers_offset + config->item_size;
}

static struct storage_setup *appl_storage_setup(int id)
{
   if (id) {
      size_t cid = id < 0 ? -id : id;
      for (int index = 0; index < storage_setups_count; ++index) {
         if (storage_setups[index].config->id == cid) {
            if (id < 0 || storage_setups[index].init_state == STORAGE_INITIALIZED) {
               return &storage_setups[index];
            }
         }
      }
   }
   return NULL;
}

#if defined(CONFIG_EEPROM_APPL_STORAGE)

#define EEPROM_BLOCK_SIZE 128

static int appl_eeprom_storage_read_memory(const struct device *storage_device, off_t mem_addr, uint8_t *data, size_t num_bytes)
{
   int rc = eeprom_read(storage_device, mem_addr, data, num_bytes);
   if (rc) {
      LOG_DBG("Storage: reading %d@0x%lx failed, %d", num_bytes, mem_addr, rc);
   } else {
      LOG_DBG("Storage: read %d@0x%lx ", num_bytes, mem_addr);
   }
   return rc;
}

static int appl_eeprom_storage_write_memory(const struct device *storage_device, off_t mem_addr, const uint8_t *data, size_t num_bytes)
{
   int rc = eeprom_write(storage_device, mem_addr, data, num_bytes);
   if (rc) {
      LOG_DBG("Storage: writing %d@0x%lx failed, %d", num_bytes, mem_addr, rc);
      k_sleep(K_MSEC(1000));
   } else {
      LOG_DBG("Storage: written %d@0x%lx ", num_bytes, mem_addr);
   }
   return rc;
}

static int appl_eeprom_storage_erase_memory(const struct device *storage_device, off_t mem_addr, size_t num_bytes)
{
   int rc = 0;
   uint8_t data[EEPROM_BLOCK_SIZE];

   memset(data, 0xff, sizeof(data));
   while (num_bytes > 0 && !rc) {
      /* full blocks */
      size_t block_bytes = MIN(num_bytes, sizeof(data));
      rc = eeprom_write(storage_device, mem_addr, data, block_bytes);
      num_bytes -= block_bytes;
      mem_addr += block_bytes;
   }
   return rc;
}

static int appl_eeprom_storage_get_page_info_by_offs(off_t mem_addr, struct flash_pages_info *info)
{
   info->index = mem_addr / EEPROM_BLOCK_SIZE;
   info->size = EEPROM_BLOCK_SIZE;
   info->start_offset = info->index * EEPROM_BLOCK_SIZE;
   return 0;
}
#endif

#if defined(CONFIG_FLASH_APPL_STORAGE)

static int appl_flash_storage_read_memory(const struct device *storage_device, off_t mem_addr, uint8_t *data, size_t num_bytes)
{
   int rc = flash_read(storage_device, mem_addr, data, num_bytes);
   if (rc) {
      LOG_DBG("Storage: reading %d@0x%lx failed, %d", num_bytes, mem_addr, rc);
   } else {
      LOG_DBG("Storage: read %d@0x%lx ", num_bytes, mem_addr);
   }
   return rc;
}

static int appl_flash_storage_write_memory(const struct device *storage_device, off_t mem_addr, const uint8_t *data, size_t num_bytes)
{
   int rc = flash_write(storage_device, mem_addr, data, num_bytes);
   if (rc) {
      LOG_DBG("Storage: writing %d@0x%lx failed, %d", num_bytes, mem_addr, rc);
      k_sleep(K_MSEC(1000));
   } else {
      LOG_DBG("Storage: written %d@0x%lx ", num_bytes, mem_addr);
   }
   return rc;
}

static int appl_flash_storage_erase_memory(const struct device *storage_device, off_t mem_addr, size_t num_bytes)
{
   int rc = flash_erase(storage_device, mem_addr, num_bytes);
   if (rc) {
      LOG_DBG("Storage: erasing %d@0x%lx failed, %d", num_bytes, mem_addr, rc);
   } else {
      LOG_DBG("Storage: erased %d@0x%lx ", num_bytes, mem_addr);
   }
   return rc;
}

static int appl_flash_storage_get_page_info_by_offs(const struct device *storage_device, off_t mem_addr, struct flash_pages_info *info)
{
   return flash_get_page_info_by_offs(storage_device, mem_addr, info);
}
#endif

int appl_storage_read_memory(const struct storage_config *cfg, off_t mem_addr, uint8_t *data, size_t num_bytes)
{
   if (device_is_ready(cfg->storage_device)) {
      if (cfg->is_flash_device) {
#if defined(CONFIG_FLASH_APPL_STORAGE)
         return appl_flash_storage_read_memory(cfg->storage_device, mem_addr, data, num_bytes);
#endif
      } else {
#if defined(CONFIG_EEPROM_APPL_STORAGE)
         return appl_eeprom_storage_read_memory(cfg->storage_device, mem_addr, data, num_bytes);
#endif
      }
   }
   return -ENOTSUP;
}

int appl_storage_write_memory(const struct storage_config *cfg, off_t mem_addr, const uint8_t *data, size_t num_bytes)
{
   if (device_is_ready(cfg->storage_device)) {
      if (cfg->is_flash_device) {
#if defined(CONFIG_FLASH_APPL_STORAGE)
         return appl_flash_storage_write_memory(cfg->storage_device, mem_addr, data, num_bytes);
#endif
      } else {
#if defined(CONFIG_EEPROM_APPL_STORAGE)
         return appl_eeprom_storage_write_memory(cfg->storage_device, mem_addr, data, num_bytes);
#endif
      }
   }
   return -ENOTSUP;
}

int appl_storage_erase_memory(const struct storage_config *cfg, off_t mem_addr, size_t num_bytes)
{
   if (device_is_ready(cfg->storage_device)) {
      if (cfg->is_flash_device) {
#if defined(CONFIG_FLASH_APPL_STORAGE)
         return appl_flash_storage_erase_memory(cfg->storage_device, mem_addr, num_bytes);
#endif
      } else {
#if defined(CONFIG_EEPROM_APPL_STORAGE)
         return appl_eeprom_storage_erase_memory(cfg->storage_device, mem_addr, num_bytes);
#endif
      }
   }
   return -ENOTSUP;
}

static int appl_storage_get_page_info_by_offs(const struct storage_config *cfg, off_t mem_addr, struct flash_pages_info *info)
{
   if (device_is_ready(cfg->storage_device)) {
      if (cfg->is_flash_device) {
#if defined(CONFIG_FLASH_APPL_STORAGE)
         return appl_flash_storage_get_page_info_by_offs(cfg->storage_device, mem_addr, info);
#endif
      } else {
#if defined(CONFIG_EEPROM_APPL_STORAGE)
         return appl_eeprom_storage_get_page_info_by_offs(mem_addr, info);
#endif
      }
   }
   return -ENOTSUP;
}

static bool only_ff(const uint8_t *data, size_t len)
{
   for (int index = 0; index < len; ++index) {
      if (data[index] != 0xff) {
         return false;
      }
   }
   return true;
}

static void appl_storage_init_headers(const struct storage_setup *setup)
{
   appl_storage_write_memory(setup->config, setup->headers_offset, setup->header, sizeof(setup->header));
}

static int appl_storage_init_offset(struct storage_setup *setup)
{
   int rc = 0;
   uint8_t data[MAX_ITEM_SIZE];

   k_mutex_lock(&storage_mutex, K_FOREVER);
   if (!(rc = appl_storage_read_memory(setup->config, setup->headers_offset, data, sizeof(setup->header)))) {
      if (memcmp(data, setup->header, sizeof(setup->header)) != 0) {
         LOG_INF("Storage: format %s 0x%lx.", setup->config->desc, setup->headers_offset);
         LOG_HEXDUMP_DBG(data, sizeof(setup->header), "Storage: header read");
         LOG_HEXDUMP_DBG(setup->header, sizeof(setup->header), "Storage: header expected");
         appl_storage_erase_memory(setup->config, setup->headers_offset, setup->end_offset - setup->headers_offset);
         appl_storage_init_headers(setup);
         setup->current_offset = appl_storage_start_offset(setup);
         LOG_INF("Storage: format %s ready.", setup->config->desc);
      } else {
         for (int addr = appl_storage_start_offset(setup); addr < setup->end_offset; addr += sizeof(data)) {
            if (!(rc = appl_storage_read_memory(setup->config, addr, data, sizeof(data)))) {
               for (int index = 0;
                    index < sizeof(data) && (addr + index) < setup->end_offset;
                    index += setup->item_size) {
                  if (only_ff(&data[index], TIME_SIZE)) {
                     setup->current_offset = addr + index;
                     k_mutex_unlock(&storage_mutex);
                     return rc;
                  }
               }
            }
         }
      }
   }
   k_mutex_unlock(&storage_mutex);
   return rc;
}

int appl_storage_add(const struct storage_config *config)
{
   int rc = 0;
   size_t index_setup = 0;
   uint8_t data[16];
   struct flash_pages_info info;
   const struct device *dev = config->storage_device;
   const char *dev_type = "\?\?\?";
   struct storage_setup *setup = appl_storage_setup(-config->id);

   if (!setup) {
      LOG_INF("Storage add");
      index_setup = storage_setups_count;
      setup = &storage_setups[index_setup];
   } else {
      for (; index_setup < storage_setups_count; ++index_setup) {
         if (&storage_setups[index_setup] == setup) {
            break;
         }
      }
   }

   if (config->is_flash_device) {
#if defined(CONFIG_FLASH_APPL_STORAGE)
      dev_type = "SPI flash";
#else
      LOG_WRN("Storage-%d: flash not supported!", index_setup);
      return -EINVAL;
#endif
   } else {
#if defined(CONFIG_EEPROM_APPL_STORAGE)
      dev_type = "I2C EEPROM";
#else
      LOG_WRN("Storage-%d: EEPROM not supported!", index_setup);
      return -EINVAL;
#endif
   }
   if (dev == NULL) {
      LOG_WRN("Storage-%d: could not get %s driver", index_setup, dev_type);
      return -EINVAL;
   } else if (!device_is_ready(dev)) {
      LOG_WRN("Storage-%d: %s device is not ready", index_setup, dev->name);
      return -EINVAL;
   } else {
      rc = appl_storage_read_memory(config, 0, data, 1);
      if (rc) {
         LOG_WRN("Storage-%d: %s read failed, %d", index_setup, dev->name, rc);
         return -rc;
      }
   }

   rc = appl_storage_get_page_info_by_offs(config, 0, &info);
   if (rc) {
      LOG_WRN("Storage-%d: %s could not get page info, %d", index_setup, dev->name, rc);
      return rc;
   }
   setup->config = config;
   setup->item_size = (config->value_size + TIME_SIZE);
   setup->headers_offset = 0;
   setup->current_offset = appl_storage_start_offset(setup);
   setup->end_offset = setup->headers_offset + config->pages * info.size;
   sys_put_be32(config->magic, setup->header);
   sys_put_be32(config->version, &(setup->header[4]));

   LOG_INF("%s/%s-%d: page-size 0x%x, off 0x%lx, index 0x%x", config->desc, config->storage_device->name, index_setup,
           info.size, info.start_offset, info.index);
   LOG_INF("%s/%s-%d: 0x%lx-0x%lx", config->desc, config->storage_device->name, index_setup,
           setup->headers_offset, setup->end_offset);

   rc = appl_storage_init_offset(setup);
   if (rc) {
      setup->init_state = STORAGE_INITIALIZE_ERROR;
      return rc;
   }
   setup->init_state = STORAGE_INITIALIZED;

   for (off_t addr = setup->headers_offset; addr < setup->end_offset; addr += sizeof(data)) {
      rc = appl_storage_read_memory(setup->config, addr, data, sizeof(data));
      if (!rc && !only_ff(data, sizeof(data))) {
         char label[24];
         snprintf(label, sizeof(label), "Storage: @0x%lx", addr);
         LOG_HEXDUMP_DBG(data, sizeof(data), label);
      }
   }
   if (storage_setups_count == index_setup) {
      storage_setups_count++;
   }
   return 0;
}

static int appl_storage_init(void)
{
   int rc = 0;
   size_t index_config = 0;
   size_t index_setup = storage_setups_count;
   off_t end = 0;
   uint8_t data[16];
   struct flash_pages_info info;
   const struct storage_config *config = storage_configs;
   const struct device *dev = NULL;
   const char *dev_type = "\?\?\?";
   bool ok = false;

   LOG_INF("Storage init");

   while (index_config < storage_config_count) {
      if (dev != config->storage_device) {
         ok = true;
         end = 0;
         dev = config->storage_device;
         if (config->is_flash_device) {
#if defined(CONFIG_FLASH_APPL_STORAGE)
            dev_type = "SPI flash";
#else
            LOG_WRN("Storage-%d: flash not supported!", index_config);
            ok = false;
#endif
         } else {
#if defined(CONFIG_EEPROM_APPL_STORAGE)
            dev_type = "I2C EEPROM";
#else
            LOG_WRN("Storage-%d: EEPROM not supported!", index_config);
            ok = false;
#endif
         }
         if (ok) {
            if (dev == NULL) {
               LOG_WRN("Storage-%d: could not get %s driver", index_config, dev_type);
               ok = false;
            } else if (!device_is_ready(dev)) {
               LOG_WRN("Storage-%d: %s device is not ready", index_config, dev->name);
               ok = false;
            } else {
               rc = appl_storage_read_memory(config, 0, data, 1);
               if (rc) {
                  LOG_WRN("Storage-%d: %s read failed, %d", index_config, dev->name, rc);
                  ok = false;
               }
            }
         }
      }
      if (ok) {
         rc = appl_storage_get_page_info_by_offs(config, end, &info);
         if (!rc) {
            struct storage_setup *setup = &storage_setups[index_setup++];
            setup->config = config;
            setup->item_size = (config->value_size + TIME_SIZE);
            setup->headers_offset = end;
            setup->current_offset = appl_storage_start_offset(setup);
            setup->end_offset = setup->headers_offset + config->pages * info.size;
            sys_put_be32(config->magic, setup->header);
            sys_put_be32(config->version, &(setup->header[4]));
            end = setup->end_offset;

            LOG_INF("%s/%s-%d: page-size 0x%x, off 0x%lx, index 0x%x", config->desc, config->storage_device->name, index_config,
                    info.size, info.start_offset, info.index);
            LOG_INF("%s/%s-%d: 0x%lx-0x%lx", config->desc, config->storage_device->name, index_config,
                    setup->headers_offset, setup->end_offset);
         } else {
            LOG_WRN("Storage-%d: %s could not get page info, %d", index_config, dev->name, rc);
         }
      }
      ++index_config;
      ++config;
   }

   index_config = storage_setups_count;
   storage_setups_count = index_setup;
   index_setup = index_config;

   while (index_setup < storage_setups_count) {
      if (appl_storage_init_offset(&storage_setups[index_setup])) {
         storage_setups[index_setup].init_state = STORAGE_INITIALIZE_ERROR;
      } else {
         storage_setups[index_setup].init_state = STORAGE_INITIALIZED;
      }
      ++index_setup;
   }
   index_setup = index_config;
   while (index_setup < storage_setups_count && !rc) {
      struct storage_setup *setup = &storage_setups[index_setup++];
      if (setup->init_state == STORAGE_INITIALIZED) {
         for (off_t addr = setup->headers_offset; addr < setup->end_offset; addr += sizeof(data)) {
            rc = appl_storage_read_memory(setup->config, addr, data, sizeof(data));
            if (!rc && !only_ff(data, sizeof(data))) {
               char label[24];
               snprintf(label, sizeof(label), "Storage: @0x%lx", addr);
               LOG_HEXDUMP_DBG(data, sizeof(data), label);
            }
         }
      }
   }

   return 0;
}

SYS_INIT(appl_storage_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static int appl_storage_write_item(struct storage_setup *setup, int64_t time, const uint8_t *value, size_t value_size)
{
   int rc;
   int next;
   int data_size = setup->item_size + 1;
   uint8_t data[MAX_ITEM_SIZE + 1];

   memset(data, 0, sizeof(data));
   k_mutex_lock(&storage_mutex, K_FOREVER);
   if (setup->current_offset + data_size <= setup->end_offset) {
      rc = appl_storage_read_memory(setup->config, setup->current_offset, data, data_size);
      next = setup->current_offset + setup->item_size;
   } else {
      rc = appl_storage_read_memory(setup->config, setup->current_offset, data, setup->item_size);
      next = appl_storage_start_offset(setup);
      rc = rc || appl_storage_read_memory(setup->config, next, &data[setup->item_size], 1);
   }
   if (!rc) {
      if (data[setup->item_size] != 0xff) {
         if (setup->config->is_flash_device) {
            struct flash_pages_info info;
            rc = appl_storage_get_page_info_by_offs(setup->config, next, &info);
            rc = rc || appl_storage_erase_memory(setup->config, info.start_offset, info.size);
            if (!rc && info.start_offset == setup->headers_offset) {
               appl_storage_init_headers(setup);
            }
         } else {
            rc = appl_storage_erase_memory(setup->config, next, 1);
         }
      }
      time = (time / MSEC_PER_SEC) & 0x7fffffffffffL;
      sys_put_be48(time, data);
      value_size = MIN(value_size, setup->config->value_size);
      memmove(&data[TIME_SIZE], value, value_size);
      rc = rc || appl_storage_write_memory(setup->config, setup->current_offset, data, setup->item_size);
      if (!rc) {
         setup->current_offset = next;
      }
   }
   k_mutex_unlock(&storage_mutex);

   return rc;
}

static int appl_storage_read_item(const struct storage_setup *setup, off_t *current, int64_t *times, uint8_t *value, size_t value_size)
{
   int rc = 0;
   off_t offset;
   uint8_t data[MAX_ITEM_SIZE];

   if (current) {
      offset = *current;
   } else {
      k_mutex_lock(&storage_mutex, K_FOREVER);
      offset = setup->current_offset;
      k_mutex_unlock(&storage_mutex);
   }

   if (offset == appl_storage_start_offset(setup)) {
      offset = setup->end_offset;
   }
   offset -= setup->item_size;
   rc = appl_storage_read_memory(setup->config, offset, data, setup->item_size);
   if (!rc) {
      if (only_ff(data, TIME_SIZE)) {
         rc = 0;
      } else {
         rc = setup->config->value_size;
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
   int rc = -EINVAL;
   const struct storage_setup *setup = appl_storage_setup(id);
   if (setup) {
      size_t read = 0;
      off_t current;
      uint8_t data[sizeof(uint16_t)];

      k_mutex_lock(&storage_mutex, K_FOREVER);
      current = setup->current_offset;
      k_mutex_unlock(&storage_mutex);
      while (read < count) {
         LOG_DBG("Read %s %d/%d", setup->config->desc, read, count);
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
   }
   return rc;
}

int appl_storage_write_bytes_item(size_t id, uint8_t *data, size_t data_size)
{
   struct storage_setup *setup = appl_storage_setup(id);
   if (setup) {
      int64_t now = 0;
      appl_get_now(&now);
      return appl_storage_write_item(setup, now, data, data_size);
   } else {
      return -EINVAL;
   }
}

int appl_storage_read_bytes_item(size_t id, size_t index, int64_t *time, uint8_t *data, size_t data_size)
{
   int rc = -EINVAL;
   const struct storage_setup *setup = appl_storage_setup(id);
   if (setup) {
      off_t current;

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
         LOG_INF("Read %s: %d", setup->config->desc, rc);
      }
   }
   return rc;
}

#else /* defined(STORAGE_DEV_FLASH) || defined(STORAGE_DEV_EEPROM) */

int appl_storage_read_memory(const struct storage_config *cfg, off_t mem_addr, uint8_t *data, size_t num_bytes)
{
   (void)mem_addr;
   (void)data;
   (void)num_bytes;
   return -ENOTSUP;
}

int appl_storage_write_memory(const struct storage_config *cfg, off_t mem_addr, const uint8_t *data, size_t num_bytes)
{
   (void)mem_addr;
   (void)data;
   (void)num_bytes;
   return -ENOTSUP;
}

int appl_storage_erase_memory(const struct storage_config *cfg, off_t mem_addr, size_t num_bytes)
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

#endif /* defined(STORAGE_DEV_FLASH) || defined(STORAGE_DEV_EEPROM) */