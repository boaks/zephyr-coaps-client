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
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/reboot.h>

#ifdef CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION
#define APP_VERSION_STRING CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION
#else /* No McuBoot */
/* auto generated header file during west build */
#include "app_version.h"
#endif

#include "io_job_queue.h"

#include "appl_diagnose.h"
#include "appl_settings.h"
#include "appl_time.h"
#include "parse.h"
#include "sh_cmd.h"

#define MSEC_PER_HOUR (MSEC_PER_SEC * 60 * 60)

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

#define CONFIG_DIAGNOSE_STACK_SIZE 2048

static K_THREAD_STACK_DEFINE(appl_diagnose_stack, CONFIG_DIAGNOSE_STACK_SIZE);
static struct k_thread appl_diagnose_thread;
static K_SEM_DEFINE(appl_diagnose_shutdown, 0, 1);

static atomic_t shutdown_delay = ATOMIC_INIT(-1);
static atomic_t reboot_cause = ATOMIC_INIT(-1);

#define DIAGNOSE_READ_CAUSE 0
#define DIAGNOSE_REBOOTS 1
#define DIAGNOSE_SHUTDOWN_NOW 2
static atomic_t diagnose_flags = ATOMIC_INIT(0);

static const struct device *const wdt = DEVICE_DT_GET_OR_NULL(DT_ALIAS(watchdog0));
static int wdt_channel_id = -1;

static volatile uint32_t reset_cause = 0;
static volatile int32_t reset_error = 0;

static char appl_version[16] = {'v', 0};

const char *appl_get_version(void)
{
   return appl_version;
}

struct work_queue_check {
   struct k_spinlock lock;
   int64_t last;
   bool pending;
};

static struct work_queue_check diagnose_work_queue_check;

static void diagnose_work_queue_check_init(void)
{
   int64_t now = k_uptime_get();
   K_SPINLOCK(&diagnose_work_queue_check.lock)
   {
      diagnose_work_queue_check.last = now;
      diagnose_work_queue_check.pending = false;
   }
}

static void diagnose_watchdog_feed_fn(struct k_work *work)
{
   wdt_feed(wdt, wdt_channel_id);
   diagnose_work_queue_check_init();
   LOG_INF("alive check done.");
}

static K_WORK_DELAYABLE_DEFINE(diagnose_watchdog_feed_work, diagnose_watchdog_feed_fn);

static void diagnose_watchdog_system_queue_fn(struct k_work *work)
{
   LOG_DBG("alive check sys-queue.");
   k_work_schedule(&diagnose_watchdog_feed_work, K_MSEC(100));
}

static K_WORK_DELAYABLE_DEFINE(diagnose_watchdog_system_queue_work, diagnose_watchdog_system_queue_fn);

static void diagnose_watchdog_io_queue_fn(struct k_work *work)
{
   LOG_DBG("alive check io-queue.");
   work_schedule_for_io_queue(&diagnose_watchdog_system_queue_work, K_MSEC(100));
}

static K_WORK_DELAYABLE_DEFINE(diagnose_watchdog_io_queue_work, diagnose_watchdog_io_queue_fn);

void watchdog_feed(void)
{
   if (wdt && wdt_channel_id >= 0) {
      int64_t now = k_uptime_get();
      K_SPINLOCK(&diagnose_work_queue_check.lock)
      {
         int delta = (now - diagnose_work_queue_check.last) / MSEC_PER_SEC;
         if (!diagnose_work_queue_check.pending && delta > 120) {
            diagnose_work_queue_check.pending = true;
            // feed via work queue schedule => add monitoring for that queue
            LOG_DBG("alive check cmd-queue.");
            work_schedule_for_cmd_queue(&diagnose_watchdog_io_queue_work, K_MSEC(100));
            wdt_feed(wdt, wdt_channel_id);
         }
      }
   }
}

static void appl_reboot_fn(void *p1, void *p2, void *p3)
{
   (void)p1;
   (void)p2;
   (void)p3;

   int error = 0;
   k_sem_take(&appl_diagnose_shutdown, K_FOREVER);

   error = atomic_get(&reboot_cause);
   if (error >= 0) {
      appl_settings_add_reboot_code((uint16_t)error);
   }

   if (!atomic_test_bit(&diagnose_flags, DIAGNOSE_SHUTDOWN_NOW)) {
      uint32_t delay_ms = atomic_get(&shutdown_delay);
      while (delay_ms > 0) {
         if (k_sem_take(&appl_diagnose_shutdown, K_MSEC(delay_ms)) == -EAGAIN) {
            break;
         }
         if (atomic_test_bit(&diagnose_flags, DIAGNOSE_SHUTDOWN_NOW)) {
            break;
         }
         delay_ms = atomic_get(&shutdown_delay);
      }
   }
   sys_reboot(SYS_REBOOT_COLD);
}

