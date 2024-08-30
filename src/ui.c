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
#ifdef CONFIG_LED
#include <zephyr/drivers/led.h>
#endif
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "power_manager.h"
// #include "appl_diagnose.h"
#include "io_job_queue.h"
#include "parse.h"
#include "sh_cmd.h"
#include "ui.h"

LOG_MODULE_REGISTER(UI, CONFIG_UI_LOG_LEVEL);

#define LED_RED_NODE DT_ALIAS(led0)
#define LED_GREEN_NODE DT_ALIAS(led1)
#define LED_BLUE_NODE DT_ALIAS(led2)

#define LED_MULTI_NODE DT_ALIAS(multi_leds)

#define OUT_LTE_NODE_1 DT_ALIAS(led3)
#define OUT_LTE_NODE_2 DT_ALIAS(out1)
#define OUT_LTE_NODE_3 DT_ALIAS(out2)

#define CALL_BUTTON_NODE DT_ALIAS(sw0)

#define CONFIG_BUTTON_NODE_1 DT_ALIAS(sw1)
#define CONFIG_SWITCH_NODE_1 DT_ALIAS(sw2)
#define CONFIG_SWITCH_NODE_2 DT_ALIAS(sw3)

#define BUTTON_LONG_MS 5000
#define BUTTON_DEBOUNCE_MS 50
#define BUTTON_MIN_PAUSE_MS 1000

#define LED_BLINK_MS 500
#define LED_BLINKING_MS 300

#if (!DT_NODE_HAS_STATUS(CALL_BUTTON_NODE, okay))
#warning "Board without call-button: sw0 alias is not defined in devicetree"
#endif

typedef struct gpio_device {
   const char *desc;
   const struct gpio_dt_spec gpio_spec;
   bool init;
} gpio_device_t;

typedef struct gpio_device_ext {
   const char *desc;
   const struct gpio_dt_spec gpio_spec;
   bool gpio;
   bool on;
   bool init;
   led_op_t op;
} gpio_device_ext_t;

#define GPIO_DEVICE_INIT(NODE) \
   {                           \
       "", GPIO_DT_SPEC_GET(NODE, gpios), false}

#define GPIO_NAMED_DEVICE_INIT(NAME, NODE) \
   {                                       \
       NAME, GPIO_DT_SPEC_GET(NODE, gpios), false}

#define GPIO_NAMED_DEVICE_INIT_EXT(NAME, NODE) \
   {                                           \
       NAME, GPIO_DT_SPEC_GET(NODE, gpios), true, false, false, LED_CLEAR}

#ifdef CONFIG_LED
#if (DT_NODE_HAS_STATUS(LED_MULTI_NODE, okay))
#define GPIO_NAMED_DEVICE_INIT_PMIC(NAME, NODE, PIN) \
   {                                                 \
       NAME, {                                       \
                 .port = DEVICE_DT_GET(NODE),        \
                 .pin = PIN,                         \
                 .dt_flags = 0,                      \
             },                                      \
       false,                                        \
       false,                                        \
       false,                                        \
       LED_CLEAR}
#else /* DT_NODE_HAS_STATUS(LED_MULTI_NODE, okay) */
#undef CONFIG_LED
#endif /* DT_NODE_HAS_STATUS(LED_MULTI_NODE, okay) */
#endif /* CONFIG_LED */

#if DT_NODE_HAS_STATUS(CONFIG_BUTTON_NODE_1, okay) && DT_NODE_HAS_STATUS(CONFIG_SWITCH_NODE_1, okay) && DT_NODE_HAS_STATUS(CONFIG_SWITCH_NODE_2, okay)
static gpio_device_t config_button_1_spec = GPIO_DEVICE_INIT(CONFIG_BUTTON_NODE_1);
static gpio_device_t config_switch_1_spec = GPIO_DEVICE_INIT(CONFIG_SWITCH_NODE_1);
static gpio_device_t config_switch_2_spec = GPIO_DEVICE_INIT(CONFIG_SWITCH_NODE_2);
#endif

