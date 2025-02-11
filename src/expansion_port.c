/*
 * Copyright (c) 2025 Achim Kraus CloudCoap.net
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
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <stdio.h>

#include "expansion_port.h"
#include "parse.h"
#include "power_manager.h"
#include "sh_cmd.h"

#if DT_NODE_HAS_STATUS(DT_ALIAS(exp_port_enable), okay) || DT_NODE_HAS_STATUS(DT_ALIAS(regulator_3v3), okay)

#define POWER_UP_TIME_MS 50
#define POWER_DOWN_TIME_MS 25

enum exp_port_mode {
   EXP_PORT_AUTO,
   EXP_PORT_ON,
   EXP_PORT_OFF,
};

static volatile enum exp_port_mode mode = EXP_PORT_AUTO;
static atomic_t enable_counter = ATOMIC_INIT(0);

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

#if DT_NODE_HAS_STATUS(DT_ALIAS(exp_port_enable), okay)

static const struct gpio_dt_spec exp_port_gpio_spec = GPIO_DT_SPEC_GET(DT_ALIAS(exp_port_enable), gpios);

#define READY_DEVICE exp_port_gpio_spec.port

static int expansion_port_power_internal(bool enable)
{
   int ret = -ENOTSUP;
   if (device_is_ready(READY_DEVICE)) {
      if (!enable) {
         k_sleep(K_MSEC(POWER_DOWN_TIME_MS));
      }
      ret = gpio_pin_set_dt(&exp_port_gpio_spec, enable ? 1 : 0);
      if (!ret && enable) {
         k_sleep(K_MSEC(POWER_UP_TIME_MS));
      }
   }
   return ret;
}

static int expansion_port_power_get(void)
{
   return gpio_pin_get_dt(&exp_port_gpio_spec);
}

static int expansion_port_init(void)
{
   if (device_is_ready(READY_DEVICE)) {
#if defined(CONFIG_ENABLE_EXPANSION_PORT)
      gpio_pin_configure_dt(&exp_port_gpio_spec, GPIO_OUTPUT_ACTIVE);
#else  /* defined(CONFIG_ENABLE_EXPANSION_PORT) */
      gpio_pin_configure_dt(&exp_port_gpio_spec, GPIO_OUTPUT_INACTIVE);
#endif /* defined(CONFIG_ENABLE_EXPANSION_PORT) */
   }
   return 0;
}

SYS_INIT(expansion_port_init, POST_KERNEL, CONFIG_EXPANSION_PORT_INIT_PRIORITY);

#else /* DT_NODE_HAS_STATUS(DT_ALIAS(exp_port_enable), okay) */

static const struct device *npm1300_buck2_dev = DEVICE_DT_GET(DT_ALIAS(regulator_3v3));

#define READY_DEVICE npm1300_buck2_dev

static int expansion_port_power_internal(bool enable)
{
   int ret = 0;
   if (enable || power_manager_is_suspended()) {
      if (!enable) {
         k_sleep(K_MSEC(POWER_DOWN_TIME_MS));
      }
      ret = power_manager_3v3(enable);
      if (!ret && enable) {
         k_sleep(K_MSEC(POWER_UP_TIME_MS));
      }
   }
   return 0;
}

static int expansion_port_power_get(void)
{
   return power_manager_is_3v3_enabled();
}

#endif /* DT_NODE_HAS_STATUS(DT_ALIAS(exp_port_enable), okay) */

int expansion_port_power(bool enable)
{
   int ret = -ENOTSUP;
   if (EXP_PORT_AUTO == mode) {
      bool change = false;
      if (enable) {
         change = (atomic_inc(&enable_counter) == 0);
      } else {
         change = (atomic_dec(&enable_counter) == 1);
         if (atomic_get(&enable_counter) < 0) {
            LOG_WRN("Expansion enable counter released too often!");
            atomic_clear(&enable_counter);
         }
      }
      if (change) {
         ret = expansion_port_power_internal(enable);
      } else {
         ret = 0;
      }
   }
   return ret;
}

#ifdef CONFIG_SH_CMD

static int expansion_port_mode(const char *config)
{
   int res = 0;
   const char *cur = config;
   char value[6];

   memset(value, 0, sizeof(value));
   cur = parse_next_text(cur, ' ', value, sizeof(value));
   if (!value[0]) {
      if (!device_is_ready(READY_DEVICE)) {
         LOG_INF("Expansion port power line not available.");
      } else {
         res = expansion_port_power_get();
         cur = res > 0 ? "ON" : res < 0 ? "ERR"
                                        : "OFF";
         config = "unknown";
         switch (mode) {
            case EXP_PORT_AUTO:
               config = "auto";
               break;
            case EXP_PORT_ON:
               config = "on";
               break;
            case EXP_PORT_OFF:
               config = "off";
               break;
         }
         LOG_INF("Expansion port mode %s, state %s (count: %d)", config, cur, (int)atomic_get(&enable_counter));
      }
   } else {
      if (!stricmp("auto", value)) {
         mode = EXP_PORT_AUTO;
         atomic_clear(&enable_counter);
         res = expansion_port_power_internal(false);
      } else if (!stricmp("on", value)) {
         mode = EXP_PORT_ON;
         res = expansion_port_power_internal(true);
      } else if (!stricmp("off", value)) {
         mode = EXP_PORT_OFF;
         res = expansion_port_power_internal(false);
      } else {
         res = -EINVAL;
      }
   }

   return res;
}

static void expansion_port_mode_help(void)
{
   LOG_INF("> help expan:");
   LOG_INF("  expan        : show expansion port mode.");
   LOG_INF("  expan <mode> : set expansion port mode.");
   LOG_INF("        on     : switch expansion port on.");
   LOG_INF("        off    : switch expansion port off.");
   LOG_INF("        auto   : switch expansion port to auto mode.");
}

SH_CMD(expan, "", "configure expansion port.", expansion_port_mode, expansion_port_mode_help, 0);

#endif /* CONFIG_SH_CMD */

#else

int expansion_port_power(bool enable)
{
   return -ENOTSUP;
}

#endif
