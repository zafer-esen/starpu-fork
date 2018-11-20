/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2009-2017  Université de Bordeaux
 * Copyright (C) 2010, 2011, 2012, 2013, 2015  Centre National de la Recherche Scientifique
 * Copyright (C) 2010, 2011  Institut National de Recherche en Informatique et Automatique
 * Copyright (C) 2011  Télécom-SudParis
 * Copyright (C) 2011-2012  INRIA
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

#include <stdlib.h>
#include <stdio.h>
#ifdef __linux__
#include <sys/utsname.h>
#endif
#include <common/config.h>
#include <common/utils.h>
#include <core/progress_hook.h>
#include <core/workers.h>
#include <core/debug.h>
#include <core/task.h>
#include <core/sched_policy.h>
#include <datawizard/malloc.h>
#include <profiling/profiling.h>
#include <starpu_task_list.h>
#include <datawizard/memory_nodes.h>

#include <drivers/cpu/driver_cpu.h>
#include <drivers/cuda/driver_cuda.h>
#include <drivers/opencl/driver_opencl.h>

#ifdef STARPU_SIMGRID
#include <core/simgrid.h>
#endif

#if defined(_WIN32) && !defined(__CYGWIN__)
#include <windows.h>
#endif

//#define ARGO 1
//#ifdef ARGO
#include "argo.h"
//#endif
//#include "../../../argodsm-stage/src/argo.h"


/* acquire/release semantic for concurrent initialization/de-initialization */
static starpu_pthread_mutex_t init_mutex = STARPU_PTHREAD_MUTEX_INITIALIZER;
static starpu_pthread_cond_t init_cond = STARPU_PTHREAD_COND_INITIALIZER;
static int init_count = 0;
static enum { UNINITIALIZED, CHANGING, INITIALIZED } initialized = UNINITIALIZED;

static int keys_initialized;
static starpu_pthread_key_t worker_key;

static struct _starpu_machine_config config;

static int disable_kernels;

int _starpu_is_initialized(void)
{
	return initialized == INITIALIZED;
}

struct _starpu_machine_config *_starpu_get_machine_config(void)
{
	return &config;
}

int _starpu_get_disable_kernels(void)
{
	return disable_kernels;
}

/* Makes sure that at least one of the workers of type <arch> can execute
 * <task>, for at least one of its implementations. */
static uint32_t _starpu_worker_exists_and_can_execute(struct starpu_task *task,
						      enum starpu_worker_archtype arch)
{
	int i;
	int nworkers = starpu_worker_get_count();

	_starpu_codelet_check_deprecated_fields(task->cl);

	for (i = 0; i < nworkers; i++)
	{
		if (starpu_worker_get_type(i) != arch)
			continue;

		unsigned impl;
		for (impl = 0; impl < STARPU_MAXIMPLEMENTATIONS; impl++)
		{
			/* We could call task->cl->can_execute(i, task, impl)
			   here, it would definitely work. It is probably
			   cheaper to check whether it is necessary in order to
			   avoid a useless function call, though. */
			unsigned test_implementation = 0;
			switch (arch)
			{
			case STARPU_CPU_WORKER:
				if (task->cl->cpu_funcs[impl] != NULL)
					test_implementation = 1;
				break;
			case STARPU_CUDA_WORKER:
				if (task->cl->cuda_funcs[impl] != NULL)
					test_implementation = 1;
				break;
			case STARPU_OPENCL_WORKER:
				if (task->cl->opencl_funcs[impl] != NULL)
					test_implementation = 1;
				break;
			default:
				STARPU_ABORT();
			}

			if (!test_implementation)
				continue;

			if (task->cl->can_execute(i, task, impl))
				return 1;
		}
	}

	return 0;
}

/* in case a task is submitted, we may check whether there exists a worker
   that may execute the task or not */
uint32_t _starpu_worker_exists(struct starpu_task *task)
{
	_starpu_codelet_check_deprecated_fields(task->cl);
	if (task->cl->where == STARPU_NOWHERE)
		return 1;

	if (!(task->cl->where & config.worker_mask))
		return 0;

	if (!task->cl->can_execute)
		return 1;

#if defined(STARPU_USE_CPU) || defined(STARPU_SIMGRID)
	if ((task->cl->where & STARPU_CPU) &&
	    _starpu_worker_exists_and_can_execute(task, STARPU_CPU_WORKER))
		return 1;
#endif
#if defined(STARPU_USE_CUDA) || defined(STARPU_SIMGRID)
	if ((task->cl->where & STARPU_CUDA) &&
	    _starpu_worker_exists_and_can_execute(task, STARPU_CUDA_WORKER))
		return 1;
#endif
#if defined(STARPU_USE_OPENCL) || defined(STARPU_SIMGRID)
	if ((task->cl->where & STARPU_OPENCL) &&
	    _starpu_worker_exists_and_can_execute(task, STARPU_OPENCL_WORKER))
		return 1;
#endif
	return 0;
}

uint32_t _starpu_can_submit_cuda_task(void)
{
	return (STARPU_CUDA & config.worker_mask);
}

uint32_t _starpu_can_submit_cpu_task(void)
{
	return (STARPU_CPU & config.worker_mask);
}

uint32_t _starpu_can_submit_opencl_task(void)
{
	return (STARPU_OPENCL & config.worker_mask);
}

static int _starpu_can_use_nth_implementation(enum starpu_worker_archtype arch, struct starpu_codelet *cl, unsigned nimpl)
{
	switch(arch)
	{
	case STARPU_ANY_WORKER:
	{
		int cpu_func_enabled=1, cuda_func_enabled=1, opencl_func_enabled=1;

#if defined(STARPU_USE_CPU) || defined(STARPU_SIMGRID)
		starpu_cpu_func_t cpu_func = _starpu_task_get_cpu_nth_implementation(cl, nimpl);
		cpu_func_enabled = cpu_func != NULL && starpu_cpu_worker_get_count();
#endif
#if defined(STARPU_USE_CUDA) || defined(STARPU_SIMGRID)
		starpu_cuda_func_t cuda_func = _starpu_task_get_cuda_nth_implementation(cl, nimpl);
		cuda_func_enabled = cuda_func != NULL && starpu_cuda_worker_get_count();
#endif
#if defined(STARPU_USE_OPENCL) || defined(STARPU_SIMGRID)
		starpu_opencl_func_t opencl_func = _starpu_task_get_opencl_nth_implementation(cl, nimpl);
		opencl_func_enabled = opencl_func != NULL && starpu_opencl_worker_get_count();
#endif

		return (cpu_func_enabled && cuda_func_enabled && opencl_func_enabled);
	}
	case STARPU_CPU_WORKER:
	{
		starpu_cpu_func_t func = _starpu_task_get_cpu_nth_implementation(cl, nimpl);
		return func != NULL;
	}
	case STARPU_CUDA_WORKER:
	{
		starpu_cuda_func_t func = _starpu_task_get_cuda_nth_implementation(cl, nimpl);
		return func != NULL;
	}
	case STARPU_OPENCL_WORKER:
	{
		starpu_opencl_func_t func = _starpu_task_get_opencl_nth_implementation(cl, nimpl);
		return func != NULL;
	}
	default:
		STARPU_ASSERT_MSG(0, "Unknown arch type %d", arch);
	}
	return 0;
}