#if (DT_NODE_HAS_STATUS(LED_RED_NODE, okay))
#define UI_LED
#define UI_RED
static gpio_device_ext_t led_red_spec = GPIO_NAMED_DEVICE_INIT_EXT("red ", LED_RED_NODE);
#elif defined(CONFIG_LED)
#define UI_LED
#define UI_RED
// led 0 of nRF9161 feather
static gpio_device_ext_t led_red_spec = GPIO_NAMED_DEVICE_INIT_PMIC("red ", LED_MULTI_NODE, 0);
#endif

#if (DT_NODE_HAS_STATUS(LED_GREEN_NODE, okay))
#define UI_LED
#define UI_GREEN
static gpio_device_ext_t led_green_spec = GPIO_NAMED_DEVICE_INIT_EXT("green ", LED_GREEN_NODE);
#elif defined(CONFIG_LED)
#define UI_LED
#define UI_GREEN
// led 1 of nRF9161 feather
static gpio_device_ext_t led_green_spec = GPIO_NAMED_DEVICE_INIT_PMIC("green ", LED_MULTI_NODE, 1);
#endif

#if (DT_NODE_HAS_STATUS(LED_BLUE_NODE, okay))
#define UI_LED
#define UI_BLUE
static gpio_device_ext_t led_blue_spec = GPIO_NAMED_DEVICE_INIT_EXT("blue ", LED_BLUE_NODE);
#elif defined(CONFIG_LED)
#define UI_LED
#define UI_BLUE
// led 2 of nRF9161 feather
static gpio_device_ext_t led_blue_spec = GPIO_NAMED_DEVICE_INIT_PMIC("blue ", LED_MULTI_NODE, 2);
#endif

#ifndef UI_LED
#warning "Board without LEDS: no led0, led1, nor led2 alias defined in devicetree"
#endif

#if DT_NODE_HAS_STATUS(OUT_LTE_NODE_1, okay)
#define UI_OUT
static gpio_device_ext_t out_lte_1_spec = GPIO_NAMED_DEVICE_INIT_EXT("lte1 ", OUT_LTE_NODE_1);
#endif
#if DT_NODE_HAS_STATUS(OUT_LTE_NODE_2, okay)
#define UI_OUT
static gpio_device_ext_t out_lte_2_spec = GPIO_NAMED_DEVICE_INIT_EXT("lte2 ", OUT_LTE_NODE_2);
#endif
#if DT_NODE_HAS_STATUS(OUT_LTE_NODE_3, okay)
#define UI_OUT
static gpio_device_ext_t out_lte_3_spec = GPIO_NAMED_DEVICE_INIT_EXT("lte3 ", OUT_LTE_NODE_3);
#endif

#if (DT_NODE_HAS_STATUS(CALL_BUTTON_NODE, okay))
#define UI_IN
static gpio_device_t button_spec = GPIO_DEVICE_INIT(CALL_BUTTON_NODE);

static struct gpio_callback button_cb_data;
static ui_callback_handler_t button_callback;
static volatile int button_active;
static volatile int button_counter;

static void ui_button_handle_fn(struct k_work *work);
static void ui_button_enable_interrupt_fn(struct k_work *work);

static K_WORK_DEFINE(button_work, ui_button_handle_fn);
static K_WORK_DELAYABLE_DEFINE(button_timer_work, ui_button_handle_fn);
static K_WORK_DELAYABLE_DEFINE(button_enable_interrupt_work, ui_button_enable_interrupt_fn);
#endif

#if defined(UI_LED)
static void ui_led_timer_expiry_fn(struct k_work *work);
#endif /* UI_LED */

#ifdef UI_RED
static K_WORK_DELAYABLE_DEFINE(led_red_timer_work, ui_led_timer_expiry_fn);
#endif
#ifdef UI_GREEN
static K_WORK_DELAYABLE_DEFINE(led_green_timer_work, ui_led_timer_expiry_fn);
#endif
#ifdef UI_BLUE
static K_WORK_DELAYABLE_DEFINE(led_blue_timer_work, ui_led_timer_expiry_fn);
#endif

static volatile bool ui_enabled = true;
static volatile bool ui_prio_mode = false;

