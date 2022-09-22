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

#include "ui.h"
#include <zephyr.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#define LED_RED_NODE DT_ALIAS(led0)
#define LED_GREEN_NODE DT_ALIAS(led1)
#define LED_BLUE_NODE DT_ALIAS(led2)

#define CALL_BUTTON_NODE DT_ALIAS(sw0)

#define CONFIG_BUTTON_NODE_1 DT_ALIAS(sw1)
#define CONFIG_SWITCH_NODE_1 DT_ALIAS(sw2)
#define CONFIG_SWITCH_NODE_2 DT_ALIAS(sw3)

#if DT_NODE_HAS_STATUS(LED_RED_NODE, okay)
#define LED_RED DT_GPIO_LABEL(LED_RED_NODE, gpios)
#define PIN_RED DT_GPIO_PIN(LED_RED_NODE, gpios)
#define FLAGS_RED DT_GPIO_FLAGS(LED_RED_NODE, gpios)
#else
/* A build error here means your board isn't set up to blink the red LED. */
#error "Unsupported board: led0 (red) devicetree alias is not defined"
#define LED_RED ""
#define PIN_RED 0
#define FLAGS_RED 0
#endif

#if DT_NODE_HAS_STATUS(LED_GREEN_NODE, okay)
#define LED_GREEN DT_GPIO_LABEL(LED_GREEN_NODE, gpios)
#define PIN_GREEN DT_GPIO_PIN(LED_GREEN_NODE, gpios)
#define FLAGS_GREEN DT_GPIO_FLAGS(LED_GREEN_NODE, gpios)
#else
/* A build error here means your board isn't set up to blink the green LED. */
#error "Unsupported board: led1 (green) devicetree alias is not defined"
#define LED_GREEN ""
#define PIN_GREEN 0
#define FLAGS_GREEN 0
#endif

#if DT_NODE_HAS_STATUS(LED_BLUE_NODE, okay)
#define LED_BLUE DT_GPIO_LABEL(LED_BLUE_NODE, gpios)
#define PIN_BLUE DT_GPIO_PIN(LED_BLUE_NODE, gpios)
#define FLAGS_BLUE DT_GPIO_FLAGS(LED_BLUE_NODE, gpios)
#else
/* A build error here means your board isn't set up to blink the blue LED. */
#error "Unsupported board: led2 (blue) devicetree alias is not defined"
#define LED_BLUE ""
#define PIN_BLUE 0
#define FLAGS_BLUE 0
#endif

#if DT_NODE_HAS_STATUS(CALL_BUTTON_NODE, okay)
#define CALL_BUTTON_GPIO_LABEL DT_GPIO_LABEL(CALL_BUTTON_NODE, gpios)
#define CALL_BUTTON_GPIO_PIN DT_GPIO_PIN(CALL_BUTTON_NODE, gpios)
#define CALL_BUTTON_GPIO_FLAGS (GPIO_INPUT | DT_GPIO_FLAGS(CALL_BUTTON_NODE, gpios))
#else
/* A build error here means your board isn't set up to for sw0 (call button). */
#error "Unsupported board: sw0 devicetree alias is not defined"
#define CALL_BUTTON_GPIO_LABEL ""
#define CALL_BUTTON_GPIO_PIN 0
#define CALL_BUTTON_GPIO_FLAGS 0
#endif

#if DT_NODE_HAS_STATUS(CONFIG_BUTTON_NODE_1, okay) && DT_NODE_HAS_STATUS(CONFIG_SWITCH_NODE_1, okay) && DT_NODE_HAS_STATUS(CONFIG_SWITCH_NODE_2, okay)
#define CONFIG_BUTTON_1_GPIO_LABEL DT_GPIO_LABEL(CONFIG_BUTTON_NODE_1, gpios)
#define CONFIG_BUTTON_1_GPIO_PIN DT_GPIO_PIN(CONFIG_BUTTON_NODE_1, gpios)
#define CONFIG_BUTTON_1_GPIO_FLAGS (GPIO_INPUT | DT_GPIO_FLAGS(CONFIG_BUTTON_NODE_1, gpios))
#define CONFIG_SWITCH_1_GPIO_LABEL DT_GPIO_LABEL(CONFIG_SWITCH_NODE_1, gpios)
#define CONFIG_SWITCH_1_GPIO_PIN DT_GPIO_PIN(CONFIG_SWITCH_NODE_1, gpios)
#define CONFIG_SWITCH_1_GPIO_FLAGS (GPIO_INPUT | DT_GPIO_FLAGS(CONFIG_SWITCH_NODE_1, gpios))
#define CONFIG_SWITCH_2_GPIO_LABEL DT_GPIO_LABEL(CONFIG_SWITCH_NODE_2, gpios)
#define CONFIG_SWITCH_2_GPIO_PIN DT_GPIO_PIN(CONFIG_SWITCH_NODE_2, gpios)
#define CONFIG_SWITCH_2_GPIO_FLAGS (GPIO_INPUT | DT_GPIO_FLAGS(CONFIG_SWITCH_NODE_2, gpios))
static const struct device *config_button_1_dev;
static const struct device *config_switch_1_dev;
static const struct device *config_switch_2_dev;
#endif

