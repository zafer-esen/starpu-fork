/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2010-2015  Université de Bordeaux
 * Copyright (C) 2010-2013  Centre National de la Recherche Scientifique
 * Copyright (C) 2011  INRIA
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

#include <starpu.h>
#include <common/config.h>
#include <common/utils.h>
#include <core/sched_policy.h>
#include <profiling/profiling.h>
#include <common/barrier.h>
#include <core/debug.h>

static int use_prefetch = 0;
static double idle[STARPU_NMAXWORKERS];
static double idle_start[STARPU_NMAXWORKERS];

int starpu_get_prefetch_flag(void)
{
	return use_prefetch;
}

static struct starpu_sched_policy *predefined_policies[] =
{
	&_starpu_sched_eager_policy,
	&_starpu_sched_prio_policy,
	&_starpu_sched_random_policy,
	&_starpu_sched_ws_policy,
	&_starpu_sched_dm_policy,
	&_starpu_sched_dmda_policy,
	&_starpu_sched_dmda_ready_policy,
	&_starpu_sched_dmda_sorted_policy,
	&_starpu_sched_parallel_heft_policy,
	&_starpu_sched_peager_policy,
	NULL
};

struct starpu_sched_policy **starpu_sched_get_predefined_policies()
{
	return predefined_policies;
}

struct starpu_sched_policy *_starpu_get_sched_policy(struct _starpu_sched_ctx *sched_ctx)
{
	return sched_ctx->sched_policy;
}

/*
 *	Methods to initialize the scheduling policy
 */

static void load_sched_policy(struct starpu_sched_policy *sched_policy, struct _starpu_sched_ctx *sched_ctx)
{
	STARPU_ASSERT(sched_policy);

#ifdef STARPU_VERBOSE
	if (sched_policy->policy_name)
	{
		if (sched_policy->policy_description)
                        _STARPU_DEBUG("Use %s scheduler (%s)\n", sched_policy->policy_name, sched_policy->policy_description);
                else
                        _STARPU_DEBUG("Use %s scheduler \n", sched_policy->policy_name);

	}
#endif

	struct starpu_sched_policy *policy = sched_ctx->sched_policy;
	memcpy(policy, sched_policy, sizeof(*policy));
}

static struct starpu_sched_policy *find_sched_policy_from_name(const char *policy_name)
{
	if (!policy_name)
		return NULL;

	if (strncmp(policy_name, "heft", 5) == 0)
	{
		_STARPU_DISP("Warning: heft is now called \"dmda\".\n");
		return &_starpu_sched_dmda_policy;
	}

	struct starpu_sched_policy **policy;
	for(policy=predefined_policies ; *policy!=NULL ; policy++)
	{
		struct starpu_sched_policy *p = *policy;
		if (p->policy_name)
		{
			if (strcmp(policy_name, p->policy_name) == 0)
			{
				/* we found a policy with the requested name */
				return p;
			}
		}
	}
	if (strcmp(policy_name, "help") != 0)
	     fprintf(stderr, "Warning: scheduling policy \"%s\" was not found, try \"help\" to get a list\n", policy_name);

	/* nothing was found */
	return NULL;
}

static void display_sched_help_message(void)
{
	const char *sched_env = starpu_getenv("STARPU_SCHED");
	if (sched_env && (strcmp(sched_env, "help") == 0))
	{
		/* display the description of all predefined policies */
		struct starpu_sched_policy **policy;

		fprintf(stderr, "\nThe variable STARPU_SCHED can be set to one of the following strings:\n");
		for(policy=predefined_policies ; *policy!=NULL ; policy++)
		{
			struct starpu_sched_policy *p = *policy;
			fprintf(stderr, "%s\t-> %s\n", p->policy_name, p->policy_description);
		}
		fprintf(stderr, "\n");
	 }
}

struct starpu_sched_policy *_starpu_select_sched_policy(struct _starpu_machine_config *config, const char *required_policy)
{
	struct starpu_sched_policy *selected_policy = NULL;
	struct starpu_conf *user_conf = &config->conf;

	if(required_policy)
		selected_policy = find_sched_policy_from_name(required_policy);

	/* First, we check whether the application explicitely gave a scheduling policy or not */
	if (!selected_policy && user_conf && (user_conf->sched_policy))
		return user_conf->sched_policy;

