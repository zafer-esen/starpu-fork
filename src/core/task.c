/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2009-2015, 2017  Université de Bordeaux
 * Copyright (C) 2010, 2011, 2012, 2013, 2014  Centre National de la Recherche Scientifique
 * Copyright (C) 2011  Télécom-SudParis
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
#include <starpu_profiling.h>
#include <core/workers.h>
#include <core/sched_ctx.h>
#include <core/jobs.h>
#include <core/task.h>
#include <core/task_bundle.h>
#include <common/config.h>
#include <common/utils.h>
#include <common/fxt.h>
#include <profiling/profiling.h>
#include <profiling/bound.h>
#include <math.h>
#include <string.h>
#include <core/debug.h>
#include <time.h>
#include <signal.h>
#include "argo.h"
#ifdef STARPU_HAVE_WINDOWS
#include <windows.h>
#endif
#include <core/sched_ctx.h>

/* XXX this should be reinitialized when StarPU is shutdown (or we should make
 * sure that no task remains !) */
/* TODO we could make this hierarchical to avoid contention ? */
//static starpu_pthread_cond_t submitted_cond = STARPU_PTHREAD_COND_INITIALIZER;

/* This key stores the task currently handled by the thread, note that we
 * cannot use the worker structure to store that information because it is
 * possible that we have a task with a NULL codelet, which means its callback
 * could be executed by a user thread as well. */
static starpu_pthread_key_t current_task_key;
static int limit_min_submitted_tasks;
static int limit_max_submitted_tasks;
static int watchdog_crash;

/* Called once at starpu_init */
void _starpu_task_init(void)
{
	STARPU_PTHREAD_KEY_CREATE(&current_task_key, NULL);
	limit_min_submitted_tasks = starpu_get_env_number("STARPU_LIMIT_MIN_SUBMITTED_TASKS");
	limit_max_submitted_tasks = starpu_get_env_number("STARPU_LIMIT_MAX_SUBMITTED_TASKS");
	watchdog_crash = starpu_get_env_number("STARPU_WATCHDOG_CRASH");
}

void _starpu_task_deinit(void)
{
	STARPU_PTHREAD_KEY_DELETE(current_task_key);
}

void starpu_task_init(struct starpu_task *task)
{
	/* TODO: memcpy from a template instead? benchmark it */

	STARPU_ASSERT(task);

	/* As most of the fields must be initialised at NULL, let's put 0
	 * everywhere */
	memset(task, 0, sizeof(struct starpu_task));

	task->sequential_consistency = 1;

	/* Now we can initialise fields which recquire custom value */
#if STARPU_DEFAULT_PRIO != 0
	task->priority = STARPU_DEFAULT_PRIO;
#endif

	task->detach = 1;

#if STARPU_TASK_INVALID != 0
	task->status = STARPU_TASK_INVALID;
#endif

	task->predicted = NAN;
	task->predicted_transfer = NAN;

	task->magic = 42;
	task->sched_ctx = STARPU_NMAX_SCHED_CTXS;

	task->flops = 0.0;

	task->scheduled = 0;

	task->dyn_handles = NULL;
	task->dyn_interfaces = NULL;
}

/* Free all the ressources allocated for a task, without deallocating the task
 * structure itself (this is required for statically allocated tasks).
 * All values previously set by the user, like codelet and handles, remain
 * unchanged */
void starpu_task_clean(struct starpu_task *task)
{
	STARPU_ASSERT(task);
	task->magic = 0;

	/* If a buffer was allocated to store the profiling info, we free it. */
	if (task->profiling_info)
	{
		free(task->profiling_info);
		task->profiling_info = NULL;
	}

	/* If case the task is (still) part of a bundle */
	starpu_task_bundle_t bundle = task->bundle;
	if (bundle)
		starpu_task_bundle_remove(bundle, task);

	if (task->dyn_handles)
	{
		free(task->dyn_handles);
		task->dyn_handles = NULL;
		free(task->dyn_interfaces);
		task->dyn_interfaces = NULL;
	}

	struct _starpu_job *j = (struct _starpu_job *)task->starpu_private;

	if (j)
	{
		_starpu_job_destroy(j);
		task->starpu_private = NULL;
	}
}

struct starpu_task * STARPU_ATTRIBUTE_MALLOC starpu_task_create(void)
{
	struct starpu_task *task;
	//	//_STARPU_DISP("CREATING TASK\n");
	task = (struct starpu_task *) malloc(sizeof(struct starpu_task));
	STARPU_ASSERT(task);

	starpu_task_init(task);

	/* Dynamically allocated tasks are destroyed by default */
	task->destroy = 1;

	return task;
}

/* Free the ressource allocated during starpu_task_create. This function can be
 * called automatically after the execution of a task by setting the "destroy"
 * flag of the starpu_task structure (default behaviour). Calling this function
 * on a statically allocated task results in an undefined behaviour. */
