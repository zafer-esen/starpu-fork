/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2009-2014  Université de Bordeaux
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

#include <common/config.h>
#include <common/utils.h>
#include <core/task.h>
#include <datawizard/coherency.h>
#include <datawizard/copy_driver.h>
#include <datawizard/write_back.h>
#include <core/dependencies/data_concurrency.h>
#include <core/sched_policy.h>

/* Explicitly ask StarPU to allocate room for a piece of data on the specified
 * memory node. */
int starpu_data_request_allocation(starpu_data_handle_t handle, unsigned node)
{
	struct _starpu_data_request *r;

	STARPU_ASSERT(handle);

	_starpu_spin_lock(&handle->header_lock);

	r = _starpu_create_data_request(handle, NULL, &handle->per_node[node], node, STARPU_NONE, 0, 1);

	/* we do not increase the refcnt associated to the request since we are
	 * not waiting for its termination */

	_starpu_post_data_request(r, node);

	_starpu_spin_unlock(&handle->header_lock);

	return 0;
}

struct user_interaction_wrapper
{
	starpu_data_handle_t handle;
	enum starpu_data_access_mode mode;
	int node;
	starpu_pthread_cond_t cond;
	starpu_pthread_mutex_t lock;
	unsigned finished;
	unsigned async;
	void (*callback)(void *);
	void (*callback_fetch_data)(void *); // called after fetch_data
	void *callback_arg;
	struct starpu_task *pre_sync_task;
	struct starpu_task *post_sync_task;
};

/*
 *	Non Blocking data request from application
 */
/* put the current value of the data into RAM */
static void _starpu_data_acquire_fetch_data_callback(void *arg)
{
	struct user_interaction_wrapper *wrapper = (struct user_interaction_wrapper *) arg;
	starpu_data_handle_t handle = wrapper->handle;

	/* At that moment, the caller holds a reference to the piece of data.
	 * We enqueue the "post" sync task in the list associated to the handle
	 * so that it is submitted by the starpu_data_release
	 * function. */
	if (wrapper->post_sync_task)
		_starpu_add_post_sync_tasks(wrapper->post_sync_task, handle);

	wrapper->callback(wrapper->callback_arg);

	free(wrapper);
}

static void _starpu_data_acquire_continuation_non_blocking(void *arg)
{
	int ret;
	struct user_interaction_wrapper *wrapper = (struct user_interaction_wrapper *) arg;

	starpu_data_handle_t handle = wrapper->handle;

	STARPU_ASSERT(handle);

	if (wrapper->node >= 0)
	{
		struct _starpu_data_replicate *replicate = &handle->per_node[wrapper->node];

		ret = _starpu_fetch_data_on_node(handle, replicate, wrapper->mode, 0, 1,
						 _starpu_data_acquire_fetch_data_callback, wrapper);
		STARPU_ASSERT(!ret);
	}
	else
		_starpu_data_acquire_fetch_data_callback(wrapper);
}

static void starpu_data_acquire_cb_pre_sync_callback(void *arg)
{
	struct user_interaction_wrapper *wrapper = (struct user_interaction_wrapper *) arg;

	/* we try to get the data, if we do not succeed immediately, we set a
 	* callback function that will be executed automatically when the data is
 	* available again, otherwise we fetch the data directly */
	if (!_starpu_attempt_to_submit_data_request_from_apps(wrapper->handle, wrapper->mode,
			_starpu_data_acquire_continuation_non_blocking, wrapper))
	{
		/* no one has locked this data yet, so we proceed immediately */
		_starpu_data_acquire_continuation_non_blocking(wrapper);
	}
}

/* The data must be released by calling starpu_data_release later on */
int starpu_data_acquire_on_node_cb(starpu_data_handle_t handle, int node,
			   enum starpu_data_access_mode mode, void (*callback)(void *), void *arg)
{
	STARPU_ASSERT(handle);
	STARPU_ASSERT_MSG(handle->nchildren == 0, "Acquiring a partitioned data (%p) is not possible", handle);
        _STARPU_LOG_IN();