	/* Otherwise, we look if the application specified the name of a policy to load */
	const char *sched_pol_name;
	sched_pol_name = starpu_getenv("STARPU_SCHED");
	if (sched_pol_name == NULL && user_conf && user_conf->sched_policy_name)
		sched_pol_name = user_conf->sched_policy_name;

	if (!selected_policy && sched_pol_name)
		selected_policy = find_sched_policy_from_name(sched_pol_name);

	/* Perhaps there was no policy that matched the name */
	if (selected_policy)
		return selected_policy;

	/* If no policy was specified, we use the greedy policy as a default */
	return &_starpu_sched_eager_policy;
}

void _starpu_init_sched_policy(struct _starpu_machine_config *config, struct _starpu_sched_ctx *sched_ctx, struct starpu_sched_policy *selected_policy)
{
	/* Perhaps we have to display some help */
	display_sched_help_message();

	/* Prefetch is activated by default */
	use_prefetch = starpu_get_env_number("STARPU_PREFETCH");
	if (use_prefetch == -1)
		use_prefetch = 1;

	/* Set calibrate flag */
	_starpu_set_calibrate_flag(config->conf.calibrate);

	load_sched_policy(selected_policy, sched_ctx);

	_STARPU_TRACE_WORKER_SCHEDULING_PUSH;
	sched_ctx->sched_policy->init_sched(sched_ctx->id);
	_STARPU_TRACE_WORKER_SCHEDULING_POP;
}

void _starpu_deinit_sched_policy(struct _starpu_sched_ctx *sched_ctx)
{
	struct starpu_sched_policy *policy = sched_ctx->sched_policy;
	if (policy->deinit_sched)
	{
		_STARPU_TRACE_WORKER_SCHEDULING_PUSH;
		policy->deinit_sched(sched_ctx->id);
		_STARPU_TRACE_WORKER_SCHEDULING_POP;
	}
}

static void _starpu_push_task_on_specific_worker_notify_sched(struct starpu_task *task, struct _starpu_worker *worker, int workerid, int perf_workerid)
{
	//	_STARPU_DISP("starpu sched push task on specific worker + notify\n");
	/* if we push a task on a specific worker, notify all the sched_ctxs the worker belongs to */
	struct _starpu_sched_ctx *sched_ctx;
	struct _starpu_sched_ctx_list *l = NULL;
        for (l = worker->sched_ctx_list; l; l = l->next)
        {
		sched_ctx = _starpu_get_sched_ctx_struct(l->sched_ctx);
		if (sched_ctx->sched_policy != NULL && sched_ctx->sched_policy->push_task_notify)
		{
			_STARPU_TRACE_WORKER_SCHEDULING_PUSH;
			sched_ctx->sched_policy->push_task_notify(task, workerid, perf_workerid, sched_ctx->id);
			_STARPU_TRACE_WORKER_SCHEDULING_POP;
		}
	}
}

/* Enqueue a task into the list of tasks explicitely attached to a worker. In
 * case workerid identifies a combined worker, a task will be enqueued into
 * each worker of the combination. */
