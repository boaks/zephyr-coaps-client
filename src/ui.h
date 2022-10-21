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

typedef void (*ui_callback_handler_t)(void);

typedef enum { LED_COLOR_RED, LED_COLOR_BLUE, LED_COLOR_GREEN } led_t;

typedef enum { LED_SET, LED_CLEAR, LED_TOGGLE, LED_BLINK } led_op_t;

void ui_led_op(led_t led, led_op_t op);
// application layer
void ui_lte_1_op(led_op_t op);
// mobile ip layer
void ui_lte_2_op(led_op_t op);
// mobile connection layer
void ui_lte_3_op(led_op_t op);
int ui_init(ui_callback_handler_t button_callback);
int ui_config(void);

#endif /* UI_H */
