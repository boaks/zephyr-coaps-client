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

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>

#include "appl_diagnose.h"
#include "appl_settings.h"
#include "modem.h"
#include "modem_at.h"
#include "parse.h"
#include "sh_cmd.h"

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

#define CONFIG_SH_CMD_UNLOCK_SECONDS 60

#define CONFIG_SH_CMD_THREAD_PRIO 10
#define CONFIG_SH_CMD_MAX_LEN 512
#define CONFIG_SH_CMD_STACK_SIZE 2048
#define CONFIG_SH_CMD_HEAP_SIZE (1024 + 512)
#define CONFIG_SH_AT_RESPONSE_MAX_LEN 256

struct sh_cmd_queue {
   void *queue_reserved;
   k_timeout_t delay;
   char data[]; /* Null-terminated cmd string */
};

static void sh_cmd_execute_fn(struct k_work *work);
static void sh_cmd_app_inactive_fn(struct k_work *work);
static void sh_cmd_wait_fn(struct k_work *work);
static void at_cmd_response_fn(struct k_work *work);

static int64_t at_cmd_time = 0;
static size_t sh_cmd_max_length = 0;

static char sh_cmd_buf[CONFIG_SH_CMD_MAX_LEN];
static char at_response_buf[CONFIG_SH_AT_RESPONSE_MAX_LEN];

#define BIT_SH_CMD_PROTECTED (BIT_SH_CMD_LAST + 1)
#define SH_CMD_PROTECTED BIT(BIT_SH_CMD_PROTECTED)

static atomic_t sh_cmd_state = ATOMIC_INIT(SH_CMD_PROTECTED);

static struct k_spinlock sh_cmd_app_active_lock;
static int64_t sh_cmd_app_active_end = 0;

static K_WORK_DELAYABLE_DEFINE(sh_cmd_app_inactive_work, sh_cmd_app_inactive_fn);
static K_WORK_DELAYABLE_DEFINE(sh_cmd_schedule_work, sh_cmd_execute_fn);
static K_WORK_DEFINE(sh_cmd_execute_work, sh_cmd_execute_fn);
static K_WORK_DEFINE(at_cmd_response_work, at_cmd_response_fn);

static struct k_work_q sh_cmd_work_q;
static K_THREAD_STACK_DEFINE(sh_cmd_stack, CONFIG_SH_CMD_STACK_SIZE);

static K_QUEUE_DEFINE(sh_cmd_queue);
static K_HEAP_DEFINE(sh_cmd_heap, CONFIG_SH_CMD_HEAP_SIZE);

static int line_length(const uint8_t *data, size_t length)
{
   int len = length;
   if (len > 0) {
      --len;
      while (len >= 0 && (data[len] == '\n' || data[len] == '\r')) {
         --len;
      }
      ++len;
   }
   return len;
}

static void sh_cmd_result(int res)
{
   bool finish = atomic_test_and_clear_bit(&sh_cmd_state, BIT_SH_CMD_EXECUTING);
   if (res > 0) {
      // noops
   } else {
      if (res < -1) {
         const char *desc = "";
         switch (res) {
            case -EFAULT:
               desc = "off";
               break;
            case -EBUSY:
               desc = "busy";
               break;
            case -EINVAL:
               desc = "invalid parameter";
               break;
            case -ESHUTDOWN:
               desc = "in shutdown";
               break;
            case -EINPROGRESS:
               desc = "in progress";
               break;
            case -ENOTSUP:
               desc = "not supported";
               break;
            case -ETIME:
               desc = "timeout";
               break;
            default:
               desc = strerror(-res);
               break;
         }
         LOG_INF("ERROR %d (%s)\n", -res, desc);
      }

      if (finish) {
         if (res < 0) {
            printk("ERROR\n");
         } else {
            printk("OK\n");
         }
         sh_cmd_wait_fn(NULL);
      }
   }
}