static int _starpu_push_task_on_specific_worker(struct starpu_task *task, int workerid)
{
	//	_STARPU_DISP("starpu sched push task on specific worker\n");
	int nbasic_workers = (int)starpu_worker_get_count();

	/* Is this a basic worker or a combined worker ? */
	int is_basic_worker = (workerid < nbasic_workers);

	unsigned memory_node;
	struct _starpu_worker *worker = NULL;
	struct _starpu_combined_worker *combined_worker = NULL;

	if (is_basic_worker)
	{
		worker = _starpu_get_worker_struct(workerid);
		memory_node = worker->memory_node;
	}
	else
	{
		combined_worker = _starpu_get_combined_worker_struct(workerid);
		memory_node = combined_worker->memory_node;
	}

	if (use_prefetch)
		starpu_prefetch_task_input_on_node(task, memory_node);

	if (is_basic_worker)
		_starpu_push_task_on_specific_worker_notify_sched(task, worker, workerid, workerid);
	else
	{
		/* Notify all workers of the combined worker */
		int worker_size = combined_worker->worker_size;
		int *combined_workerid = combined_worker->combined_workerid;

		int j;
		for (j = 0; j < worker_size; j++)
		{
			int subworkerid = combined_workerid[j];
			_starpu_push_task_on_specific_worker_notify_sched(task, _starpu_get_worker_struct(subworkerid), subworkerid, workerid);
		}
	}

#ifdef STARPU_USE_SC_HYPERVISOR
	starpu_sched_ctx_call_pushed_task_cb(workerid, task->sched_ctx);
#endif //STARPU_USE_SC_HYPERVISOR
	unsigned i;
	if (is_basic_worker)
	{
		unsigned node = starpu_worker_get_memory_node(workerid);
		if (_starpu_task_uses_multiformat_handles(task))
		{
			for (i = 0; i < task->cl->nbuffers; i++)
			{
				struct starpu_task *conversion_task;
				starpu_data_handle_t handle;

				handle = STARPU_TASK_GET_HANDLE(task, i);
				if (!_starpu_handle_needs_conversion_task(handle, node))
					continue;

				conversion_task = _starpu_create_conversion_task(handle, node);
				conversion_task->mf_skip = 1;
				conversion_task->execute_on_a_specific_worker = 1;
				conversion_task->workerid = workerid;
				_starpu_task_submit_conversion_task(conversion_task, workerid);
				//_STARPU_DEBUG("Pushing a conversion task\n");
			}

			for (i = 0; i < task->cl->nbuffers; i++)
			{
				starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(task, i);
				handle->mf_node = node;
			}
		}
//		if(task->sched_ctx != _starpu_get_initial_sched_ctx()->id)

		if(task->priority > 0)
			return _starpu_push_local_task(worker, task, 1);
		else
			return _starpu_push_local_task(worker, task, 0);
	}
	else
	{
		/* This is a combined worker so we create task aliases */
		int worker_size = combined_worker->worker_size;
		int *combined_workerid = combined_worker->combined_workerid;

		int ret = 0;

		struct _starpu_job *job = _starpu_get_job_associated_to_task(task);
		job->task_size = worker_size;
		job->combined_workerid = workerid;
		job->active_task_alias_count = 0;

		STARPU_PTHREAD_BARRIER_INIT(&job->before_work_barrier, NULL, worker_size);
		STARPU_PTHREAD_BARRIER_INIT(&job->after_work_barrier, NULL, worker_size);
		job->after_work_busy_barrier = worker_size;

		/* Note: we have to call that early, or else the task may have
		 * disappeared already */
		starpu_push_task_end(task);

		int j;
		for (j = 0; j < worker_size; j++)
		{
			struct starpu_task *alias = starpu_task_dup(task);
			alias->destroy = 1;

			worker = _starpu_get_worker_struct(combined_workerid[j]);
			ret |= _starpu_push_local_task(worker, alias, 0);
		}

		return ret;
	}
}

/* the generic interface that call the proper underlying implementation */

int _starpu_push_task(struct _starpu_job *j)
{
	//	_STARPU_DISP("starpu_push_task1\n");
	if(j->task->prologue_callback_func)
		j->task->prologue_callback_func(j->task->prologue_callback_arg);

	struct starpu_task *task = j->task;
	struct _starpu_sched_ctx *sched_ctx = _starpu_get_sched_ctx_struct(task->sched_ctx);
	unsigned nworkers = 0;
	int ret;

	_STARPU_LOG_IN();

	_starpu_increment_nready_tasks_of_sched_ctx(task->sched_ctx, task->flops);
	task->status = STARPU_TASK_READY;
	//	_STARPU_DISP("starpu_push_task2\n");
#ifdef HAVE_AYUDAME_H
	if (AYU_event)
	{
		intptr_t id = -1;
		AYU_event(AYU_ADDTASKTOQUEUE, j->job_id, &id);
	}
#endif
	/* if the context does not have any workers save the tasks in a temp list */
	if(!sched_ctx->is_initial_sched)
	{
		/*if there are workers in the ctx that are not able to execute tasks
		  we consider the ctx empty */
		nworkers = _starpu_nworkers_able_to_execute_task(task, sched_ctx);

		if(nworkers == 0)
		{
			STARPU_PTHREAD_MUTEX_LOCK(&sched_ctx->empty_ctx_mutex);
			starpu_task_list_push_front(&sched_ctx->empty_ctx_tasks, task);
			STARPU_PTHREAD_MUTEX_UNLOCK(&sched_ctx->empty_ctx_mutex);
#ifdef STARPU_USE_SC_HYPERVISOR
			if(sched_ctx != NULL && sched_ctx->id != 0 && sched_ctx->perf_counters != NULL 
			   && sched_ctx->perf_counters->notify_empty_ctx)
			{
				_STARPU_TRACE_HYPERVISOR_BEGIN();
				sched_ctx->perf_counters->notify_empty_ctx(sched_ctx->id, task);
				_STARPU_TRACE_HYPERVISOR_END();
			}
#endif
			return 0;
		}
	}
	//	_STARPU_DISP("starpu_push_task3\n");

	/* in case there is no codelet associated to the task (that's a control
	 * task), we directly execute its callback and enforce the
	 * corresponding dependencies */
	if (task->cl == NULL || task->cl->where == STARPU_NOWHERE)
	{
		_starpu_handle_job_termination(j);
		_STARPU_LOG_OUT_TAG("handle_job_termination");
		return 0;
	}
	//	//_STARPU_DISP("starpu_push_task4\n");
	ret = _starpu_push_task_to_workers(task);
	if (ret == -EAGAIN)
		/* pushed to empty context, that's fine */
		ret = 0;
	//_STARPU_DISP("starpu_push_task5\n");
	return ret;
}