void appl_reboot(int error, const k_timeout_t delay)
{
   atomic_set_bit(&diagnose_flags, DIAGNOSE_REBOOTS);
   uint32_t delay_ms = (uint32_t)k_ticks_to_ms_floor64(delay.ticks);
   atomic_set(&reboot_cause, error);
   if (delay_ms > 0) {
      atomic_set(&shutdown_delay, delay_ms);
   } else {
      atomic_set_bit(&diagnose_flags, DIAGNOSE_SHUTDOWN_NOW);
   }
   k_sem_give(&appl_diagnose_shutdown);
}

bool appl_reboots(void)
{
   return atomic_test_bit(&diagnose_flags, DIAGNOSE_REBOOTS);
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
      case ERROR_CODE_REBOOT_CMD:
         return "cmd reboot";
      case ERROR_CODE_REBOOT_MANUAL:
         return "manual reboot";
      case ERROR_CODE_UPDATE:
         return "update";
      case ERROR_CODE_LOW_VOLTAGE:
         return "low voltage";
      case ERROR_CODE_REINIT_CMD:
         return "cmd reinit";
   }
   return "\?\?\?";
}

int appl_reboot_cause_description(size_t index, int flags, char *buf, size_t len)
{
   int64_t reboot_time;
   uint16_t reboot_code;

   int err = appl_settings_get_reboot_code(index, &reboot_time, &reboot_code);
   if (err > 0) {
      int pos = 0;
      if (index == 0) {
         pos += snprintf(buf + pos, len - pos, "Last code: ");
      }
      if (reboot_time) {
         pos += appl_format_time(reboot_time, buf + pos, len - pos);
         pos += snprintf(buf + pos, len - pos, " ");
      }
      pos += snprintf(buf + pos, len - pos, "%s", appl_get_reboot_desciption(reboot_code));
      if (flags) {
         pos += snprintf(buf + pos, len - pos, " (0x%04x)", reboot_code);
      } else {
         reboot_code = ERROR_DETAIL(reboot_code);
         if (reboot_code) {
            pos += snprintf(buf + pos, len - pos, " %d", reboot_code);
         }
      }
      err = pos;
   }

   return err;
}

uint32_t appl_reset_cause(int *flags, uint16_t *reboot_code)
{
   uint32_t cause = 0;
   if (!atomic_test_and_set_bit(&diagnose_flags, DIAGNOSE_READ_CAUSE)) {
      reset_error = hwinfo_get_reset_cause(&cause);
      if (!reset_error) {
         hwinfo_clear_reset_cause();
         if (!cause) {
            // the nRF9160 uses 0 (no reset cause) to indicate POR
            uint32_t supported = 0;
            hwinfo_get_supported_reset_cause(&supported);
            if (!(supported & RESET_POR)) {
               LOG_INF("nRF9160 no reset cause, add POR");
               cause = RESET_POR;
            }
         }
         reset_cause = cause;
      }
   }
   LOG_INF("Reset cause 0x%04x", reset_cause);
   if (reset_cause) {
      // supported flags: 0x1b3
      if (reset_cause & RESET_PIN) {
         LOG_INF("PIN");
         if (flags) {
            *flags |= FLAG_RESET;
         }
      }
      if (reset_cause & RESET_SOFTWARE) {
         uint16_t code = 0;
         int rc = appl_settings_get_reboot_code(0, NULL, &code);
         int reboot = ERROR_CLASS(code);
         int detail = ERROR_DETAIL(code);
         if (rc > 0 && reboot == ERROR_CODE_TOO_MANY_FAILURES) {
            LOG_INF("Reboot 1.");
            if (!detail) {
               code = ERROR_CODE(ERROR_CODE_TOO_MANY_FAILURES, 1);
            }
            if (flags) {
               *flags |= FLAG_REBOOT_RETRY;
            }
         } else if (rc > 0 && reboot == ERROR_CODE_INIT_NO_SUCCESS) {
            LOG_INF("Reboot %u.", detail);
            if (flags) {
               *flags |= FLAG_REBOOT_RETRY;
            }
         } else if (rc > 0 && reboot == ERROR_CODE_LOW_VOLTAGE) {
            LOG_INF("Reboot low voltage.");
            if (flags) {
               *flags |= FLAG_REBOOT_LOW_VOLTAGE;
            }
         } else {
            LOG_INF("Reboot");
            if (flags) {
               *flags |= FLAG_REBOOT;
            }
         }
         if (rc > 0 && reboot_code) {
            *reboot_code = code;
         }
      }
      if (reset_cause & RESET_POR) {
         LOG_INF("Power-On");
         if (flags) {
            *flags |= FLAG_POWER_ON;
         }
      }
      if (reset_cause & RESET_WATCHDOG) {
         LOG_INF("WATCHDOG");
      }
      if (reset_cause & RESET_DEBUG) {
         LOG_INF("DEBUG");
      }
      if (reset_cause & RESET_LOW_POWER_WAKE) {
         LOG_INF("LOWPOWER");
      }
      if (reset_cause & RESET_CPU_LOCKUP) {
         LOG_INF("CPU");
      }
   } else {
      LOG_INF("none");
   }
   return reset_cause;
}