void _starpu_task_destroy(struct starpu_task *task)
{

	/* If starpu_task_destroy is called in a callback, we just set the destroy
	   flag. The task will be destroyed after the callback returns */
	if (task == starpu_task_get_current()
	    && _starpu_get_local_worker_status() == STATUS_CALLBACK)
	{
		task->destroy = 1;
	}
	else
	{
		starpu_task_clean(task);
		/* TODO handle the case of task with detach = 1 and destroy = 1 */
		/* TODO handle the case of non terminated tasks -> return -EINVAL */

		/* Does user want StarPU release cl_arg ? */
		if (task->cl_arg_free)
			free(task->cl_arg);

		/* Does user want StarPU release callback_arg ? */
		if (task->callback_arg_free)
			free(task->callback_arg);

		/* Does user want StarPU release prologue_callback_arg ? */
		if (task->prologue_callback_arg_free)
			free(task->prologue_callback_arg);

		free(task);
	}
}

void starpu_task_destroy(struct starpu_task *task)
{
	STARPU_ASSERT(task);
	STARPU_ASSERT_MSG(!task->destroy || !task->detach, "starpu_task_destroy must not be called for task with destroy = 1 and detach = 1");
	_starpu_task_destroy(task);
}

int starpu_task_wait(struct starpu_task *task)
{
        _STARPU_LOG_IN();
	STARPU_ASSERT(task);

	STARPU_ASSERT_MSG(!task->detach, "starpu_task_wait can only be called on tasks with detach = 0");

	if (task->detach || task->synchronous)
	{
		_STARPU_DEBUG("Task is detached or asynchronous. Waiting returns immediately\n");
		_STARPU_LOG_OUT_TAG("einval");
		return -EINVAL;
	}

	STARPU_ASSERT_MSG(_starpu_worker_may_perform_blocking_calls(), "starpu_task_wait must not be called from a task or callback");

	struct _starpu_job *j = (struct _starpu_job *)task->starpu_private;

	_starpu_wait_job(j);

	/* as this is a synchronous task, the liberation of the job
	   structure was deferred */
	if (task->destroy)
		_starpu_task_destroy(task);

        _STARPU_LOG_OUT();
	return 0;
}

struct _starpu_job *_starpu_get_job_associated_to_task(struct starpu_task *task)
{
	STARPU_ASSERT(task);

	if (!task->starpu_private)
	{
		struct _starpu_job *j = _starpu_job_create(task);
		task->starpu_private = j;
	}

	return (struct _starpu_job *)task->starpu_private;
}

/* NB in case we have a regenerable task, it is possible that the job was
 * already counted. */
int _starpu_submit_job(struct _starpu_job *j)
{

	struct starpu_task *task = j->task;
	//_STARPU_DISP("_starpu submit job1\n");
	_STARPU_LOG_IN();
	/* notify bound computation of a new task */
	_starpu_bound_record(j);

	_starpu_increment_nsubmitted_tasks_of_sched_ctx(j->task->sched_ctx);

#ifdef STARPU_USE_SC_HYPERVISOR
	struct _starpu_sched_ctx *sched_ctx = _starpu_get_sched_ctx_struct(j->task->sched_ctx);
	if(sched_ctx != NULL && j->task->sched_ctx != _starpu_get_initial_sched_ctx()->id && j->task->sched_ctx != STARPU_NMAX_SCHED_CTXS
	   && sched_ctx->perf_counters != NULL)
	{
		_starpu_compute_buffers_footprint(j->task->cl->model, STARPU_CPU_DEFAULT, 0, j);
		int i;
		size_t data_size = 0;
		if (j->task->cl)
			for(i = 0; i < j->task->cl->nbuffers; i++)
			{
				starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(task, i);
				if (handle != NULL)
					data_size += _starpu_data_get_size(handle);
			}

		_STARPU_TRACE_HYPERVISOR_BEGIN();
		sched_ctx->perf_counters->notify_submitted_job(j->task, j->footprint, data_size);
		_STARPU_TRACE_HYPERVISOR_END();
	}
#endif//STARPU_USE_SC_HYPERVISOR
	//_STARPU_DISP("_starpu submit job2\n");
	/* We retain handle reference count */
	if (task->cl)
	{
		unsigned i;
		for (i=0; i<task->cl->nbuffers; i++)
		{
			starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(task, i);
			_starpu_spin_lock(&handle->header_lock);
			handle->busy_count++;
			_starpu_spin_unlock(&handle->header_lock);
		}
	}
	//_STARPU_DISP("_starpu submit job3\n");
	STARPU_PTHREAD_MUTEX_LOCK(&j->sync_mutex);

	_starpu_handle_job_submission(j);
	//_STARPU_DISP("_starpu submit job4\n");
	int ret = _starpu_enforce_deps_and_schedule(j);

	_STARPU_LOG_OUT();
	return ret;
}

/* Note: this is racy, so valgrind would complain. But since we'll always put
 * the same values, this is not a problem. */