int _starpu_push_task_to_workers(struct starpu_task *task)
{
	//_STARPU_DISP("starpu sched push task to workers\n");
	struct _starpu_sched_ctx *sched_ctx = _starpu_get_sched_ctx_struct(task->sched_ctx);
	unsigned nworkers = 0;

	_STARPU_TRACE_JOB_PUSH(task, task->priority > 0);

	/* if the contexts still does not have workers put the task back to its place in
	   the empty ctx list */
	if(!sched_ctx->is_initial_sched)
	{
		/*if there are workers in the ctx that are not able to execute tasks
		  we consider the ctx empty */
		nworkers = _starpu_nworkers_able_to_execute_task(task, sched_ctx);

		if (nworkers == 0)
		{
			STARPU_PTHREAD_MUTEX_LOCK(&sched_ctx->empty_ctx_mutex);
			starpu_task_list_push_back(&sched_ctx->empty_ctx_tasks, task);
			STARPU_PTHREAD_MUTEX_UNLOCK(&sched_ctx->empty_ctx_mutex);
#ifdef STARPU_USE_SC_HYPERVISOR
			if(sched_ctx != NULL && sched_ctx->id != 0 && sched_ctx->perf_counters != NULL 
			   && sched_ctx->perf_counters->notify_empty_ctx)
			{
				_STARPU_TRACE_HYPERVISOR_BEGIN();
				sched_ctx->perf_counters->notify_empty_ctx(sched_ctx->id, task);
				_STARPU_TRACE_HYPERVISOR_END();
			}
#endif

			return -EAGAIN;
		}
	}

	_starpu_profiling_set_task_push_start_time(task);

	int ret;
	if (STARPU_UNLIKELY(task->execute_on_a_specific_worker))
	{
		unsigned node = starpu_worker_get_memory_node(task->workerid);
		if (starpu_get_prefetch_flag())
			starpu_prefetch_task_input_on_node(task, node);

		ret = _starpu_push_task_on_specific_worker(task, task->workerid);
	}
	else
	{
		struct _starpu_machine_config *config = _starpu_get_machine_config();

		/* When a task can only be executed on a given arch and we have
		 * only one memory node for that arch, we can systematically
		 * prefetch before the scheduling decision. */
		if (starpu_get_prefetch_flag()) {
			if (task->cl->where == STARPU_CPU && config->cpus_nodeid >= 0)
				starpu_prefetch_task_input_on_node(task, config->cpus_nodeid);
			else if (task->cl->where == STARPU_CUDA && config->cuda_nodeid >= 0)
				starpu_prefetch_task_input_on_node(task, config->cuda_nodeid);
			else if (task->cl->where == STARPU_OPENCL && config->opencl_nodeid >= 0)
				starpu_prefetch_task_input_on_node(task, config->opencl_nodeid);
		}

		STARPU_ASSERT(sched_ctx->sched_policy->push_task);
		/* check out if there are any workers in the context */
		starpu_pthread_rwlock_t *changing_ctx_mutex = _starpu_sched_ctx_get_changing_ctx_mutex(sched_ctx->id);
		STARPU_PTHREAD_RWLOCK_RDLOCK(changing_ctx_mutex);
		nworkers = starpu_sched_ctx_get_nworkers(sched_ctx->id);
		if (nworkers == 0)
			ret = -1;
		else
		{
			_STARPU_TRACE_WORKER_SCHEDULING_PUSH;
			ret = sched_ctx->sched_policy->push_task(task);
			_STARPU_TRACE_WORKER_SCHEDULING_POP;
		}
		STARPU_PTHREAD_RWLOCK_UNLOCK(changing_ctx_mutex);

		if(ret == -1)
		{
			fprintf(stderr, "repush task \n");
			_STARPU_TRACE_JOB_POP(task, task->priority > 0);
			ret = _starpu_push_task_to_workers(task);
		}
	}
	/* Note: from here, the task might have been destroyed already! */
	_STARPU_LOG_OUT();
	return ret;

}

