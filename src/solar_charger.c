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
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <stdlib.h>

#include "io_job_queue.h"
#include "parse.h"
#include "power_manager.h"
#include "sh_cmd.h"

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

#define SOLAR_CHRG_NODE DT_PATH(solar_control)

#if (!DT_NODE_HAS_STATUS(SOLAR_CHRG_NODE, okay))
#warning "Solar-charger not defined in devicetree"
#endif

static void solar_pgood_changed(const struct device *dev, struct gpio_callback *cb,
                                uint32_t pins);

static void solar_charger_voltage_handle_fn(struct k_work *work);
static void solar_charger_enable_interrupt_work_fn(struct k_work *work);

typedef struct solar_control_config {
   struct gpio_dt_spec enable_gpios;
   struct gpio_dt_spec power_good_gpios;
   struct gpio_dt_spec charging_gpios;
   uint32_t low_voltage;
   uint32_t max_voltage;
   uint32_t interval;
   uint32_t power_good_interval;
} t_solar_control_config;

static const t_solar_control_config solar_config = {
    .enable_gpios = GPIO_DT_SPEC_GET_OR(SOLAR_CHRG_NODE, enable_gpios, {}),
    .power_good_gpios = GPIO_DT_SPEC_GET_OR(SOLAR_CHRG_NODE, power_good_gpios, {}),
    .charging_gpios = GPIO_DT_SPEC_GET_OR(SOLAR_CHRG_NODE, charging_gpios, {}),
    .low_voltage = DT_PROP(SOLAR_CHRG_NODE, low_voltage),
    .max_voltage = DT_PROP(SOLAR_CHRG_NODE, max_voltage),
    .interval = DT_PROP(SOLAR_CHRG_NODE, check_interval),
    .power_good_interval = DT_PROP(SOLAR_CHRG_NODE, change_interval),
};

/* enable automatic mode. enables/disables charging according battery level */
/* Disable charging drains the power because of the 10k pull down of the solar charger! */
#define CHARGER_MODE_AUTO 0
/* Keep battery threshold to enable/disable charging */
#define CHARGER_MODE_CHARGE 1
/* Indicates, that the solar charger powered */
#define CHARGER_MODE_PGOOD 2

static volatile uint16_t emulated_voltage = PM_INVALID_VOLTAGE;
static atomic_t solar_charger_mode = ATOMIC_INIT(0);

static struct gpio_callback solar_pgood_cb_data;
static K_WORK_DELAYABLE_DEFINE(solar_charger_voltage_work, solar_charger_voltage_handle_fn);
static K_WORK_DELAYABLE_DEFINE(solar_charger_enable_interrupt_work, solar_charger_enable_interrupt_work_fn);

static int solar_init_input(void)
{
   int ret = -ENOTSUP;
   if (device_is_ready(solar_config.power_good_gpios.port)) {
      ret = gpio_pin_configure_dt(&solar_config.power_good_gpios, GPIO_INPUT);
      gpio_init_callback(&solar_pgood_cb_data, solar_pgood_changed, BIT(solar_config.power_good_gpios.pin));
      ret = gpio_add_callback(solar_config.power_good_gpios.port, &solar_pgood_cb_data);
      if (ret < 0) {
         return ret;
      }
   }
   if (device_is_ready(solar_config.charging_gpios.port)) {
      ret = gpio_pin_configure_dt(&solar_config.charging_gpios, GPIO_INPUT);
   }
   return ret;
}

static int solar_init_output(void)
{
   int ret = -ENOTSUP;
   if (device_is_ready(solar_config.enable_gpios.port)) {
      ret = gpio_pin_configure_dt(&solar_config.enable_gpios, GPIO_OUTPUT_ACTIVE);
   }
   return ret;
}

static int solar_enable_charger(int state)
{
   int ret = -ENOTSUP;
   if (device_is_ready(solar_config.enable_gpios.port)) {
      ret = gpio_pin_set_dt(&solar_config.enable_gpios, state);
   }
   return ret;
}

int solar_power_is_good(void)
{
   int ret = -ENOTSUP;
   if (device_is_ready(solar_config.power_good_gpios.port)) {
      ret = gpio_pin_get_dt(&solar_config.power_good_gpios);
   }
   return ret;
}