static void at_coneval_result(const char *result)
{
   unsigned int status = 0;
   unsigned int rrc = 0;
   unsigned int quality = 0;
   int rsrp = 0;
   int rsrq = 0;
   int snr = 0;
   int err = sscanf(result, "%u,%u,%u,%d,%d,%d,",
                    &status, &rrc, &quality, &rsrp, &rsrq, &snr);
   if (err == 1) {
      const char *desc = NULL;
      lte_network_info_t info;

      switch (status) {
         case 1:
            memset(&info, 0, sizeof(info));
            if (!modem_get_network_info(&info) && info.cell != LTE_LC_CELL_EUTRAN_ID_INVALID) {
               LOG_INF("> eval failed: cell %d/0x%08x not available!", info.cell, info.cell);
               return;
            } else {
               desc = "cell not available";
            }
            break;
         case 2:
            desc = "UICC missing (SIM card)";
            break;
         case 3:
            desc = "only barred cells available";
            break;
         case 4:
            desc = "modem busy";
            break;
         case 5:
            desc = "evaluation aborted";
            break;
         case 6:
            desc = "not registered";
            break;
         case 7:
            desc = "unspecific failure";
            break;
      }
      if (desc) {
         LOG_INF("> eval failed: %s", desc);
      } else {
         LOG_INF("> eval failed: %d", status);
      }
   } else if (err == 6) {
      const char *desc = NULL;
      switch (quality) {
         case 5:
            desc = "bad";
            break;
         case 6:
            desc = "poor";
            break;
         case 7:
            desc = "normal";
            break;
         case 8:
            desc = "good";
            break;
         case 9:
            desc = "excellent";
            break;
      }
      rsrp -= 140;
      rsrq = (rsrq - 39) / 2;
      snr -= 24;
      if (desc) {
         LOG_INF("> eval: quality %s, rsrp %d dBm, rsrq %d dB, snr %d dB",
                 desc, rsrp, rsrq, snr);
      } else {
         LOG_INF("> eval: quality %d, rsrp %d dBm, rsrq %d dB, snr %d dB",
                 quality, rsrp, rsrq, snr);
      }
   } else {
      LOG_INF("> eval parse %d", err);
   }
}

void sh_cmd_at_finish(void)
{
   if (atomic_test_and_clear_bit(&sh_cmd_state, BIT_SH_CMD_AT_PENDING)) {
      at_cmd_time = k_uptime_get() - at_cmd_time;
      if (at_cmd_time > 5000) {
         LOG_INF("%ld s", (long)((at_cmd_time + 500) / 1000));
      } else if (at_cmd_time > 500) {
         LOG_INF("%ld ms", (long)at_cmd_time);
      }
      sh_app_set_inactive(K_NO_WAIT);
      sh_cmd_wait_fn(NULL);
   }
}

static void at_cmd_response_fn(struct k_work *work)
{
   int index = strstart(at_response_buf, "%CONEVAL: ", true);

   printk("%s", at_response_buf);
   if (index > 0) {
      at_coneval_result(&at_response_buf[index]);
   }

   if (strend(at_response_buf, "OK", false)) {
      sh_cmd_result(1);
   } else if (strend(at_response_buf, "ERROR", false)) {
      sh_cmd_result(1);
   } else {
      sh_cmd_result(-1);
   }
   sh_cmd_at_finish();
}

static void at_cmd_resp_callback(const char *at_response)
{
   size_t len = strlen(at_response);
   if (len > sizeof(at_response_buf) - 1) {
      len = sizeof(at_response_buf) - 1;
   }
   len = line_length(at_response, len);
   strncpy(at_response_buf, at_response, len);
   at_response_buf[len] = 0;
   k_work_submit_to_queue(&sh_cmd_work_q, &at_cmd_response_work);
}

#define RESULT(X) ((X < 0) ? (X) : 0)
#define PENDING(X) ((X < 0) ? (X) : 1)