	struct user_interaction_wrapper *wrapper = (struct user_interaction_wrapper *) malloc(sizeof(struct user_interaction_wrapper));
	STARPU_ASSERT(wrapper);

	wrapper->handle = handle;
	wrapper->node = node;
	wrapper->mode = mode;
	wrapper->callback = callback;
	wrapper->callback_arg = arg;
	STARPU_PTHREAD_COND_INIT(&wrapper->cond, NULL);
	STARPU_PTHREAD_MUTEX_INIT(&wrapper->lock, NULL);
	wrapper->finished = 0;
	wrapper->pre_sync_task = NULL;
	wrapper->post_sync_task = NULL;

	STARPU_PTHREAD_MUTEX_LOCK(&handle->sequential_consistency_mutex);
	int sequential_consistency = handle->sequential_consistency;
	if (sequential_consistency)
	{
		struct starpu_task *new_task;
		wrapper->pre_sync_task = starpu_task_create();
		wrapper->pre_sync_task->detach = 1;
		wrapper->pre_sync_task->callback_func = starpu_data_acquire_cb_pre_sync_callback;
		wrapper->pre_sync_task->callback_arg = wrapper;

		wrapper->post_sync_task = starpu_task_create();
		wrapper->post_sync_task->detach = 1;

#ifdef STARPU_USE_FXT
                struct _starpu_job *job = _starpu_get_job_associated_to_task(wrapper->pre_sync_task);
                job->model_name = "acquire_cb_pre";
                job = _starpu_get_job_associated_to_task(wrapper->post_sync_task);
                job->model_name = "acquire_cb_post";
#endif

		new_task = _starpu_detect_implicit_data_deps_with_handle(wrapper->pre_sync_task, wrapper->post_sync_task, &_starpu_get_job_associated_to_task(wrapper->post_sync_task)->implicit_dep_slot, handle, mode);
		STARPU_PTHREAD_MUTEX_UNLOCK(&handle->sequential_consistency_mutex);

		if (new_task)
		{
			int ret = _starpu_task_submit_internally(new_task);
			STARPU_ASSERT(!ret);
		}

		/* TODO detect if this is superflous */
		int ret = _starpu_task_submit_internally(wrapper->pre_sync_task);
		STARPU_ASSERT(!ret);
	}
	else
	{
		STARPU_PTHREAD_MUTEX_UNLOCK(&handle->sequential_consistency_mutex);

		starpu_data_acquire_cb_pre_sync_callback(wrapper);
	}

        _STARPU_LOG_OUT();
	return 0;
}

int starpu_data_acquire_cb(starpu_data_handle_t handle,
			   enum starpu_data_access_mode mode, void (*callback)(void *), void *arg)
{
	return starpu_data_acquire_on_node_cb(handle, 0, mode, callback, arg);
}

/*
 *	Block data request from application
 */
static inline void _starpu_data_acquire_continuation(void *arg)
{
	struct user_interaction_wrapper *wrapper = (struct user_interaction_wrapper *) arg;

	starpu_data_handle_t handle = wrapper->handle;

	STARPU_ASSERT(handle);

	if (wrapper->node >= 0)
	{
		int ret;
		struct _starpu_data_replicate *replicate = &handle->per_node[wrapper->node];

		ret = _starpu_fetch_data_on_node(handle, replicate, wrapper->mode, 0, 0, NULL, NULL);
		STARPU_ASSERT(!ret);
	}

	/* continuation of starpu_data_acquire */
	STARPU_PTHREAD_MUTEX_LOCK(&wrapper->lock);
	wrapper->finished = 1;
	STARPU_PTHREAD_COND_SIGNAL(&wrapper->cond);
	STARPU_PTHREAD_MUTEX_UNLOCK(&wrapper->lock);
}