/* This is called right after the scheduler has pushed a task to a queue
 * but just before releasing mutexes: we need the task to still be alive!
 */
int starpu_push_task_end(struct starpu_task *task)
{
	//_STARPU_DISP("starpu sched push task end\n");
	_starpu_profiling_set_task_push_end_time(task);
	task->scheduled = 1;
	return 0;
}

/* This is called right after the scheduler has pushed a task to a queue
 * but just before releasing mutexes: we need the task to still be alive!
 */
int _starpu_pop_task_end(struct starpu_task *task)
{
	if (!task)
		return 0;
	_STARPU_TRACE_JOB_POP(task, task->priority > 0);
	return 0;
}

/*
 * Given a handle that needs to be converted in order to be used on the given
 * node, returns a task that takes care of the conversion.
 */
struct starpu_task *_starpu_create_conversion_task(starpu_data_handle_t handle,
						   unsigned int node)
{
	return _starpu_create_conversion_task_for_arch(handle, starpu_node_get_kind(node));
}

struct starpu_task *_starpu_create_conversion_task_for_arch(starpu_data_handle_t handle,
						   enum starpu_node_kind node_kind)
{
	//_STARPU_DISP("starpu create conversion\n");
	struct starpu_task *conversion_task;

#if defined(STARPU_USE_OPENCL) || defined(STARPU_USE_CUDA) || defined(STARPU_SIMGRID)
	struct starpu_multiformat_interface *format_interface;
#endif

	conversion_task = starpu_task_create();
	conversion_task->synchronous = 0;
	STARPU_TASK_SET_HANDLE(conversion_task, handle, 0);

#if defined(STARPU_USE_OPENCL) || defined(STARPU_USE_CUDA) || defined(STARPU_SIMGRID)
	/* The node does not really matter here */
	format_interface = (struct starpu_multiformat_interface *) starpu_data_get_interface_on_node(handle, 0);
#endif

	_starpu_spin_lock(&handle->header_lock);
	handle->refcnt++;
	handle->busy_count++;
	_starpu_spin_unlock(&handle->header_lock);

	switch(node_kind)
	{
	case STARPU_CPU_RAM:
		switch (starpu_node_get_kind(handle->mf_node))
		{
		case STARPU_CPU_RAM:
			STARPU_ABORT();
#if defined(STARPU_USE_CUDA) || defined(STARPU_SIMGRID)
		case STARPU_CUDA_RAM:
		{
			struct starpu_multiformat_data_interface_ops *mf_ops;
			mf_ops = (struct starpu_multiformat_data_interface_ops *) handle->ops->get_mf_ops(format_interface);
			conversion_task->cl = mf_ops->cuda_to_cpu_cl;
			break;
		}
#endif
#if defined(STARPU_USE_OPENCL) || defined(STARPU_SIMGRID)
		case STARPU_OPENCL_RAM:
		{
			struct starpu_multiformat_data_interface_ops *mf_ops;
			mf_ops = (struct starpu_multiformat_data_interface_ops *) handle->ops->get_mf_ops(format_interface);
			conversion_task->cl = mf_ops->opencl_to_cpu_cl;
			break;
		}
#endif
		default:
			_STARPU_ERROR("Oops : %u\n", handle->mf_node);
		}
		break;
#if defined(STARPU_USE_CUDA) || defined(STARPU_SIMGRID)
	case STARPU_CUDA_RAM:
		{
			struct starpu_multiformat_data_interface_ops *mf_ops;
			mf_ops = (struct starpu_multiformat_data_interface_ops *) handle->ops->get_mf_ops(format_interface);
			conversion_task->cl = mf_ops->cpu_to_cuda_cl;
			break;
		}
#endif
#if defined(STARPU_USE_OPENCL) || defined(STARPU_SIMGRID)
	case STARPU_OPENCL_RAM:
	{
		struct starpu_multiformat_data_interface_ops *mf_ops;
		mf_ops = (struct starpu_multiformat_data_interface_ops *) handle->ops->get_mf_ops(format_interface);
		conversion_task->cl = mf_ops->cpu_to_opencl_cl;
		break;
	}
#endif
	default:
		STARPU_ABORT();
	}