static void appl_cause_description_append(uint32_t bit, const char *desc, uint32_t *cause, int *index, char *buf, size_t len)
{
   if (cause && *cause & bit) {
      *cause &= ~bit;
      *index += snprintf(buf + *index, len - *index, "%s, ", desc);
   }
}

static int appl_cause_description(uint32_t cause, char *buf, size_t len)
{
   int index = 0;

   if (cause) {
      appl_cause_description_append(RESET_PIN, "Reset", &cause, &index, buf, len);
      appl_cause_description_append(RESET_SOFTWARE, "Reboot", &cause, &index, buf, len);
      appl_cause_description_append(RESET_BROWNOUT, "Brownout", &cause, &index, buf, len);
      appl_cause_description_append(RESET_POR, "Power-On", &cause, &index, buf, len);
      appl_cause_description_append(RESET_WATCHDOG, "Watchdog", &cause, &index, buf, len);
      appl_cause_description_append(RESET_DEBUG, "Debug", &cause, &index, buf, len);
      appl_cause_description_append(RESET_SECURITY, "Security", &cause, &index, buf, len);
      appl_cause_description_append(RESET_LOW_POWER_WAKE, "Low-Power", &cause, &index, buf, len);
      appl_cause_description_append(RESET_CPU_LOCKUP, "CPU", &cause, &index, buf, len);
      appl_cause_description_append(RESET_PARITY, "Parity", &cause, &index, buf, len);
      appl_cause_description_append(RESET_HARDWARE, "HW", &cause, &index, buf, len);
      appl_cause_description_append(RESET_USER, "User", &cause, &index, buf, len);
      appl_cause_description_append(RESET_TEMPERATURE, "Temperature", &cause, &index, buf, len);

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
   } else {
      index += snprintf(buf + index, len - index, "none");
   }
   return index;
}

int appl_reset_cause_description(char *buf, size_t len)
{
   int index = 0;

   if (atomic_test_bit(&diagnose_flags, DIAGNOSE_READ_CAUSE) && len > 8) {
      int error = reset_error;
      if (error) {
         index += snprintf(buf + index, len - index, "%d (%s)", error, strerror(-error));
      } else {
         index += appl_cause_description(reset_cause, buf, len);
      }
   }
   return index;
}

#ifdef CONFIG_SH_CMD

static int sh_cmd_reboot(const char *parameter)
{
   if (appl_reboots()) {
      LOG_INF(">> device already reboots!");
   } else {
      long id = 0;
      uint16_t reboot_code = 0;
      bool boot = (parameter == parse_next_long(parameter, 10, &id));
      int err = appl_settings_get_reboot_code(0, NULL, &reboot_code);
      if (boot) {
         if (err == 1 && ERROR_CLASS(reboot_code) == ERROR_CODE_REBOOT_CMD) {
            id = ERROR_DETAIL(reboot_code);
         }
      } else if (err == -EINVAL) {
         LOG_INF("reboot codes not supported!");
      } else if (err == 1 && reboot_code == ERROR_CODE(ERROR_CODE_REBOOT_CMD, id)) {
         LOG_INF("device already rebooted %u", (uint16_t)id);
      } else {
         boot = true;
      }
      if (boot) {
         appl_reboot(ERROR_CODE(ERROR_CODE_REBOOT_CMD, id), K_MSEC(2000));
         LOG_INF(">> device reboot %u ...", (uint16_t)id);
      }
   }
   return 0;
}

static void sh_cmd_reboot_help(void)
{
   LOG_INF("> help reboot:");
   LOG_INF("  reboot     : reboot device <last> (forced).");
   LOG_INF("  reboot <n> : reboot device <n>, if <last> was not the same <n>.");
}

#define REBOOT_INFOS 4