static const struct sh_cmd_entry *sh_cmd_get(const char *cmd)
{
   STRUCT_SECTION_FOREACH(sh_cmd_entry, e)
   {
      if (strstartsep(cmd, e->cmd, true, " ") > 0) {
         return e;
      } else if (e->at_cmd && e->at_cmd[0] && strstartsep(cmd, e->at_cmd, true, " =") > 0) {
         return e;
      }
   }
   return NULL;
}

#ifdef CONFIG_LOG_BACKEND_UART_THROTTLE
#define PAUSE_HELP 6
#define PAUSE_MS 40
#else /* CONFIG_LOG_BACKEND_UART_THROTTLE */
#define PAUSE_HELP 8
#define PAUSE_MS 25
#endif /* CONFIG_LOG_BACKEND_UART_THROTTLE */

static int sh_cmd_help(const char *parameter)
{
   bool full = false;
   int counter = 0;

   if (*parameter) {
      const struct sh_cmd_entry *cmd = sh_cmd_get(parameter);
      const char *msg = cmd ? "no details available." : "cmd unknown.";
      if (cmd && cmd->help && cmd->help_handler) {
         cmd->help_handler();
         return 0;
      } else if (stricmp(parameter, "full") == 0) {
         full = true;
      } else {
         LOG_INF("> help %s:", parameter);
         LOG_INF("  %s", msg);
         return 0;
      }
   }
   LOG_INF("> help: (%s)", appl_get_version());
   LOG_INF("  %-*s: generic modem at-cmd.(*)", sh_cmd_max_length, "at\?\?\?");

   STRUCT_SECTION_FOREACH(sh_cmd_entry, e)
   {
      if (e->help) {
         int index = 0;
         char details[6] = "(";

         if (e->at_cmd) {
            details[++index] = '*';
         }
         if (e->protect) {
#ifdef CONFIG_SH_CMD_UNLOCK
            details[++index] = '#';
#else
            // skip
            continue;
#endif /* CONFIG_SH_CMD_UNLOCK */
         }
         if (e->help_handler) {
            details[++index] = '?';
         }
         if (index) {
            details[++index] = ')';
            details[++index] = 0;
         } else {
            details[0] = 0;
         }
         LOG_INF("  %-*s: %s%s", sh_cmd_max_length, e->cmd, e->help, details);
         ++counter;
         if ((counter % PAUSE_HELP) == 0) {
            k_sleep(K_MSEC(PAUSE_MS));
         }
      }
   }
   LOG_INF("  %-*s: AT-cmd is used, maybe busy.", sh_cmd_max_length, "*");
#ifdef CONFIG_SH_CMD_UNLOCK
   LOG_INF("  %-*s: protected <cmd>, requires 'unlock' ahead.", sh_cmd_max_length, "#");
#endif /* CONFIG_SH_CMD_UNLOCK */
   LOG_INF("  %-*s: help <cmd> available.", sh_cmd_max_length, "?");
   if (full) {
      STRUCT_SECTION_FOREACH(sh_cmd_entry, e)
      {
         if (e->help && e->help_handler) {
            if (e->protect) {
#ifndef CONFIG_SH_CMD_UNLOCK
               // skip
               continue;
#endif /* CONFIG_SH_CMD_UNLOCK */
            }
            LOG_INF("");
            k_sleep(K_MSEC(PAUSE_MS * 2));
            e->help_handler();
         }
      }
   }
   return 0;
}

static void sh_cmd_help_help(void)
{
   /* empty by intention */
}

SH_CMD(help, NULL, NULL, sh_cmd_help, sh_cmd_help_help, 0);

#ifdef CONFIG_SH_CMD_UNLOCK

static int64_t sh_cmd_unlocked = 0;

static int sh_cmd_unlock(const char *parameter)
{
   if (appl_settings_unlock(parameter)) {
      LOG_INF("unlocked for %ds.", CONFIG_SH_CMD_UNLOCK_SECONDS);
      sh_cmd_unlocked = k_uptime_get() + (CONFIG_SH_CMD_UNLOCK_SECONDS * MSEC_PER_SEC);
      atomic_clear_bit(&sh_cmd_state, BIT_SH_CMD_PROTECTED);
   } else {
      LOG_WRN("failed to unlock.");
      sh_cmd_unlocked = 0;
      atomic_set_bit(&sh_cmd_state, BIT_SH_CMD_PROTECTED);
   }
   return 0;
}

