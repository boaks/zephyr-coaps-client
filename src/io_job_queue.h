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

#ifndef IO_JOB_QUEUE_H
#define IO_JOB_QUEUE_H

#include <zephyr/kernel.h>

extern struct k_work_q io_job_queue;

void io_job_queue_init(void);

int work_schedule_for_io_queue(struct k_work_delayable *dwork,
                               k_timeout_t delay);

int work_reschedule_for_io_queue(struct k_work_delayable *dwork,
                                 k_timeout_t delay);

int work_submit_to_io_queue(struct k_work *work);

#endif /* IO_JOB_QUEUE_H */
