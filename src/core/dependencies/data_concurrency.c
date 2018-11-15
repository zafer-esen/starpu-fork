/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2010-2012,2014-2015  Université de Bordeaux
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

#include <core/dependencies/data_concurrency.h>
#include <datawizard/coherency.h>
#include <core/sched_policy.h>
#include <common/starpu_spinlock.h>
#include <datawizard/sort_data_handles.h>
#include <datawizard/memory_nodes.h>

/*
 * We have a kind of dining philosophers problem: various tasks are accessing
 * various data concurrently in different modes: STARPU_R, STARPU_RW, STARPU_W,
 * STARPU_SCRATCH and STARPU_REDUX. STARPU_RW is managed as a STARPU_W access.
 * We have the following constraints:
 *
 * - A single STARPU_W access is allowed at a time.
 * - Concurrent STARPU_R accesses are allowed.
 * - Concurrent STARPU_SCRATCH accesses are allowed.
 * - Concurrent STARPU_REDUX accesses are allowed.
 *
 * What we do here is implementing the Dijkstra solutions: handles are sorted
 * by pointer value order, and tasks call
 * _starpu_attempt_to_submit_data_request for each requested data in that order
 * (see _starpu_sort_task_handles call in _starpu_submit_job_enforce_data_deps).
 *
 * _starpu_attempt_to_submit_data_request will either:
 * - obtain access to the data, and thus the task can proceed with acquiring
 *   other data (see _submit_job_enforce_data_deps)
 * - queue a request on the data handle
 *
 * When a task finishes, it calls _starpu_notify_data_dependencies for each
 * data, to free its acquisitions. This will look whether the first queued
 * request can be fulfilled, and in such case make the task try to acquire its
 * next data.
 *
 * The same mechanism is used for application data aquisition
 * (starpu_data_acquire).
 */

/*
 * Check to see whether the first queued request can proceed, and return it in
 * such case.
 */
/* the header lock must be taken by the caller */
static struct _starpu_data_requester *may_unlock_data_req_list_head(starpu_data_handle_t handle)
{
	struct _starpu_data_requester_list *req_list;

	if (handle->reduction_refcnt > 0)
	{
		req_list = &handle->reduction_req_list;
	}
	else
	{
		if (_starpu_data_requester_list_empty(&handle->reduction_req_list))
			req_list = &handle->req_list;
		else
			req_list = &handle->reduction_req_list;
	}

	/* if there is no one to unlock ... */
	if (_starpu_data_requester_list_empty(req_list))
		return NULL;

	/* if there is no reference to the data anymore, we can use it */
	if (handle->refcnt == 0)
		return _starpu_data_requester_list_pop_front(req_list);

	if (handle->current_mode == STARPU_W)
		return NULL;

	/* data->current_mode == STARPU_R, so we can process more readers */
	struct _starpu_data_requester *r = _starpu_data_requester_list_front(req_list);

	enum starpu_data_access_mode r_mode = r->mode;
	if (r_mode == STARPU_RW)
		r_mode = STARPU_W;

	/* If this is a STARPU_R, STARPU_SCRATCH or STARPU_REDUX type of
	 * access, we only proceed if the cuurrent mode is the same as the
	 * requested mode. */
	if (r_mode == handle->current_mode)
		return _starpu_data_requester_list_pop_front(req_list);
	else
		return NULL;
}

/* Try to submit a data request, in case the request can be processed
 * immediatly, return 0, if there is still a dependency that is not compatible
 * with the current mode, the request is put in the per-handle list of
 * "requesters", and this function returns 1. */
static unsigned _starpu_attempt_to_submit_data_request(unsigned request_from_codelet,
						       starpu_data_handle_t handle, enum starpu_data_access_mode mode,
						       void (*callback)(void *), void *argcb,
						       struct _starpu_job *j, unsigned buffer_index)
{
	if (mode == STARPU_RW)
		mode = STARPU_W;

	/* Take the lock protecting the header. We try to do some progression
	 * in case this is called from a worker, otherwise we just wait for the
	 * lock to be available. */
	if (request_from_codelet)
	{
		int cpt = 0;
		while (cpt < STARPU_SPIN_MAXTRY && _starpu_spin_trylock(&handle->header_lock))
		{
			cpt++;
			_starpu_datawizard_progress(_starpu_memory_node_get_local_key(), 0);
		}
		if (cpt == STARPU_SPIN_MAXTRY)
			_starpu_spin_lock(&handle->header_lock);
	}
	else
	{
		_starpu_spin_lock(&handle->header_lock);
	}

	/* If we have a request that is not used for the reduction, and that a
	 * reduction is pending, we put it at the end of normal list, and we
	 * use the reduction_req_list instead */
	unsigned pending_reduction = (handle->reduction_refcnt > 0);
	unsigned frozen = 0;