	STARPU_CODELET_SET_MODE(conversion_task->cl, STARPU_RW, 0);
	return conversion_task;
}

static
struct _starpu_sched_ctx* _get_next_sched_ctx_to_pop_into(struct _starpu_worker *worker)
{
	//_STARPU_DISP("starpu next sched ctx to pop into\n");	
	struct _starpu_sched_ctx_list *l = NULL;
	if(!worker->reverse_phase)
	{
		/* find a context in which the worker hasn't poped yet */
		for (l = worker->sched_ctx_list; l; l = l->next)
		{
			if(!worker->poped_in_ctx[l->sched_ctx])
			{
				worker->poped_in_ctx[l->sched_ctx] = !worker->poped_in_ctx[l->sched_ctx];
				return	_starpu_get_sched_ctx_struct(l->sched_ctx);
			}
		}
		worker->reverse_phase = !worker->reverse_phase;
	}
	if(worker->reverse_phase)
	{
		/* if the context has already poped in every one start from the begining */
		for (l = worker->sched_ctx_list; l; l = l->next)
		{
			if(worker->poped_in_ctx[l->sched_ctx])
			{
				worker->poped_in_ctx[l->sched_ctx] = !worker->poped_in_ctx[l->sched_ctx];
				return	_starpu_get_sched_ctx_struct(l->sched_ctx);
			}
		}
		worker->reverse_phase = !worker->reverse_phase;
	}	
	worker->poped_in_ctx[worker->sched_ctx_list->sched_ctx] = !worker->poped_in_ctx[worker->sched_ctx_list->sched_ctx];
	return _starpu_get_sched_ctx_struct(worker->sched_ctx_list->sched_ctx);
}

struct starpu_task *_starpu_pop_task(struct _starpu_worker *worker)
{
	//	//_STARPU_DISP("starpu sched pop task1\n");
	struct starpu_task *task;
	int worker_id;
	unsigned node;

	/* We can't tell in advance which task will be picked up, so we measure
	 * a timestamp, and will attribute it afterwards to the task. */
	int profiling = starpu_profiling_status_get();
	struct timespec pop_start_time;
	if (profiling)
		_starpu_clock_gettime(&pop_start_time);

pick:
	/* perhaps there is some local task to be executed first */
	task = _starpu_pop_local_task(worker);

