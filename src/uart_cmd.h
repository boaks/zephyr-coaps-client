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

#ifndef UART_CMD_H_
#define UART_CMD_H_

#include <stddef.h>
#include <zephyr/sys/util_macro.h>
#include <zephyr/toolchain/common.h>

typedef int (*uart_cmd_handler_t)(const char *parameter);
typedef void (*uart_help_handler_t)(void);

struct uart_cmd_entry {
   const char *cmd;
   const char *at_cmd;
   const char *help;
   const uart_cmd_handler_t handler;
   const uart_help_handler_t help_handler;
   int send;
};

#define UART_CMD(command, _at, _text, _handler, _help, _send)  \
   static STRUCT_SECTION_ITERABLE(uart_cmd_entry, cmd_##command) = { \
       .cmd = #command,                                        \
       .at_cmd = _at,                                          \
       .help = _text,                                          \
       .handler = _handler,                                    \
       .help_handler = _help,                                  \
       .send = _send,                                          \
   }

#endif /* UART_CMD_H_ */