	/* If we are currently performing a reduction, we freeze any request
	 * that is not explicitely a reduction task. */
	unsigned is_a_reduction_task = (request_from_codelet && j->reduction_task);

	if (pending_reduction && !is_a_reduction_task)
		frozen = 1;

	/* If there is currently nobody accessing the piece of data, or it's
	 * not another writter and if this is the same type of access as the
	 * current one, we can proceed. */
	unsigned put_in_list = 1;

	enum starpu_data_access_mode previous_mode = handle->current_mode;

	if (!frozen && ((handle->refcnt == 0) || (!(mode == STARPU_W) && (handle->current_mode == mode))))
	{
		/* Detect whether this is the end of a reduction phase */
			/* We don't want to start multiple reductions of the
			 * same handle at the same time ! */

		if ((handle->reduction_refcnt == 0) && (previous_mode == STARPU_REDUX) && (mode != STARPU_REDUX))
		{
			_starpu_data_end_reduction_mode(handle);

			/* Since we need to perform a mode change, we freeze
			 * the request if needed. */
			put_in_list = (handle->reduction_refcnt > 0);
		}
		else
		{
			put_in_list = 0;
		}
	}

	if (put_in_list)
	{
		/* there cannot be multiple writers or a new writer
		 * while the data is in read mode */

		handle->busy_count++;
		/* enqueue the request */
		struct _starpu_data_requester *r = _starpu_data_requester_new();
		r->mode = mode;
		r->is_requested_by_codelet = request_from_codelet;
		r->j = j;
		r->buffer_index = buffer_index;
		r->ready_data_callback = callback;
		r->argcb = argcb;

		/* We put the requester in a specific list if this is a reduction task */
		struct _starpu_data_requester_list *req_list =
			is_a_reduction_task?&handle->reduction_req_list:&handle->req_list;

		_starpu_data_requester_list_push_back(req_list, r);

		/* failed */
		put_in_list = 1;
	}
	else
	{
		handle->refcnt++;
		handle->busy_count++;

		/* Do not write to handle->current_mode if it is already
		 * R. This avoids a spurious warning from helgrind when
		 * the following happens:
		 * acquire(R) in thread A
		 * acquire(R) in thread B
		 * release_data_on_node() in thread A
		 * helgrind would shout that the latter reads current_mode
		 * unsafely.
		 *
		 * This actually basically explains helgrind that it is a
		 * shared R acquisition.
		 */
		if (mode != STARPU_R || handle->current_mode != mode)
			handle->current_mode = mode;

		if ((mode == STARPU_REDUX) && (previous_mode != STARPU_REDUX))
			_starpu_data_start_reduction_mode(handle);

		/* success */
		put_in_list = 0;
	}

	_starpu_spin_unlock(&handle->header_lock);
	return put_in_list;

}

unsigned _starpu_attempt_to_submit_data_request_from_apps(starpu_data_handle_t handle, enum starpu_data_access_mode mode,
							  void (*callback)(void *), void *argcb)
{
	return _starpu_attempt_to_submit_data_request(0, handle, mode, callback, argcb, NULL, 0);
}

static unsigned attempt_to_submit_data_request_from_job(struct _starpu_job *j, unsigned buffer_index)
{
	/* Note that we do not access j->task->handles, but j->ordered_buffers
	 * which is a sorted copy of it. */
	starpu_data_handle_t handle = _STARPU_JOB_GET_ORDERED_BUFFER_HANDLE(j, buffer_index);
	enum starpu_data_access_mode mode = _STARPU_JOB_GET_ORDERED_BUFFER_MODE(j, buffer_index);

	return _starpu_attempt_to_submit_data_request(1, handle, mode, NULL, NULL, j, buffer_index);
}

/* Acquire all data of the given job, one by one in handle pointer value order
 */
static unsigned _submit_job_enforce_data_deps(struct _starpu_job *j, unsigned start_buffer_index)
{
	unsigned buf;

	unsigned nbuffers = j->task->cl->nbuffers;
	for (buf = start_buffer_index; buf < nbuffers; buf++)
	{
		if (buf)
		{
			starpu_data_handle_t handle_m1 = _STARPU_JOB_GET_ORDERED_BUFFER_HANDLE(j, buf-1);
			starpu_data_handle_t handle = _STARPU_JOB_GET_ORDERED_BUFFER_HANDLE(j, buf);
			if (handle_m1 == handle)
				/* We have already requested this data, skip it. This
				 * depends on ordering putting writes before reads, see
				 * _starpu_compar_handles.  */
				continue;
		}

                j->task->status = STARPU_TASK_BLOCKED_ON_DATA;
                if (attempt_to_submit_data_request_from_job(j, buf))
		{
			return 1;
                }
	}

	return 0;
}

/* Sort the data used by the given job by handle pointer value order, and
 * acquire them in that order */
