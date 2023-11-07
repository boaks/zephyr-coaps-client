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
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#ifdef CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION
#define APP_VERSION_STRING CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION
#else /* No McuBoot */
/* auto generated header file during west build */
#include "app_version.h"
#endif

#include "appl_diagnose.h"
#include "appl_storage.h"
#include "appl_storage_config.h"
#include "ui.h"
#include "uart_cmd.h"

#define MSEC_PER_HOUR (MSEC_PER_SEC * 60 * 60)

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

#define CONFIG_DIAGNOSE_STACK_SIZE 2048

static K_THREAD_STACK_DEFINE(appl_diagnose_stack, CONFIG_DIAGNOSE_STACK_SIZE);
static struct k_thread appl_diagnose_thread;
static K_SEM_DEFINE(appl_diagnose_shutdown, 0, 1);

static volatile bool reboots = false;
static volatile bool shutdown_now = false;

static atomic_t shutdown_delay = ATOMIC_INIT(-1);
static atomic_t reboot_cause = ATOMIC_INIT(-1);

static atomic_t read_reset_cause = ATOMIC_INIT(0);

static const struct device *const wdt = DEVICE_DT_GET_OR_NULL(DT_ALIAS(watchdog0));
static int wdt_channel_id = -1;

static volatile uint32_t reset_cause = 0;

static char appl_version[16] = {'v', 0};

const char *appl_get_version(void)
{
   return appl_version;
}

void watchdog_feed(void)
{
   if (wdt && wdt_channel_id >= 0) {
      wdt_feed(wdt, wdt_channel_id);
   }
}

static void appl_reboot_fn(void *p1, void *p2, void *p3)
{
   (void) p1;
   (void) p2;
   (void) p3;

   int error = 0;
   k_sem_take(&appl_diagnose_shutdown, K_FOREVER);

   error = atomic_get(&reboot_cause);
   if (error >= 0) {
      appl_storage_write_int_item(REBOOT_CODE_ID, (uint16_t)error);
   }

   if (!shutdown_now) {
      uint32_t delay_ms = atomic_get(&shutdown_delay);
      while (delay_ms > 0) {
         if (k_sem_take(&appl_diagnose_shutdown, K_MSEC(delay_ms)) == -EAGAIN) {
            break;
         }
         if (shutdown_now) {
            break;
         }
         delay_ms = atomic_get(&shutdown_delay);
      }
   }
   sys_reboot(SYS_REBOOT_COLD);
}

void appl_reboot(int error, const k_timeout_t delay)
{
   reboots = true;
   uint32_t delay_ms = (uint32_t)k_ticks_to_ms_floor64(delay.ticks);
   atomic_set(&reboot_cause, error);
   if (delay_ms > 0) {
      atomic_set(&shutdown_delay, delay_ms);
   } else {
      shutdown_now = true;
   }
   k_sem_give(&appl_diagnose_shutdown);
}

bool appl_reboots(void)
{
   return reboots;
}

const char *appl_get_reboot_desciption(int error)
{
   switch (ERROR_CLASS(error)) {
      case ERROR_CODE_INIT_NO_LTE:
         return "no initial network";
      case ERROR_CODE_INIT_NO_DTLS:
         return "no dtls";
      case ERROR_CODE_INIT_NO_SUCCESS:
         return "no initial success";
      case ERROR_CODE_OPEN_SOCKET:
         return "open socket";
      case ERROR_CODE_TOO_MANY_FAILURES:
         return "too many failures";
      case ERROR_CODE_MODEM_FAULT:
         return "modem fault";
      case ERROR_CODE_CMD:
         return "reboot cmd";
      case ERROR_CODE_MANUAL_TRIGGERED:
         return "reboot triggered";
      case ERROR_CODE_UPDATE:
         return "update";
   }
   return "\?\?\?";
}

