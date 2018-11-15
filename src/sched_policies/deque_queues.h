/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2010-2011, 2015  Université de Bordeaux
 *
 * StarPU is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * StarPU is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU Lesser General Public License in COPYING.LGPL for more details.
 */

/* Deque queues, ready for use by schedulers */

#ifndef __DEQUE_QUEUES_H__
#define __DEQUE_QUEUES_H__

#include <starpu.h>
#include <core/jobs.h>

struct _starpu_deque_jobq
{
	/* the actual list */
	struct _starpu_job_list jobq;

	/* the number of tasks currently in the queue */
	unsigned njobs;

	/* the number of tasks that were processed */
	int nprocessed;

	/* only meaningful if the queue is only used by a single worker */
	double exp_start; /* Expected start date of first task in the queue */
	double exp_end; /* Expected end date of last task in the queue */
	double exp_len; /* Expected duration of the set of tasks in the queue */
};

struct _starpu_deque_jobq *_starpu_create_deque(void);
void _starpu_destroy_deque(struct _starpu_deque_jobq *deque);

struct starpu_task *_starpu_deque_pop_task(struct _starpu_deque_jobq *deque_queue, int workerid);
struct _starpu_job_list *_starpu_deque_pop_every_task(struct _starpu_deque_jobq *deque_queue, starpu_pthread_mutex_t *sched_mutex, int workerid);

unsigned _starpu_get_deque_njobs(struct _starpu_deque_jobq *deque_queue);
int _starpu_get_deque_nprocessed(struct _starpu_deque_jobq *deque_queue);


#endif // __DEQUE_QUEUES_H__