int starpu_worker_can_execute_task(unsigned workerid, struct starpu_task *task, unsigned nimpl)
{
	/* TODO: check that the task operand sizes will fit on that device */
	return (task->cl->where & config.workers[workerid].worker_mask) &&
		_starpu_can_use_nth_implementation(config.workers[workerid].arch, task->cl, nimpl) &&
		(!task->cl->can_execute || task->cl->can_execute(workerid, task, nimpl));
}



int starpu_combined_worker_can_execute_task(unsigned workerid, struct starpu_task *task, unsigned nimpl)
{
	/* TODO: check that the task operand sizes will fit on that device */

	struct starpu_codelet *cl = task->cl;
	unsigned nworkers = config.topology.nworkers;

	/* Is this a parallel worker ? */
	if (workerid < nworkers)
	{
		return !!((task->cl->where & config.workers[workerid].worker_mask) &&
				_starpu_can_use_nth_implementation(config.workers[workerid].arch, task->cl, nimpl) &&
				(!task->cl->can_execute || task->cl->can_execute(workerid, task, nimpl)));
	}
	else
	{
		if ((cl->type == STARPU_SPMD)
#ifdef STARPU_HAVE_HWLOC
				|| (cl->type == STARPU_FORKJOIN)
#endif
				)
		{
			/* TODO we should add other types of constraints */

			/* Is the worker larger than requested ? */
			int worker_size = (int)config.combined_workers[workerid - nworkers].worker_size;
			int worker0 = config.combined_workers[workerid - nworkers].combined_workerid[0];
			return !!((worker_size <= task->cl->max_parallelism) &&
				_starpu_can_use_nth_implementation(config.workers[worker0].arch, task->cl, nimpl) &&
				(!task->cl->can_execute || task->cl->can_execute(workerid, task, nimpl)));
		}
		else
		{
			/* We have a sequential task but a parallel worker */
			return 0;
		}
	}
}

/*
 * Runtime initialization methods
 */

static void _starpu_init_worker_queue(struct _starpu_worker *workerarg)
{
	starpu_pthread_cond_t *cond = &workerarg->sched_cond;
	starpu_pthread_mutex_t *mutex = &workerarg->sched_mutex;

	unsigned memory_node = workerarg->memory_node;

	_starpu_memory_node_register_condition(cond, mutex, memory_node);
}

/*
 * Returns 0 if the given driver is one of the drivers that must be launched by
 * the application itself, and not by StarPU, 1 otherwise.
 */
static unsigned _starpu_may_launch_driver(struct starpu_conf *conf,
					  struct starpu_driver *d)
{
	if (conf->n_not_launched_drivers == 0 ||
	    conf->not_launched_drivers == NULL)
		return 1;

	/* Is <d> in conf->not_launched_drivers ? */
	unsigned i;
	for (i = 0; i < conf->n_not_launched_drivers; i++)
	{
		if (d->type != conf->not_launched_drivers[i].type)
			continue;

		switch (d->type)
		{
		case STARPU_CPU_WORKER:
			if (d->id.cpu_id == conf->not_launched_drivers[i].id.cpu_id)
				return 0;
			break;
		case STARPU_CUDA_WORKER:
			if (d->id.cuda_id == conf->not_launched_drivers[i].id.cuda_id)
				return 0;
			break;
#ifdef STARPU_USE_OPENCL
		case STARPU_OPENCL_WORKER:
			if (d->id.opencl_id == conf->not_launched_drivers[i].id.opencl_id)
				return 0;
			break;
#endif
		default:
			STARPU_ABORT();
		}
	}

	return 1;
}

#ifdef STARPU_PERF_DEBUG
struct itimerval prof_itimer;
#endif

static void _starpu_worker_init(struct _starpu_worker *workerarg, struct _starpu_machine_config *pconfig)
{
	workerarg->config = pconfig;
	STARPU_PTHREAD_MUTEX_INIT(&workerarg->mutex, NULL);
	/* arch initialized by topology.c */
	/* worker_mask initialized by topology.c */
	/* perf_arch initialized by topology.c */
	/* worker_thread initialized below */
	/* devid initialized by topology.c */
	/* bindid initialized by topology.c */
	/* workerid initialized by topology.c */
	workerarg->combined_workerid = workerarg->workerid;
	workerarg->current_rank = 0;
	workerarg->worker_size = 1;
	STARPU_PTHREAD_COND_INIT(&workerarg->started_cond, NULL);
	STARPU_PTHREAD_COND_INIT(&workerarg->ready_cond, NULL);
	/* memory_node initialized by topology.c */
	STARPU_PTHREAD_COND_INIT(&workerarg->sched_cond, NULL);
	STARPU_PTHREAD_MUTEX_INIT(&workerarg->sched_mutex, NULL);
	starpu_task_list_init(&workerarg->local_tasks);
	workerarg->current_task = NULL;
	workerarg->set = NULL;
	workerarg->worker_is_running = 0;
	workerarg->worker_is_initialized = 0;
	workerarg->status = STATUS_INITIALIZING;
	/* name initialized by driver */
	/* short_name initialized by driver */
	workerarg->run_by_starpu = 1;

	workerarg->sched_ctx_list = NULL;
	workerarg->nsched_ctxs = 0;
	_starpu_barrier_counter_init(&workerarg->tasks_barrier, 0);

	workerarg->has_prev_init = 0;

	int ctx;
	for(ctx = 0; ctx < STARPU_NMAX_SCHED_CTXS; ctx++)
	{
		workerarg->removed_from_ctx[ctx] = 0;
		workerarg->shares_tasks_lists[ctx] = 0;
		workerarg->poped_in_ctx[ctx] = 0;
	}
	workerarg->reverse_phase = 0;

	/* cpu_set/hwloc_cpu_set initialized in topology.c */
}

void _starpu_worker_start(struct _starpu_worker *worker, unsigned fut_key)
{
	(void) fut_key;
	int devid = worker->devid;
	(void) devid;

#ifdef STARPU_USE_FXT
	_starpu_fxt_register_thread(worker->bindid);

	unsigned memnode = worker->memory_node;
	_STARPU_TRACE_WORKER_INIT_START(fut_key, worker->workerid, devid, memnode);
#endif

	_starpu_memory_node_set_local_key(&worker->memory_node);

	_starpu_set_local_worker_key(worker);

	STARPU_PTHREAD_MUTEX_LOCK(&worker->mutex);
	worker->worker_is_running = 1;
	STARPU_PTHREAD_COND_SIGNAL(&worker->started_cond);
	STARPU_PTHREAD_MUTEX_UNLOCK(&worker->mutex);
	worker->spinning_backoff = 1;

	_starpu_bind_thread_on_cpu(worker->config, worker->bindid, worker->workerid);

#if defined(STARPU_PERF_DEBUG) && !defined(STARPU_SIMGRID)
	setitimer(ITIMER_PROF, &prof_itimer, NULL);
#endif

        _STARPU_DEBUG("worker %d is ready on logical cpu %d\n", devid, worker->bindid);
#ifdef STARPU_HAVE_HWLOC
	_STARPU_DEBUG("worker %d cpuset start at %d\n", devid, hwloc_bitmap_first(worker->initial_hwloc_cpu_set));
#endif
}