	//	//_STARPU_DISP("starpu sched pop task2\n");
	/* get tasks from the stacks of the strategy */
	if(!task)
	{		
		struct _starpu_sched_ctx *sched_ctx ;
#ifndef STARPU_NON_BLOCKING_DRIVERS
		int been_here[STARPU_NMAX_SCHED_CTXS];
		int i;
		for(i = 0; i < STARPU_NMAX_SCHED_CTXS; i++)
			been_here[i] = 0;
		//	//_STARPU_DISP("starpu sched pop task3\n");
		while(!task)
#endif
		{
			//	//_STARPU_DISP("starpu sched pop task4\n");
	if(worker->nsched_ctxs == 1){
				sched_ctx = _starpu_get_initial_sched_ctx();
				//	//_STARPU_DISP("IF starpu sched pop task5\n");
	}
			else
			{
				//		//_STARPU_DISP("ELSE starpu sched pop task5\n");
				while(1)
				{
					sched_ctx = _get_next_sched_ctx_to_pop_into(worker);

					if(worker->removed_from_ctx[sched_ctx->id] == 1 && worker->shares_tasks_lists[sched_ctx->id] == 1)
					{
						_starpu_worker_gets_out_of_ctx(sched_ctx->id, worker);
						worker->removed_from_ctx[sched_ctx->id] = 0;
						sched_ctx = NULL;
					}
					else
						break;
				}
			}

			////_STARPU_DISP("endafter while starpu sched pop task5\n");
			if(sched_ctx && sched_ctx->id != STARPU_NMAX_SCHED_CTXS)
			{
				if (sched_ctx->sched_policy && sched_ctx->sched_policy->pop_task)
				{
					task = sched_ctx->sched_policy->pop_task(sched_ctx->id);
					_starpu_pop_task_end(task);
				}
			}
				////_STARPU_DISP("starpu sched pop task6\n");
			if(!task)
			{
				/* it doesn't matter if it shares tasks list or not in the scheduler,
				   if it does not have any task to pop just get it out of here */
				/* however if it shares a task list it will be removed as soon as he 
				  finishes this job (in handle_job_termination) */
				if(worker->removed_from_ctx[sched_ctx->id])
				{
					_starpu_worker_gets_out_of_ctx(sched_ctx->id, worker);
					worker->removed_from_ctx[sched_ctx->id] = 0;
				}
#ifdef STARPU_USE_SC_HYPERVISOR
				struct starpu_sched_ctx_performance_counters *perf_counters = sched_ctx->perf_counters;
				if(sched_ctx->id != 0 && perf_counters != NULL && perf_counters->notify_idle_cycle && _starpu_sched_ctx_allow_hypervisor(sched_ctx->id))
				{
//					_STARPU_TRACE_HYPERVISOR_BEGIN();
					perf_counters->notify_idle_cycle(sched_ctx->id, worker->workerid, 1.0);
//					_STARPU_TRACE_HYPERVISOR_END();
				}
#endif //STARPU_USE_SC_HYPERVISOR
				
#ifndef STARPU_NON_BLOCKING_DRIVERS
				if(been_here[sched_ctx->id] || worker->nsched_ctxs == 1)
					break;
				been_here[sched_ctx->id] = 1;
#endif
			}
		}
	  }

	////_STARPU_DISP("starpu sched pop task7\n");
	if (!task)
	{
		idle_start[worker->workerid] = starpu_timing_now();
		return NULL;
	}

	if(idle_start[worker->workerid] != 0.0)
	{
		double idle_end = starpu_timing_now();
		idle[worker->workerid] += (idle_end - idle_start[worker->workerid]);
		idle_start[worker->workerid] = 0.0;
	}



#ifdef STARPU_USE_SC_HYPERVISOR
	struct _starpu_sched_ctx *sched_ctx = _starpu_get_sched_ctx_struct(task->sched_ctx);
	struct starpu_sched_ctx_performance_counters *perf_counters = sched_ctx->perf_counters;

	if(sched_ctx->id != 0 && perf_counters != NULL && perf_counters->notify_poped_task && _starpu_sched_ctx_allow_hypervisor(sched_ctx->id))
	{
//		_STARPU_TRACE_HYPERVISOR_BEGIN();
		perf_counters->notify_poped_task(task->sched_ctx, worker->workerid);
//		_STARPU_TRACE_HYPERVISOR_END();
	}
#endif //STARPU_USE_SC_HYPERVISOR


	/* Make sure we do not bother with all the multiformat-specific code if
	 * it is not necessary. */
	if (!_starpu_task_uses_multiformat_handles(task))
		goto profiling;


	/* This is either a conversion task, or a regular task for which the
	 * conversion tasks have already been created and submitted */
	if (task->mf_skip)
		goto profiling;

	worker_id = starpu_worker_get_id();
	if (!starpu_worker_can_execute_task(worker_id, task, 0))
		return task;

	node = starpu_worker_get_memory_node(worker_id);

	/*
	 * We do have a task that uses multiformat handles. Let's create the
	 * required conversion tasks.
	 */
	STARPU_PTHREAD_MUTEX_UNLOCK(&worker->sched_mutex);
	unsigned i;
	for (i = 0; i < task->cl->nbuffers; i++)
	{
		struct starpu_task *conversion_task;
		starpu_data_handle_t handle;

		handle = STARPU_TASK_GET_HANDLE(task, i);
		if (!_starpu_handle_needs_conversion_task(handle, node))
			continue;
		conversion_task = _starpu_create_conversion_task(handle, node);
		conversion_task->mf_skip = 1;
		conversion_task->execute_on_a_specific_worker = 1;
		conversion_task->workerid = worker_id;
		/*
		 * Next tasks will need to know where these handles have gone.
		 */
		handle->mf_node = node;
		_starpu_task_submit_conversion_task(conversion_task, worker_id);
	}