static void sh_cmd_unlock_help(void)
{
   LOG_INF("> help unlock:");
   LOG_INF("  unlock <password>  : unlock protected cmds for %ds.", CONFIG_SH_CMD_UNLOCK_SECONDS);
}

static int sh_cmd_lock(const char *parameter)
{
   sh_cmd_unlocked = 0;
   atomic_set_bit(&sh_cmd_state, BIT_SH_CMD_PROTECTED);
   return 0;
}

SH_CMD(unlock, NULL, "unlock protected cmds.", sh_cmd_unlock, sh_cmd_unlock_help, 0);
SH_CMD(lock, NULL, "lock protected cmds.", sh_cmd_lock, NULL, 0);
#endif /* CONFIG_SH_CMD_UNLOCK */

static int sh_cmd(const char *cmd_buf, bool insecure)
{
   int res = 0;
   int i;
   const struct sh_cmd_entry *cmd = sh_cmd_get(cmd_buf);
   const char *at_cmd = cmd_buf;

   if (cmd) {
      i = strstartsep(cmd_buf, cmd->cmd, true, " ");
      if (!i && cmd->at_cmd && cmd->at_cmd[0]) {
         i = strstartsep(cmd_buf, cmd->at_cmd, true, " =");
      }
      if (cmd_buf[i] && !cmd->help_handler) {
         LOG_INF("%s doesn't support parameter '%s'!", cmd->cmd, &cmd_buf[i]);
         return 1;
      }
#ifdef CONFIG_SH_CMD_UNLOCK
      {
         int64_t now = k_uptime_get();
         if (now > sh_cmd_unlocked) {
            // expired
            atomic_set_bit(&sh_cmd_state, BIT_SH_CMD_PROTECTED);
            if (cmd->protect && insecure) {
               LOG_INF("%s is protected!", cmd->cmd);
               return 1;
            }
         } else {
            // extend timeout
            sh_cmd_unlocked = now + (CONFIG_SH_CMD_UNLOCK_SECONDS * MSEC_PER_SEC);
         }
      }
#else
      if (cmd->protect && insecure) {
         goto at_cmd_modem;
      }
#endif /* CONFIG_SH_CMD_UNLOCK */

      if (cmd->at_cmd) {
         if (cmd->at_cmd[0] && !cmd->handler) {
            /* simple AT cmd*/
            at_cmd = cmd->at_cmd;
            goto at_cmd_modem;
         } else {
            /* handler AT cmd*/
            if (atomic_test_and_set_bit(&sh_cmd_state, BIT_SH_CMD_AT_PENDING)) {
               LOG_INF("Modem pending ...");
               return 1;
            }
            sh_app_set_active();
            at_cmd_time = k_uptime_get();
            res = cmd->handler(&cmd_buf[i]);
            res = RESULT(res);
            if (!modem_at_async_pending()) {
               sh_cmd_at_finish();
            }
         }
      } else {
         res = cmd->handler(&cmd_buf[i]);
         res = RESULT(res);
      }
      if (res == -EINVAL && cmd->help_handler) {
         cmd->help_handler();
      }
      return res;
   }
at_cmd_modem:
   if (!strstart(at_cmd, "AT", true)) {
      LOG_INF("ignore > %s", at_cmd);
      LOG_INF("> 'help' for available commands.");
      return -1;
   }
   if (atomic_test_and_set_bit(&sh_cmd_state, BIT_SH_CMD_AT_PENDING)) {
      LOG_INF("Modem pending ...");
      return 1;
   }
   LOG_INF(">%s", at_cmd);
   sh_app_set_active();
   at_cmd_time = k_uptime_get();
   res = modem_at_cmd_async(at_cmd_resp_callback, NULL, at_cmd);
   if (res < 0) {
      sh_cmd_at_finish();
   } else {
      res = 1;
   }
   return res;
}

