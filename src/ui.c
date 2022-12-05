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
#include <logging/log.h>
#include <zephyr.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#include "io_job_queue.h"

LOG_MODULE_DECLARE(COAP_CLIENT, CONFIG_COAP_CLIENT_LOG_LEVEL);

#define LED_RED_NODE DT_ALIAS(led0)
#define LED_GREEN_NODE DT_ALIAS(led1)
#define LED_BLUE_NODE DT_ALIAS(led2)

#define OUT_LTE_NODE_1 DT_ALIAS(led3)
#define OUT_LTE_NODE_2 DT_ALIAS(out1)
#define OUT_LTE_NODE_3 DT_ALIAS(out2)

#define CALL_BUTTON_NODE DT_ALIAS(sw0)

#define CONFIG_BUTTON_NODE_1 DT_ALIAS(sw1)
#define CONFIG_SWITCH_NODE_1 DT_ALIAS(sw2)
#define CONFIG_SWITCH_NODE_2 DT_ALIAS(sw3)

#if (!DT_NODE_HAS_STATUS(LED_RED_NODE, okay))
/* A build error here means your board isn't set up to blink the red LED. */
#error "Unsupported board: led0 (red) devicetree alias is not defined"
#define LED_RED ""
#define PIN_RED 0
#define FLAGS_RED 0
#endif

#if (!DT_NODE_HAS_STATUS(LED_GREEN_NODE, okay))
/* A build error here means your board isn't set up to blink the green LED. */
#error "Unsupported board: led1 (green) devicetree alias is not defined"
#endif

#if (!DT_NODE_HAS_STATUS(LED_BLUE_NODE, okay))
/* A build error here means your board isn't set up to blink the blue LED. */
#error "Unsupported board: led2 (blue) devicetree alias is not defined"
#endif

#if (!DT_NODE_HAS_STATUS(CALL_BUTTON_NODE, okay))
/* A build error here means your board isn't set up to for sw0 (call button). */
#error "Unsupported board: sw0 devicetree alias is not defined"
#endif

typedef struct gpio_device {
   const struct gpio_dt_spec gpio_spec;
   bool init;
} gpio_device_t;

#define GPIO_DEVICE_INIT(NODE)             \
   {                                       \
      GPIO_DT_SPEC_GET(NODE, gpios), false \
   }

#if DT_NODE_HAS_STATUS(CONFIG_BUTTON_NODE_1, okay) && DT_NODE_HAS_STATUS(CONFIG_SWITCH_NODE_1, okay) && DT_NODE_HAS_STATUS(CONFIG_SWITCH_NODE_2, okay)
static gpio_device_t config_button_1_spec = GPIO_DEVICE_INIT(CONFIG_BUTTON_NODE_1);
static gpio_device_t config_switch_1_spec = GPIO_DEVICE_INIT(CONFIG_SWITCH_NODE_1);
static gpio_device_t config_switch_2_spec = GPIO_DEVICE_INIT(CONFIG_SWITCH_NODE_2);
#endif

static gpio_device_t led_red_spec = GPIO_DEVICE_INIT(LED_RED_NODE);
static gpio_device_t led_green_spec = GPIO_DEVICE_INIT(LED_GREEN_NODE);
static gpio_device_t led_blue_spec = GPIO_DEVICE_INIT(LED_BLUE_NODE);
#if DT_NODE_HAS_STATUS(OUT_LTE_NODE_1, okay)
static gpio_device_t out_lte_1_spec = GPIO_DEVICE_INIT(OUT_LTE_NODE_1);
#endif
#if DT_NODE_HAS_STATUS(OUT_LTE_NODE_2, okay)
static gpio_device_t out_lte_2_spec = GPIO_DEVICE_INIT(OUT_LTE_NODE_2);
#endif
#if DT_NODE_HAS_STATUS(OUT_LTE_NODE_3, okay)
static gpio_device_t out_lte_3_spec = GPIO_DEVICE_INIT(OUT_LTE_NODE_3);
#endif
static gpio_device_t button_spec = GPIO_DEVICE_INIT(CALL_BUTTON_NODE);

static struct gpio_callback button_cb_data;
static ui_callback_handler_t button_callback;
static volatile bool button_active;
static volatile unsigned int button_counter;

static void ui_led_timer_expiry_fn(struct k_work *work);
static void ui_button_pressed_fn(struct k_work *work);

static K_WORK_DEFINE(button_pressed_work, ui_button_pressed_fn);
static K_WORK_DEFINE(button_released_work, ui_button_pressed_fn);
static K_WORK_DELAYABLE_DEFINE(button_long_pressed_work, ui_button_pressed_fn);
static K_WORK_DELAYABLE_DEFINE(led_red_timer_work, ui_led_timer_expiry_fn);
static K_WORK_DELAYABLE_DEFINE(led_green_timer_work, ui_led_timer_expiry_fn);
static K_WORK_DELAYABLE_DEFINE(led_blue_timer_work, ui_led_timer_expiry_fn);

