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

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "io_job_queue.h"
#include "ui.h"

LOG_MODULE_REGISTER(UI, CONFIG_UI_LOG_LEVEL);

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

#define BUTTON_LONG_MS 5000
#define BUTTON_DITHER_MS 1000
#define BUTTON_DEBOUNCE_MS 25
#define BUTTON_DEBOUNCE_ON_MS 50
#define BUTTON_DEBOUNCE_OFF_MS 500

#if (!DT_NODE_HAS_STATUS(CALL_BUTTON_NODE, okay))
/* A build error here means your board isn't set up to for sw0 (call button). */
#error "Unsupported board: sw0 devicetree alias is not defined"
#endif

typedef struct gpio_device {
   const char *desc;
   const struct gpio_dt_spec gpio_spec;
   bool init;
} gpio_device_t;

typedef struct gpio_device_ext {
   const char *desc;
   const struct gpio_dt_spec gpio_spec;
   bool init;
   led_op_t op;
} gpio_device_ext_t;

#define GPIO_DEVICE_INIT(NODE)                 \
   {                                           \
      "", GPIO_DT_SPEC_GET(NODE, gpios), false \
   }

#define GPIO_NAMED_DEVICE_INIT(NAME, NODE)       \
   {                                             \
      NAME, GPIO_DT_SPEC_GET(NODE, gpios), false \
   }

#define GPIO_NAMED_DEVICE_INIT_EXT(NAME, NODE)              \
   {                                                        \
      NAME, GPIO_DT_SPEC_GET(NODE, gpios), false, LED_CLEAR \
   }

#if DT_NODE_HAS_STATUS(CONFIG_BUTTON_NODE_1, okay) && DT_NODE_HAS_STATUS(CONFIG_SWITCH_NODE_1, okay) && DT_NODE_HAS_STATUS(CONFIG_SWITCH_NODE_2, okay)
static gpio_device_t config_button_1_spec = GPIO_DEVICE_INIT(CONFIG_BUTTON_NODE_1);
static gpio_device_t config_switch_1_spec = GPIO_DEVICE_INIT(CONFIG_SWITCH_NODE_1);
static gpio_device_t config_switch_2_spec = GPIO_DEVICE_INIT(CONFIG_SWITCH_NODE_2);
#endif

#if (DT_NODE_HAS_STATUS(LED_RED_NODE, okay))
static gpio_device_ext_t led_red_spec = GPIO_NAMED_DEVICE_INIT_EXT("red ", LED_RED_NODE);
#endif
#if (DT_NODE_HAS_STATUS(LED_GREEN_NODE, okay))
static gpio_device_ext_t led_green_spec = GPIO_NAMED_DEVICE_INIT_EXT("green ", LED_GREEN_NODE);
#endif
#if (DT_NODE_HAS_STATUS(LED_BLUE_NODE, okay))
static gpio_device_ext_t led_blue_spec = GPIO_NAMED_DEVICE_INIT_EXT("blue ", LED_BLUE_NODE);
#endif
#if DT_NODE_HAS_STATUS(OUT_LTE_NODE_1, okay)
static gpio_device_ext_t out_lte_1_spec = GPIO_NAMED_DEVICE_INIT_EXT("lte1 ", OUT_LTE_NODE_1);
#endif
#if DT_NODE_HAS_STATUS(OUT_LTE_NODE_2, okay)
static gpio_device_ext_t out_lte_2_spec = GPIO_NAMED_DEVICE_INIT_EXT("lte2 ", OUT_LTE_NODE_2);
#endif
#if DT_NODE_HAS_STATUS(OUT_LTE_NODE_3, okay)
static gpio_device_ext_t out_lte_3_spec = GPIO_NAMED_DEVICE_INIT_EXT("lte3 ", OUT_LTE_NODE_3);
#endif

static gpio_device_t button_spec = GPIO_DEVICE_INIT(CALL_BUTTON_NODE);

static struct gpio_callback button_cb_data;
static ui_callback_handler_t button_callback;
static volatile bool button_active;
static volatile int button_counter;

