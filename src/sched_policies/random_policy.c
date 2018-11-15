/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2010-2013  Université de Bordeaux
 * Copyright (C) 2010, 2011, 2012, 2013  Centre National de la Recherche Scientifique
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

/* Policy attributing tasks randomly to workers */

#include <starpu_rand.h>
#include <core/workers.h>
#include <core/sched_ctx.h>
#include <sched_policies/fifo_queues.h>
#ifdef HAVE_AYUDAME_H
#include <Ayudame.h>
#endif

static int _random_push_task(struct starpu_task *task, unsigned prio)
{
	/* find the queue */


	double alpha_sum = 0.0;

	unsigned sched_ctx_id = task->sched_ctx;
	struct starpu_worker_collection *workers = starpu_sched_ctx_get_worker_collection(sched_ctx_id);
        int worker;
	int worker_arr[STARPU_NMAXWORKERS];
	double speedup_arr[STARPU_NMAXWORKERS];
	int size = 0;
	struct starpu_sched_ctx_iterator it;
        if(workers->init_iterator)
                workers->init_iterator(workers, &it);

        while(workers->has_next(workers, &it))
	{
                worker = workers->get_next(workers, &it);
		int impl = 0;
		for(impl = 0; impl < STARPU_MAXIMPLEMENTATIONS; impl++)
		{
			if(starpu_worker_can_execute_task(worker, task, impl))
			{
				enum starpu_perfmodel_archtype perf_arch = starpu_worker_get_perf_archtype(worker);
				double speedup = starpu_worker_get_relative_speedup(perf_arch);
				alpha_sum += speedup;
				speedup_arr[size] = speedup;
				worker_arr[size++] = worker;
				break;
			}
		}
	}

	double random = starpu_drand48()*alpha_sum;
//	_STARPU_DEBUG("my rand is %e\n", random);

	if(size == 0)
		return -ENODEV;

	unsigned selected = worker_arr[size - 1];

	double alpha = 0.0;
	int i;
	for(i = 0; i < size; i++)
	{
                worker = worker_arr[i];
		double worker_alpha = speedup_arr[i];
		
		if (alpha + worker_alpha >= random)
		{
			/* we found the worker */
			selected = worker;
			break;
		}
		
		alpha += worker_alpha;
	}


#ifdef HAVE_AYUDAME_H
	if (AYU_event)
	{
		intptr_t id = selected;
		AYU_event(AYU_ADDTASKTOQUEUE, _starpu_get_job_associated_to_task(task)->job_id, &id);
	}
#endif

	return starpu_push_local_task(selected, task, prio);
}

static int random_push_task(struct starpu_task *task)
{
        return _random_push_task(task, !!task->priority);
}

static void initialize_random_policy(unsigned sched_ctx_id)
{
	starpu_sched_ctx_create_worker_collection(sched_ctx_id, STARPU_WORKER_LIST);
	starpu_srand48(time(NULL));
}

static void deinitialize_random_policy(unsigned sched_ctx_id)
{
	starpu_sched_ctx_delete_worker_collection(sched_ctx_id);
}

struct starpu_sched_policy _starpu_sched_random_policy =
{
	.init_sched = initialize_random_policy,
	.add_workers = NULL,
	.remove_workers = NULL,
	.deinit_sched = deinitialize_random_policy,
	.push_task = random_push_task,
	.pop_task = NULL,
	.pre_exec_hook = NULL,
	.post_exec_hook = NULL,
	.pop_every_task = NULL,
	.policy_name = "random",
	.policy_description = "weighted random based on worker overall performance"
};