static K_MUTEX_DEFINE(ui_mutex);

static void ui_button_pressed_fn(struct k_work *work)
{
   static volatile int duration = 0;

   if (&button_pressed_work == work) {
      LOG_INF("UI button pressed %u", button_counter);
      duration = 0;
      work_reschedule_for_io_queue(&button_long_pressed_work, K_MSEC(5000));
   } else if (&button_released_work == work) {
      LOG_INF("UI button released %u", button_counter);
      k_work_cancel_delayable(&button_long_pressed_work);
      if (duration == 0) {
         duration = 1;
         ui_led_op(LED_COLOR_BLUE, LED_TOGGLE);
         if (button_callback != NULL) {
            button_callback(0);
            LOG_INF("UI button callback %u", button_counter);
         }
      }
   } else if (&button_long_pressed_work.work == work) {
      LOG_INF("UI button long pressed %u", button_counter);
      if (duration == 0) {
         duration = 2;
         ui_led_op(LED_COLOR_BLUE, LED_BLINK);
         ui_led_op(LED_COLOR_GREEN, LED_BLINK);
         ui_led_op(LED_COLOR_RED, LED_BLINK);
         if (button_callback != NULL) {
            button_callback(1);
            LOG_INF("UI button long callback %u", button_counter);
         }
      }
   }
}

static void ui_button_pressed(const struct device *dev, struct gpio_callback *cb,
                              uint32_t pins)
{
   if ((BIT(button_spec.gpio_spec.pin) & pins) == 0) {
      return;
   }
   if (button_active) {
      button_active = false;
      gpio_pin_interrupt_configure_dt(&button_spec.gpio_spec, GPIO_INT_LEVEL_ACTIVE);
      work_submit_to_io_queue(&button_released_work);
   } else {
      button_active = true;
      ++button_counter;
      gpio_pin_interrupt_configure_dt(&button_spec.gpio_spec, GPIO_INT_LEVEL_INACTIVE);
      work_submit_to_io_queue(&button_pressed_work);
   }
}

static int ui_init_input(gpio_device_t *input_spec)
{
   int ret = -ENOTSUP;
   if (input_spec && device_is_ready(input_spec->gpio_spec.port)) {
      ret = gpio_pin_configure_dt(&input_spec->gpio_spec, GPIO_INPUT);
      if (!ret) {
         input_spec->init = true;
      }
   }
   return ret;
}

static int ui_init_button(void)
{
   int ret = ui_init_input(&button_spec);
   if (ret < 0) {
      return ret;
   }
   button_counter = 0;
   button_active = false;
   ret = gpio_pin_interrupt_configure_dt(&button_spec.gpio_spec, GPIO_INT_LEVEL_ACTIVE);
   if (ret < 0) {
      return ret;
   }
   gpio_init_callback(&button_cb_data, ui_button_pressed, BIT(button_spec.gpio_spec.pin));
   ret = gpio_add_callback(button_spec.gpio_spec.port, &button_cb_data);
   return ret;
}

static void ui_led_timer_expiry_fn(struct k_work *work)
{
   if (&led_red_timer_work.work == work) {
      ui_led_op(LED_COLOR_RED, LED_CLEAR);
   } else if (&led_green_timer_work.work == work) {
      ui_led_op(LED_COLOR_GREEN, LED_CLEAR);
   } else if (&led_blue_timer_work.work == work) {
      ui_led_op(LED_COLOR_BLUE, LED_CLEAR);
   }
}

static void ui_op(gpio_device_t *output_spec, led_op_t op, struct k_work_delayable *timer)
{
   k_mutex_lock(&ui_mutex, K_FOREVER);
   if (timer) {
      k_work_cancel_delayable(timer);
   }
   if (output_spec != NULL && output_spec->init) {
      switch (op) {
         case LED_SET:
            gpio_pin_set_dt(&output_spec->gpio_spec, 1);
            break;
         case LED_CLEAR:
            gpio_pin_set_dt(&output_spec->gpio_spec, 0);
            break;
         case LED_TOGGLE:
            gpio_pin_toggle_dt(&output_spec->gpio_spec);
            break;
         case LED_BLINK:
            if (timer) {
               gpio_pin_set_dt(&output_spec->gpio_spec, 1);
               work_schedule_for_io_queue(timer, K_MSEC(500));
            }
            break;
      }
   }
   k_mutex_unlock(&ui_mutex);
}

