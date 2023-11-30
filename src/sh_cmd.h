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

#ifndef SH_CMD_H_
#define SH_CMD_H_

#include <stddef.h>
#include <zephyr/kernel.h>

typedef int (*sh_cmd_handler_t)(const char *parameter);
typedef void (*sh_cmd_help_handler_t)(void);

struct sh_cmd_entry {
   const char *cmd;
   const char *at_cmd;
   const char *help;
   const sh_cmd_handler_t handler;
   const sh_cmd_help_handler_t help_handler;
   int send;
};

#define SH_CMD(command, _at, _text, _handler, _help, _send)              \
   static const STRUCT_SECTION_ITERABLE(sh_cmd_entry, cmd_##command) = { \
       .cmd = #command,                                                  \
       .at_cmd = _at,                                                    \
       .help = _text,                                                    \
       .handler = _handler,                                              \
       .help_handler = _help,                                            \
       .send = _send,                                                    \
   }

int sh_cmd_execute(const char *cmd);
int sh_cmd_schedule(const char *cmd, const k_timeout_t delay);

#define SH_CMD_EXECUTING 1
#define AT_CMD_PENDING 2

int sh_busy(void);

#endif /* SH_CMD_H_ */
