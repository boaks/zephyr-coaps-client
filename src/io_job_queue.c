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

#include "io_job_queue.h"

#ifdef CONFIG_USE_IO_JOB_QUEUE

#define IO_JOB_QUEUE_STACK_SIZE 4096
#define IO_JOB_QUEUE_PRIORITY 5

static K_THREAD_STACK_DEFINE(io_job_queue_stack, IO_JOB_QUEUE_STACK_SIZE);

struct k_work_q io_job_queue;
#endif

void io_job_queue_init(void)
{
#ifdef CONFIG_USE_IO_JOB_QUEUE
   struct k_work_queue_config cfg = {
       .name = "io_workq",
   };

   k_work_queue_start(
       &io_job_queue,
       io_job_queue_stack,
       K_THREAD_STACK_SIZEOF(io_job_queue_stack),
       IO_JOB_QUEUE_PRIORITY,
       &cfg);
#endif
}

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