void _starpu_codelet_check_deprecated_fields(struct starpu_codelet *cl)
{
	if (!cl)
		return;

	int is_where_unset = cl->where == 0;
	unsigned i, some_impl;

	/* Check deprecated and unset fields (where, <device>_func,
 	 * <device>_funcs) */

	/* CPU */
	if (cl->cpu_func && cl->cpu_func != STARPU_MULTIPLE_CPU_IMPLEMENTATIONS && cl->cpu_funcs[0])
	{
		//_STARPU_DISP("[warning] [struct starpu_codelet] both cpu_func and cpu_funcs are set. Ignoring cpu_func.\n");
		cl->cpu_func = STARPU_MULTIPLE_CPU_IMPLEMENTATIONS;
	}
	if (cl->cpu_func && cl->cpu_func != STARPU_MULTIPLE_CPU_IMPLEMENTATIONS)
	{
		cl->cpu_funcs[0] = cl->cpu_func;
		cl->cpu_func = STARPU_MULTIPLE_CPU_IMPLEMENTATIONS;
	}
	some_impl = 0;
	for (i = 0; i < STARPU_MAXIMPLEMENTATIONS; i++)
		if (cl->cpu_funcs[i])
		{
			some_impl = 1;
			break;
		}
	if (some_impl && cl->cpu_func == 0)
	{
		cl->cpu_func = STARPU_MULTIPLE_CPU_IMPLEMENTATIONS;
	}
	if (some_impl && is_where_unset)
	{
		cl->where |= STARPU_CPU;
	}

	/* CUDA */
	if (cl->cuda_func && cl->cuda_func != STARPU_MULTIPLE_CUDA_IMPLEMENTATIONS && cl->cuda_funcs[0])
	{
		_STARPU_DISP("[warning] [struct starpu_codelet] both cuda_func and cuda_funcs are set. Ignoring cuda_func.\n");
		cl->cuda_func = STARPU_MULTIPLE_CUDA_IMPLEMENTATIONS;
	}
	if (cl->cuda_func && cl->cuda_func != STARPU_MULTIPLE_CUDA_IMPLEMENTATIONS)
	{
		cl->cuda_funcs[0] = cl->cuda_func;
		cl->cuda_func = STARPU_MULTIPLE_CUDA_IMPLEMENTATIONS;
	}
	some_impl = 0;
	for (i = 0; i < STARPU_MAXIMPLEMENTATIONS; i++)
		if (cl->cuda_funcs[i])
		{
			some_impl = 1;
			break;
		}
	if (some_impl && cl->cuda_func == 0)
	{
		cl->cuda_func = STARPU_MULTIPLE_CUDA_IMPLEMENTATIONS;
	}
	if (some_impl && is_where_unset)
	{
		cl->where |= STARPU_CUDA;
	}

	/* OpenCL */
	if (cl->opencl_func && cl->opencl_func != STARPU_MULTIPLE_OPENCL_IMPLEMENTATIONS && cl->opencl_funcs[0])
	{
		_STARPU_DISP("[warning] [struct starpu_codelet] both opencl_func and opencl_funcs are set. Ignoring opencl_func.\n");
		cl->opencl_func = STARPU_MULTIPLE_OPENCL_IMPLEMENTATIONS;
	}
	if (cl->opencl_func && cl->opencl_func != STARPU_MULTIPLE_OPENCL_IMPLEMENTATIONS)
	{
		cl->opencl_funcs[0] = cl->opencl_func;
		cl->opencl_func = STARPU_MULTIPLE_OPENCL_IMPLEMENTATIONS;
	}
	some_impl = 0;
	for (i = 0; i < STARPU_MAXIMPLEMENTATIONS; i++)
		if (cl->opencl_funcs[i])
		{
			some_impl = 1;
			break;
		}
	if (some_impl && cl->opencl_func == 0)
	{
		cl->opencl_func = STARPU_MULTIPLE_OPENCL_IMPLEMENTATIONS;
	}
	if (some_impl && is_where_unset)
	{
		cl->where |= STARPU_OPENCL;
	}
}

void _starpu_task_check_deprecated_fields(struct starpu_task *task)
{
	if (task->cl)
	{
		unsigned i;
		for(i=0; i<STARPU_MIN(task->cl->nbuffers, STARPU_NMAXBUFS) ; i++)
		{
			if (task->buffers[i].handle && task->handles[i])
			{
				_STARPU_DISP("[warning][struct starpu_task] task->buffers[%u] and task->handles[%u] both set. Ignoring task->buffers[%u] ?\n", i, i, i);
				STARPU_ASSERT(task->buffers[i].mode == task->cl->modes[i]);
				STARPU_ABORT();
			}
			if (task->buffers[i].handle)
			{
				task->handles[i] = task->buffers[i].handle;
				task->cl->modes[i] = task->buffers[i].mode;
			}
		}
	}
}

