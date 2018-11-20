/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2009-2016  Université de Bordeaux
 * Copyright (C) 2010, 2011, 2012, 2013  Centre National de la Recherche Scientifique
 * Copyright (C) 2011  Télécom-SudParis
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

#include "argo.h"
#include <starpu.h>
#include <starpu_profiling.h>
#include <common/config.h>
#include <common/utils.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <sys/stat.h>
#include <core/perfmodel/perfmodel.h>
#include <core/jobs.h>
#include <core/workers.h>
#include <datawizard/datawizard.h>

#ifdef STARPU_HAVE_WINDOWS
#include <windows.h>
#endif

/* This flag indicates whether performance models should be calibrated or not.
 *	0: models need not be calibrated
 *	1: models must be calibrated
 *	2: models must be calibrated, existing models are overwritten.
 */
static unsigned calibrate_flag = 0;

void _starpu_set_calibrate_flag(unsigned val)
{
	calibrate_flag = val;
}

unsigned _starpu_get_calibrate_flag(void)
{
	return calibrate_flag;
}

enum starpu_perfmodel_archtype starpu_worker_get_perf_archtype(int workerid)
{
	struct _starpu_machine_config *config = _starpu_get_machine_config();

	/* This workerid may either be a basic worker or a combined worker */
	unsigned nworkers = config->topology.nworkers;

	if (workerid < (int)config->topology.nworkers)
		return config->workers[workerid].perf_arch;

	/* We have a combined worker */
	unsigned ncombinedworkers = config->topology.ncombinedworkers;
	STARPU_ASSERT(workerid < (int)(ncombinedworkers + nworkers));
	return config->combined_workers[workerid - nworkers].perf_arch;
}

/*
 * PER ARCH model
 */

static double per_arch_task_expected_perf(struct starpu_perfmodel *model, enum starpu_perfmodel_archtype arch, struct starpu_task *task, unsigned nimpl)
{
	double exp = NAN;
	double (*per_arch_cost_function)(struct starpu_task *task, enum starpu_perfmodel_archtype arch, unsigned nimpl);
	double (*per_arch_cost_model)(struct starpu_data_descr *);

	per_arch_cost_function = model->per_arch[arch][nimpl].cost_function;
	per_arch_cost_model = model->per_arch[arch][nimpl].cost_model;

	if (per_arch_cost_function)
		exp = per_arch_cost_function(task, arch, nimpl);
	else if (per_arch_cost_model)
		exp = per_arch_cost_model(task->buffers);

	return exp;
}

/*
 * Common model
 */

double starpu_worker_get_relative_speedup(enum starpu_perfmodel_archtype perf_archtype)
{
	if (perf_archtype < STARPU_CUDA_DEFAULT)
	{
		return _STARPU_CPU_ALPHA * (perf_archtype + 1);
	}
	else if (perf_archtype < STARPU_OPENCL_DEFAULT)
	{
		return _STARPU_CUDA_ALPHA;
	}
	else if (perf_archtype < STARPU_NARCH_VARIATIONS)
	{
		return _STARPU_OPENCL_ALPHA;
	}

	STARPU_ABORT();

	/* Never reached ! */
	return NAN;
}

static double common_task_expected_perf(struct starpu_perfmodel *model, enum starpu_perfmodel_archtype arch, struct starpu_task *task, unsigned nimpl)
{
	double exp;
	double alpha;

	if (model->cost_function)
	{
		exp = model->cost_function(task, nimpl);
		alpha = starpu_worker_get_relative_speedup(arch);

		STARPU_ASSERT(!_STARPU_IS_ZERO(alpha));

		return (exp/alpha);
	}
	else if (model->cost_model)
	{
		exp = model->cost_model(task->buffers);
		alpha = starpu_worker_get_relative_speedup(arch);

		STARPU_ASSERT(!_STARPU_IS_ZERO(alpha));

		return (exp/alpha);
	}

	return NAN;
}