void ui_led_op(led_t led, led_op_t op)
{
   switch (led) {
      case LED_NONE:
         break;
      case LED_COLOR_RED:
         ui_op(&led_red_spec, op, &led_red_timer_work);
         break;
      case LED_COLOR_BLUE:
         ui_op(&led_blue_spec, op, &led_blue_timer_work);
         break;
      case LED_COLOR_GREEN:
         ui_op(&led_green_spec, op, &led_green_timer_work);
         break;
      case LED_LTE_1:
#if DT_NODE_HAS_STATUS(OUT_LTE_NODE_1, okay)
         ui_op(&out_lte_1_spec, op, NULL);
#endif
         break;
      case LED_LTE_2:
#if DT_NODE_HAS_STATUS(OUT_LTE_NODE_2, okay)
         ui_op(&out_lte_2_spec, op, NULL);
#endif
         break;
      case LED_LTE_3:
#if DT_NODE_HAS_STATUS(OUT_LTE_NODE_3, okay)
         ui_op(&out_lte_3_spec, op, NULL);
#endif
         break;
   }
}

static int ui_init_output(gpio_device_t *output_spec)
{
   int ret = -ENOTSUP;
   if (output_spec && device_is_ready(output_spec->gpio_spec.port)) {
      ret = gpio_pin_configure_dt(&output_spec->gpio_spec, GPIO_OUTPUT_ACTIVE);
      if (!ret) {
         gpio_pin_set_dt(&output_spec->gpio_spec, 0);
         output_spec->init = true;
      }
   }
   return ret;
}

int ui_init(ui_callback_handler_t button_handler)
{
   int ret;
   LOG_INF("UI init.");

   ret = ui_init_output(&led_red_spec);
   if (ret) {
      LOG_INF("UI init: LED red failed! %d", ret);
   }
   ret = ui_init_output(&led_green_spec);
   if (ret) {
      LOG_INF("UI init: LED green failed! %d", ret);
   }
   ret = ui_init_output(&led_blue_spec);
   if (ret) {
      LOG_INF("UI init: LED blue failed! %d", ret);
   }

#if DT_NODE_HAS_STATUS(OUT_LTE_NODE_1, okay)
   ret = ui_init_output(&out_lte_1_spec);
   if (ret) {
      LOG_INF("UI init: OUT LTE 1 failed! %d", ret);
   }
#endif
#if DT_NODE_HAS_STATUS(OUT_LTE_NODE_2, okay)
   ret = ui_init_output(&out_lte_2_spec);
   if (ret) {
      LOG_INF("UI init: OUT LTE 2 failed! %d", ret);
   }
#endif
#if DT_NODE_HAS_STATUS(OUT_LTE_NODE_3, okay)
   ret = ui_init_output(&out_lte_3_spec);
   if (ret) {
      LOG_INF("UI init: OUT LTE 3 failed! %d", ret);
   }
#endif

   button_callback = button_handler;

   ret = ui_init_button();
   if (ret) {
      LOG_INF("UI init: call button failed! %d", ret);
   }

#if DT_NODE_HAS_STATUS(CONFIG_BUTTON_NODE_1, okay) && DT_NODE_HAS_STATUS(CONFIG_SWITCH_NODE_1, okay) && DT_NODE_HAS_STATUS(CONFIG_SWITCH_NODE_2, okay)
   ret = ui_init_input(&config_button_1_spec);
   if (ret) {
      LOG_INF("UI init: button 1 failed! %d", ret);
   }
   ret = ui_init_input(&config_switch_1_spec);
   if (ret) {
      LOG_INF("UI init: switch failed! %d", ret);
   }
   ret = ui_init_input(&config_switch_2_spec);
   if (ret) {
      LOG_INF("UI init: switch failed! %d", ret);
   }
#endif
   return 0;
}

int ui_config(void)
{
#if DT_NODE_HAS_STATUS(CONFIG_BUTTON_NODE_1, okay) && DT_NODE_HAS_STATUS(CONFIG_SWITCH_NODE_1, okay) && DT_NODE_HAS_STATUS(CONFIG_SWITCH_NODE_2, okay)
   if (button_spec.init &&
       config_button_1_spec.init &&
       config_switch_1_spec.init &&
       config_switch_2_spec.init) {
      int pin1 = gpio_pin_get_dt(&button_spec.gpio_spec);
      int pin2 = gpio_pin_get_dt(&config_button_1_spec.gpio_spec);
      int pin3 = gpio_pin_get_dt(&config_switch_1_spec.gpio_spec);
      int pin4 = gpio_pin_get_dt(&config_switch_2_spec.gpio_spec);
      if (pin1 >= 0 && pin2 >= 0 && pin3 >= 0 && pin4 >= 0) {
         return pin4 << 3 | pin3 << 2 | pin2 << 1 | pin1;
      }
   }
#endif
   return -1;
}