static void _starpu_launch_drivers(struct _starpu_machine_config *pconfig)
{
	pconfig->running = 1;
	pconfig->pause_depth = 0;
	pconfig->submitting = 1;
	STARPU_HG_DISABLE_CHECKING(pconfig->watchdog_ok);

	unsigned nworkers = pconfig->topology.nworkers;

	/* Launch workers asynchronously */
	unsigned cpu = 0, cuda = 0;
	unsigned worker;

#if defined(STARPU_PERF_DEBUG) && !defined(STARPU_SIMGRID)
	/* Get itimer of the main thread, to set it for the worker threads */
	getitimer(ITIMER_PROF, &prof_itimer);
#endif

#ifdef HAVE_AYUDAME_H
	if (AYU_event) AYU_event(AYU_INIT, 0, NULL);
#endif

	for (worker = 0; worker < nworkers; worker++)
	{
		struct _starpu_worker *workerarg = &pconfig->workers[worker];

		_STARPU_DEBUG("initialising worker %u\n", worker);

		_starpu_init_worker_queue(workerarg);

		struct starpu_driver driver;
		driver.type = workerarg->arch;
		switch (workerarg->arch)
		{
#if defined(STARPU_USE_CPU) || defined(STARPU_SIMGRID)
			case STARPU_CPU_WORKER:
				driver.id.cpu_id = cpu;
				if (_starpu_may_launch_driver(&pconfig->conf, &driver))
				{
					STARPU_PTHREAD_CREATE_ON(
						workerarg->name,
						&workerarg->worker_thread,
						NULL,
						_starpu_cpu_worker,
						workerarg,
						_starpu_simgrid_get_host_by_worker(workerarg));
#ifdef STARPU_USE_FXT
					STARPU_PTHREAD_MUTEX_LOCK(&workerarg->mutex);
					while (!workerarg->worker_is_running)
						STARPU_PTHREAD_COND_WAIT(&workerarg->started_cond, &workerarg->mutex);
					STARPU_PTHREAD_MUTEX_UNLOCK(&workerarg->mutex);
#endif
				}
				else
				{
					workerarg->run_by_starpu = 0;
				}
				cpu++;
				break;
#endif
#if defined(STARPU_USE_CUDA) || defined(STARPU_SIMGRID)
			case STARPU_CUDA_WORKER:
				driver.id.cuda_id = cuda;
				if (_starpu_may_launch_driver(&pconfig->conf, &driver))
				{
					STARPU_PTHREAD_CREATE_ON(
						workerarg->name,
						&workerarg->worker_thread,
						NULL,
						_starpu_cuda_worker,
						workerarg,
						_starpu_simgrid_get_host_by_worker(workerarg));
#ifdef STARPU_USE_FXT
					STARPU_PTHREAD_MUTEX_LOCK(&workerarg->mutex);
					while (!workerarg->worker_is_running)
						STARPU_PTHREAD_COND_WAIT(&workerarg->started_cond, &workerarg->mutex);
					STARPU_PTHREAD_MUTEX_UNLOCK(&workerarg->mutex);
#endif
				}
				else
				{
					workerarg->run_by_starpu = 0;
				}
				cuda++;
				break;
#endif
#if defined(STARPU_USE_OPENCL) || defined(STARPU_SIMGRID)
			case STARPU_OPENCL_WORKER:
#ifndef STARPU_SIMGRID
				starpu_opencl_get_device(workerarg->devid, &driver.id.opencl_id);
				if (!_starpu_may_launch_driver(&pconfig->conf, &driver))
				{
					workerarg->run_by_starpu = 0;
					break;
				}
#endif
				STARPU_PTHREAD_CREATE_ON(
					workerarg->name,
					&workerarg->worker_thread,
					NULL,
					_starpu_opencl_worker,
					workerarg,
					_starpu_simgrid_get_host_by_worker(workerarg));
#ifdef STARPU_USE_FXT
				STARPU_PTHREAD_MUTEX_LOCK(&workerarg->mutex);
				while (!workerarg->worker_is_running)
					STARPU_PTHREAD_COND_WAIT(&workerarg->started_cond, &workerarg->mutex);
				STARPU_PTHREAD_MUTEX_UNLOCK(&workerarg->mutex);
#endif
				break;
#endif
			default:
				STARPU_ABORT();
		}
	}

	cpu  = 0;
	cuda = 0;
	for (worker = 0; worker < nworkers; worker++)
	{
		struct _starpu_worker *workerarg = &pconfig->workers[worker];
		struct starpu_driver driver;
		driver.type = workerarg->arch;

		switch (workerarg->arch)
		{
			case STARPU_CPU_WORKER:
				driver.id.cpu_id = cpu;
				if (!_starpu_may_launch_driver(&pconfig->conf, &driver))
				{
					cpu++;
					break;
				}
				_STARPU_DEBUG("waiting for worker %u initialization\n", worker);
				STARPU_PTHREAD_MUTEX_LOCK(&workerarg->mutex);
				while (!workerarg->worker_is_initialized)
					STARPU_PTHREAD_COND_WAIT(&workerarg->ready_cond, &workerarg->mutex);
				STARPU_PTHREAD_MUTEX_UNLOCK(&workerarg->mutex);
				cpu++;
				break;
			case STARPU_CUDA_WORKER:
				driver.id.cuda_id = cuda;
				if (!_starpu_may_launch_driver(&pconfig->conf, &driver))
				{
					cuda++;
					break;
				}
				_STARPU_DEBUG("waiting for worker %u initialization\n", worker);
				STARPU_PTHREAD_MUTEX_LOCK(&workerarg->mutex);
				while (!workerarg->worker_is_initialized)
					STARPU_PTHREAD_COND_WAIT(&workerarg->ready_cond, &workerarg->mutex);
				STARPU_PTHREAD_MUTEX_UNLOCK(&workerarg->mutex);
				cuda++;
				break;
#if defined(STARPU_USE_OPENCL) || defined(STARPU_SIMGRID)
			case STARPU_OPENCL_WORKER:
#ifndef STARPU_SIMGRID
				starpu_opencl_get_device(workerarg->devid, &driver.id.opencl_id);
				if (!_starpu_may_launch_driver(&pconfig->conf, &driver))
					break;
#endif
				_STARPU_DEBUG("waiting for worker %u initialization\n", worker);
				STARPU_PTHREAD_MUTEX_LOCK(&workerarg->mutex);
				while (!workerarg->worker_is_initialized)
					STARPU_PTHREAD_COND_WAIT(&workerarg->ready_cond, &workerarg->mutex);
				STARPU_PTHREAD_MUTEX_UNLOCK(&workerarg->mutex);
				break;
#endif
			default:
				STARPU_ABORT();
		}
	}

	_STARPU_DEBUG("finished launching drivers\n");
}