void _starpu_load_perfmodel(struct starpu_perfmodel *model)
{
	if (!model || model->is_loaded)
		return;

	int load_model = _starpu_register_model(model);

	if (!load_model)
		return;

	switch (model->type)
	{
		case STARPU_PER_ARCH:
			_starpu_load_per_arch_based_model(model);
			break;
		case STARPU_COMMON:
			_starpu_load_common_based_model(model);
			break;
		case STARPU_HISTORY_BASED:
		case STARPU_NL_REGRESSION_BASED:
			_starpu_load_history_based_model(model, 1);
			break;
		case STARPU_REGRESSION_BASED:
			_starpu_load_history_based_model(model, 0);
			break;

		default:
			STARPU_ABORT();
	}

	model->is_loaded = 1;
}

static double starpu_model_expected_perf(struct starpu_task *task, struct starpu_perfmodel *model, enum starpu_perfmodel_archtype arch,  unsigned nimpl)
{
	if (model)
	{
		if (model->symbol)
			_starpu_load_perfmodel(model);

		struct _starpu_job *j = _starpu_get_job_associated_to_task(task);

		switch (model->type)
		{
			case STARPU_PER_ARCH:

				return per_arch_task_expected_perf(model, arch, task, nimpl);
			case STARPU_COMMON:
				return common_task_expected_perf(model, arch, task, nimpl);

			case STARPU_HISTORY_BASED:

				return _starpu_history_based_job_expected_perf(model, arch, j, nimpl);
			case STARPU_REGRESSION_BASED:

				return _starpu_regression_based_job_expected_perf(model, arch, j, nimpl);

			case STARPU_NL_REGRESSION_BASED:

				return _starpu_non_linear_regression_based_job_expected_perf(model, arch, j,nimpl);

			default:
				STARPU_ABORT();
		}
	}

	/* no model was found */
	return 0.0;
}

double starpu_task_expected_length(struct starpu_task *task, enum starpu_perfmodel_archtype arch, unsigned nimpl)
{
	if (!task->cl)
		/* Tasks without codelet don't actually take time */
		return 0.0;
	return starpu_model_expected_perf(task, task->cl->model, arch, nimpl);
}

double starpu_task_expected_power(struct starpu_task *task, enum starpu_perfmodel_archtype arch, unsigned nimpl)
{
	if (!task->cl)
		/* Tasks without codelet don't actually take time */
		return 0.0;
	return starpu_model_expected_perf(task, task->cl->power_model, arch, nimpl);
}

double starpu_task_expected_conversion_time(struct starpu_task *task,
					    enum starpu_perfmodel_archtype arch,
					    unsigned nimpl)
{
	unsigned i;
	double sum = 0.0;
	enum starpu_node_kind node_kind;

	for (i = 0; i < task->cl->nbuffers; i++)
	{
		starpu_data_handle_t handle;
		struct starpu_task *conversion_task;

		handle = STARPU_TASK_GET_HANDLE(task, i);
		if (!_starpu_data_is_multiformat_handle(handle))
			continue;

		if (arch < STARPU_CUDA_DEFAULT)
			node_kind = STARPU_CPU_RAM;
		else if (arch < STARPU_OPENCL_DEFAULT)
			node_kind = STARPU_CUDA_RAM;
		else
			node_kind = STARPU_OPENCL_RAM;

		if (!_starpu_handle_needs_conversion_task_for_arch(handle, node_kind))
			continue;

		conversion_task = _starpu_create_conversion_task_for_arch(handle, node_kind);
		sum += starpu_task_expected_length(conversion_task, arch, nimpl);
		_starpu_spin_lock(&handle->header_lock);
		handle->refcnt--;
		handle->busy_count--;
		if (!_starpu_data_check_not_busy(handle))
			_starpu_spin_unlock(&handle->header_lock);
		starpu_task_clean(conversion_task);
		free(conversion_task);
	}

	return sum;
}

/* Predict the transfer time (in µs) to move a handle to a memory node */
double starpu_data_expected_transfer_time(starpu_data_handle_t handle, unsigned memory_node, enum starpu_data_access_mode mode)
{
	/* If we don't need to read the content of the handle */
	if (!(mode & STARPU_R))
		return 0.0;

	if (_starpu_is_data_present_or_requested(handle, memory_node))
		return 0.0;

	size_t size = _starpu_data_get_size(handle);

	/* XXX in case we have an abstract piece of data (eg.  with the
	 * void interface, this does not introduce any overhead, and we
	 * don't even want to consider the latency that is not
	 * relevant). */
	if (size == 0)
		return 0.0;

	int src_node = _starpu_select_src_node(handle, memory_node);
	if (src_node < 0)
		/* Will just create it in place. Ideally we should take the
		 * time to create it into account */
		return 0.0;
	return starpu_transfer_predict(src_node, memory_node, size);
}