#if (DT_NODE_HAS_STATUS(CALL_BUTTON_NODE, okay))

static K_MUTEX_DEFINE(ui_mutex);
static K_SEM_DEFINE(ui_input_trigger, 1, 1);
static volatile int ui_input_duration = 0;

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
               int64_t time = now - last;
               LOG_INF("UI button short pressed #%u-%d, %d ms off.", counter, ui_input_duration, (int)time);
               if (time > BUTTON_MIN_PAUSE_MS) {
                  ui_input_duration = 2;
                  last = now;
                  ui_enable(true);
                  if (!ui_prio_mode) {
                     ui_led_op(LED_COLOR_BLUE, LED_TOGGLE);
                     if (button_callback != NULL) {
                        button_callback(0);
                        LOG_DBG("UI button callback %u", button_counter);
                     }
                  }
                  k_sem_give(&ui_input_trigger);
               } else {
                  LOG_INF("UI button ignored, pause too short.");
               }
            }
         }
      }
   } else if (&button_timer_work.work == work) {
      if (ui_input_duration == 1) {
         LOG_INF("UI button long pressed #%u-%d", counter, ui_input_duration);
         last = now;
         ui_input_duration = 3;
         ui_enable(true);
         if (!ui_prio_mode) {
            ui_led_op(LED_COLOR_BLUE, LED_BLINK);
            ui_led_op(LED_COLOR_GREEN, LED_BLINK);
            ui_led_op(LED_COLOR_RED, LED_BLINK);
            if (button_callback != NULL) {
               button_callback(1);
               LOG_DBG("UI button long callback %u", button_counter);
            }
         }
         k_sem_give(&ui_input_trigger);
      }
   }
}

static void ui_button_enable_interrupt_fn(struct k_work *work)
{
   //   LOG_DBG("UI button enable interrupt %d", button_active);
   int button = gpio_pin_get_dt(&button_spec.gpio_spec);
   if (button_active != button) {
      // stable signal
      ++button_counter;
      LOG_DBG("UI button %d/%d", button, button_counter);
      struct ui_fifo *ui_notif = k_heap_alloc(&ui_heap, sizeof(struct ui_fifo), K_NO_WAIT);
      if (ui_notif) {
         ui_notif->counter = button ? button_counter : -button_counter;
         k_fifo_put(&ui_call_button_fifo, ui_notif);
         work_submit_to_io_queue(&button_work);
      }
   } else {
      LOG_DBG("UI button ignored, instable %d/%d", button, button_counter);
   }
   // enable interrupt again
   button_active = button;
   gpio_pin_interrupt_configure_dt(&button_spec.gpio_spec,
                                   button ? GPIO_INT_LEVEL_INACTIVE : GPIO_INT_LEVEL_ACTIVE);
}

static void ui_button_pressed(const struct device *dev, struct gpio_callback *cb,
                              uint32_t pins)
{
   int res;
   if ((BIT(button_spec.gpio_spec.pin) & pins) == 0) {
      return;
   }
   LOG_DBG("UI button disable interrupt");
   gpio_pin_interrupt_configure_dt(&button_spec.gpio_spec, GPIO_INT_DISABLE);
   res = work_reschedule_for_io_queue(&button_enable_interrupt_work, K_MSEC(BUTTON_DEBOUNCE_MS));
   if (res != 1) {
      LOG_WRN("UI button failed: %d", res);
   }
}
#endif

#if (DT_NODE_HAS_STATUS(CONFIG_BUTTON_NODE_1, okay) && DT_NODE_HAS_STATUS(CONFIG_SWITCH_NODE_1, okay) && DT_NODE_HAS_STATUS(CONFIG_SWITCH_NODE_2, okay)) || (DT_NODE_HAS_STATUS(CALL_BUTTON_NODE, okay))
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
#endif