static void ui_led_timer_expiry_fn(struct k_work *work);
static void ui_button_handle_fn(struct k_work *work);
static void ui_button_enable_interrupt_fn(struct k_work *work);

static K_WORK_DEFINE(button_work, ui_button_handle_fn);
static K_WORK_DELAYABLE_DEFINE(button_timer_work, ui_button_handle_fn);
static K_WORK_DELAYABLE_DEFINE(button_enable_interrupt_work, ui_button_enable_interrupt_fn);

#if (DT_NODE_HAS_STATUS(LED_RED_NODE, okay))
static K_WORK_DELAYABLE_DEFINE(led_red_timer_work, ui_led_timer_expiry_fn);
#endif
#if (DT_NODE_HAS_STATUS(LED_GREEN_NODE, okay))
static K_WORK_DELAYABLE_DEFINE(led_green_timer_work, ui_led_timer_expiry_fn);
#endif
#if (DT_NODE_HAS_STATUS(LED_BLUE_NODE, okay))
static K_WORK_DELAYABLE_DEFINE(led_blue_timer_work, ui_led_timer_expiry_fn);
#endif

static K_MUTEX_DEFINE(ui_mutex);
static K_SEM_DEFINE(ui_input_trigger, 0, 1);
static volatile int ui_input_duration = 0;

static volatile bool ui_enabled = true;

struct ui_fifo {
   void *fifo_reserved;
   int counter;
};

static K_FIFO_DEFINE(ui_call_button_fifo);
static K_HEAP_DEFINE(ui_heap, 256);

static void ui_button_handle_fn(struct k_work *work)
{
   static int64_t last = 0;
   static int64_t start = 0;
   static int short_pressed = 0;
   static unsigned int counter = 0;

   int64_t now = k_uptime_get();

   if (&button_work == work) {
      struct ui_fifo *ui_notif = k_fifo_get(&ui_call_button_fifo, K_NO_WAIT);
      if (ui_notif) {
         bool pressed = ui_notif->counter > 0;
         counter = (pressed) ? ui_notif->counter : -ui_notif->counter;
         k_heap_free(&ui_heap, ui_notif);
         if (pressed) {
            LOG_INF("UI button pressed #%u", counter);
            ui_input_duration = 1;
            start = now;
            work_reschedule_for_io_queue(&button_timer_work, K_MSEC(BUTTON_LONG_MS));
         } else {
            int64_t time = now - start;
            LOG_INF("UI button released #%u-%d, %d ms on.", counter, ui_input_duration, (int)time);
            k_work_cancel_delayable(&button_timer_work);
            if (ui_input_duration == 1) {
               if (time > BUTTON_DEBOUNCE_ON_MS || short_pressed) {
                  short_pressed = 1;
                  ui_input_duration = 2;
                  work_reschedule_for_io_queue(&button_timer_work, K_MSEC(BUTTON_DEBOUNCE_OFF_MS));
               } else {
                  LOG_INF("UI button on ignored.");
                  ui_input_duration = 0;
               }
            }
         }
      }
   } else if (&button_timer_work.work == work) {
      short_pressed = 0;
      if (ui_input_duration == 1) {
         LOG_INF("UI button long pressed #%u-%d", counter, ui_input_duration);
         last = now;
         ui_input_duration = 3;
         ui_enable(true);
         ui_led_op(LED_COLOR_BLUE, LED_BLINK);
         ui_led_op(LED_COLOR_GREEN, LED_BLINK);
         ui_led_op(LED_COLOR_RED, LED_BLINK);
         if (button_callback != NULL) {
            button_callback(1);
            LOG_DBG("UI button long callback %u", button_counter);
         }
         k_sem_give(&ui_input_trigger);
      } else if (ui_input_duration == 2) {
         int64_t time = now - last;
         LOG_INF("UI button short pressed #%u-%d, %d ms off.", counter, ui_input_duration, (int)time);
         if (time > BUTTON_DITHER_MS) {
            last = now;
            ui_enable(true);
            ui_led_op(LED_COLOR_BLUE, LED_TOGGLE);
            if (button_callback != NULL) {
               button_callback(0);
               LOG_DBG("UI button callback %u", button_counter);
            }
            k_sem_give(&ui_input_trigger);
         } else {
            LOG_INF("UI button off ignored.");
         }
      }
   }
}