/* application should submit new tasks to StarPU through this function */
int starpu_task_submit(struct starpu_task *task)
{
	_STARPU_LOG_IN();
	STARPU_ASSERT(task);
	STARPU_ASSERT_MSG(task->magic == 42, "Tasks must be created with starpu_task_create, or initialized with starpu_task_init.");

	int ret;
	unsigned is_sync = task->synchronous;
	starpu_task_bundle_t bundle = task->bundle;

	/* internally, StarPU manipulates a struct _starpu_job * which is a wrapper around a
	* task structure, it is possible that this job structure was already
	* allocated. */
	struct _starpu_job *j = _starpu_get_job_associated_to_task(task);

	if (!j->internal)
	{
		int nsubmitted_tasks = starpu_task_nsubmitted();
		if (limit_max_submitted_tasks >= 0 && limit_max_submitted_tasks < nsubmitted_tasks
			&& limit_min_submitted_tasks >= 0 && limit_min_submitted_tasks < nsubmitted_tasks)
			starpu_task_wait_for_n_submitted(limit_min_submitted_tasks);
	}


	if (j->internal)
	{
		// Internal tasks are submitted to initial context
		task->sched_ctx = _starpu_get_initial_sched_ctx()->id;
	}
	else if (task->sched_ctx == STARPU_NMAX_SCHED_CTXS)
	{
		// If the task has not specified a context, we set the current context
		task->sched_ctx = _starpu_sched_ctx_get_current_context();
	}

	if (is_sync)
	{
		/* Perhaps it is not possible to submit a synchronous
		 * (blocking) task */
		STARPU_ASSERT_MSG(_starpu_worker_may_perform_blocking_calls(), "submitting a synchronous task must not be done from a task or a callback");
		task->detach = 0;
	}

	_starpu_task_check_deprecated_fields(task);
	_starpu_codelet_check_deprecated_fields(task->cl);

	if (task->cl)
	{
		unsigned i;
		_STARPU_TRACE_UPDATE_TASK_CNT(0);

		/* Check buffers */
		if (task->dyn_handles == NULL)
			STARPU_ASSERT_MSG(task->cl->nbuffers <= STARPU_NMAXBUFS, "Codelet %p has too many buffers (%d vs max %d). Either use --enable-maxbuffers configure option to increase the max, or use dyn_handles instead of handles.", task->cl, task->cl->nbuffers, STARPU_NMAXBUFS);

		if (task->dyn_handles)
		{
			task->dyn_interfaces = malloc(task->cl->nbuffers * sizeof(void *));
		}

		for (i = 0; i < task->cl->nbuffers; i++)
		{
			starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(task, i);
			/* Make sure handles are valid */
			STARPU_ASSERT_MSG(handle->magic == 42, "data %p is invalid (was it already unregistered?)", handle);
			/* Make sure handles are not partitioned */
			STARPU_ASSERT_MSG(handle->nchildren == 0, "only unpartitioned data (or the pieces of a partitioned data) can be used in a task");
			/* Provide the home interface for now if any,
			 * for can_execute hooks */
			if (handle->home_node != -1)
				_STARPU_TASK_SET_INTERFACE(task, starpu_data_get_interface_on_node(handle, handle->home_node), i);
		}

		/* Check the type of worker(s) required by the task exist */
		if (!_starpu_worker_exists(task))
		{
			_STARPU_LOG_OUT_TAG("ENODEV");
			return -ENODEV;
		}

		/* In case we require that a task should be explicitely
		 * executed on a specific worker, we make sure that the worker
		 * is able to execute this task.  */
		if (task->execute_on_a_specific_worker && !starpu_combined_worker_can_execute_task(task->workerid, task, 0))
		{
			_STARPU_LOG_OUT_TAG("ENODEV");
			return -ENODEV;
		}

		_starpu_detect_implicit_data_deps(task);


		if (task->cl->model && task->cl->model->symbol)
			_starpu_load_perfmodel(task->cl->model);

	}

	if (bundle)
	{
		/* We need to make sure that models for other tasks of the
		 * bundle are also loaded, so the scheduler can estimate the
		 * duration of the whole bundle */
		STARPU_PTHREAD_MUTEX_LOCK(&bundle->mutex);

		struct _starpu_task_bundle_entry *entry;
		entry = bundle->list;

		while (entry)
		{
			if (entry->task->cl->model && entry->task->cl->model->symbol)
				_starpu_load_perfmodel(entry->task->cl->model);

			if (entry->task->cl->power_model && entry->task->cl->power_model->symbol)
				_starpu_load_perfmodel(entry->task->cl->power_model);

			entry = entry->next;
		}

		STARPU_PTHREAD_MUTEX_UNLOCK(&bundle->mutex);
	}

	/* If profiling is activated, we allocate a structure to store the
	 * appropriate info. */
	struct starpu_profiling_task_info *info;
	int profiling = starpu_profiling_status_get();
	info = _starpu_allocate_profiling_info_if_needed(task);
	task->profiling_info = info;

	/* The task is considered as block until we are sure there remains not
	 * dependency. */
	task->status = STARPU_TASK_BLOCKED;


	if (profiling)
		_starpu_clock_gettime(&info->submit_time);

	ret = _starpu_submit_job(j);
#ifdef STARPU_SIMGRID
	MSG_process_sleep(0.000001);
#endif

	if (is_sync)
	{
		_starpu_wait_job(j);
		if (task->destroy)
		     _starpu_task_destroy(task);
	}

        _STARPU_LOG_OUT();
	return ret;
}