unsigned _starpu_submit_job_enforce_data_deps(struct _starpu_job *j)
{
	struct starpu_codelet *cl = j->task->cl;

	if ((cl == NULL) || (cl->nbuffers == 0))
		return 0;

	/* Compute an ordered list of the different pieces of data so that we
	 * grab then according to a total order, thus avoiding a deadlock
	 * condition */
	unsigned i;
	for (i=0 ; i<cl->nbuffers ; i++)
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

	_starpu_sort_task_handles(_STARPU_JOB_GET_ORDERED_BUFFERS(j), cl->nbuffers);

	return _submit_job_enforce_data_deps(j, 0);
}

/* This request got fulfilled, continue with the other requests of the
 * corresponding job */
static unsigned unlock_one_requester(struct _starpu_data_requester *r)
{
	struct _starpu_job *j = r->j;
	unsigned nbuffers = j->task->cl->nbuffers;
	unsigned buffer_index = r->buffer_index;

	if (buffer_index + 1 < nbuffers)
		/* not all buffers are protected yet */
		return _submit_job_enforce_data_deps(j, buffer_index + 1);
	else
		return 0;
}

/* This is called when a task is finished with a piece of data
 * (or on starpu_data_release)
 *
 * The header lock must already be taken by the caller.
 * This may free the handle if it was lazily unregistered (1 is returned in
 * that case). The handle pointer thus becomes invalid for the caller.
 */
int _starpu_notify_data_dependencies(starpu_data_handle_t handle)
{
	_starpu_spin_checklocked(&handle->header_lock);
	/* A data access has finished so we remove a reference. */
	STARPU_ASSERT(handle->refcnt > 0);
	handle->refcnt--;
	STARPU_ASSERT(handle->busy_count > 0);
	handle->busy_count--;
	if (_starpu_data_check_not_busy(handle))
		/* Handle was destroyed, nothing left to do.  */
		return 1;

	/* In case there is a pending reduction, and that this is the last
	 * requester, we may go back to a "normal" coherency model. */
	if (handle->reduction_refcnt > 0)
	{
		//fprintf(stderr, "NOTIFY REDUCTION TASK RED REFCNT %d\n", handle->reduction_refcnt);
		handle->reduction_refcnt--;
		if (handle->reduction_refcnt == 0)
			_starpu_data_end_reduction_mode_terminate(handle);
	}

	if (handle->unlocking_reqs)
		/*
		 * Our caller is already running the unlock loop below (we were
		 * most probably called from the ready_data_callback call
		 * below). Avoid looping again (which would potentially mean
		 * unbounded recursion), our caller will continue doing the
		 * unlock work for us.
		 */
		return 0;

	handle->unlocking_reqs = 1;
	struct _starpu_data_requester *r;
	while ((r = may_unlock_data_req_list_head(handle)))
	{
		/* STARPU_RW accesses are treated as STARPU_W */
		enum starpu_data_access_mode r_mode = r->mode;
		if (r_mode == STARPU_RW)
			r_mode = STARPU_W;

		int put_in_list = 1;
		if ((handle->reduction_refcnt == 0) && (handle->current_mode == STARPU_REDUX) && (r_mode != STARPU_REDUX))
		{
			_starpu_data_end_reduction_mode(handle);

			/* Since we need to perform a mode change, we freeze
			 * the request if needed. */
			put_in_list = (handle->reduction_refcnt > 0);
		}
		else
		{
			put_in_list = 0;
		}

		if (put_in_list)
		{
			/* We need to put the request back because we must
			 * perform a reduction before. */
			_starpu_data_requester_list_push_front(&handle->req_list, r);
		}
		else
		{
			/* The data is now attributed to that request so we put a
			 * reference on it. */
			handle->refcnt++;
			handle->busy_count++;

			enum starpu_data_access_mode previous_mode = handle->current_mode;
			handle->current_mode = r_mode;

			/* In case we enter in a reduction mode, we invalidate all per
			 * worker replicates. Note that the "per_node" replicates are
			 * kept intact because we'll reduce a valid copy of the
			 * "per-node replicate" with the per-worker replicates .*/
			if ((r_mode == STARPU_REDUX) && (previous_mode != STARPU_REDUX))
				_starpu_data_start_reduction_mode(handle);

			_starpu_spin_unlock(&handle->header_lock);

			if (r->is_requested_by_codelet)
			{
				if (!unlock_one_requester(r))
					_starpu_push_task(r->j);
			}
			else
			{
				STARPU_ASSERT(r->ready_data_callback);

				/* execute the callback associated with the data requester */
				r->ready_data_callback(r->argcb);
			}

			_starpu_data_requester_delete(r);

			_starpu_spin_lock(&handle->header_lock);
			STARPU_ASSERT(handle->busy_count > 0);
			handle->busy_count--;
			if (_starpu_data_check_not_busy(handle))
				return 1;
		}
	}
	handle->unlocking_reqs = 0;

	return 0;
}