#if (DT_NODE_HAS_STATUS(CALL_BUTTON_NODE, okay))
static int ui_init_button(void)
{
   int ret = ui_init_input(&button_spec);
   if (ret < 0) {
      return ret;
   }
   button_counter = 0;
   button_active = gpio_pin_get_dt(&button_spec.gpio_spec);
   k_work_cancel_delayable(&button_enable_interrupt_work);

   gpio_init_callback(&button_cb_data, ui_button_pressed, BIT(button_spec.gpio_spec.pin));
   ret = gpio_add_callback(button_spec.gpio_spec.port, &button_cb_data);
   if (ret < 0) {
      return ret;
   }

   ret = gpio_pin_interrupt_configure_dt(&button_spec.gpio_spec, GPIO_INT_LEVEL_ACTIVE);

   return ret;
}
#endif

#if defined(UI_LED) || defined(UI_OU)

#ifdef CONFIG_LED

static int ui_led_on(gpio_device_ext_t *output_spec)
{
   int rc = led_on(output_spec->gpio_spec.port, output_spec->gpio_spec.pin);
   if (!rc) {
      output_spec->on = true;
   }
   return rc;
}

static int ui_led_off(gpio_device_ext_t *output_spec)
{
   int rc = led_off(output_spec->gpio_spec.port, output_spec->gpio_spec.pin);
   if (!rc) {
      output_spec->on = false;
   }
   return rc;
}

static int ui_led_toggle(gpio_device_ext_t *output_spec)
{
   if (output_spec->on) {
      return ui_led_off(output_spec);
   } else {
      return ui_led_on(output_spec);
   }
}
#else /* CONFIG_LED */
static inline int ui_led_on(gpio_device_ext_t *output_spec)
{
   (void)output_spec;
   // empty by intention
   return -ENOTSUP;
}

static inline int ui_led_off(gpio_device_ext_t *output_spec)
{
   (void)output_spec;
   // empty by intention
   return -ENOTSUP;
}

static inline int ui_led_toggle(gpio_device_ext_t *output_spec)
{
   (void)output_spec;
   // empty by intention
   return -ENOTSUP;
}

#endif /* CONFIG_LED */

static void ui_op(gpio_device_ext_t *output_spec, led_op_t op, struct k_work_delayable *timer)
{
   k_mutex_lock(&ui_mutex, K_FOREVER);
   if (timer) {
      k_work_cancel_delayable(timer);
   }
   if (output_spec != NULL && output_spec->init) {
      const struct gpio_dt_spec *gpio_spec = &output_spec->gpio_spec;
      bool gpio = output_spec->gpio;
      switch (op) {
         case LED_SET:
            if (output_spec->op != op) {
               if (gpio) {
                  gpio_pin_set_dt(gpio_spec, 1);
               } else {
                  ui_led_on(output_spec);
               }
               LOG_DBG("UI: %sLED set", output_spec->desc);
            }
            break;
         case LED_CLEAR:
            if (output_spec->op != op) {
               if (gpio) {
                  gpio_pin_set_dt(gpio_spec, 0);
               } else {
                  ui_led_off(output_spec);
               }
               LOG_DBG("UI: %sLED clear", output_spec->desc);
            }
            break;
         case LED_TOGGLE:
            if (gpio) {
               gpio_pin_toggle_dt(gpio_spec);
            } else {
               ui_led_toggle(output_spec);
            }
            LOG_DBG("UI: %sLED toggle", output_spec->desc);
            break;
         case LED_BLINK:
            if (timer) {
               if (gpio) {
                  gpio_pin_set_dt(gpio_spec, 1);
               } else {
                  ui_led_on(output_spec);
               }
               work_reschedule_for_io_queue(timer, K_MSEC(LED_BLINK_MS));
               LOG_DBG("UI: %sLED blink", output_spec->desc);
            }
            break;
         case LED_BLINKING:
            if (timer) {
               if (gpio) {
                  gpio_pin_set_dt(gpio_spec, 1);
               } else {
                  ui_led_on(output_spec);
               }
               work_reschedule_for_io_queue(timer, K_MSEC(LED_BLINKING_MS));
               LOG_DBG("UI: %sLED start blinking", output_spec->desc);
            }
            break;
         case LED_INTERNAL_TIMER:
            if (timer && output_spec->op == LED_BLINKING) {
               if (gpio) {
                  gpio_pin_toggle_dt(gpio_spec);
               } else {
                  ui_led_toggle(output_spec);
               }
               work_reschedule_for_io_queue(timer, K_MSEC(300));
               LOG_DBG("UI: %sLED blinking", output_spec->desc);
               op = LED_BLINKING;
            } else {
               if (gpio) {
                  gpio_pin_set_dt(gpio_spec, 0);
               } else {
                  ui_led_off(output_spec);
               }
               op = LED_CLEAR;
            }
            break;
      }
      output_spec->op = op;
   }
   k_mutex_unlock(&ui_mutex);
}