/* The data must be released by calling starpu_data_release later on */
int starpu_data_acquire_on_node(starpu_data_handle_t handle, int node, enum starpu_data_access_mode mode)
{
	STARPU_ASSERT(handle);
	STARPU_ASSERT_MSG(handle->nchildren == 0, "Acquiring a partitioned data is not possible");
        _STARPU_LOG_IN();

	/* unless asynchronous, it is forbidden to call this function from a callback or a codelet */
	STARPU_ASSERT_MSG(_starpu_worker_may_perform_blocking_calls(), "Acquiring a data synchronously is not possible from a codelet or from a task callback, use starpu_data_acquire_cb instead.");

	if (_starpu_data_is_multiformat_handle(handle) &&
	    _starpu_handle_needs_conversion_task(handle, 0))
	{
		struct starpu_task *task = _starpu_create_conversion_task(handle, 0);
		int ret;
		_starpu_spin_lock(&handle->header_lock);
		handle->refcnt--;
		handle->busy_count--;
		handle->mf_node = 0;
		_starpu_spin_unlock(&handle->header_lock);
		task->synchronous = 1;
		ret = _starpu_task_submit_internally(task);
		STARPU_ASSERT(!ret);
	}

	struct user_interaction_wrapper wrapper =
	{
		.handle = handle,
		.mode = mode,
		.node = node,
		.finished = 0
	};

	STARPU_PTHREAD_COND_INIT(&wrapper.cond, NULL);
	STARPU_PTHREAD_MUTEX_INIT(&wrapper.lock, NULL);

//	_STARPU_DEBUG("TAKE sequential_consistency_mutex starpu_data_acquire\n");
	STARPU_PTHREAD_MUTEX_LOCK(&handle->sequential_consistency_mutex);
	int sequential_consistency = handle->sequential_consistency;
	if (sequential_consistency)
	{
		struct starpu_task *new_task;
		wrapper.pre_sync_task = starpu_task_create();
		wrapper.pre_sync_task->detach = 0;

		wrapper.post_sync_task = starpu_task_create();
		wrapper.post_sync_task->detach = 1;

#ifdef STARPU_USE_FXT
                struct _starpu_job *job = _starpu_get_job_associated_to_task(wrapper.pre_sync_task);
                job->model_name = "acquire_pre";
                job = _starpu_get_job_associated_to_task(wrapper.post_sync_task);
                job->model_name = "acquire_post";
#endif

		new_task = _starpu_detect_implicit_data_deps_with_handle(wrapper.pre_sync_task, wrapper.post_sync_task, &_starpu_get_job_associated_to_task(wrapper.post_sync_task)->implicit_dep_slot, handle, mode);
		STARPU_PTHREAD_MUTEX_UNLOCK(&handle->sequential_consistency_mutex);
		if (new_task)
		{
			int ret = _starpu_task_submit_internally(new_task);
			STARPU_ASSERT(!ret);
		}

		/* TODO detect if this is superflous */
		wrapper.pre_sync_task->synchronous = 1;
		int ret = _starpu_task_submit_internally(wrapper.pre_sync_task);
		STARPU_ASSERT(!ret);
	}
	else
	{
		STARPU_PTHREAD_MUTEX_UNLOCK(&handle->sequential_consistency_mutex);
	}

	/* we try to get the data, if we do not succeed immediately, we set a
 	* callback function that will be executed automatically when the data is
 	* available again, otherwise we fetch the data directly */
	if (!_starpu_attempt_to_submit_data_request_from_apps(handle, mode, _starpu_data_acquire_continuation, &wrapper))
	{
		if (node >= 0)
		{
			/* no one has locked this data yet, so we proceed immediately */
			struct _starpu_data_replicate *replicate = &handle->per_node[node];
			int ret = _starpu_fetch_data_on_node(handle, replicate, mode, 0, 0, NULL, NULL);
			STARPU_ASSERT(!ret);
		}
	}
	else
	{
		STARPU_PTHREAD_MUTEX_LOCK(&wrapper.lock);
		while (!wrapper.finished)
			STARPU_PTHREAD_COND_WAIT(&wrapper.cond, &wrapper.lock);
		STARPU_PTHREAD_MUTEX_UNLOCK(&wrapper.lock);
	}
	STARPU_PTHREAD_COND_DESTROY(&wrapper.cond);
	STARPU_PTHREAD_MUTEX_DESTROY(&wrapper.lock);

	/* At that moment, the caller holds a reference to the piece of data.
	 * We enqueue the "post" sync task in the list associated to the handle
	 * so that it is submitted by the starpu_data_release
	 * function. */
	if (sequential_consistency)
		_starpu_add_post_sync_tasks(wrapper.post_sync_task, handle);

        _STARPU_LOG_OUT();
	return 0;
}

