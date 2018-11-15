/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2010-2013, 2015  Université de Bordeaux
 * Copyright (C) 2010, 2011, 2012, 2013  Centre National de la Recherche Scientifique
 * Copyright (C) 2011, 2012  INRIA
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

/* Work stealing policy */

#include <float.h>

#include <core/workers.h>
#include <sched_policies/deque_queues.h>
#include <core/debug.h>

#ifdef HAVE_AYUDAME_H
#include <Ayudame.h>
#endif

struct _starpu_work_stealing_data
{
	struct _starpu_deque_jobq **queue_array;
	unsigned rr_worker;
	/* keep track of the work performed from the beginning of the algorithm to make
	 * better decisions about which queue to select when stealing or deferring work
	 */
	unsigned performed_total;
	unsigned last_pop_worker;
	unsigned last_push_worker;
};

#ifdef USE_OVERLOAD

/**
 * Minimum number of task we wait for being processed before we start assuming
 * on which worker the computation would be faster.
 */
static int calibration_value = 0;

#endif /* USE_OVERLOAD */


/**
 * Return a worker from which a task can be stolen.
 * Selecting a worker is done in a round-robin fashion, unless
 * the worker previously selected doesn't own any task,
 * then we return the first non-empty worker.
 */
static unsigned select_victim_round_robin(unsigned sched_ctx_id)
{
	struct _starpu_work_stealing_data *ws = (struct _starpu_work_stealing_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);
	unsigned worker = ws->last_pop_worker;
	unsigned nworkers = starpu_sched_ctx_get_nworkers(sched_ctx_id);

	starpu_pthread_mutex_t *victim_sched_mutex;
	starpu_pthread_cond_t *victim_sched_cond;

	/* If the worker's queue is empty, let's try
	 * the next ones */
	while (1)
	{
		unsigned njobs;

		starpu_worker_get_sched_condition(worker, &victim_sched_mutex, &victim_sched_cond);
		/* Here helgrind would shout that this is unprotected, but we
		 * are fine with getting outdated values, this is just an
		 * estimation */
		njobs = ws->queue_array[worker]->njobs;

		if (njobs)
			break;

		worker = (worker + 1) % nworkers;
		if (worker == ws->last_pop_worker)
		{
			/* We got back to the first worker,
			 * don't go in infinite loop */
			break;
		}
	}

	ws->last_pop_worker = (worker + 1) % nworkers;

	return worker;
}

/**
 * Return a worker to whom add a task.
 * Selecting a worker is done in a round-robin fashion.
 */
static unsigned select_worker_round_robin(unsigned sched_ctx_id)
{
	struct _starpu_work_stealing_data *ws = (struct _starpu_work_stealing_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);
	unsigned worker = ws->last_push_worker;
	unsigned nworkers = starpu_sched_ctx_get_nworkers(sched_ctx_id);

	ws->last_push_worker = (ws->last_push_worker + 1) % nworkers;

	return worker;
}

#ifdef USE_OVERLOAD

/**
 * Return a ratio helpful to determine whether a worker is suitable to steal
 * tasks from or to put some tasks in its queue.
 *
 * \return	a ratio with a positive or negative value, describing the current state of the worker :
 * 		a smaller value implies a faster worker with an relatively emptier queue : more suitable to put tasks in
 * 		a bigger value implies a slower worker with an reletively more replete queue : more suitable to steal tasks from
 */
static float overload_metric(unsigned sched_ctx_id, unsigned id)
{
	struct _starpu_work_stealing_data *ws = (struct _starpu_work_stealing_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);
	float execution_ratio = 0.0f;
	float current_ratio = 0.0f;

	int nprocessed = _starpu_get_deque_nprocessed(ws->queue_array[id]);
	unsigned njobs = _starpu_get_deque_njobs(ws->queue_array[id]);

	/* Did we get enough information ? */
	if (performed_total > 0 && nprocessed > 0)
	{
		/* How fast or slow is the worker compared to the other workers */
		execution_ratio = (float) nprocessed / performed_total;
		/* How replete is its queue */
		current_ratio = (float) njobs / nprocessed;
	}
	else
	{
		return 0.0f;
	}

	return (current_ratio - execution_ratio);
}

/**
 * Return the most suitable worker from which a task can be stolen.
 * The number of previously processed tasks, total and local,
 * and the number of tasks currently awaiting to be processed
 * by the tasks are taken into account to select the most suitable
 * worker to steal task from.
 */
static unsigned select_victim_overload(unsigned sched_ctx_id)
{
	unsigned worker;
	float  worker_ratio;
	unsigned best_worker = 0;
	float best_ratio = FLT_MIN;

	/* Don't try to play smart until we get
	 * enough informations. */
	if (performed_total < calibration_value)
		return select_victim_round_robin(sched_ctx_id);

	struct starpu_worker_collection *workers = starpu_sched_ctx_get_worker_collection(sched_ctx_id);

	struct starpu_sched_ctx_iterator it;
        if(workers->init_iterator)
                workers->init_iterator(workers, &it);

	while(workers->has_next(workers, &it))
        {
                worker = workers->get_next(workers, &it);
		worker_ratio = overload_metric(sched_ctx_id, worker);

		if (worker_ratio > best_ratio)
		{
			best_worker = worker;
			best_ratio = worker_ratio;
		}
	}

	return best_worker;
}