/* Data transfer performance modeling */
double starpu_task_expected_data_transfer_time(unsigned memory_node, struct starpu_task *task)
{
	unsigned nbuffers = task->cl->nbuffers;
	unsigned buffer;

	double penalty = 0.0;


	void *local_ptr;
	size_t data_size;

	for (buffer = 0; buffer < nbuffers; buffer++)
	{
		starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(task, buffer);
		enum starpu_data_access_mode mode = STARPU_CODELET_GET_MODE(task->cl, buffer);
		local_ptr = starpu_data_get_local_ptr(handle);
		data_size = starpu_data_get_size(handle);
		//Potentially want to add argotime here
		penalty += starpu_data_expected_transfer_time(handle, memory_node, mode);
	}

	return penalty;
}

/* Return the expected duration of the entire task bundle in µs */
double starpu_task_bundle_expected_length(starpu_task_bundle_t bundle, enum starpu_perfmodel_archtype arch, unsigned nimpl)
{
	double expected_length = 0.0;

	/* We expect the length of the bundle the be the sum of the different tasks length. */
	STARPU_PTHREAD_MUTEX_LOCK(&bundle->mutex);

	struct _starpu_task_bundle_entry *entry;
	entry = bundle->list;

	while (entry)
	{
		if(!entry->task->scheduled)
		{
			double task_length = starpu_task_expected_length(entry->task, arch, nimpl);

			/* In case the task is not calibrated, we consider the task
			 * ends immediately. */
			if (task_length > 0.0)
				expected_length += task_length;
		}

		entry = entry->next;
	}

	STARPU_PTHREAD_MUTEX_UNLOCK(&bundle->mutex);

	return expected_length;
}

/* Return the expected power consumption of the entire task bundle in J */
double starpu_task_bundle_expected_power(starpu_task_bundle_t bundle, enum starpu_perfmodel_archtype arch, unsigned nimpl)
{
	double expected_power = 0.0;

	/* We expect total consumption of the bundle the be the sum of the different tasks consumption. */
	STARPU_PTHREAD_MUTEX_LOCK(&bundle->mutex);

	struct _starpu_task_bundle_entry *entry;
	entry = bundle->list;

	while (entry)
	{
		double task_power = starpu_task_expected_power(entry->task, arch, nimpl);

		/* In case the task is not calibrated, we consider the task
		 * ends immediately. */
		if (task_power > 0.0)
			expected_power += task_power;

		entry = entry->next;
	}

	STARPU_PTHREAD_MUTEX_UNLOCK(&bundle->mutex);

	return expected_power;
}

/* Return the time (in µs) expected to transfer all data used within the bundle */
double starpu_task_bundle_expected_data_transfer_time(starpu_task_bundle_t bundle, unsigned memory_node)
{
	STARPU_PTHREAD_MUTEX_LOCK(&bundle->mutex);

	struct _starpu_handle_list *handles = NULL;

	/* We list all the handle that are accessed within the bundle. */

	/* For each task in the bundle */
	struct _starpu_task_bundle_entry *entry = bundle->list;
	while (entry)
	{
		struct starpu_task *task = entry->task;

		if (task->cl)
		{
			unsigned b;
			for (b = 0; b < task->cl->nbuffers; b++)
			{
				starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(task, b);
				enum starpu_data_access_mode mode = STARPU_CODELET_GET_MODE(task->cl, b);

				if (!(mode & STARPU_R))
					continue;

				/* Insert the handle in the sorted list in case
				 * it's not already in that list. */
				_insertion_handle_sorted(&handles, handle, mode);
			}
		}

		entry = entry->next;
	}

	STARPU_PTHREAD_MUTEX_UNLOCK(&bundle->mutex);

	/* Compute the sum of data transfer time, and destroy the list */

	double total_exp = 0.0;

	while (handles)
	{
		struct _starpu_handle_list *current = handles;
		handles = handles->next;

		double exp;
		exp = starpu_data_expected_transfer_time(current->handle, memory_node, current->mode);

		total_exp += exp;

		free(current);
	}

	return total_exp;
}