int starpu_data_acquire(starpu_data_handle_t handle, enum starpu_data_access_mode mode)
{
	return starpu_data_acquire_on_node(handle, 0, mode);
}

/* This function must be called after starpu_data_acquire so that the
 * application release the data */
void starpu_data_release_on_node(starpu_data_handle_t handle, int node)
{
	STARPU_ASSERT(handle);

	/* In case there are some implicit dependencies, unlock the "post sync" tasks */
	_starpu_unlock_post_sync_tasks(handle);

	/* The application can now release the rw-lock */
	if (node >= 0)
		_starpu_release_data_on_node(handle, 0, &handle->per_node[node]);
	else
	{
		_starpu_spin_lock(&handle->header_lock);
		if (!_starpu_notify_data_dependencies(handle))
			_starpu_spin_unlock(&handle->header_lock);
	}
}

void starpu_data_release(starpu_data_handle_t handle)
{
	starpu_data_release_on_node(handle, 0);
}

static void _prefetch_data_on_node(void *arg)
{
	struct user_interaction_wrapper *wrapper = (struct user_interaction_wrapper *) arg;
	starpu_data_handle_t handle = wrapper->handle;
        int ret;

	struct _starpu_data_replicate *replicate = &handle->per_node[wrapper->node];
	ret = _starpu_fetch_data_on_node(handle, replicate, STARPU_R, wrapper->async, wrapper->async, NULL, NULL);
        STARPU_ASSERT(!ret);

	if (wrapper->async)
		free(wrapper);
	else
	{
		STARPU_PTHREAD_MUTEX_LOCK(&wrapper->lock);
		wrapper->finished = 1;
		STARPU_PTHREAD_COND_SIGNAL(&wrapper->cond);
		STARPU_PTHREAD_MUTEX_UNLOCK(&wrapper->lock);
	}

	_starpu_spin_lock(&handle->header_lock);
	if (!_starpu_notify_data_dependencies(handle))
		_starpu_spin_unlock(&handle->header_lock);
}

static
int _starpu_prefetch_data_on_node_with_mode(starpu_data_handle_t handle, unsigned node, unsigned async, enum starpu_data_access_mode mode)
{
	STARPU_ASSERT(handle);

	/* it is forbidden to call this function from a callback or a codelet */
	STARPU_ASSERT_MSG(async || _starpu_worker_may_perform_blocking_calls(), "Synchronous prefetch is not possible from a task or a callback");

	struct user_interaction_wrapper *wrapper = (struct user_interaction_wrapper *) malloc(sizeof(*wrapper));

	wrapper->handle = handle;
	wrapper->node = node;
	wrapper->async = async;
	STARPU_PTHREAD_COND_INIT(&wrapper->cond, NULL);
	STARPU_PTHREAD_MUTEX_INIT(&wrapper->lock, NULL);
	wrapper->finished = 0;

	if (!_starpu_attempt_to_submit_data_request_from_apps(handle, mode, _prefetch_data_on_node, wrapper))
	{
		/* we can immediately proceed */
		struct _starpu_data_replicate *replicate = &handle->per_node[node];

		STARPU_PTHREAD_COND_DESTROY(&wrapper->cond);
		STARPU_PTHREAD_MUTEX_DESTROY(&wrapper->lock);
		free(wrapper);

		_starpu_fetch_data_on_node(handle, replicate, mode, async, async, NULL, NULL);

		/* remove the "lock"/reference */

		_starpu_spin_lock(&handle->header_lock);

		if (!async)
		{
			/* Release our refcnt, like _starpu_release_data_on_node would do */
			replicate->refcnt--;
			STARPU_ASSERT(replicate->refcnt >= 0);
			STARPU_ASSERT(handle->busy_count > 0);
			handle->busy_count--;
		}

		/* In case there was a temporary handle (eg. used for reduction), this
		 * handle may have requested to be destroyed when the data is released
		 * */
		if (!_starpu_notify_data_dependencies(handle))
			_starpu_spin_unlock(&handle->header_lock);
	}
	else if (!async)
	{
		STARPU_PTHREAD_MUTEX_LOCK(&wrapper->lock);
		while (!wrapper->finished)
			STARPU_PTHREAD_COND_WAIT(&wrapper->cond, &wrapper->lock);
		STARPU_PTHREAD_MUTEX_UNLOCK(&wrapper->lock);
		STARPU_PTHREAD_COND_DESTROY(&wrapper->cond);
		STARPU_PTHREAD_MUTEX_DESTROY(&wrapper->lock);
		free(wrapper);
	}

	return 0;
}