int _starpu_task_submit_internally(struct starpu_task *task)
{
	struct _starpu_job *j = _starpu_get_job_associated_to_task(task);
	j->internal = 1;
	return starpu_task_submit(task);
}

/* application should submit new tasks to StarPU through this function */
int starpu_task_submit_to_ctx(struct starpu_task *task, unsigned sched_ctx_id)
{
	task->sched_ctx = sched_ctx_id;
	return starpu_task_submit(task);
}

/* The StarPU core can submit tasks directly to the scheduler or a worker,
 * skipping dependencies completely (when it knows what it is doing).  */
int _starpu_task_submit_nodeps(struct starpu_task *task)
{
	_starpu_task_check_deprecated_fields(task);
	_starpu_codelet_check_deprecated_fields(task->cl);

	if (task->cl)
	{
		if (task->cl->model)
			_starpu_load_perfmodel(task->cl->model);

		if (task->cl->power_model)
			_starpu_load_perfmodel(task->cl->power_model);
	}

	struct _starpu_job *j = _starpu_get_job_associated_to_task(task);

	if (j->internal)
	{
		// Internal tasks are submitted to initial context
		j->task->sched_ctx = _starpu_get_initial_sched_ctx()->id;
	}
	else if (task->sched_ctx == STARPU_NMAX_SCHED_CTXS)
	{
		// If the task has not specified a context, we set the current context
		j->task->sched_ctx = _starpu_sched_ctx_get_current_context();
	}

	_starpu_increment_nsubmitted_tasks_of_sched_ctx(j->task->sched_ctx);
	STARPU_PTHREAD_MUTEX_LOCK(&j->sync_mutex);

	_starpu_handle_job_submission(j);

	if (task->cl)
	{
		/* This would be done by data dependencies checking */
		unsigned i;
		for (i=0 ; i<task->cl->nbuffers ; i++)
		{
			starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(j->task, i);
			_STARPU_JOB_SET_ORDERED_BUFFER_HANDLE(j, handle, i);
			enum starpu_data_access_mode mode = STARPU_CODELET_GET_MODE(j->task->cl, i);
			_STARPU_JOB_SET_ORDERED_BUFFER_MODE(j, mode, i);
			int node = -1;
			if (j->task->cl->specific_nodes)
				node = STARPU_CODELET_GET_NODE(j->task->cl, i);
			_STARPU_JOB_SET_ORDERED_BUFFER_NODE(j, node, i);
		}
	}

	STARPU_PTHREAD_MUTEX_UNLOCK(&j->sync_mutex);
	//_STARPU_DISP("_starpu_ submit no deps\n");
	return _starpu_push_task(j);
}

/*
 * worker->sched_mutex must be locked when calling this function.
 */
int _starpu_task_submit_conversion_task(struct starpu_task *task,
					unsigned int workerid)
{
	STARPU_ASSERT(task->cl);
	STARPU_ASSERT(task->execute_on_a_specific_worker);

	_starpu_task_check_deprecated_fields(task);
	_starpu_codelet_check_deprecated_fields(task->cl);

	/* We should factorize that */
	if (task->cl->model)
		_starpu_load_perfmodel(task->cl->model);

	if (task->cl->power_model)
		_starpu_load_perfmodel(task->cl->power_model);

	/* We retain handle reference count */
	unsigned i;
	for (i=0; i<task->cl->nbuffers; i++)
	{
		starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(task, i);
		_starpu_spin_lock(&handle->header_lock);
		handle->busy_count++;
		_starpu_spin_unlock(&handle->header_lock);
	}

	struct _starpu_job *j = _starpu_get_job_associated_to_task(task);

	if (j->internal)
	{
		// Internal tasks are submitted to initial context
		j->task->sched_ctx = _starpu_get_initial_sched_ctx()->id;
	}
	else if (task->sched_ctx == STARPU_NMAX_SCHED_CTXS)
	{
		// If the task has not specified a context, we set the current context
		j->task->sched_ctx = _starpu_sched_ctx_get_current_context();
	}

	_starpu_increment_nsubmitted_tasks_of_sched_ctx(j->task->sched_ctx);
	STARPU_PTHREAD_MUTEX_LOCK(&j->sync_mutex);
	_starpu_handle_job_submission(j);
	_starpu_increment_nready_tasks_of_sched_ctx(j->task->sched_ctx, j->task->flops);
	for (i=0 ; i<task->cl->nbuffers ; i++)
	{
		starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(j->task, i);
		_STARPU_JOB_SET_ORDERED_BUFFER_HANDLE(j, handle, i);
		enum starpu_data_access_mode mode = STARPU_CODELET_GET_MODE(j->task->cl, i);
		_STARPU_JOB_SET_ORDERED_BUFFER_MODE(j, mode, i);
		int node = -1;
		if (j->task->cl->specific_nodes)
			node = STARPU_CODELET_GET_NODE(j->task->cl, i);
		_STARPU_JOB_SET_ORDERED_BUFFER_NODE(j, node, i);
	}

        _STARPU_LOG_IN();

	task->status = STARPU_TASK_READY;
	_starpu_profiling_set_task_push_start_time(task);

	unsigned node = starpu_worker_get_memory_node(workerid);
	if (starpu_get_prefetch_flag())
		starpu_prefetch_task_input_on_node(task, node);

	struct _starpu_worker *worker;
	worker = _starpu_get_worker_struct(workerid);
	starpu_task_list_push_back(&worker->local_tasks, task);

	_starpu_profiling_set_task_push_end_time(task);

        _STARPU_LOG_OUT();
	STARPU_PTHREAD_MUTEX_UNLOCK(&j->sync_mutex);

	return 0;
}