void _starpu_set_local_worker_key(struct _starpu_worker *worker)
{
	STARPU_ASSERT(keys_initialized);
	STARPU_PTHREAD_SETSPECIFIC(worker_key, worker);
}

struct _starpu_worker *_starpu_get_local_worker_key(void)
{
	if (!keys_initialized)
		return NULL;
	return (struct _starpu_worker *) STARPU_PTHREAD_GETSPECIFIC(worker_key);
}

/* Initialize the starpu_conf with default values */
int starpu_conf_init(struct starpu_conf *conf)
{
	if (!conf)
		return -EINVAL;

	memset(conf, 0, sizeof(*conf));
	conf->magic = 42;
	conf->sched_policy_name = starpu_getenv("STARPU_SCHED");
	conf->sched_policy = NULL;

	/* Note that starpu_get_env_number returns -1 in case the variable is
	 * not defined */
	/* Backward compatibility: check the value of STARPU_NCPUS if
	 * STARPU_NCPU is not set. */
	conf->ncpus = starpu_get_env_number("STARPU_NCPU");
	if (conf->ncpus == -1)
		conf->ncpus = starpu_get_env_number("STARPU_NCPUS");
	conf->ncuda = starpu_get_env_number("STARPU_NCUDA");
	conf->nopencl = starpu_get_env_number("STARPU_NOPENCL");
	conf->calibrate = starpu_get_env_number("STARPU_CALIBRATE");
	conf->bus_calibrate = starpu_get_env_number("STARPU_BUS_CALIBRATE");

	if (conf->calibrate == -1)
	     conf->calibrate = 0;

	if (conf->bus_calibrate == -1)
	     conf->bus_calibrate = 0;

	conf->use_explicit_workers_bindid = 0; /* TODO */
	conf->use_explicit_workers_cuda_gpuid = 0; /* TODO */
	conf->use_explicit_workers_opencl_gpuid = 0; /* TODO */

	conf->single_combined_worker = starpu_get_env_number("STARPU_SINGLE_COMBINED_WORKER");
	if (conf->single_combined_worker == -1)
	     conf->single_combined_worker = 0;

#if defined(STARPU_DISABLE_ASYNCHRONOUS_COPY)
	conf->disable_asynchronous_copy = 1;
#else
	conf->disable_asynchronous_copy = starpu_get_env_number("STARPU_DISABLE_ASYNCHRONOUS_COPY");
	if (conf->disable_asynchronous_copy == -1)
		conf->disable_asynchronous_copy = 0;
#endif

#if defined(STARPU_DISABLE_ASYNCHRONOUS_CUDA_COPY)
	conf->disable_asynchronous_cuda_copy = 1;
#else
	conf->disable_asynchronous_cuda_copy = starpu_get_env_number("STARPU_DISABLE_ASYNCHRONOUS_CUDA_COPY");
	if (conf->disable_asynchronous_cuda_copy == -1)
		conf->disable_asynchronous_cuda_copy = 0;
#endif

#if defined(STARPU_DISABLE_ASYNCHRONOUS_OPENCL_COPY)
	conf->disable_asynchronous_opencl_copy = 1;
#else
	conf->disable_asynchronous_opencl_copy = starpu_get_env_number("STARPU_DISABLE_ASYNCHRONOUS_OPENCL_COPY");
	if (conf->disable_asynchronous_opencl_copy == -1)
		conf->disable_asynchronous_opencl_copy = 0;
#endif

	/* 64MiB by default */
	conf->trace_buffer_size = starpu_get_env_number_default("STARPU_TRACE_BUFFER_SIZE", 64) << 20;
	return 0;
}

static void _starpu_conf_set_value_against_environment(char *name, int *value)
{
	int number;
	number = starpu_get_env_number(name);
	if (number != -1)
	{
		*value = number;
	}
}

void _starpu_conf_check_environment(struct starpu_conf *conf)
{
	char *sched = starpu_getenv("STARPU_SCHED");
	if (sched)
	{
		conf->sched_policy_name = sched;
	}

	_starpu_conf_set_value_against_environment("STARPU_NCPUS", &conf->ncpus);
	_starpu_conf_set_value_against_environment("STARPU_NCPU", &conf->ncpus);
	_starpu_conf_set_value_against_environment("STARPU_NCUDA", &conf->ncuda);
	_starpu_conf_set_value_against_environment("STARPU_NOPENCL", &conf->nopencl);
	_starpu_conf_set_value_against_environment("STARPU_CALIBRATE", &conf->calibrate);
	_starpu_conf_set_value_against_environment("STARPU_BUS_CALIBRATE", &conf->bus_calibrate);
#ifdef STARPU_SIMGRID
	if (conf->calibrate == 2)
	{
		_STARPU_DISP("Warning: History will be cleared due to calibrate or STARPU_CALIBRATE being set to 2. This will prevent simgrid from having task simulation times!");
	}
	if (conf->bus_calibrate)
	{
		_STARPU_DISP("Warning: Bus calibration will be cleared due to bus_calibrate or STARPU_BUS_CALIBRATE being set. This will prevent simgrid from having data transfer simulation times!");
	}
#endif
	_starpu_conf_set_value_against_environment("STARPU_SINGLE_COMBINED_WORKER", &conf->single_combined_worker);
	_starpu_conf_set_value_against_environment("STARPU_DISABLE_ASYNCHRONOUS_COPY", &conf->disable_asynchronous_copy);
	_starpu_conf_set_value_against_environment("STARPU_DISABLE_ASYNCHRONOUS_CUDA_COPY", &conf->disable_asynchronous_cuda_copy);
	_starpu_conf_set_value_against_environment("STARPU_DISABLE_ASYNCHRONOUS_OPENCL_COPY", &conf->disable_asynchronous_opencl_copy);
}