#if defined(UI_LED)

static void ui_led_timer_expiry_fn(struct k_work *work)
{
#ifdef UI_RED
   if (&led_red_timer_work.work == work) {
      ui_op(&led_red_spec, LED_INTERNAL_TIMER, &led_red_timer_work);
      return;
   }
#endif
#ifdef UI_GREEN
   if (&led_green_timer_work.work == work) {
      ui_op(&led_green_spec, LED_INTERNAL_TIMER, &led_green_timer_work);
      return;
   }
#endif

#ifdef UI_BLUE
   if (&led_blue_timer_work.work == work) {
      ui_op(&led_blue_spec, LED_INTERNAL_TIMER, &led_blue_timer_work);
      return;
   }
#endif
}

static const led_task_t *ui_led_task = NULL;
static const led_task_t *ui_led_loop = NULL;
static uint16_t ui_led_loop_counter = 0;
static void ui_led_task_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(ui_led_task_work, ui_led_task_fn);

static void ui_led_task_fn(struct k_work *work)
{
   led_task_t task = {.led = LED_NONE};

   k_mutex_lock(&ui_mutex, K_FOREVER);
   if (ui_led_task) {
      task = *ui_led_task;
      if (task.loop) {
         if (ui_led_loop != ui_led_task) {
            ui_led_loop = ui_led_task;
            ui_led_loop_counter = task.loop - 1;
         } else {
            ui_led_loop_counter--;
         }
      }
      if (task.time_ms) {
         ++ui_led_task;
         if ((ui_led_task->loop || !ui_led_task->time_ms) && ui_led_loop_counter) {
            ui_led_task = ui_led_loop;
            LOG_DBG("ui led task loop %u.", ui_led_loop_counter);
         } else {
            LOG_DBG("ui led task next.");
         }
      }
   }
   k_mutex_unlock(&ui_mutex);
   if (task.led != LED_NONE) {
      ui_led_op(task.led, task.op);
      if (task.time_ms) {
         power_manager_pulse(K_MSEC(task.time_ms + 100));
         work_reschedule_for_io_queue(&ui_led_task_work, K_MSEC(task.time_ms));
      } else {
         LOG_DBG("ui led task finished.");
      }
   }
}

#endif /* UI_LED */

static int ui_init_output(gpio_device_ext_t *output_spec)
{
   int ret = -ENOTSUP;
   if (output_spec && device_is_ready(output_spec->gpio_spec.port)) {
      if (output_spec->gpio) {
         ret = gpio_pin_configure_dt(&output_spec->gpio_spec, GPIO_OUTPUT_ACTIVE);
         if (!ret) {
            gpio_pin_set_dt(&output_spec->gpio_spec, 0);
            output_spec->init = true;
         }
      } else {
         ret = ui_led_off(output_spec);
         if (!ret) {
            output_spec->init = true;
         }
      }
   }
   return ret;
}
#endif /* UI_LED || UI_OU */

int ui_led_op(led_t led, led_op_t op)
{
   if (!ui_prio_mode) {
      return ui_led_op_prio(led, op);
   } else {
      return -EACCES;
   }
}

