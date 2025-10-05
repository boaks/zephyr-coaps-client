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
   int protect;
};

#define SH_CMD(command, _at, _text, _handler, _help, _protect)           \
   static const STRUCT_SECTION_ITERABLE(sh_cmd_entry, cmd_##command) = { \
       .cmd = #command,                                                  \
       .at_cmd = _at,                                                    \
       .help = _text,                                                    \
       .handler = _handler,                                              \
       .help_handler = _help,                                            \
       .protect = _protect,                                              \
   }

#define BIT_SH_CMD_EXECUTING 0
#define BIT_SH_CMD_AT_PENDING 1
#define BIT_SH_CMD_APP_ACTIVE 2
#define BIT_SH_CMD_QUEUED 3

#define BIT_SH_CMD_LAST BIT_SH_CMD_QUEUED

#define SH_CMD_EXECUTING BIT(BIT_SH_CMD_EXECUTING)
#define SH_CMD_AT_PENDING BIT(BIT_SH_CMD_AT_PENDING)
#define SH_CMD_APP_ACTIVE BIT(BIT_SH_CMD_APP_ACTIVE)
#define SH_CMD_QUEUED BIT(BIT_SH_CMD_QUEUED)

#ifdef CONFIG_SH_CMD

void sh_cmd_at_finish(void);

int sh_cmd_execute(const char *cmd);
int sh_cmd_schedule(const char *cmd, const k_timeout_t delay);

int sh_cmd_prepend(const char *cmd, const k_timeout_t delay);
int sh_cmd_append(const char *cmd, const k_timeout_t delay);

int sh_busy(void);
int sh_protected(void);
int sh_app_active(void);
int sh_app_set_active(void);
int sh_app_set_inactive(const k_timeout_t delay);

#else

#define sh_cmd_execute(cmd) 0
#define sh_cmd_schedule(cmd, delay) 0
#define sh_cmd_append(cmd, delay) 0;
#define sh_busy() 0
#define sh_protected() 0

#endif

#endif /* SH_CMD_H_ */