int starpu_init(struct starpu_conf *user_conf)
{
	unsigned worker;
	int ret;
	int ctr=0;

	/* This initializes _starpu_silent, thus needs to be early */
	_starpu_util_init();


#ifdef STARPU_SIMGRID
	/* This initializes the simgrid thread library, thus needs to be early */
	_starpu_simgrid_init();
#endif

	//#ifdef ARGO

	argo_init(100L*1024l*1024l*1024l);//60gb global address space
	if(argo_number_of_nodes() > 1 && argo_node_id() != 0*(argo_number_of_nodes()-1)){ //We set last node as compute so most/all mem is remote for worst case scenario
		//if(argo_number_of_nodes() > 1 && argo_node_id() != 0){
		_STARPU_DISP("argo memnodes stuck here\n");
		argo_finalize();
		exit(EXIT_SUCCESS);
	}
	else{
		_STARPU_DISP("INITED ARGO - i am compute node :%d\n", argo_node_id());
	}
	//#endif


	STARPU_PTHREAD_MUTEX_LOCK(&init_mutex);
	while (initialized == CHANGING)
		/* Wait for the other one changing it */
		STARPU_PTHREAD_COND_WAIT(&init_cond, &init_mutex);
	init_count++;
	if (initialized == INITIALIZED)
	{
		/* He initialized it, don't do it again, and let the others get the mutex */
		STARPU_PTHREAD_MUTEX_UNLOCK(&init_mutex);
		return 0;
	}


	/* initialized == UNINITIALIZED */
	initialized = CHANGING;
	STARPU_PTHREAD_MUTEX_UNLOCK(&init_mutex);

#ifdef STARPU_SIMGRID
	/* Warn when the lots of stacks malloc()-ated by simgrid for transfer
	 * processes will take a long time to get initialized */
	char *perturb = starpu_getenv("MALLOC_PERTURB_");
	if (perturb && perturb[0] && atoi(perturb) != 0)
		_STARPU_DISP("Warning: MALLOC_PERTURB_ is set to non-zero, this makes simgrid runs very slow\n");
#else
#ifdef __GNUC__
#ifndef __OPTIMIZE__
	_STARPU_DISP("Warning: StarPU was configured with --enable-debug (-O0), and is thus not optimized\n");
#endif
#endif
#ifdef STARPU_SPINLOCK_CHECK
	_STARPU_DISP("Warning: StarPU was configured with --enable-spinlock-check, which slows down a bit\n");
#endif
#if 0
#ifndef STARPU_NO_ASSERT
	_STARPU_DISP("Warning: StarPU was configured without --enable-fast\n");
#endif
#endif
#ifdef STARPU_MEMORY_STATS
	_STARPU_DISP("Warning: StarPU was configured with --enable-memory-stats, which slows down a bit\n");
#endif
#ifdef STARPU_VERBOSE
	_STARPU_DISP("Warning: StarPU was configured with --enable-verbose, which slows down a bit\n");
#endif
#ifdef STARPU_USE_FXT
	_STARPU_DISP("Warning: StarPU was configured with --with-fxt, which slows down a bit\n");
#endif
#ifdef STARPU_PERF_DEBUG
	_STARPU_DISP("Warning: StarPU was configured with --enable-perf-debug, which slows down a bit\n");
#endif
#ifdef STARPU_MODEL_DEBUG
	_STARPU_DISP("Warning: StarPU was configured with --enable-model-debug, which slows down a bit\n");
#endif
#ifdef STARPU_ENABLE_STATS
	_STARPU_DISP("Warning: StarPU was configured with --enable-stats, which slows down a bit\n");
#endif
#ifdef __linux__
	{
		struct utsname buf;
		if (uname(&buf) == 0
		 && (!strncmp(buf.release, "4.7.", 4)
		  || !strncmp(buf.release, "4.8.", 4)))
			_STARPU_DISP("Warning: This system is running a 4.7 or 4.8 kernel. These have a severe scheduling performance regression issue, please upgrade to at least 4.9.\n");
	}
#endif
#endif

#if defined(_WIN32) && !defined(__CYGWIN__)
	WSADATA wsadata;
	WSAStartup(MAKEWORD(1,0), &wsadata);
#endif

	srand(2008);

#ifdef HAVE_AYUDAME_H
#ifndef AYU_RT_STARPU
#define AYU_RT_STARPU 4
#endif
	if (AYU_event)
	{
		enum ayu_runtime_t ayu_rt = AYU_RT_STARPU;
		AYU_event(AYU_PREINIT, 0, (void*) &ayu_rt);
	}
#endif

	/* store the pointer to the user explicit configuration during the
	 * initialization */
	if (user_conf == NULL)
		 starpu_conf_init(&config.conf);
	else
	{
		if (user_conf->magic != 42)
		{
			_STARPU_DISP("starpu_conf structure needs to be initialized with starpu_conf_init\n");
			return -EINVAL;
		}
		config.conf = *user_conf;
	}
	_starpu_conf_check_environment(&config.conf);

	/* Make a copy of arrays */
	if (config.conf.sched_policy_name)
		config.conf.sched_policy_name = strdup(config.conf.sched_policy_name);
	if (config.conf.n_cuda_opengl_interoperability)
	{
		size_t size = config.conf.n_cuda_opengl_interoperability * sizeof(*config.conf.cuda_opengl_interoperability);
		unsigned *copy = malloc(size);
		memcpy(copy, config.conf.cuda_opengl_interoperability, size);
		config.conf.cuda_opengl_interoperability = copy;
	}
	if (config.conf.n_not_launched_drivers)
	{
		size_t size = config.conf.n_not_launched_drivers * sizeof(*config.conf.not_launched_drivers);
		struct starpu_driver *copy = malloc(size);
		memcpy(copy, config.conf.not_launched_drivers, size);
		config.conf.not_launched_drivers = copy;
	}

	_starpu_init_all_sched_ctxs(&config);
	_starpu_init_progression_hooks();

	_starpu_init_tags();

#ifdef STARPU_USE_FXT
	_starpu_init_fxt_profiling(config.conf.trace_buffer_size);
#endif

	_starpu_open_debug_logfile();

	_starpu_data_interface_init();

	_starpu_timing_init();

	_starpu_profiling_init();

	_starpu_load_bus_performance_files();

	ret = _starpu_build_topology(&config);
	if (ret)
	{
		starpu_perfmodel_free_sampling_directories();
		STARPU_PTHREAD_MUTEX_LOCK(&init_mutex);
		init_count--;

		_starpu_destroy_machine_config(&config);

		initialized = UNINITIALIZED;
		/* Let somebody else try to do it */
		STARPU_PTHREAD_COND_SIGNAL(&init_cond);
		STARPU_PTHREAD_MUTEX_UNLOCK(&init_mutex);
		return ret;
	}

	_starpu_task_init();

	for (worker = 0; worker < config.topology.nworkers; worker++)
		_starpu_worker_init(&config.workers[worker], &config);


	disable_kernels = starpu_get_env_number("STARPU_DISABLE_KERNELS");
	STARPU_PTHREAD_KEY_CREATE(&worker_key, NULL);
	keys_initialized = 1;

	struct starpu_sched_policy *selected_policy = _starpu_select_sched_policy(&config, config.conf.sched_policy_name);

	_starpu_create_sched_ctx(selected_policy, NULL, -1, 1, "init", 0, 0, 0, 0);

	_starpu_initialize_registered_performance_models();

	/* Launch "basic" workers (ie. non-combined workers) */
	_starpu_launch_drivers(&config);

	_starpu_watchdog_init();

	_starpu_profiling_start();

	STARPU_PTHREAD_MUTEX_LOCK(&init_mutex);
	initialized = INITIALIZED;
	/* Tell everybody that we initialized */
	STARPU_PTHREAD_COND_BROADCAST(&init_cond);
	STARPU_PTHREAD_MUTEX_UNLOCK(&init_mutex);

	_STARPU_DEBUG("Initialisation finished\n");
	return 0;
}

/*
 * Handle runtime termination
 */