int ui_led_op_prio(led_t led, led_op_t op)
{
   if (!ui_enabled) {
      if (led == LED_COLOR_ALL || led == LED_COLOR_RED ||
          led == LED_COLOR_BLUE || led == LED_COLOR_GREEN) {
         return -EACCES;
      }
   }
   switch (led) {
      case LED_NONE:
         break;
      case LED_COLOR_ALL:
         ui_led_op_prio(LED_COLOR_RED, op);
         ui_led_op_prio(LED_COLOR_BLUE, op);
         ui_led_op_prio(LED_COLOR_GREEN, op);
         break;
      case LED_COLOR_RED:
#ifdef UI_RED
         ui_op(&led_red_spec, op, &led_red_timer_work);
#endif
         break;
      case LED_COLOR_BLUE:
#ifdef UI_BLUE
         ui_op(&led_blue_spec, op, &led_blue_timer_work);
#endif
         break;
      case LED_COLOR_GREEN:
#ifdef UI_GREEN
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

int ui_led_tasks(const led_task_t *tasks)
{
#if defined(UI_LED)
   k_mutex_lock(&ui_mutex, K_FOREVER);
   ui_led_task = tasks;
   ui_led_loop = NULL;
   ui_led_loop_counter = 0;
   k_mutex_unlock(&ui_mutex);
   ui_led_task_fn(NULL);
#endif
   return 0;
}

int ui_init(ui_callback_handler_t button_handler)
{
   int ret;
   LOG_INF("UI init.");

#ifdef UI_RED
   ret = ui_init_output(&led_red_spec);
   if (ret) {
      LOG_INF("UI init: LED red failed! %d", ret);
   }
#endif
#ifdef UI_GREEN
   ret = ui_init_output(&led_green_spec);
   if (ret) {
      LOG_INF("UI init: LED green failed! %d", ret);
   }
#endif
#ifdef UI_BLUE
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

#if (DT_NODE_HAS_STATUS(CALL_BUTTON_NODE, okay))
   button_callback = button_handler;
   ret = ui_init_button();
   if (ret) {
      LOG_INF("UI init: call button failed! %d", ret);
   }
#else
   (void)button_handler;
   ret = 0;
#endif

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

void ui_enable(bool enable)
{
   if (ui_enabled != enable) {
      if (!enable) {
         ui_led_op_prio(LED_COLOR_RED, LED_CLEAR);
         ui_led_op_prio(LED_COLOR_BLUE, LED_CLEAR);
         ui_led_op_prio(LED_COLOR_GREEN, LED_CLEAR);
      }
      ui_enabled = enable;
   }
}

void ui_prio(bool enable)
{
   ui_prio_mode = enable;
}

int ui_input(k_timeout_t timeout)
{
   int rc = 0;

#if (DT_NODE_HAS_STATUS(CALL_BUTTON_NODE, okay))
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
#else
   (void)timeout;
#endif

   return rc;
}

#ifdef CONFIG_SH_CMD

static void ui_finish_prio_fn(struct k_work *work)
{
   ui_led_op_prio(LED_COLOR_ALL, LED_CLEAR);
   ui_prio_mode = false;
}

static K_WORK_DELAYABLE_DEFINE(ui_finish_prio_work, ui_finish_prio_fn);

static int sh_cmd_led(const char *parameter)
{
   k_timeout_t timeout = K_NO_WAIT;
   led_t led = LED_NONE;
   led_op_t op = LED_CLEAR;
   char value1[7];
   char value2[10];
   const char *cur = parameter;

   memset(value1, 0, sizeof(value1));
   memset(value2, 0, sizeof(value2));

   cur = parse_next_text(cur, ' ', value1, sizeof(value1));
   cur = parse_next_text(cur, ' ', value2, sizeof(value2));

   if (!stricmp("red", value1)) {
      led = LED_COLOR_RED;
   } else if (!stricmp("blue", value1)) {
      led = LED_COLOR_BLUE;
   } else if (!stricmp("green", value1)) {
      led = LED_COLOR_GREEN;
   } else if (!stricmp("all", value1)) {
      led = LED_COLOR_ALL;
   } else {
      LOG_INF("led %s", parameter);
      LOG_INF("color '%s' not supported!", value1);
      return -EINVAL;
   }
   if (!stricmp("on", value2)) {
      op = LED_SET;
      timeout = K_SECONDS(10);
   } else if (!stricmp("off", value2)) {
      op = LED_CLEAR;
   } else if (!stricmp("blink", value2)) {
      op = LED_BLINK;
      timeout = K_MSEC(LED_BLINK_MS + 200);
   } else if (!stricmp("blinking", value2)) {
      op = LED_BLINKING;
      timeout = K_MSEC(LED_BLINKING_MS * 21);
   } else {
      LOG_INF("led %s", parameter);
      LOG_INF("operation '%s' not supported!", value2);
      return -EINVAL;
   }
   if (timeout.ticks) {
      ui_enable(true);
      ui_prio_mode = true;
      power_manager_pulse(timeout);
      work_reschedule_for_io_queue(&ui_finish_prio_work, timeout);
   }
   ui_led_op_prio(led, op);

   return 0;
}

static void sh_cmd_led_help(void)
{
   LOG_INF("> help led:");
   LOG_INF("  led <color> <op> : apply opartion on color LED.");
   LOG_INF("      <color>      : red, blue, green, or all.");
   LOG_INF("              <op> : on, off, blink, or blinking.");
}

SH_CMD(led, NULL, "led command.", sh_cmd_led, sh_cmd_led_help, 0);

#if defined(UI_LED) && defined(CONFIG_SH_CMD_UI_LED_TASK_TEST)

static const led_task_t led_reboot[] = {
    {.loop = 4, .time_ms = 499, .led = LED_COLOR_RED, .op = LED_SET},
    {.time_ms = 1, .led = LED_COLOR_RED, .op = LED_CLEAR},
    {.time_ms = 499, .led = LED_COLOR_BLUE, .op = LED_SET},
    {.time_ms = 1, .led = LED_COLOR_BLUE, .op = LED_CLEAR},
    {.time_ms = 0, .led = LED_COLOR_ALL, .op = LED_CLEAR},
};

static const led_task_t led_no_host[] = {
    {.time_ms = 1000, .led = LED_COLOR_ALL, .op = LED_SET},
    {.time_ms = 1000, .led = LED_COLOR_ALL, .op = LED_CLEAR},
    {.time_ms = 1000, .led = LED_COLOR_BLUE, .op = LED_SET},
    {.time_ms = 1000, .led = LED_COLOR_BLUE, .op = LED_CLEAR},
    {.loop = 2, .time_ms = 1000, .led = LED_COLOR_RED, .op = LED_SET},
    {.time_ms = 1000, .led = LED_COLOR_RED, .op = LED_CLEAR},
    {.time_ms = 0, .led = LED_COLOR_RED, .op = LED_CLEAR},
};

static const led_task_t led_all[] = {
    {.loop = 3, .time_ms = 1000, .led = LED_COLOR_ALL, .op = LED_SET},
    {.time_ms = 1000, .led = LED_COLOR_ALL, .op = LED_CLEAR},
    {.loop = 1, .time_ms = 1000, .led = LED_COLOR_BLUE, .op = LED_SET},
    {.time_ms = 1000, .led = LED_COLOR_BLUE, .op = LED_CLEAR},
    {.loop = 4, .time_ms = 1000, .led = LED_COLOR_RED, .op = LED_SET},
    {.time_ms = 1000, .led = LED_COLOR_RED, .op = LED_CLEAR},
    {.time_ms = 0, .led = LED_COLOR_RED, .op = LED_CLEAR},
};

static int sh_cmd_ledt(const char *parameter)
{
   char value[10];
   const char *cur = parameter;
   const led_task_t *task = led_all;
   memset(value, 0, sizeof(value));

   cur = parse_next_text(cur, ' ', value, sizeof(value));
   if (!stricmp("no", value)) {
      task = led_no_host;
   } else if (!stricmp("reboot", value)) {
      task = led_reboot;
   }
   ui_enable(true);
   ui_led_tasks(task);
   return 0;
}

static void sh_cmd_ledt_help(void)
{
   LOG_INF("> help ledt:");
   LOG_INF("  ledt no : LED signals 'no host'.");
   LOG_INF("  ledt    : LED signals 'reboot'.");
}

SH_CMD(ledt, NULL, "led tasks.", sh_cmd_ledt, sh_cmd_ledt_help, 0);

#endif /* UI_LED && CONFIG_SH_CMD_UI_LED_TASK_TEST */

#endif /* CONFIG_SH_CMD */