/**
 * Return the most suitable worker to whom add a task.
 * The number of previously processed tasks, total and local,
 * and the number of tasks currently awaiting to be processed
 * by the tasks are taken into account to select the most suitable
 * worker to add a task to.
 */
static unsigned select_worker_overload(unsigned sched_ctx_id)
{
	unsigned worker;
	float  worker_ratio;
	unsigned best_worker = 0;
	float best_ratio = FLT_MAX;

	/* Don't try to play smart until we get
	 * enough informations. */
	if (performed_total < calibration_value)
		return select_worker_round_robin(sched_ctx_id);

	struct starpu_worker_collection *workers = starpu_sched_ctx_get_worker_collection(sched_ctx_id);

	struct starpu_sched_ctx_iterator it;
        if(workers->init_iterator)
                workers->init_iterator(workers, &it);

	while(workers->has_next(workers, &it))
        {
                worker = workers->get_next(workers, &it);

		worker_ratio = overload_metric(sched_ctx_id, worker);

		if (worker_ratio < best_ratio)
		{
			best_worker = worker;
			best_ratio = worker_ratio;
		}
	}

	return best_worker;
}

#endif /* USE_OVERLOAD */


/**
 * Return a worker from which a task can be stolen.
 * This is a phony function used to call the right
 * function depending on the value of USE_OVERLOAD.
 */
static inline unsigned select_victim(unsigned sched_ctx_id)
{
#ifdef USE_OVERLOAD
	return select_victim_overload(sched_ctx_id);
#else
	return select_victim_round_robin(sched_ctx_id);
#endif /* USE_OVERLOAD */
}

/**
 * Return a worker from which a task can be stolen.
 * This is a phony function used to call the right
 * function depending on the value of USE_OVERLOAD.
 */
static inline unsigned select_worker(unsigned sched_ctx_id)
{
#ifdef USE_OVERLOAD
	return select_worker_overload(sched_ctx_id);
#else
	return select_worker_round_robin(sched_ctx_id);
#endif /* USE_OVERLOAD */
}