static const struct device *led_red_dev;
static const struct device *led_green_dev;
static const struct device *led_blue_dev;
static const struct device *button_dev;

static struct gpio_callback button_cb_data;
static ui_callback_handler_t button_callback;

static void led_timer_expiry_fn(struct k_work *work);
static void button_pressed_fn(struct k_work *work);

static K_WORK_DEFINE(button_pressed_work, button_pressed_fn);
static K_WORK_DELAYABLE_DEFINE(led_red_timer_work, led_timer_expiry_fn);
static K_WORK_DELAYABLE_DEFINE(led_green_timer_work, led_timer_expiry_fn);
static K_WORK_DELAYABLE_DEFINE(led_blue_timer_work, led_timer_expiry_fn);

static K_MUTEX_DEFINE(led_mutex);

static void button_pressed_fn(struct k_work *work)
{

   ui_led_op(LED_COLOR_BLUE, LED_TOGGLE);

   if (button_callback != NULL) {
      button_callback();
   }
}

static void button_pressed(const struct device *dev, struct gpio_callback *cb,
                           uint32_t pins)
{
   k_work_submit(&button_pressed_work);
}

static void initButton(void)
{
   button_dev = device_get_binding(CALL_BUTTON_GPIO_LABEL);
   if (button_dev == NULL) {
      return;
   }
   int ret = gpio_pin_configure(button_dev, CALL_BUTTON_GPIO_PIN, CALL_BUTTON_GPIO_FLAGS);
   if (ret < 0) {
      button_dev = NULL;
      return;
   }
   ret = gpio_pin_interrupt_configure(button_dev, CALL_BUTTON_GPIO_PIN, GPIO_INT_EDGE_TO_ACTIVE);
   if (ret < 0) {
      return;
   }
   gpio_init_callback(&button_cb_data, button_pressed, BIT(CALL_BUTTON_GPIO_PIN));
   ret = gpio_add_callback(button_dev, &button_cb_data);
   if (ret < 0) {
      return;
   }
#if DT_NODE_HAS_STATUS(CONFIG_BUTTON_NODE_1, okay) && DT_NODE_HAS_STATUS(CONFIG_SWITCH_NODE_1, okay) && DT_NODE_HAS_STATUS(CONFIG_SWITCH_NODE_2, okay)

   config_button_1_dev = device_get_binding(CONFIG_BUTTON_1_GPIO_LABEL);
   if (config_button_1_dev == NULL) {
      return;
   }
   ret = gpio_pin_configure(config_button_1_dev, CONFIG_BUTTON_1_GPIO_PIN, CONFIG_BUTTON_1_GPIO_FLAGS);
   if (ret < 0) {
      config_button_1_dev = NULL;
      return;
   }
   config_switch_1_dev = device_get_binding(CONFIG_SWITCH_1_GPIO_LABEL);
   if (config_switch_1_dev == NULL) {
      return;
   }
   ret = gpio_pin_configure(config_switch_1_dev, CONFIG_SWITCH_1_GPIO_PIN, CONFIG_SWITCH_1_GPIO_FLAGS);
   if (ret < 0) {
      config_switch_1_dev = NULL;
      return;
   }
   config_switch_2_dev = device_get_binding(CONFIG_SWITCH_2_GPIO_LABEL);
   if (config_switch_2_dev == NULL) {
      return;
   }
   ret = gpio_pin_configure(config_switch_2_dev, CONFIG_SWITCH_2_GPIO_PIN, CONFIG_SWITCH_2_GPIO_FLAGS);
   if (ret < 0) {
      config_switch_2_dev = NULL;
      return;
   }
#endif
}