static void sh_cmd_execute_fn(struct k_work *work)
{
   int res = 0;

   if (&sh_cmd_schedule_work.work == work) {
      // scheduled from remote
      LOG_INF("...> %s", sh_cmd_buf);
      res = sh_cmd(sh_cmd_buf, false);
   } else {
      // executed from sh
      res = sh_cmd(sh_cmd_buf, true);
   }
   sh_cmd_result(res);
}

static void sh_cmd_wait_fn(struct k_work *work)
{
   if (!atomic_test_and_set_bit(&sh_cmd_state, BIT_SH_CMD_EXECUTING)) {
      struct sh_cmd_queue *sh_cmd = k_queue_get(&sh_cmd_queue, K_NO_WAIT);
      if (sh_cmd) {
         uint32_t delay_ms = (uint32_t)k_ticks_to_ms_floor64(sh_cmd->delay.ticks);
         strncpy(sh_cmd_buf, sh_cmd->data, sizeof(sh_cmd_buf) - 1);
         LOG_INF("> cmd '%s' scheduled (%u ms).", sh_cmd_buf, delay_ms);
         k_work_reschedule_for_queue(&sh_cmd_work_q, &sh_cmd_schedule_work, sh_cmd->delay);
         k_heap_free(&sh_cmd_heap, sh_cmd);
      } else {
         if (atomic_test_and_clear_bit(&sh_cmd_state, BIT_SH_CMD_QUEUED)) {
            LOG_INF("No cmd left.");
         }
         atomic_clear_bit(&sh_cmd_state, BIT_SH_CMD_EXECUTING);
      }
   } else if (work) {
      LOG_INF("> cmd busy, still waiting.");
   }
}

// from sh
int sh_cmd_execute(const char *cmd)
{
   size_t len = strlen(cmd);
   if (len >= sizeof(sh_cmd_buf)) {
      return -EINVAL;
   }
   if (!atomic_test_and_set_bit(&sh_cmd_state, BIT_SH_CMD_EXECUTING)) {
      strncpy(sh_cmd_buf, cmd, sizeof(sh_cmd_buf) - 1);
      k_work_submit_to_queue(&sh_cmd_work_q, &sh_cmd_execute_work);
      return 0;
   }
   return -EBUSY;
}

// from remote
int sh_cmd_schedule(const char *cmd, const k_timeout_t delay)
{
   size_t len = strlen(cmd);
   if (len >= sizeof(sh_cmd_buf)) {
      return -EINVAL;
   }
   if (!atomic_test_and_set_bit(&sh_cmd_state, BIT_SH_CMD_EXECUTING)) {
      strncpy(sh_cmd_buf, cmd, sizeof(sh_cmd_buf) - 1);
      k_work_reschedule_for_queue(&sh_cmd_work_q, &sh_cmd_schedule_work, delay);
      return 0;
   }
   return -EBUSY;
}

static int sh_cmd_put(bool head, const char *cmd, const k_timeout_t delay)
{
   size_t len = sizeof(struct sh_cmd_queue) + strlen(cmd) + sizeof(char);
   struct sh_cmd_queue *item = k_heap_alloc(&sh_cmd_heap, len, K_NO_WAIT);
   if (item) {
      item->delay = delay;
      strcpy(item->data, cmd);
      if (head) {
         k_queue_prepend(&sh_cmd_queue, item);
      } else {
         k_queue_append(&sh_cmd_queue, item);
      }
      atomic_set_bit(&sh_cmd_state, BIT_SH_CMD_QUEUED);
      LOG_DBG("> cmd appended.");
      sh_cmd_wait_fn(NULL);
      return 0;
   } else {
      LOG_INF("> cmd-queue full, cmd dropped.");
   }
   return -EBUSY;
}