#ifdef STARPU_DEVEL
#warning TODO rewrite ... this will not scale at all now
#endif
static struct starpu_task *ws_pop_task(unsigned sched_ctx_id)
{
	struct _starpu_work_stealing_data *ws = (struct _starpu_work_stealing_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	struct starpu_task *task;
	struct _starpu_deque_jobq *q;

	int workerid = starpu_worker_get_id();

	STARPU_ASSERT(workerid != -1);

	q = ws->queue_array[workerid];

	task = _starpu_deque_pop_task(q, workerid);
	if (task)
	{
		/* there was a local task */
		ws->performed_total++;
		q->nprocessed++;
		q->njobs--;
		return task;
	}
	starpu_pthread_mutex_t *worker_sched_mutex;
	starpu_pthread_cond_t *worker_sched_cond;
	starpu_worker_get_sched_condition(workerid, &worker_sched_mutex, &worker_sched_cond);
	STARPU_PTHREAD_MUTEX_UNLOCK(worker_sched_mutex);
       

	/* we need to steal someone's job */
	unsigned victim = select_victim(sched_ctx_id);

	starpu_pthread_mutex_t *victim_sched_mutex;
	starpu_pthread_cond_t *victim_sched_cond;

	starpu_worker_get_sched_condition(victim, &victim_sched_mutex, &victim_sched_cond);
	STARPU_PTHREAD_MUTEX_LOCK(victim_sched_mutex);
	struct _starpu_deque_jobq *victimq = ws->queue_array[victim];

	task = _starpu_deque_pop_task(victimq, workerid);
	if (task)
	{
		_STARPU_TRACE_WORK_STEALING(q, workerid);
		ws->performed_total++;

		/* Beware : we have to increase the number of processed tasks of
		 * the stealer, not the victim ! */
		q->nprocessed++;
		victimq->njobs--;
	}

	STARPU_PTHREAD_MUTEX_UNLOCK(victim_sched_mutex);

	STARPU_PTHREAD_MUTEX_LOCK(worker_sched_mutex);
	if(!task)
	{
		task = _starpu_deque_pop_task(q, workerid);
		if (task)
		{
			/* there was a local task */
			ws->performed_total++;
			q->nprocessed++;
			q->njobs--;
			return task;
		}
	}

	return task;
}

static
int ws_push_task(struct starpu_task *task)
{
	unsigned sched_ctx_id = task->sched_ctx;
	struct _starpu_work_stealing_data *ws = (struct _starpu_work_stealing_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	struct _starpu_deque_jobq *deque_queue;
	struct _starpu_job *j = _starpu_get_job_associated_to_task(task);
	int workerid = starpu_worker_get_id();

	unsigned worker = 0;
	struct starpu_worker_collection *workers = starpu_sched_ctx_get_worker_collection(sched_ctx_id);
	struct starpu_sched_ctx_iterator it;
	if(workers->init_iterator)
		workers->init_iterator(workers, &it);
	
	while(workers->has_next(workers, &it))
	{
		worker = workers->get_next(workers, &it);
		starpu_pthread_mutex_t *sched_mutex;
		starpu_pthread_cond_t *sched_cond;
		starpu_worker_get_sched_condition(worker, &sched_mutex, &sched_cond);
		STARPU_PTHREAD_MUTEX_LOCK(sched_mutex);
	}
	
	
	/* If the current thread is not a worker but
	 * the main thread (-1), we find the better one to
	 * put task on its queue */
	if (workerid == -1)
		workerid = select_worker(sched_ctx_id);

	deque_queue = ws->queue_array[workerid];

#ifdef HAVE_AYUDAME_H
	if (AYU_event)
	{
		intptr_t id = workerid;
		AYU_event(AYU_ADDTASKTOQUEUE, j->job_id, &id);
	}
#endif
	_starpu_job_list_push_back(&deque_queue->jobq, j);
	deque_queue->njobs++;
	starpu_push_task_end(task);

	workers->init_iterator(workers, &it);
	while(workers->has_next(workers, &it))
	{
		worker = workers->get_next(workers, &it);
		starpu_pthread_mutex_t *sched_mutex;
		starpu_pthread_cond_t *sched_cond;
		starpu_worker_get_sched_condition(worker, &sched_mutex, &sched_cond);
#ifndef STARPU_NON_BLOCKING_DRIVERS
		STARPU_PTHREAD_COND_SIGNAL(sched_cond);
#endif
		STARPU_PTHREAD_MUTEX_UNLOCK(sched_mutex);
	}
		
	return 0;
}

static void ws_add_workers(unsigned sched_ctx_id, int *workerids,unsigned nworkers)
{
	struct _starpu_work_stealing_data *ws = (struct _starpu_work_stealing_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	unsigned i;
	int workerid;

	for (i = 0; i < nworkers; i++)
	{
		workerid = workerids[i];
		starpu_sched_ctx_worker_shares_tasks_lists(workerid, sched_ctx_id);
		ws->queue_array[workerid] = _starpu_create_deque();

		/* Tell helgrid that we are fine with getting outdated values,
		 * this is just an estimation */
		STARPU_HG_DISABLE_CHECKING(ws->queue_array[workerid]->njobs);

		/**
		 * The first WS_POP_TASK will increase NPROCESSED though no task was actually performed yet,
		 * we need to initialize it at -1.
		 */
		ws->queue_array[workerid]->nprocessed = -1;
		ws->queue_array[workerid]->njobs = 0;
	}
}

static void ws_remove_workers(unsigned sched_ctx_id, int *workerids, unsigned nworkers)
{
	struct _starpu_work_stealing_data *ws = (struct _starpu_work_stealing_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	unsigned i;
	int workerid;

	for (i = 0; i < nworkers; i++)
	{
		workerid = workerids[i];
		_starpu_destroy_deque(ws->queue_array[workerid]);
	}
}

static void initialize_ws_policy(unsigned sched_ctx_id)
{
	starpu_sched_ctx_create_worker_collection(sched_ctx_id, STARPU_WORKER_LIST);

	struct _starpu_work_stealing_data *ws = (struct _starpu_work_stealing_data*)malloc(sizeof(struct _starpu_work_stealing_data));
	starpu_sched_ctx_set_policy_data(sched_ctx_id, (void*)ws);

	ws->last_pop_worker = 0;
	ws->last_push_worker = 0;

	/**
	 * The first WS_POP_TASK will increase PERFORMED_TOTAL though no task was actually performed yet,
	 * we need to initialize it at -1.
	 */
	ws->performed_total = -1;

	ws->queue_array = (struct _starpu_deque_jobq**)malloc(STARPU_NMAXWORKERS*sizeof(struct _starpu_deque_jobq*));
}

static void deinit_ws_policy(unsigned sched_ctx_id)
{
	struct _starpu_work_stealing_data *ws = (struct _starpu_work_stealing_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	free(ws->queue_array);
        free(ws);
        starpu_sched_ctx_delete_worker_collection(sched_ctx_id);
}

struct starpu_sched_policy _starpu_sched_ws_policy =
{
	.init_sched = initialize_ws_policy,
	.deinit_sched = deinit_ws_policy,
	.add_workers = ws_add_workers,
	.remove_workers = ws_remove_workers,
	.push_task = ws_push_task,
	.pop_task = ws_pop_task,
	.pre_exec_hook = NULL,
	.post_exec_hook = NULL,
	.pop_every_task = NULL,
	.policy_name = "ws",
	.policy_description = "work stealing"
};
