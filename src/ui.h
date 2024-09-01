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

#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <zephyr/sys_clock.h>

typedef void (*ui_callback_handler_t)(int duration);

typedef enum { LED_NONE,
               LED_COLOR_RED,
               LED_COLOR_BLUE,
               LED_COLOR_GREEN,
               LED_COLOR_ALL,
               LED_LTE_1, /* application layer */
               LED_LTE_2, /* mobile ip layer */
               LED_LTE_3, /* mobile connection layer */
#ifdef CONFIG_UART_LED
               LED_UART, /* UART active indicator */
#undef CONFIG_MFD_NPM1300_BUCK2_LED
#elif CONFIG_MFD_NPM1300_BUCK2_LED
               LED_BUCK2, /* BUCK2 active indicator */
#endif
} led_t;

typedef enum { LED_CLEAR,
               LED_SET,
               LED_TOGGLE,
               LED_BLINK,
               LED_BLINKING,
               LED_INTERNAL_TIMER,
} led_op_t;

typedef struct led_task {
   uint16_t loop;
   uint16_t time_ms;
   led_t led;
   led_op_t op;
} led_task_t;

int ui_led_op(led_t led, led_op_t op);
int ui_led_op_prio(led_t led, led_op_t op);
int ui_led_tasks(const led_task_t *tasks);
int ui_init(ui_callback_handler_t button_callback);
int ui_config(void);
void ui_enable(bool enable);
void ui_prio(bool enable);
int ui_input(k_timeout_t timeout);

#endif /* UI_H */