static void _starpu_terminate_workers(struct _starpu_machine_config *pconfig)
{
	int status = 0;
	unsigned workerid;

	starpu_wake_all_blocked_workers();

	for (workerid = 0; workerid < pconfig->topology.nworkers; workerid++)
	{
		_STARPU_DEBUG("wait for worker %u\n", workerid);

		struct _starpu_worker_set *set = pconfig->workers[workerid].set;
		struct _starpu_worker *worker = &pconfig->workers[workerid];

		/* in case StarPU termination code is called from a callback,
 		 * we have to check if pthread_self() is the worker itself */
		if (set)
		{
			if (!set->joined)
			{
#ifdef STARPU_SIMGRID
				status = starpu_pthread_join(set->worker_thread, NULL);
#else
				if (!pthread_equal(pthread_self(), set->worker_thread))
					status = starpu_pthread_join(set->worker_thread, NULL);
#endif
				if (status)
				{
#ifdef STARPU_VERBOSE
					_STARPU_DEBUG("starpu_pthread_join -> %d\n", status);
#endif
				}
				set->joined = 1;
			}
		}
		else
		{
			if (!worker->run_by_starpu)
				goto out;

#ifdef STARPU_SIMGRID
			status = starpu_pthread_join(worker->worker_thread, NULL);
#else
			if (!pthread_equal(pthread_self(), worker->worker_thread))
				status = starpu_pthread_join(worker->worker_thread, NULL);
#endif
			if (status)
			{
#ifdef STARPU_VERBOSE
				_STARPU_DEBUG("starpu_pthread_join -> %d\n", status);
#endif
			}
		}

out:
		STARPU_ASSERT(starpu_task_list_empty(&worker->local_tasks));
		_starpu_sched_ctx_list_delete(&worker->sched_ctx_list);
	}
}

/* Condition variable and mutex used to pause/resume. */
static starpu_pthread_cond_t pause_cond = STARPU_PTHREAD_COND_INITIALIZER;
static starpu_pthread_mutex_t pause_mutex = STARPU_PTHREAD_MUTEX_INITIALIZER;

void _starpu_may_pause(void)
{
	/* pause_depth is just protected by a memory barrier */
	STARPU_RMB();

	if (STARPU_UNLIKELY(config.pause_depth > 0)) {
		STARPU_PTHREAD_MUTEX_LOCK(&pause_mutex);
		if (config.pause_depth > 0) {
			STARPU_PTHREAD_COND_WAIT(&pause_cond, &pause_mutex);
		}
		STARPU_PTHREAD_MUTEX_UNLOCK(&pause_mutex);
	}
}

unsigned _starpu_machine_is_running(void)
{
	unsigned ret;
	/* running is just protected by a memory barrier */
	STARPU_RMB();

	ANNOTATE_HAPPENS_AFTER(&config.running);
	ret = config.running;
	ANNOTATE_HAPPENS_BEFORE(&config.running);
	return ret;
}

void starpu_pause()
{
	STARPU_HG_DISABLE_CHECKING(config.pause_depth);
	config.pause_depth += 1;
}

void starpu_resume()
{
	STARPU_PTHREAD_MUTEX_LOCK(&pause_mutex);
	config.pause_depth -= 1;
	if (!config.pause_depth) {
		STARPU_PTHREAD_COND_BROADCAST(&pause_cond);
	}
	STARPU_PTHREAD_MUTEX_UNLOCK(&pause_mutex);
}

unsigned _starpu_worker_can_block(unsigned memnode STARPU_ATTRIBUTE_UNUSED, struct _starpu_worker *worker STARPU_ATTRIBUTE_UNUSED)
{
#ifdef STARPU_NON_BLOCKING_DRIVERS
	return 0;
#else
	unsigned can_block = 1;

	struct starpu_driver driver;
	driver.type = worker->arch;
	switch (driver.type)
	{
	case STARPU_CPU_WORKER:
		driver.id.cpu_id = worker->devid;
		break;
	case STARPU_CUDA_WORKER:
		driver.id.cuda_id = worker->devid;
		break;
#ifdef STARPU_USE_OPENCL
	case STARPU_OPENCL_WORKER:
		starpu_opencl_get_device(worker->devid, &driver.id.opencl_id);
		break;
#endif
	default:
		goto always_launch;
	}
	if (!_starpu_may_launch_driver(&config.conf, &driver))
		return 0;

always_launch:

#ifndef STARPU_SIMGRID
	if (!_starpu_check_that_no_data_request_exists(memnode))
		can_block = 0;
#endif

	if (!_starpu_machine_is_running())
		can_block = 0;

	if (!_starpu_execute_registered_progression_hooks())
		can_block = 0;

	return can_block;
#endif
}

static void _starpu_kill_all_workers(struct _starpu_machine_config *pconfig)
{
	/* set the flag which will tell workers to stop */
	ANNOTATE_HAPPENS_AFTER(&config.running);
	pconfig->running = 0;
	/* running is just protected by a memory barrier */
	ANNOTATE_HAPPENS_BEFORE(&config.running);
	STARPU_WMB();
	starpu_wake_all_blocked_workers();
}

void starpu_display_stats()
{
	starpu_profiling_bus_helper_display_summary();
	starpu_profiling_worker_helper_display_summary();
}

void starpu_shutdown(void)
{
	STARPU_PTHREAD_MUTEX_LOCK(&init_mutex);
	init_count--;
	STARPU_ASSERT_MSG(init_count >= 0, "Number of calls to starpu_shutdown() can not be higher than the number of calls to starpu_init()\n");
	if (init_count)
	{
		_STARPU_DEBUG("Still somebody needing StarPU, don't deinitialize\n");
		STARPU_PTHREAD_MUTEX_UNLOCK(&init_mutex);
		return;
	}

		argo_finalize();


	/* We're last */
	initialized = CHANGING;
	STARPU_PTHREAD_MUTEX_UNLOCK(&init_mutex);
	

	/* If the workers are frozen, no progress can be made. */
	STARPU_ASSERT(config.pause_depth <= 0);

	starpu_task_wait_for_no_ready();

	/* tell all workers to shutdown */
	_starpu_kill_all_workers(&config);

	{
	     int stats = starpu_get_env_number("STARPU_STATS");
	     if (stats != 0)
	     {
		  _starpu_display_msi_stats();
		  _starpu_display_alloc_cache_stats();
		  _starpu_display_comm_amounts();
	     }
	}

	starpu_profiling_bus_helper_display_summary();
	starpu_profiling_worker_helper_display_summary();

	_starpu_deinitialize_registered_performance_models();

	_starpu_watchdog_shutdown();

	/* wait for their termination */
	_starpu_terminate_workers(&config);

	{
	     int stats = starpu_get_env_number("STARPU_MEMORY_STATS");
	     if (stats != 0)
	     {
		  // Display statistics on data which have not been unregistered
		  starpu_data_display_memory_stats();
	     }
	}

	_starpu_delete_all_sched_ctxs();

	_starpu_destroy_topology(&config);

#ifdef STARPU_USE_FXT
	_starpu_stop_fxt_profiling();
#endif

	_starpu_data_interface_shutdown();

	/* Drop all remaining tags */
	_starpu_tag_clear();

	_starpu_close_debug_logfile();

	keys_initialized = 0;
	STARPU_PTHREAD_KEY_DELETE(worker_key);

	_starpu_task_deinit();

	STARPU_PTHREAD_MUTEX_LOCK(&init_mutex);
	initialized = UNINITIALIZED;
	/* Let someone else that wants to initialize it again do it */
	STARPU_PTHREAD_COND_SIGNAL(&init_cond);
	STARPU_PTHREAD_MUTEX_UNLOCK(&init_mutex);

	/* Clear memory */
	free((char*) config.conf.sched_policy_name);
	if (config.conf.n_cuda_opengl_interoperability)
		free(config.conf.cuda_opengl_interoperability);
	if (config.conf.n_not_launched_drivers)
		free(config.conf.not_launched_drivers);

#ifdef HAVE_AYUDAME_H
	if (AYU_event) AYU_event(AYU_FINISH, 0, NULL);
#endif
_starpu_print_idle_time();
	_STARPU_DEBUG("Shutdown finished\n");
}