int starpu_data_prefetch_on_node(starpu_data_handle_t handle, unsigned node, unsigned async)
{
	return _starpu_prefetch_data_on_node_with_mode(handle, node, async, STARPU_R);
}

/*
 *	It is possible to specify that a piece of data can be discarded without
 *	impacting the application.
 */
int _starpu_has_not_important_data;
void starpu_data_advise_as_important(starpu_data_handle_t handle, unsigned is_important)
{
	if (!is_important)
		_starpu_has_not_important_data = 1;

	_starpu_spin_lock(&handle->header_lock);

	/* first take all the children lock (in order !) */
	unsigned child;
	for (child = 0; child < handle->nchildren; child++)
	{
		/* make sure the intermediate children is advised as well */
		starpu_data_handle_t child_handle = starpu_data_get_child(handle, child);
		if (child_handle->nchildren > 0)
			starpu_data_advise_as_important(child_handle, is_important);
	}

	handle->is_not_important = !is_important;

	/* now the parent may be used again so we release the lock */
	_starpu_spin_unlock(&handle->header_lock);

}

void starpu_data_set_sequential_consistency_flag(starpu_data_handle_t handle, unsigned flag)
{
	_starpu_spin_lock(&handle->header_lock);

	unsigned child;
	for (child = 0; child < handle->nchildren; child++)
	{
		/* make sure that the flags are applied to the children as well */
		starpu_data_handle_t child_handle = starpu_data_get_child(handle, child);
		if (child_handle->nchildren > 0)
			starpu_data_set_sequential_consistency_flag(child_handle, flag);
	}

	STARPU_PTHREAD_MUTEX_LOCK(&handle->sequential_consistency_mutex);
	handle->sequential_consistency = flag;
	STARPU_PTHREAD_MUTEX_UNLOCK(&handle->sequential_consistency_mutex);

	_starpu_spin_unlock(&handle->header_lock);
}

/* By default, sequential consistency is enabled */
static unsigned default_sequential_consistency_flag = 1;

unsigned starpu_data_get_default_sequential_consistency_flag(void)
{
	return default_sequential_consistency_flag;
}

void starpu_data_set_default_sequential_consistency_flag(unsigned flag)
{
	default_sequential_consistency_flag = flag;
}

/* Query the status of the handle on the specified memory node. */
void starpu_data_query_status(starpu_data_handle_t handle, int memory_node, int *is_allocated, int *is_valid, int *is_requested)
{
// XXX : this is just a hint, so we don't take the lock ...
//	_starpu_spin_lock(&handle->header_lock);

	if (is_allocated)
		*is_allocated = handle->per_node[memory_node].allocated;

	if (is_valid)
		*is_valid = (handle->per_node[memory_node].state != STARPU_INVALID);

	if (is_requested)
	{
		int requested = 0;

		unsigned node;
		for (node = 0; node < STARPU_MAXNODES; node++)
		{
			if (handle->per_node[memory_node].requested & (1UL << node))
			{
				requested = 1;
				break;
			}
		}

		*is_requested = requested;
	}

//	_starpu_spin_unlock(&handle->header_lock);
}
