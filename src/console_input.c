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

#include <logging/log.h>
#include <zephyr.h>
#include <zephyr/console/console.h>
#include <sys/reboot.h>

#include "modem.h"

#define CONSOLE_INPUT_STACK_SIZE 1024
#define CONSOLE_INPUT_PRIORITY 5

static K_THREAD_STACK_DEFINE(console_input_stack, CONSOLE_INPUT_STACK_SIZE);
static struct k_thread console_input_thread;

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

static void console_input_fn(void *p1, void *p2, void *p3)
{
   int index = 0;
   int ready = 0;
   int rc;
   char buf[32];
   (void)p1;
   (void)p2;
   (void)p3;

   console_init();
   LOG_INF("console input ready.");

   while (true) {
      rc = console_getchar();
      if (rc == '\n' || rc == '\r') {
         buf[index] = 0;
         LOG_INF("in> '%s'", buf);
         ready = 1;
         index = 0;
      } else if (rc >= 0) {
         buf[index++] = rc;
         if (index >= sizeof(buf) - 1) {
            buf[sizeof(buf) - 1] = 0;
            LOG_INF("in>> '%s'", buf);
            index = 0;
         }
      }
      if (ready) {
         ready = 0;
         if (strcmp(buf, "reset") == 0) {
            modem_factory_reset();
            LOG_INF("in> modem reseted.");
         } else if (strcmp(buf, "reboot") == 0) {
            LOG_INF("in> modem reboot ...");
            k_sleep(K_SECONDS(2));
            sys_reboot(SYS_REBOOT_COLD);
         } else if (strcmp(buf, "off") == 0) {
            modem_set_offline();
            LOG_INF("in> modem offline.");
         } else if (strcmp(buf, "on") == 0) {
            modem_set_normal();
            LOG_INF("in> modem online.");
         }
      }
   }
}

int console_init_input(void)
{
   k_thread_create(&console_input_thread,
                   console_input_stack,
                   CONSOLE_INPUT_STACK_SIZE,
                   console_input_fn,
                   NULL, NULL, NULL,
                   CONSOLE_INPUT_PRIORITY, 0, K_NO_WAIT);
   return 0;
}