static int sh_cmd_read_reboots(const char *parameter)
{
   ARG_UNUSED(parameter);
   char buf[128];
   int err = 0;

   for (size_t index = 0; index < REBOOT_INFOS; ++index) {
      err = appl_reboot_cause_description(index, 1, buf, sizeof(buf));
      if (err > 0) {
         LOG_INF("%s", buf);
      } else {
         if (index == 0) {
            if (err == 0) {
               LOG_INF("Reboot codes not available.");
            } else if (err == -EINVAL) {
               LOG_INF("Reboot codes not supported.");
               err = 0;
            }
         }
         break;
      }
   }

   return err > 0 ? 0 : err;
}

static int sh_cmd_read_restarts(const char *parameter)
{
   ARG_UNUSED(parameter);

   uint32_t supported = 0;
   int len = 0;
   char buf[128];

   memset(buf, 0, sizeof(buf));
   hwinfo_get_supported_reset_cause(&supported);
   len = appl_cause_description(supported, buf, sizeof(buf));
   LOG_INF("Supported  : 0x%04x, %s", supported, buf);

   if (atomic_test_bit(&diagnose_flags, DIAGNOSE_READ_CAUSE)) {
      memset(buf, 0, sizeof(buf));
      len = appl_reset_cause_description(buf, sizeof(buf));
      LOG_INF("Reset cause: %s", buf);
   } else {
      LOG_WRN("Reset cause not read.");
   }

   return 0;
}

SH_CMD(reboot, NULL, "reboot device.", sh_cmd_reboot, sh_cmd_reboot_help, 0);
SH_CMD(reboots, NULL, "read reboot codes.", sh_cmd_read_reboots, NULL, 0);
SH_CMD(restarts, NULL, "read restart reasons.", sh_cmd_read_restarts, NULL, 0);

#if defined(CONFIG_SH_CMD_TEST)
static int sh_cmd_fail(const char *parameter)
{
   ARG_UNUSED(parameter);

   uint8_t val = 0;
   char *p = (char *)sh_cmd_fail;
   // cause failure
   val = *p;
   *p = 0;
   return 0;
}

static int sh_cmd_kill_stack(const char *parameter)
{
   ARG_UNUSED(parameter);
   char blob[8192];
   char *p = blob;

   LOG_INF("kill-stack %p", p);
   k_sleep(K_MSEC(100));
   memset(p, 0xaa, sizeof(blob));
   p -= sizeof(blob);
   LOG_INF("kill-stack %p", p);
   k_sleep(K_MSEC(100));
   memset(p, 0xaa, sizeof(blob));
   return 0;
}

static int sh_cmd_oops(const char *parameter)
{
   ARG_UNUSED(parameter);
   k_oops();
   return 0;
}

SH_CMD(fail, NULL, "cause a failure (access *NULL).", sh_cmd_fail, NULL, 0);
SH_CMD(kill, NULL, "cause a stack failure (corrupts stack).", sh_cmd_kill_stack, NULL, 0);
SH_CMD(oops, NULL, "cause a k_oops().", sh_cmd_oops, NULL, 0);

#ifdef CONFIG_ASSERT
static int sh_cmd_assert(const char *parameter)
{
   ARG_UNUSED(parameter);
   __ASSERT(sh_cmd_assert == NULL, "sh_cmd assert");
   return 0;
}
SH_CMD(assert, NULL, "cause an assert.", sh_cmd_assert, NULL, 0);
#endif

#endif /* CONFIG_SH_CMD_TEST */
#endif /* CONFIG_SH_CMD */

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

#ifndef CONFIG_RESET_ON_FATAL_ERROR
void k_sys_fatal_error_handler(unsigned int reason, const z_arch_esf_t *esf)
{
   (void)arch_irq_lock();
   LOG_PANIC();
   if (esf) {
      printk("fatal error %d 0x%x", reason, esf->basic.lr);
   } else {
      printk("fatal error %d", reason);
   }
   for (;;) {
      k_sleep(K_MSEC(100));
   }
}
#endif /* CONFIG_RESET_ON_FATAL_ERROR */

static int appl_diagnose_init(void)
{
   const char *mcu_appl_version = APP_VERSION_STRING;
   k_tid_t id = 0;

   appl_watchdog_init();

   id = k_thread_create(&appl_diagnose_thread,
                        appl_diagnose_stack,
                        CONFIG_DIAGNOSE_STACK_SIZE,
                        appl_reboot_fn, NULL, NULL, NULL,
                        K_HIGHEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);
   k_thread_name_set(id, "shutdown");

   memset(appl_version, 0, sizeof(appl_version));
   appl_version[0] = 'v';
   for (int i = 0; i < sizeof(appl_version) - 2; ++i) {
      char c = mcu_appl_version[i];
      if (c == 0 /* || c == '+' */) {
         break;
      }
      appl_version[i + 1] = c;
   }
   diagnose_work_queue_check_init();

   return 0;
}

SYS_INIT(appl_diagnose_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