static void ui_button_enable_interrupt_fn(struct k_work *work)
{
   //   LOG_DBG("UI button enable interrupt %d", button_active);
   if (button_active) {
      gpio_pin_interrupt_configure_dt(&button_spec.gpio_spec, GPIO_INT_LEVEL_INACTIVE);
   } else {
      gpio_pin_interrupt_configure_dt(&button_spec.gpio_spec, GPIO_INT_LEVEL_ACTIVE);
   }
}

static void ui_button_pressed(const struct device *dev, struct gpio_callback *cb,
                              uint32_t pins)
{
   struct ui_fifo *ui_notif;

   if ((BIT(button_spec.gpio_spec.pin) & pins) == 0) {
      return;
   }

   //   LOG_DBG("UI button disable interrupt %d", button_active);
   gpio_pin_interrupt_configure_dt(&button_spec.gpio_spec, GPIO_INT_DISABLE);
   if (button_active) {
      button_active = false;
   } else {
      button_active = true;
      ++button_counter;
   }
   work_reschedule_for_io_queue(&button_enable_interrupt_work, K_MSEC(BUTTON_DEBOUNCE_MS));
   ui_notif = k_heap_alloc(&ui_heap, sizeof(struct ui_fifo), K_NO_WAIT);
   if (ui_notif) {
      ui_notif->counter = button_active ? button_counter : -button_counter;
      k_fifo_put(&ui_call_button_fifo, ui_notif);
      work_submit_to_io_queue(&button_work);
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
   k_work_cancel_delayable(&button_enable_interrupt_work);

   gpio_init_callback(&button_cb_data, ui_button_pressed, BIT(button_spec.gpio_spec.pin));
   ret = gpio_add_callback(button_spec.gpio_spec.port, &button_cb_data);
   if (ret < 0) {
      return ret;
   }

   ret = gpio_pin_interrupt_configure_dt(&button_spec.gpio_spec, GPIO_INT_LEVEL_ACTIVE);

   return ret;
}

static void ui_op(gpio_device_ext_t *output_spec, led_op_t op, struct k_work_delayable *timer)
{
   k_mutex_lock(&ui_mutex, K_FOREVER);
   if (timer) {
      k_work_cancel_delayable(timer);
   }
   if (output_spec != NULL && output_spec->init) {
      const struct gpio_dt_spec *gpio_spec = &output_spec->gpio_spec;
      switch (op) {
         case LED_SET:
            if (output_spec->op != op) {
               gpio_pin_set_dt(gpio_spec, 1);
               LOG_DBG("UI: %sLED set", output_spec->desc);
            }
            break;
         case LED_CLEAR:
            if (output_spec->op != op) {
               gpio_pin_set_dt(gpio_spec, 0);
               LOG_DBG("UI: %sLED clear", output_spec->desc);
            }
            break;
         case LED_TOGGLE:
            gpio_pin_toggle_dt(gpio_spec);
            LOG_DBG("UI: %sLED toggle", output_spec->desc);
            break;
         case LED_BLINK:
            if (timer) {
               gpio_pin_set_dt(gpio_spec, 1);
               work_reschedule_for_io_queue(timer, K_MSEC(500));
               LOG_DBG("UI: %sLED blink", output_spec->desc);
            }
            break;
         case LED_BLINKING:
            if (timer) {
               gpio_pin_set_dt(gpio_spec, 1);
               work_reschedule_for_io_queue(timer, K_MSEC(300));
               LOG_DBG("UI: %sLED start blinking", output_spec->desc);
            }
            break;
         case LED_INTERNAL_TIMER:
            if (timer && output_spec->op == LED_BLINKING) {
               gpio_pin_toggle_dt(gpio_spec);
               work_reschedule_for_io_queue(timer, K_MSEC(300));
               LOG_DBG("UI: %sLED blinking", output_spec->desc);
               op = LED_BLINKING;
            } else {
               gpio_pin_set_dt(gpio_spec, 0);
               op = LED_CLEAR;
            }
            break;
      }
      output_spec->op = op;
   }
   k_mutex_unlock(&ui_mutex);
}

static void ui_led_timer_expiry_fn(struct k_work *work)
{
#if (DT_NODE_HAS_STATUS(LED_RED_NODE, okay))
   if (&led_red_timer_work.work == work) {
      ui_op(&led_red_spec, LED_INTERNAL_TIMER, &led_red_timer_work);
      return;
   }
#endif
#if (DT_NODE_HAS_STATUS(LED_GREEN_NODE, okay))
   if (&led_green_timer_work.work == work) {
      ui_op(&led_green_spec, LED_INTERNAL_TIMER, &led_green_timer_work);
      return;
   }
#endif
#if (DT_NODE_HAS_STATUS(LED_BLUE_NODE, okay))
   if (&led_blue_timer_work.work == work) {
      ui_op(&led_blue_spec, LED_INTERNAL_TIMER, &led_blue_timer_work);
      return;
   }
#endif
}

int ui_led_op(led_t led, led_op_t op)
{
   if (!ui_enabled) {
      return -EACCES;
   }
   switch (led) {
      case LED_NONE:
         break;
      case LED_COLOR_ALL:
         ui_led_op(LED_COLOR_RED, op);
         ui_led_op(LED_COLOR_BLUE, op);
         ui_led_op(LED_COLOR_GREEN, op);
         break;
      case LED_COLOR_RED:
#if (DT_NODE_HAS_STATUS(LED_RED_NODE, okay))
         ui_op(&led_red_spec, op, &led_red_timer_work);
#endif
         break;
      case LED_COLOR_BLUE:
#if (DT_NODE_HAS_STATUS(LED_BLUE_NODE, okay))
         ui_op(&led_blue_spec, op, &led_blue_timer_work);
#endif
         break;
      case LED_COLOR_GREEN:
#if (DT_NODE_HAS_STATUS(LED_GREEN_NODE, okay))
         ui_op(&led_green_spec, op, &led_green_timer_work);
#endif
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
   return 0;
}

static int ui_init_output(gpio_device_ext_t *output_spec)
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

#if (DT_NODE_HAS_STATUS(LED_RED_NODE, okay))
   ret = ui_init_output(&led_red_spec);
   if (ret) {
      LOG_INF("UI init: LED red failed! %d", ret);
   }
#endif
#if (DT_NODE_HAS_STATUS(LED_GREEN_NODE, okay))
   ret = ui_init_output(&led_green_spec);
   if (ret) {
      LOG_INF("UI init: LED green failed! %d", ret);
   }
#endif
#if (DT_NODE_HAS_STATUS(LED_BLUE_NODE, okay))
   ret = ui_init_output(&led_blue_spec);
   if (ret) {
      LOG_INF("UI init: LED blue failed! %d", ret);
   }
#endif
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

int ui_enable(bool enable)
{
   ui_enabled = enable;
   return 0;
}

int ui_input(k_timeout_t timeout)
{
   int rc = 0;

   ui_input_duration = 0;
   k_sem_reset(&ui_input_trigger);

   while (true) {
      rc = k_sem_take(&ui_input_trigger, timeout);
      if (rc == -EAGAIN || !rc) {
         if (!ui_input_duration) {
            LOG_INF("UI input timeout");
            break;
         }
         if (!rc && ui_input_duration > 1) {
            rc = ui_input_duration - 2;
            ui_input_duration = 0;
            LOG_INF("UI input duration %d", rc);
            break;
         }
      }
      LOG_INF("UI input continue");
   }
   return rc;
}