unsigned starpu_worker_get_count(void)
{
	return config.topology.nworkers;
}

int starpu_worker_get_count_by_type(enum starpu_worker_archtype type)
{
	switch (type)
	{
		case STARPU_CPU_WORKER:
			return config.topology.ncpus;

		case STARPU_CUDA_WORKER:
			return config.topology.ncudagpus;

		case STARPU_OPENCL_WORKER:
			return config.topology.nopenclgpus;

		default:
			return -EINVAL;
	}
}

unsigned starpu_combined_worker_get_count(void)
{
	return config.topology.ncombinedworkers;
}

unsigned starpu_cpu_worker_get_count(void)
{
	return config.topology.ncpus;
}

unsigned starpu_cuda_worker_get_count(void)
{
	return config.topology.ncudagpus;
}

unsigned starpu_opencl_worker_get_count(void)
{
	return config.topology.nopenclgpus;
}

int starpu_asynchronous_copy_disabled(void)
{
	return config.conf.disable_asynchronous_copy;
}

int starpu_asynchronous_cuda_copy_disabled(void)
{
	return config.conf.disable_asynchronous_cuda_copy;
}

int starpu_asynchronous_opencl_copy_disabled(void)
{
	return config.conf.disable_asynchronous_opencl_copy;
}

/* When analyzing performance, it is useful to see what is the processing unit
 * that actually performed the task. This function returns the id of the
 * processing unit actually executing it, therefore it makes no sense to use it
 * within the callbacks of SPU functions for instance. If called by some thread
 * that is not controlled by StarPU, starpu_worker_get_id returns -1. */
int starpu_worker_get_id(void)
{
	struct _starpu_worker * worker;

	worker = _starpu_get_local_worker_key();
	if (worker)
	{
		return worker->workerid;
	}
	else
	{
		/* there is no worker associated to that thread, perhaps it is
		 * a thread from the application or this is some SPU worker */
		return -1;
	}
}

int starpu_combined_worker_get_id(void)
{
	struct _starpu_worker *worker;

	worker = _starpu_get_local_worker_key();
	if (worker)
	{
		return worker->combined_workerid;
	}
	else
	{
		/* there is no worker associated to that thread, perhaps it is
		 * a thread from the application or this is some SPU worker */
		return -1;
	}
}

int starpu_combined_worker_get_size(void)
{
	struct _starpu_worker *worker;

	worker = _starpu_get_local_worker_key();
	if (worker)
	{
		return worker->worker_size;
	}
	else
	{
		/* there is no worker associated to that thread, perhaps it is
		 * a thread from the application or this is some SPU worker */
		return -1;
	}
}

int starpu_combined_worker_get_rank(void)
{
	struct _starpu_worker *worker;

	worker = _starpu_get_local_worker_key();
	if (worker)
	{
		return worker->current_rank;
	}
	else
	{
		/* there is no worker associated to that thread, perhaps it is
		 * a thread from the application or this is some SPU worker */
		return -1;
	}
}

int starpu_worker_get_devid(int id)
{
	return config.workers[id].devid;
}

struct _starpu_worker *_starpu_get_worker_struct(unsigned id)
{
	return &config.workers[id];
}

unsigned starpu_worker_is_combined_worker(int id)
{
	return id >= (int)config.topology.nworkers;
}

unsigned _starpu_get_nsched_ctxs()
{
	return config.topology.nsched_ctxs;
}

struct _starpu_sched_ctx *_starpu_get_sched_ctx_struct(unsigned id)
{
	if (id == STARPU_NMAX_SCHED_CTXS) return NULL;
	return &config.sched_ctxs[id];
}

struct _starpu_combined_worker *_starpu_get_combined_worker_struct(unsigned id)
{
	unsigned basic_worker_count = starpu_worker_get_count();

	STARPU_ASSERT(id >= basic_worker_count);
	return &config.combined_workers[id - basic_worker_count];
}

enum starpu_worker_archtype starpu_worker_get_type(int id)
{
	return config.workers[id].arch;
}

int starpu_worker_get_ids_by_type(enum starpu_worker_archtype type, int *workerids, int maxsize)
{
	unsigned nworkers = starpu_worker_get_count();

	int cnt = 0;

	unsigned id;
	for (id = 0; id < nworkers; id++)
	{
		if (starpu_worker_get_type(id) == type)
		{
			/* Perhaps the array is too small ? */
			if (cnt >= maxsize)
				return -ERANGE;

			workerids[cnt++] = id;
		}
	}

	return cnt;
}

int starpu_worker_get_by_type(enum starpu_worker_archtype type, int num)
{
	unsigned nworkers = starpu_worker_get_count();

	int cnt = 0;

	unsigned id;
	for (id = 0; id < nworkers; id++)
	{
		if (starpu_worker_get_type(id) == type)
		{
			if (num == cnt)
				return id;
			cnt++;
		}
	}

	/* Not found */
	return -1;
}

int starpu_worker_get_by_devid(enum starpu_worker_archtype type, int devid)
{
	unsigned nworkers = starpu_worker_get_count();

	unsigned id;
	for (id = 0; id < nworkers; id++)
		if (starpu_worker_get_type(id) == type && starpu_worker_get_devid(id) == devid)
			return id;

	/* Not found */
	return -1;
}

void starpu_worker_get_name(int id, char *dst, size_t maxlen)
{
	char *name = config.workers[id].name;

	snprintf(dst, maxlen, "%s", name);
}

/* Retrieve the status which indicates what the worker is currently doing. */
enum _starpu_worker_status _starpu_worker_get_status(int workerid)
{
	return config.workers[workerid].status;
}

/* Change the status of the worker which indicates what the worker is currently
 * doing (eg. executing a callback). */
void _starpu_worker_set_status(int workerid, enum _starpu_worker_status status)
{
	config.workers[workerid].status = status;
}

void starpu_worker_get_sched_condition(int workerid, starpu_pthread_mutex_t **sched_mutex, starpu_pthread_cond_t **sched_cond)
{
	*sched_cond = &config.workers[workerid].sched_cond;
	*sched_mutex = &config.workers[workerid].sched_mutex;
}

