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

#define BIT_SH_CMD_EXECUTING 0
#define BIT_AT_CMD_PENDING 1

#define SH_CMD_EXECUTING BIT(BIT_SH_CMD_EXECUTING)
#define AT_CMD_PENDING BIT(BIT_AT_CMD_PENDING)

#ifdef CONFIG_SH_CMD

int sh_cmd_execute(const char *cmd);
int sh_cmd_schedule(const char *cmd, const k_timeout_t delay);

int sh_cmd_append(const char *cmd, const k_timeout_t delay);

int sh_busy(void);
#else

#define sh_cmd_execute(cmd) 0
#define sh_cmd_schedule(cmd, delay) 0
#define sh_cmd_append(cmd, delay) 0;
#define sh_busy() 0

#endif

#endif /* SH_CMD_H_ */