int sh_cmd_prepend(const char *cmd, const k_timeout_t delay)
{
   return sh_cmd_put(true, cmd, delay);
}

int sh_cmd_append(const char *cmd, const k_timeout_t delay)
{
   return sh_cmd_put(false, cmd, delay);
}

int sh_busy(void)
{
   return atomic_get(&sh_cmd_state) & (SH_CMD_EXECUTING | SH_CMD_AT_PENDING | SH_CMD_QUEUED);
}

int sh_protected(void)
{
#ifdef CONFIG_SH_CMD_UNLOCK
   return atomic_test_bit(&sh_cmd_state, BIT_SH_CMD_PROTECTED);
#else  /* CONFIG_SH_CMD_UNLOCK */
   return SH_CMD_PROTECTED;
#endif /* CONFIG_SH_CMD_UNLOCK */
}

int sh_app_active()
{
   return atomic_test_bit(&sh_cmd_state, BIT_SH_CMD_APP_ACTIVE);
}

int sh_app_set_active(void)
{
   int res = 0;
   K_SPINLOCK(&sh_cmd_app_active_lock)
   {
      k_work_cancel_delayable(&sh_cmd_app_inactive_work);
      sh_cmd_app_active_end = 0;
      res = atomic_test_and_set_bit(&sh_cmd_state, BIT_SH_CMD_APP_ACTIVE) ? 1 : 0;
   }
   if (!res) {
      LOG_INF("sh app active");
   }
   return res;
}

static void sh_cmd_app_inactive_fn(struct k_work *work)
{
   K_SPINLOCK(&sh_cmd_app_active_lock)
   {
      sh_cmd_app_active_end = 0;
      atomic_clear_bit(&sh_cmd_state, BIT_SH_CMD_APP_ACTIVE);
   }
   LOG_INF("sh app inactive");
}

int sh_app_set_inactive(const k_timeout_t delay)
{
   int res = 0;
   if (atomic_test_bit(&sh_cmd_state, BIT_SH_CMD_APP_ACTIVE)) {
      k_ticks_t end = delay.ticks + sys_clock_tick_get() - K_MSEC(50).ticks;
      K_SPINLOCK(&sh_cmd_app_active_lock)
      {
         if ((end - sh_cmd_app_active_end) > 0) {
            sh_cmd_app_active_end = end;
            res = k_work_reschedule_for_queue(&sh_cmd_work_q, &sh_cmd_app_inactive_work, delay);
         }
      }
   }
   return res;
}

#ifdef CONFIG_USE_JOB_QUEUE_ALIVE_CHECK
static void sh_cmd_alive_fn(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(sh_cmd_alive_work, sh_cmd_alive_fn);

static void sh_cmd_alive_fn(struct k_work *work)
{
   LOG_INF("SH alive");
   k_work_reschedule_for_queue(&sh_cmd_work_q, &sh_cmd_alive_work, K_MSEC(30000));
}
#endif /* CONFIG_USE_JOB_QUEUE_ALIVE_CHECK */

static int sh_cmd_init(void)
{
   struct k_work_queue_config sh_cmd_cfg = {
       .name = "sh_cmd_workq",
   };

   STRUCT_SECTION_FOREACH(sh_cmd_entry, e)
   {
      if (e->cmd) {
         sh_cmd_max_length = MAX(sh_cmd_max_length, strlen(e->cmd));
      }
   }
   sh_cmd_max_length++;

   k_work_queue_start(&sh_cmd_work_q, sh_cmd_stack,
                      K_THREAD_STACK_SIZEOF(sh_cmd_stack),
                      CONFIG_SH_CMD_THREAD_PRIO, &sh_cmd_cfg);

#ifdef CONFIG_USE_JOB_QUEUE_ALIVE_CHECK
   k_work_reschedule_for_queue(&sh_cmd_work_q, &sh_cmd_alive_work, K_MSEC(30000));
#endif /* CONFIG_USE_JOB_QUEUE_ALIVE_CHECK */

   return 0;
}

SYS_INIT(sh_cmd_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