void starpu_codelet_init(struct starpu_codelet *cl)
{
	memset(cl, 0, sizeof(struct starpu_codelet));
}

void starpu_codelet_display_stats(struct starpu_codelet *cl)
{
	unsigned worker;
	unsigned nworkers = starpu_worker_get_count();

	if (cl->name)
		fprintf(stderr, "Statistics for codelet %s\n", cl->name);
	else if (cl->model && cl->model->symbol)
		fprintf(stderr, "Statistics for codelet %s\n", cl->model->symbol);

	unsigned long total = 0;

	for (worker = 0; worker < nworkers; worker++)
		total += cl->per_worker_stats[worker];

	for (worker = 0; worker < nworkers; worker++)
	{
		char name[32];
		starpu_worker_get_name(worker, name, 32);

		fprintf(stderr, "\t%s -> %lu / %lu (%2.2f %%)\n", name, cl->per_worker_stats[worker], total, (100.0f*cl->per_worker_stats[worker])/total);
	}
}

/*
 * We wait for all the tasks that have already been submitted. Note that a
 * regenerable is not considered finished until it was explicitely set as
 * non-regenerale anymore (eg. from a callback).
 */
int starpu_task_wait_for_all(void)
{
	unsigned nsched_ctxs = _starpu_get_nsched_ctxs();
	unsigned sched_ctx_id = nsched_ctxs == 1 ? 0 : starpu_sched_ctx_get_context();

	/* if there is no indication about which context to wait,
	   we wait for all tasks submitted to starpu */
	if (sched_ctx_id == STARPU_NMAX_SCHED_CTXS)
	{
		_STARPU_DEBUG("Waiting for all tasks\n");
		STARPU_ASSERT_MSG(_starpu_worker_may_perform_blocking_calls(), "starpu_task_wait_for_all must not be called from a task or callback");

#ifdef HAVE_AYUDAME_H
		if (AYU_event) AYU_event(AYU_BARRIER, 0, NULL);
#endif
		struct _starpu_machine_config *config = (struct _starpu_machine_config *)_starpu_get_machine_config();
		if(config->topology.nsched_ctxs == 1)
			starpu_task_wait_for_all_in_ctx(0);
		else
		{
			int s;
			for(s = 0; s < STARPU_NMAX_SCHED_CTXS; s++)
			{
				if(config->sched_ctxs[s].id != STARPU_NMAX_SCHED_CTXS)
				{
					starpu_task_wait_for_all_in_ctx(config->sched_ctxs[s].id);
				}
			}
		}

		return 0;
	}
	else
	{
		_STARPU_DEBUG("Waiting for tasks submitted to context %u\n", sched_ctx_id);
		_starpu_wait_for_all_tasks_of_sched_ctx(sched_ctx_id);
#ifdef HAVE_AYUDAME_H
		/* TODO: improve Temanejo into knowing about contexts ... */
		if (AYU_event) AYU_event(AYU_BARRIER, 0, NULL);
#endif
	}
	return 0;
}

int starpu_task_wait_for_all_in_ctx(unsigned sched_ctx)
{
	_STARPU_TRACE_EVENT("starpu_task_wait_for_all");
	_starpu_wait_for_all_tasks_of_sched_ctx(sched_ctx);
#ifdef HAVE_AYUDAME_H
	if (AYU_event) AYU_event(AYU_BARRIER, 0, NULL);
#endif

	return 0;
}

/*
 * We wait until there's a certain number of the tasks that have already been
 * submitted left. Note that a regenerable is not considered finished until it
 * was explicitely set as non-regenerale anymore (eg. from a callback).
 */
int starpu_task_wait_for_n_submitted(unsigned n)
{
	unsigned nsched_ctxs = _starpu_get_nsched_ctxs();
	unsigned sched_ctx_id = nsched_ctxs == 1 ? 0 : starpu_sched_ctx_get_context();

	/* if there is no indication about which context to wait,
	   we wait for all tasks submitted to starpu */
	if (sched_ctx_id == STARPU_NMAX_SCHED_CTXS)
	{
		_STARPU_DEBUG("Waiting for all tasks\n");
		STARPU_ASSERT_MSG(_starpu_worker_may_perform_blocking_calls(), "starpu_task_wait_for_n_submitted must not be called from a task or callback");

		struct _starpu_machine_config *config = (struct _starpu_machine_config *)_starpu_get_machine_config();
		if(config->topology.nsched_ctxs == 1)
			_starpu_wait_for_n_submitted_tasks_of_sched_ctx(0, n);
		else
		{
			int s;
			for(s = 0; s < STARPU_NMAX_SCHED_CTXS; s++)
			{
				if(config->sched_ctxs[s].id != STARPU_NMAX_SCHED_CTXS)
				{
					_starpu_wait_for_n_submitted_tasks_of_sched_ctx(config->sched_ctxs[s].id, n);
				}
			}
		}

		return 0;
	}
	else
	{
		_STARPU_DEBUG("Waiting for tasks submitted to context %u\n", sched_ctx_id);
		_starpu_wait_for_n_submitted_tasks_of_sched_ctx(sched_ctx_id, n);
	}
	return 0;
}