int solar_is_charging(void)
{
   int ret = -ENOTSUP;
   if (device_is_ready(solar_config.charging_gpios.port)) {
      ret = gpio_pin_get_dt(&solar_config.charging_gpios);
   }
   return ret;
}

int solar_is_enabled(void)
{
   int ret = -ENOTSUP;
   if (device_is_ready(solar_config.enable_gpios.port)) {
      ret = gpio_pin_get_dt(&solar_config.enable_gpios);
   }
   return ret;
}

static void solar_set_auto(int state)
{
   if (device_is_ready(solar_config.enable_gpios.port)) {
      if (state && !atomic_test_and_set_bit(&solar_charger_mode, CHARGER_MODE_AUTO)) {
         atomic_set_bit_to(&solar_charger_mode, CHARGER_MODE_PGOOD, !solar_power_is_good());
         gpio_pin_interrupt_configure_dt(&solar_config.power_good_gpios, GPIO_INT_EDGE_BOTH);
         work_reschedule_for_io_queue(&solar_charger_voltage_work, K_NO_WAIT);
      } else if (!state && atomic_test_and_clear_bit(&solar_charger_mode, CHARGER_MODE_AUTO)) {
         gpio_pin_interrupt_configure_dt(&solar_config.power_good_gpios, GPIO_INT_DISABLE);
         k_work_cancel_delayable(&solar_charger_voltage_work);
         k_work_cancel_delayable(&solar_charger_enable_interrupt_work);
         solar_enable_charger(1);
      }
   }
}

static const char *solar_pgood_desc(int rc)
{
   if (rc < 0) {
      return "";
   } else if (rc > 0) {
      return "power good, ";
   } else {
      return "no power, ";
   }
}

static const char *solar_mode_desc(int pgood, int charging, int enabled)
{
   if (pgood == 0) {
      return "battery";
   } else if (charging > 0) {
      return "charging";
   } else if (enabled) {
      return "full";
   } else {
      return "disabled";
   }
}

static void solar_charger_voltage_handle_fn(struct k_work *work)
{
   int pgood = solar_power_is_good();
   int charging = solar_is_charging();
   int enable = solar_is_enabled();
   uint16_t voltage = emulated_voltage;

   if (voltage == PM_INVALID_VOLTAGE) {
      power_manager_voltage(&voltage);
   }

   if (voltage < PM_INVALID_VOLTAGE) {
      LOG_INF("Solar charger: %u mV, %s%s.", voltage, solar_pgood_desc(pgood), solar_mode_desc(pgood, charging, enable));
   } else {
      LOG_INF("Solar charger: %s%s.", solar_pgood_desc(pgood), solar_mode_desc(pgood, charging, enable));
   }

   if (atomic_test_bit(&solar_charger_mode, CHARGER_MODE_AUTO)) {
      if (pgood) {
         int interval = solar_config.power_good_interval; // interval until battery voltage gets available
         if (voltage < PM_INVALID_VOLTAGE) {
            interval = solar_config.interval;
            if (voltage > solar_config.max_voltage && atomic_test_and_clear_bit(&solar_charger_mode, CHARGER_MODE_CHARGE)) {
               solar_enable_charger(0);
               atomic_set_bit(&solar_charger_mode, CHARGER_MODE_PGOOD);
               LOG_INF("Solar charger: switching off");
            } else if (voltage < solar_config.low_voltage && !atomic_test_and_set_bit(&solar_charger_mode, CHARGER_MODE_CHARGE)) {
               solar_enable_charger(1);
               atomic_set_bit(&solar_charger_mode, CHARGER_MODE_PGOOD);
               LOG_INF("Solar charger: switching on");
            } else if (!atomic_test_and_set_bit(&solar_charger_mode, CHARGER_MODE_PGOOD)) {
               if (atomic_test_bit(&solar_charger_mode, CHARGER_MODE_CHARGE)) {
                  LOG_INF("Solar charger: restore on");
                  solar_enable_charger(1);
               } else {
                  LOG_INF("Solar charger: restore off");
                  solar_enable_charger(0);
               }
            }
         } else {
            solar_enable_charger(0);
         }
         work_reschedule_for_io_queue(&solar_charger_voltage_work, K_SECONDS(interval));
      } else if (atomic_test_and_clear_bit(&solar_charger_mode, CHARGER_MODE_PGOOD)) {
         LOG_INF("Solar charger: no charging power.");
         solar_enable_charger(1);
         k_work_cancel_delayable(&solar_charger_voltage_work);
      }
   }
}

