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

#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include "io_job_queue.h"

LOG_MODULE_REGISTER(WORK_QUEUE, CONFIG_WORK_QUEUE_LOG_LEVEL);

#ifdef CONFIG_USE_IO_JOB_QUEUE

#define IO_JOB_QUEUE_STACK_SIZE 2048
#define IO_JOB_QUEUE_PRIORITY 5

struct k_work_q io_job_queue;
static K_THREAD_STACK_DEFINE(io_job_queue_stack, IO_JOB_QUEUE_STACK_SIZE);
#endif /* CONFIG_USE_IO_JOB_QUEUE */

#define CONFIG_CMD_STACK_SIZE 2048
#define CONFIG_CMD_THREAD_PRIO 10

static struct k_work_q cmd_queue;
static K_THREAD_STACK_DEFINE(cmd_stack, CONFIG_CMD_STACK_SIZE);

#ifdef CONFIG_USE_JOB_QUEUE_ALIVE_CHECK
static void work_alive_io_fn(struct k_work *work);
static void work_alive_cmd_fn(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(work_alive_io_work, work_alive_io_fn);
static K_WORK_DELAYABLE_DEFINE(work_alive_cmd_work, work_alive_cmd_fn);

static void work_alive_io_fn(struct k_work *work)
{
   LOG_INF("I/O alive");
   work_reschedule_for_io_queue(&work_alive_io_work, K_MSEC(15000));
}

static void work_alive_cmd_fn(struct k_work *work)
{
   LOG_INF("CMD alive");
   work_reschedule_for_cmd_queue(&work_alive_cmd_work, K_MSEC(15000));
}
#endif /* CONFIG_USE_JOB_QUEUE_ALIVE_CHECK */

static int queues_init(void)
{
   struct k_work_queue_config cmd_cfg = {
       .name = "cmd_workq",
   };

#ifdef CONFIG_USE_IO_JOB_QUEUE
   struct k_work_queue_config io_cfg = {
       .name = "io_workq",
   };
   k_work_queue_init(&io_job_queue);
   k_work_queue_start(
       &io_job_queue,
       io_job_queue_stack,
       K_THREAD_STACK_SIZEOF(io_job_queue_stack),
       IO_JOB_QUEUE_PRIORITY,
       &io_cfg);
#endif /* CONFIG_USE_IO_JOB_QUEUE */
#ifdef CONFIG_USE_JOB_QUEUE_ALIVE_CHECK
   work_reschedule_for_io_queue(&work_alive_io_work, K_MSEC(7500));
#endif /* CONFIG_USE_JOB_QUEUE_ALIVE_CHECK */

   k_work_queue_init(&cmd_queue);
   k_work_queue_start(&cmd_queue, cmd_stack,
                      K_THREAD_STACK_SIZEOF(cmd_stack),
                      CONFIG_CMD_THREAD_PRIO, &cmd_cfg);

#ifdef CONFIG_USE_JOB_QUEUE_ALIVE_CHECK
   work_reschedule_for_cmd_queue(&work_alive_cmd_work, K_MSEC(15000));
#endif /* CONFIG_USE_JOB_QUEUE_ALIVE_CHECK */
   return 0;
}

#define APPLICATION_PREINIT_PRIORITY 89
SYS_INIT(queues_init, APPLICATION, APPLICATION_PREINIT_PRIORITY);

int work_schedule_for_io_queue(struct k_work_delayable *dwork,
                               k_timeout_t delay)
{
#ifdef CONFIG_USE_IO_JOB_QUEUE
   return k_work_schedule_for_queue(&io_job_queue, dwork, delay);
#else
   return k_work_schedule(dwork, delay);
#endif
}

int work_reschedule_for_io_queue(struct k_work_delayable *dwork,
                                 k_timeout_t delay)
{
#ifdef CONFIG_USE_IO_JOB_QUEUE
   return k_work_reschedule_for_queue(&io_job_queue, dwork, delay);
#else
   return k_work_reschedule(dwork, delay);
#endif
}

int work_submit_to_io_queue(struct k_work *work)
{
#ifdef CONFIG_USE_IO_JOB_QUEUE
   return k_work_submit_to_queue(&io_job_queue, work);
#else
   return k_work_submit(work);
#endif
}

int work_schedule_for_cmd_queue(struct k_work_delayable *dwork,
                                k_timeout_t delay)
{
   return k_work_schedule_for_queue(&cmd_queue, dwork, delay);
}

int work_reschedule_for_cmd_queue(struct k_work_delayable *dwork,
                                  k_timeout_t delay)
{
   return k_work_reschedule_for_queue(&cmd_queue, dwork, delay);
}

int work_submit_to_cmd_queue(struct k_work *work)
{
   return k_work_submit_to_queue(&cmd_queue, work);
}