int starpu_task_wait_for_n_submitted_in_ctx(unsigned sched_ctx, unsigned n)
{
	_starpu_wait_for_n_submitted_tasks_of_sched_ctx(sched_ctx, n);

	return 0;
}
/*
 * We wait until there is no ready task any more (i.e. StarPU will not be able
 * to progress any more).
 */
int starpu_task_wait_for_no_ready(void)
{
	STARPU_ASSERT_MSG(_starpu_worker_may_perform_blocking_calls(), "starpu_task_wait_for_no_ready must not be called from a task or callback");

	struct _starpu_machine_config *config = (struct _starpu_machine_config *)_starpu_get_machine_config();
	if(config->topology.nsched_ctxs == 1)
		_starpu_wait_for_no_ready_of_sched_ctx(0);
	else
	{
		int s;
		for(s = 0; s < STARPU_NMAX_SCHED_CTXS; s++)
		{
			if(config->sched_ctxs[s].id != STARPU_NMAX_SCHED_CTXS)
			{
				_starpu_wait_for_no_ready_of_sched_ctx(config->sched_ctxs[s].id);
			}
		}
	}

	return 0;
}

void
starpu_drivers_request_termination(void)
{
	struct _starpu_machine_config *config = _starpu_get_machine_config();

	STARPU_PTHREAD_MUTEX_LOCK(&config->submitted_mutex);
	int nsubmitted = starpu_task_nsubmitted();
	config->submitting = 0;
	if (nsubmitted == 0)
	{
		ANNOTATE_HAPPENS_AFTER(&config->running);
		config->running = 0;
		ANNOTATE_HAPPENS_BEFORE(&config->running);
		STARPU_WMB();
		int s;
		for(s = 0; s < STARPU_NMAX_SCHED_CTXS; s++)
		{
			if(config->sched_ctxs[s].id != STARPU_NMAX_SCHED_CTXS)
			{
				_starpu_check_nsubmitted_tasks_of_sched_ctx(config->sched_ctxs[s].id);
			}
		}
	}

	STARPU_PTHREAD_MUTEX_UNLOCK(&config->submitted_mutex);
}

int starpu_task_nsubmitted(void)
{
	int nsubmitted = 0;
	struct _starpu_machine_config *config = (struct _starpu_machine_config *)_starpu_get_machine_config();
	if(config->topology.nsched_ctxs == 1)
		nsubmitted = _starpu_get_nsubmitted_tasks_of_sched_ctx(0);
	else
	{
		int s;
		for(s = 0; s < STARPU_NMAX_SCHED_CTXS; s++)
		{
			if(config->sched_ctxs[s].id != STARPU_NMAX_SCHED_CTXS)
			{
				nsubmitted += _starpu_get_nsubmitted_tasks_of_sched_ctx(config->sched_ctxs[s].id);
			}
		}
	}
	return nsubmitted;
}


int starpu_task_nready(void)
{
	int nready = 0;
	struct _starpu_machine_config *config = (struct _starpu_machine_config *)_starpu_get_machine_config();
	if(config->topology.nsched_ctxs == 1)
		nready = starpu_sched_ctx_get_nready_tasks(0);
	else
	{
		int s;
		for(s = 0; s < STARPU_NMAX_SCHED_CTXS; s++)
		{
			if(config->sched_ctxs[s].id != STARPU_NMAX_SCHED_CTXS)
			{
				nready += starpu_sched_ctx_get_nready_tasks(config->sched_ctxs[s].id);
			}
		}
	}

	return nready;
}

/* Return the task currently executed by the worker, or NULL if this is called
 * either from a thread that is not a task or simply because there is no task
 * being executed at the moment. */
struct starpu_task *starpu_task_get_current(void)
{
	return (struct starpu_task *) STARPU_PTHREAD_GETSPECIFIC(current_task_key);
}

void _starpu_set_current_task(struct starpu_task *task)
{
	STARPU_PTHREAD_SETSPECIFIC(current_task_key, task);
}

/*
 * Returns 0 if tasks does not use any multiformat handle, 1 otherwise.
 */
int
_starpu_task_uses_multiformat_handles(struct starpu_task *task)
{
	unsigned i;
	for (i = 0; i < task->cl->nbuffers; i++)
	{
		if (_starpu_data_is_multiformat_handle(STARPU_TASK_GET_HANDLE(task, i)))
			return 1;
	}

	return 0;
}