static void solar_charger_enable_interrupt_work_fn(struct k_work *work)
{
   if (atomic_test_bit(&solar_charger_mode, CHARGER_MODE_AUTO)) {
      gpio_pin_interrupt_configure_dt(&solar_config.power_good_gpios, GPIO_INT_EDGE_BOTH);
      solar_charger_voltage_handle_fn(work);
   }
}

static void solar_pgood_changed(const struct device *dev, struct gpio_callback *cb,
                                uint32_t pins)
{
   if ((BIT(solar_config.power_good_gpios.pin) & pins) == 0) {
      return;
   }
   gpio_pin_interrupt_configure_dt(&solar_config.power_good_gpios, GPIO_INT_DISABLE);
   if (atomic_test_bit(&solar_charger_mode, CHARGER_MODE_AUTO)) {
      work_reschedule_for_io_queue(&solar_charger_enable_interrupt_work, K_SECONDS(solar_config.power_good_interval));
      work_reschedule_for_io_queue(&solar_charger_voltage_work, K_NO_WAIT);
   }
}

static int solar_charger_setup(void)
{
   int ret;
   LOG_INF("solar charger setup.");

   ret = solar_init_input();
   if (ret) {
      LOG_INF("Solar init: PGOOD or CHRG failed! %d", ret);
   }

   ret = solar_init_output();
   if (ret) {
      LOG_INF("Solar init: CE failed! %d", ret);
   } else {
      atomic_set_bit(&solar_charger_mode, CHARGER_MODE_CHARGE);
      solar_set_auto(1);
   }

   return ret;
}

SYS_INIT(solar_charger_setup, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#ifdef CONFIG_SH_CMD

static int sh_cmd_solar(const char *parameter)
{
   char value[16];
   const char *cur = parameter;

   memset(value, 0, sizeof(value));
   cur = parse_next_text(cur, ' ', value, sizeof(value));

   if (value[0]) {
      if (stricmp(value, "on") == 0) {
         solar_set_auto(0);
         solar_enable_charger(1);
      } else if (stricmp(value, "off") == 0) {
         solar_set_auto(0);
         solar_enable_charger(0);
      } else if (stricmp(value, "auto") == 0) {
         if (device_is_ready(solar_config.enable_gpios.port)) {
            solar_set_auto(1);
         } else {
            LOG_INF("solar auto not supported, no enable/disbale!");
         }
      } else if (stricmp(value, "vol") == 0) {
         memset(value, 0, sizeof(value));
         cur = parse_next_text(cur, ' ', value, sizeof(value));
         if (value[0]) {
            emulated_voltage = (uint16_t)strtol(value, NULL, 10);
         } else {
            emulated_voltage = PM_INVALID_VOLTAGE;
         }
         work_reschedule_for_io_queue(&solar_charger_voltage_work, K_NO_WAIT);
      } else {
         LOG_INF("solar '%s' not supported!", value);
         return -EINVAL;
      }
   } else {
      work_reschedule_for_io_queue(&solar_charger_voltage_work, K_NO_WAIT);
   }
   return 0;
}

static void sh_cmd_solar_help(void)
{
   LOG_INF("> help solar:");
   LOG_INF("  solar      : show solar status.");
   LOG_INF("  solar on   : enable solar charging.");
   LOG_INF("  solar off  : disable solar charging.");
   if (device_is_ready(solar_config.enable_gpios.port)) {
      LOG_INF("  solar auto : auto solar charging");
   }
}

SH_CMD(solar, NULL, "solar charger.", sh_cmd_solar, sh_cmd_solar_help, 0);

#endif /* CONFIG_SH_CMD */
