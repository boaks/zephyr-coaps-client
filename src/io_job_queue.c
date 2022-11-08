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

#include <zephyr.h>

#define IO_JOB_QUEUE_STACK_SIZE 4096
#define IO_JOB_QUEUE_PRIORITY 5

static K_THREAD_STACK_DEFINE(io_job_queue_stack, IO_JOB_QUEUE_STACK_SIZE);

struct k_work_q io_job_queue;

void io_job_queue_init(void)
{
   struct k_work_queue_config cfg = {
       .name = "io_workq",
   };

   k_work_queue_start(
       &io_job_queue,
       io_job_queue_stack,
       K_THREAD_STACK_SIZEOF(io_job_queue_stack),
       IO_JOB_QUEUE_PRIORITY,
       &cfg);
}

int work_schedule_for_io_queue(struct k_work_delayable *dwork,
                               k_timeout_t delay)
{
   return k_work_schedule_for_queue(&io_job_queue, dwork, delay);
}

int work_reschedule_for_io_queue(struct k_work_delayable *dwork,
                                 k_timeout_t delay)
{
   return k_work_reschedule_for_queue(&io_job_queue, dwork, delay);
}

int work_submit_to_io_queue(struct k_work *work)
{
   return k_work_submit_to_queue(&io_job_queue, work);
}