static void led_timer_expiry_fn(struct k_work *work)
{
   struct k_work_delayable *dwork = k_work_delayable_from_work(work);

   if (&led_red_timer_work == dwork) {
      ui_led_op(LED_COLOR_RED, LED_CLEAR);
   } else if (&led_green_timer_work == dwork) {
      ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
   } else if (&led_blue_timer_work == dwork) {
      ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
   }
}

static void ui_op(const struct device *port, gpio_pin_t pin, led_op_t op, struct k_work_delayable *timer)
{
   k_mutex_lock(&led_mutex, K_FOREVER);
   if (timer) {
      k_work_cancel_delayable(timer);
   }
   if (port != NULL) {
      switch (op) {
         case LED_SET:
            gpio_pin_set(port, pin, 1);
            break;
         case LED_CLEAR:
            gpio_pin_set(port, pin, 0);
            break;
         case LED_TOGGLE:
            gpio_pin_toggle(port, pin);
            break;
         case LED_BLINK:
            if (timer) {
               gpio_pin_set(port, pin, 1);
               k_work_schedule(timer, K_MSEC(500));
            }
            break;
      }
   }
   k_mutex_unlock(&led_mutex);
}

void ui_led_op(led_t led, led_op_t op)
{
   switch (led) {
      case LED_COLOR_RED:
         ui_op(led_red_dev, PIN_RED, op, &led_red_timer_work);
         break;
      case LED_COLOR_BLUE:
         ui_op(led_blue_dev, PIN_BLUE, op, &led_blue_timer_work);
         break;
      case LED_COLOR_GREEN:
         ui_op(led_green_dev, PIN_GREEN, op, &led_green_timer_work);
         break;
   }
}

int ui_init(ui_callback_handler_t button_handler)
{
   led_red_dev = device_get_binding(LED_RED);
   if (led_red_dev != NULL) {
      int ret = gpio_pin_configure(led_red_dev, PIN_RED, GPIO_OUTPUT_ACTIVE | FLAGS_RED);
      if (ret < 0) {
         led_red_dev = NULL;
      } else {
         gpio_pin_set(led_red_dev, PIN_RED, 0);
      }
   }

   led_green_dev = device_get_binding(LED_GREEN);
   if (led_green_dev != NULL) {
      int ret = gpio_pin_configure(led_green_dev, PIN_GREEN, GPIO_OUTPUT_ACTIVE | FLAGS_GREEN);
      if (ret < 0) {
         led_green_dev = NULL;
      } else {
         gpio_pin_set(led_green_dev, PIN_GREEN, 0);
      }
   }

   led_blue_dev = device_get_binding(LED_BLUE);
   if (led_blue_dev != NULL) {
      int ret = gpio_pin_configure(led_blue_dev, PIN_BLUE, GPIO_OUTPUT_ACTIVE | FLAGS_BLUE);
      if (ret < 0) {
         led_blue_dev = NULL;
      } else {
         gpio_pin_set(led_blue_dev, PIN_BLUE, 0);
      }
   }
   button_callback = button_handler;
   initButton();
   return 0;
}

int ui_config(void)
{
#if DT_NODE_HAS_STATUS(CONFIG_BUTTON_NODE_1, okay) && DT_NODE_HAS_STATUS(CONFIG_SWITCH_NODE_1, okay) && DT_NODE_HAS_STATUS(CONFIG_SWITCH_NODE_2, okay)
   if (button_dev && config_button_1_dev && config_switch_1_dev && config_switch_2_dev) {
      int pin1 = gpio_pin_get(button_dev, CALL_BUTTON_GPIO_PIN);
      int pin2 = gpio_pin_get(config_button_1_dev, CONFIG_BUTTON_1_GPIO_PIN);
      int pin3 = gpio_pin_get(config_switch_1_dev, CONFIG_SWITCH_1_GPIO_PIN);
      int pin4 = gpio_pin_get(config_switch_2_dev, CONFIG_SWITCH_2_GPIO_PIN);
      if (pin1 >= 0 && pin2 >= 0 && pin3 >= 0 && pin4 >= 0) {
         return pin4 << 3 | pin3 << 2 | pin2 << 1 | pin1;
      }
   }
#endif
   return 0;
}