/*
 * Checks whether the given handle needs to be converted in order to be used on
 * the node given as the second argument.
 */
int
_starpu_handle_needs_conversion_task(starpu_data_handle_t handle,
				     unsigned int node)
{
	return _starpu_handle_needs_conversion_task_for_arch(handle, starpu_node_get_kind(node));
}

int
_starpu_handle_needs_conversion_task_for_arch(starpu_data_handle_t handle,
				     enum starpu_node_kind node_kind)
{
	/*
	 * Here, we assume that CUDA devices and OpenCL devices use the
	 * same data structure. A conversion is only needed when moving
	 * data from a CPU to a GPU, or the other way around.
	 */
	switch (node_kind)
	{
		case STARPU_CPU_RAM:
			switch(starpu_node_get_kind(handle->mf_node))
			{
				case STARPU_CPU_RAM:
					return 0;
				case STARPU_CUDA_RAM:      /* Fall through */
				case STARPU_OPENCL_RAM:
					return 1;
				default:
					STARPU_ABORT();
			}
			break;
		case STARPU_CUDA_RAM:    /* Fall through */
		case STARPU_OPENCL_RAM:
			switch(starpu_node_get_kind(handle->mf_node))
			{
				case STARPU_CPU_RAM:
					return 1;
				case STARPU_CUDA_RAM:
				case STARPU_OPENCL_RAM:
					return 0;
				default:
					STARPU_ABORT();
			}
			break;
		default:
			STARPU_ABORT();
	}
	/* that instruction should never be reached */
	return -EINVAL;
}

void starpu_task_set_implementation(struct starpu_task *task, unsigned impl)
{
	_starpu_get_job_associated_to_task(task)->nimpl = impl;
}

unsigned starpu_task_get_implementation(struct starpu_task *task)
{
	return _starpu_get_job_associated_to_task(task)->nimpl;
}

void _starpu_sleep(struct timespec ts)
{
#ifdef STARPU_SIMGRID
	MSG_process_sleep(ts.tv_sec + ts.tv_nsec / 1000000000.);
#elif defined(STARPU_HAVE_WINDOWS)
	Sleep((ts.tv_sec * 1000) + (ts.tv_nsec / 1000000));
#else
	struct timespec req, rem;

	req = ts;
	while (nanosleep(&req, &rem))
		req = rem;
#endif
}

static starpu_pthread_t watchdog_thread;

/* Check from times to times that StarPU does finish some tasks */
static void *watchdog_func(void *arg)
{
	struct timespec ts;
	char *timeout_env = arg;
	unsigned long long timeout;

#ifdef _MSC_VER
	timeout = (unsigned long long) _atoi64(timeout_env);
#else
	timeout = atoll(timeout_env);
#endif
	ts.tv_sec = timeout / 1000000;
	ts.tv_nsec = (timeout % 1000000) * 1000;
	struct _starpu_machine_config *config = (struct _starpu_machine_config *)_starpu_get_machine_config();
	
	STARPU_PTHREAD_MUTEX_LOCK(&config->submitted_mutex);
	while (_starpu_machine_is_running())
	{
		int last_nsubmitted = starpu_task_nsubmitted();
		config->watchdog_ok = 0;
		STARPU_PTHREAD_MUTEX_UNLOCK(&config->submitted_mutex);

		_starpu_sleep(ts);

		STARPU_PTHREAD_MUTEX_LOCK(&config->submitted_mutex);
		if (!config->watchdog_ok && last_nsubmitted
				&& last_nsubmitted == starpu_task_nsubmitted())
		{
			fprintf(stderr,"The StarPU watchdog detected that no task finished for %u.%06us (can be configure through STARPU_WATCHDOG_TIMEOUT)\n", (unsigned)ts.tv_sec, (unsigned)ts.tv_nsec/1000);
			if (watchdog_crash)
			{
				fprintf(stderr,"Crashing the process\n");
				raise(SIGABRT);
			}
			else
				fprintf(stderr,"Set the STARPU_WATCHDOG_CRASH environment variable if you want to abort the process in such a case\n");
		}
		/* Only shout again after another period */
		config->watchdog_ok = 1;
	}
	STARPU_PTHREAD_MUTEX_UNLOCK(&config->submitted_mutex);
	return NULL;
}

void _starpu_watchdog_init(void)
{
	struct _starpu_machine_config *config = (struct _starpu_machine_config *)_starpu_get_machine_config();
	char *timeout_env = starpu_getenv("STARPU_WATCHDOG_TIMEOUT");

	STARPU_PTHREAD_MUTEX_INIT(&config->submitted_mutex, NULL);

	if (!timeout_env)
		return;

	STARPU_PTHREAD_CREATE(&watchdog_thread, NULL, watchdog_func, timeout_env);
}

void _starpu_watchdog_shutdown(void)
{
	char *timeout_env = starpu_getenv("STARPU_WATCHDOG_TIMEOUT");

	if (!timeout_env)
		return;

	starpu_pthread_join(watchdog_thread, NULL);
}