uint32_t appl_reset_cause(int *flags)
{
   uint32_t cause = 0;
   if (atomic_cas(&read_reset_cause, 0, 1) && !hwinfo_get_reset_cause(&cause)) {
      hwinfo_clear_reset_cause();
      reset_cause = cause;
   }
   if (reset_cause) {
      LOG_INF("Reset cause %x", reset_cause);
      if (reset_cause & RESET_PIN) {
         LOG_INF("PIN");
         if (flags) {
            *flags |= FLAG_RESET;
         }
      }
      if (reset_cause & RESET_SOFTWARE) {
         uint16_t reboot_code = 0;
         int rc = appl_storage_read_int_items(REBOOT_CODE_ID, 0, NULL, &reboot_code, 1);
         if (!rc && reboot_code == ERROR_CODE_TOO_MANY_FAILURES) {
            LOG_INF("Reboot 1.");
            if (flags) {
               *flags |= FLAG_REBOOT_1;
            }
         } else {
            LOG_INF("Reboot");
            if (flags) {
               *flags |= FLAG_REBOOT;
            }
         }
      }
      if (reset_cause & RESET_POR) {
         LOG_INF("POR");
      }
      if (reset_cause & RESET_WATCHDOG) {
         LOG_INF("WATCHDOG");
      }
   } else {
      LOG_INF("No reset cause available.");
   }
   return reset_cause;
}

int appl_reset_cause_description(char *buf, size_t len)
{
   int index = 0;

   if (atomic_get(&read_reset_cause) && len > 8) {
      uint32_t cause = reset_cause;
      if (cause) {
         if (cause & RESET_PIN) {
            cause &= ~RESET_PIN;
            index += snprintf(buf + index, len - index, "Reset, ");
         }
         if (cause & RESET_SOFTWARE && index < len) {
            cause &= ~RESET_SOFTWARE;
            index += snprintf(buf + index, len - index, "Reboot, ");
         }
         if (cause & RESET_POR && index < len) {
            cause &= ~RESET_POR;
            index += snprintf(buf + index, len - index, "Power On, ");
         }
         if (cause & RESET_WATCHDOG && index < len) {
            cause &= ~RESET_WATCHDOG;
            index += snprintf(buf + index, len - index, "Watchdog, ");
         }
         if (((index - 2) > len) || (cause && (index + 8 > len))) {
            // buffer overflow, reset output
            cause = reset_cause;
            index = 0;
         }
         if (cause) {
            index += snprintf(buf + index, len - index, " 0x%04x", reset_cause);
         } else if (index > 2) {
            index -= 2;
            buf[index] = 0;
         }
      }
   }
   return index;
}

static int at_cmd_reboot(const char *parameter)
{
   (void)parameter;
   if (appl_reboots()) {
      LOG_INF(">> device already reboots!");
   } else {
      LOG_INF(">> device reboot ...");
      appl_reboot(ERROR_CODE_CMD, K_MSEC(2000));
   }
   return 0;
}

UART_CMD(reboot, NULL, "reboot device.", at_cmd_reboot, NULL, 0);

static int appl_watchdog_init(void)
{
   int err;
   struct wdt_timeout_cfg wdt_config = {
       /* Reset SoC when watchdog timer expires. */
       .flags = WDT_FLAG_RESET_SOC,

       .window.min = 0,
       /* Expire watchdog after max. window, +10s extra */
       .window.max = (WATCHDOG_TIMEOUT_S + 10) * MSEC_PER_SEC,
   };

   if (!wdt) {
      LOG_INF("No watchdog device available.\n");
      return -ENOTSUP;
   }

   if (!device_is_ready(wdt)) {
      LOG_INF("%s: device not ready.\n", wdt->name);
      return -ENOTSUP;
   }

   err = wdt_install_timeout(wdt, &wdt_config);
   if (err < 0) {
      LOG_INF("Watchdog install error %d, %s\n", err, strerror(errno));
      return err;
   }
   wdt_channel_id = err;

   err = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
   if (err < 0) {
      LOG_INF("Watchdog setup error %d, %s\n", err, strerror(errno));
      return err;
   }
   watchdog_feed();
   LOG_INF("Watchdog initialized\n");
   return 0;
}

static int appl_diagnose_init(void)
{
   const char *mcu_appl_version = APP_VERSION_STRING;

   appl_watchdog_init();

   k_thread_create(&appl_diagnose_thread,
                   appl_diagnose_stack,
                   CONFIG_DIAGNOSE_STACK_SIZE,
                   appl_reboot_fn, NULL, NULL, NULL,
                   K_HIGHEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);

   memset(appl_version, 0, sizeof(appl_version));
   appl_version[0] = 'v';
   for (int i = 0; i < sizeof(appl_version) - 2; ++i) {
      char c = mcu_appl_version[i];
      if (c == 0 /* || c == '+' */) {
         break;
      }
      appl_version[i + 1] = c;
   }

   return 0;
}

SYS_INIT(appl_diagnose_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