static int directory_existence_was_tested = 0;
static char *_perf_model_dir = NULL;
static char *_perf_model_dir_codelet = NULL;
static char *_perf_model_dir_bus = NULL;
static char *_perf_model_dir_debug = NULL;
#define _PERF_MODEL_DIR_MAXLEN 256

void _starpu_set_perf_model_dirs()
{
	_perf_model_dir = malloc(_PERF_MODEL_DIR_MAXLEN);
	_perf_model_dir_codelet = malloc(_PERF_MODEL_DIR_MAXLEN);
	_perf_model_dir_bus = malloc(_PERF_MODEL_DIR_MAXLEN);
	_perf_model_dir_debug = malloc(_PERF_MODEL_DIR_MAXLEN);

#ifdef STARPU_PERF_MODEL_DIR
	/* use the directory specified at configure time */
	snprintf(_perf_model_dir, _PERF_MODEL_DIR_MAXLEN, "%s", STARPU_PERF_MODEL_DIR);
#else
	snprintf(_perf_model_dir, _PERF_MODEL_DIR_MAXLEN, "%s/.starpu/sampling/", _starpu_get_home_path());
#endif
	char *path = starpu_getenv("STARPU_PERF_MODEL_DIR");
	if (path)
	{
		snprintf(_perf_model_dir, _PERF_MODEL_DIR_MAXLEN, "%s/", path);
	}

	snprintf(_perf_model_dir_codelet, _PERF_MODEL_DIR_MAXLEN, "%s/codelets/%d/", _perf_model_dir, _STARPU_PERFMODEL_VERSION);
	snprintf(_perf_model_dir_bus, _PERF_MODEL_DIR_MAXLEN, "%s/bus/", _perf_model_dir);
	snprintf(_perf_model_dir_debug, _PERF_MODEL_DIR_MAXLEN, "%s/debug/", _perf_model_dir);
}

char *_starpu_get_perf_model_dir_codelet()
{
	_starpu_create_sampling_directory_if_needed();
	return _perf_model_dir_codelet;
}

char *_starpu_get_perf_model_dir_bus()
{
	_starpu_create_sampling_directory_if_needed();
	return _perf_model_dir_bus;
}

char *_starpu_get_perf_model_dir_debug()
{
	_starpu_create_sampling_directory_if_needed();
	return _perf_model_dir_debug;
}

void _starpu_create_sampling_directory_if_needed(void)
{
	if (!directory_existence_was_tested)
	{
		_starpu_set_perf_model_dirs();

		/* The performance of the codelets are stored in
		 * $STARPU_PERF_MODEL_DIR/codelets/ while those of the bus are stored in
		 * $STARPU_PERF_MODEL_DIR/bus/ so that we don't have name collisions */

		/* Testing if a directory exists and creating it otherwise
		   may not be safe: it is possible that the permission are
		   changed in between. Instead, we create it and check if
		   it already existed before */
		_starpu_mkpath_and_check(_perf_model_dir, S_IRWXU);

		/* Per-task performance models */
		_starpu_mkpath_and_check(_perf_model_dir_codelet, S_IRWXU);

		/* Performance of the memory subsystem */
		_starpu_mkpath_and_check(_perf_model_dir_bus, S_IRWXU);

		/* Performance debug measurements */
		_starpu_mkpath(_perf_model_dir_debug, S_IRWXU);

		directory_existence_was_tested = 1;
	}
}

void starpu_perfmodel_free_sampling_directories(void)
{
	free(_perf_model_dir);
	_perf_model_dir = NULL;
	free(_perf_model_dir_codelet);
	_perf_model_dir_codelet = NULL;
	free(_perf_model_dir_bus);
	_perf_model_dir_bus = NULL;
	free(_perf_model_dir_debug);
	_perf_model_dir_debug = NULL;
	directory_existence_was_tested = 0;
}