int starpu_wakeup_worker(int workerid, starpu_pthread_cond_t *cond, starpu_pthread_mutex_t *mutex)
{
	int success = 0;
	STARPU_PTHREAD_MUTEX_LOCK(mutex);
	if (config.workers[workerid].status == STATUS_SLEEPING)
	{
		config.workers[workerid].status = STATUS_WAKING_UP;
		STARPU_PTHREAD_COND_SIGNAL(cond);
		success = 1;
	}
	STARPU_PTHREAD_MUTEX_UNLOCK(mutex);
	return success;
}


int starpu_worker_get_nids_by_type(enum starpu_worker_archtype type, int *workerids, int maxsize)
{
	unsigned nworkers = starpu_worker_get_count();

	int cnt = 0;

	unsigned id;
	for (id = 0; id < nworkers; id++)
	{
		if (starpu_worker_get_type(id) == type)
		{
			/* Perhaps the array is too small ? */
			if (cnt >= maxsize)
				return cnt;

			workerids[cnt++] = id;
		}
	}

	return cnt;
}

int starpu_worker_get_nids_ctx_free_by_type(enum starpu_worker_archtype type, int *workerids, int maxsize)
{
	unsigned nworkers = starpu_worker_get_count();

	int cnt = 0;

	unsigned id, worker;
	unsigned found = 0;
	for (id = 0; id < nworkers; id++)
	{
		found = 0;
		if (starpu_worker_get_type(id) == type)
		{
			/* Perhaps the array is too small ? */
			if (cnt >= maxsize)
				return cnt;
			int s;
			for(s = 1; s < STARPU_NMAX_SCHED_CTXS; s++)
			{
				if(config.sched_ctxs[s].id != STARPU_NMAX_SCHED_CTXS)
				{
					struct starpu_worker_collection *workers = config.sched_ctxs[s].workers;
					struct starpu_sched_ctx_iterator it;
					if(workers->init_iterator)
						workers->init_iterator(workers, &it);

					while(workers->has_next(workers, &it))
					{
						worker = workers->get_next(workers, &it);
						if(worker == id)
						{
							found = 1;
							break;
						}
					}

					if(found) break;
				}
			}
			if(!found)
				workerids[cnt++] = id;
		}
	}

	return cnt;
}


struct _starpu_sched_ctx* _starpu_get_initial_sched_ctx(void)
{
	return &config.sched_ctxs[STARPU_GLOBAL_SCHED_CTX];
}

int starpu_worker_get_nsched_ctxs(int workerid)
{
	return config.workers[workerid].nsched_ctxs;
}

static struct _starpu_worker *
_starpu_get_worker_from_driver(struct starpu_driver *d)
{
	unsigned nworkers = starpu_worker_get_count();
	unsigned workerid;
	for (workerid = 0; workerid < nworkers; workerid++)
	{
		if (starpu_worker_get_type(workerid) == d->type)
		{
			struct _starpu_worker *worker;
			worker = _starpu_get_worker_struct(workerid);
			switch (d->type)
			{
#ifdef STARPU_USE_CPU
			case STARPU_CPU_WORKER:
				if (worker->devid == d->id.cpu_id)
					return worker;
				break;
#endif
#ifdef STARPU_USE_CUDA
			case STARPU_CUDA_WORKER:
				if (worker->devid == d->id.cuda_id)
					return worker;
				break;
#endif
#ifdef STARPU_USE_OPENCL
			case STARPU_OPENCL_WORKER:
			{
				cl_device_id device;
				starpu_opencl_get_device(worker->devid, &device);
				if (device == d->id.opencl_id)
					return worker;
				break;
			}
#endif
			default:
				_STARPU_DEBUG("Invalid device type\n");
				return NULL;
			}
		}
	}

	return NULL;
}

int
starpu_driver_run(struct starpu_driver *d)
{
	if (!d)
	{
		_STARPU_DEBUG("Invalid argument\n");
		return -EINVAL;
	}

	struct _starpu_worker *worker = _starpu_get_worker_from_driver(d);

	switch (d->type)
	{
#ifdef STARPU_USE_CPU
	case STARPU_CPU_WORKER:
		return _starpu_run_cpu(worker);
#endif
#ifdef STARPU_USE_CUDA
	case STARPU_CUDA_WORKER:
		return _starpu_run_cuda(worker);
#endif
#ifdef STARPU_USE_OPENCL
	case STARPU_OPENCL_WORKER:
		return _starpu_run_opencl(worker);
#endif
	default:
		_STARPU_DEBUG("Invalid device type\n");
		return -EINVAL;
	}
}

int
starpu_driver_init(struct starpu_driver *d)
{
	STARPU_ASSERT(d);
	struct _starpu_worker *worker = _starpu_get_worker_from_driver(d);

	switch (d->type)
	{
#ifdef STARPU_USE_CPU
	case STARPU_CPU_WORKER:
		return _starpu_cpu_driver_init(worker);
#endif
#ifdef STARPU_USE_CUDA
	case STARPU_CUDA_WORKER:
		return _starpu_cuda_driver_init(worker);
#endif
#ifdef STARPU_USE_OPENCL
	case STARPU_OPENCL_WORKER:
		return _starpu_opencl_driver_init(worker);
#endif
	default:
		return -EINVAL;
	}
}

int
starpu_driver_run_once(struct starpu_driver *d)
{
	STARPU_ASSERT(d);
	struct _starpu_worker *worker = _starpu_get_worker_from_driver(d);

	switch (d->type)
	{
#ifdef STARPU_USE_CPU
	case STARPU_CPU_WORKER:
		return _starpu_cpu_driver_run_once(worker);
#endif
#ifdef STARPU_USE_CUDA
	case STARPU_CUDA_WORKER:
		return _starpu_cuda_driver_run_once(worker);
#endif
#ifdef STARPU_USE_OPENCL
	case STARPU_OPENCL_WORKER:
		return _starpu_opencl_driver_run_once(worker);
#endif
	default:
		return -EINVAL;
	}
}

int
starpu_driver_deinit(struct starpu_driver *d)
{
	STARPU_ASSERT(d);
	struct _starpu_worker *worker = _starpu_get_worker_from_driver(d);

	switch (d->type)
	{
#ifdef STARPU_USE_CPU
	case STARPU_CPU_WORKER:
		return _starpu_cpu_driver_deinit(worker);
#endif
#ifdef STARPU_USE_CUDA
	case STARPU_CUDA_WORKER:
		return _starpu_cuda_driver_deinit(worker);
#endif
#ifdef STARPU_USE_OPENCL
	case STARPU_OPENCL_WORKER:
		return _starpu_opencl_driver_deinit(worker);
#endif
	default:
		return -EINVAL;
	}
}

void starpu_get_version(int *major, int *minor, int *release)
{
	*major = STARPU_MAJOR_VERSION;
	*minor = STARPU_MINOR_VERSION;
	*release = STARPU_RELEASE_VERSION;
}