	task->mf_skip = 1;
	starpu_task_list_push_back(&worker->local_tasks, task);
	STARPU_PTHREAD_MUTEX_LOCK(&worker->sched_mutex);
	goto pick;

profiling:
	if (profiling)
	{
		struct starpu_profiling_task_info *profiling_info;
		profiling_info = task->profiling_info;

		/* The task may have been created before profiling was enabled,
		 * so we check if the profiling_info structure is available
		 * even though we already tested if profiling is enabled. */
		if (profiling_info)
		{
			memcpy(&profiling_info->pop_start_time,
				&pop_start_time, sizeof(struct timespec));
			_starpu_clock_gettime(&profiling_info->pop_end_time);
		}
	}

	return task;
}

struct starpu_task *_starpu_pop_every_task(struct _starpu_sched_ctx *sched_ctx)
{
	//_STARPU_DISP("starpu_wait sched pop everyhook1\n");
	struct starpu_task *task = NULL;
	STARPU_ASSERT(sched_ctx->sched_policy->pop_every_task);

	/* TODO set profiling info */
	if(sched_ctx->sched_policy->pop_every_task)
	{
		_STARPU_TRACE_WORKER_SCHEDULING_PUSH;
		task = sched_ctx->sched_policy->pop_every_task(sched_ctx->id);
		_STARPU_TRACE_WORKER_SCHEDULING_POP;
	}
	return task;
}

void _starpu_sched_pre_exec_hook(struct starpu_task *task)
{
	//_STARPU_DISP("starpu_wait sched pre exect hook1\n");
	struct _starpu_sched_ctx *sched_ctx = _starpu_get_sched_ctx_struct(task->sched_ctx);
	if (sched_ctx->sched_policy->pre_exec_hook)
	{
		_STARPU_TRACE_WORKER_SCHEDULING_PUSH;
		sched_ctx->sched_policy->pre_exec_hook(task);
		_STARPU_TRACE_WORKER_SCHEDULING_POP;
	}
}

void _starpu_sched_post_exec_hook(struct starpu_task *task)
{
	//_STARPU_DISP("starpu_wait sched post exect hook1\n");
	struct _starpu_sched_ctx *sched_ctx = _starpu_get_sched_ctx_struct(task->sched_ctx);

	if (sched_ctx->sched_policy->post_exec_hook)
	{
		_STARPU_TRACE_WORKER_SCHEDULING_PUSH;
		sched_ctx->sched_policy->post_exec_hook(task);
		_STARPU_TRACE_WORKER_SCHEDULING_POP;
	}
}

void _starpu_wait_on_sched_event(void)
{
	struct _starpu_worker *worker = _starpu_get_local_worker_key();

	STARPU_PTHREAD_MUTEX_LOCK(&worker->sched_mutex);
	//_STARPU_DISP("starpu_wait on sched event1\n");
	_starpu_handle_all_pending_node_data_requests(worker->memory_node);
	//_STARPU_DISP("starpu_wait on sched event2\n");
	if (_starpu_machine_is_running())
	{
#ifndef STARPU_NON_BLOCKING_DRIVERS
		STARPU_PTHREAD_COND_WAIT(&worker->sched_cond,
					  &worker->sched_mutex);
#endif
	}

	STARPU_PTHREAD_MUTEX_UNLOCK(&worker->sched_mutex);
	//_STARPU_DISP("starpu_wait on sched event3\n");
}

/* The scheduling policy may put tasks directly into a worker's local queue so
 * that it is not always necessary to create its own queue when the local queue
 * is sufficient. If "back" not null, the task is put at the back of the queue
 * where the worker will pop tasks first. Setting "back" to 0 therefore ensures
 * a FIFO ordering. */
int starpu_push_local_task(int workerid, struct starpu_task *task, int prio)
{
	//_STARPU_DISP("starpu_push local task1\n");
	struct _starpu_worker *worker = _starpu_get_worker_struct(workerid);

	return  _starpu_push_local_task(worker, task, prio);
}


void _starpu_print_idle_time()
{
	double all_idle = 0.0;
	int i = 0;
	for(i = 0; i < STARPU_NMAXWORKERS; i++){
		printf("Worker:%d idle:%lf\n",i,idle[i]);
		all_idle += idle[i];
	}
	printf("Totalidle:%lf, average idle worker:%lf\n",all_idle,all_idle/i);
}

