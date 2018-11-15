/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2010-2016  Université de Bordeaux
 * Copyright (C) 2010, 2011, 2013  Centre National de la Recherche Scientifique
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
#include <core/task.h>
#include <datawizard/datawizard.h>
#include <profiling/bound.h>
#include <core/debug.h>

#if 0
# define _STARPU_DEP_DEBUG(fmt, ...) fprintf(stderr, fmt, ## __VA_ARGS__);
#else
# define _STARPU_DEP_DEBUG(fmt, ...)
#endif

static void _starpu_add_ghost_dependency(starpu_data_handle_t handle STARPU_ATTRIBUTE_UNUSED, unsigned long previous STARPU_ATTRIBUTE_UNUSED, struct starpu_task *next STARPU_ATTRIBUTE_UNUSED)
{
	struct _starpu_job *next_job = _starpu_get_job_associated_to_task(next);
	_starpu_bound_job_id_dep(handle, next_job, previous);
#ifdef HAVE_AYUDAME_H
	if (AYU_event)
	{
		uintptr_t AYU_data[3] = { previous, (uintptr_t) handle, (uintptr_t) handle };
		AYU_event(AYU_ADDDEPENDENCY, next_job->job_id, AYU_data);
	}
#endif
}

static void _starpu_add_dependency(starpu_data_handle_t handle STARPU_ATTRIBUTE_UNUSED, struct starpu_task *previous STARPU_ATTRIBUTE_UNUSED, struct starpu_task *next STARPU_ATTRIBUTE_UNUSED)
{
	_starpu_add_ghost_dependency(handle, _starpu_get_job_associated_to_task(previous)->job_id, next);
}

/* Read after Write (RAW) or Read after Read (RAR) */
static void _starpu_add_reader_after_writer(starpu_data_handle_t handle, struct starpu_task *pre_sync_task, struct starpu_task *post_sync_task, struct _starpu_task_wrapper_dlist *post_sync_task_dependency_slot)
{
	/* Add this task to the list of readers */
	STARPU_ASSERT(!post_sync_task_dependency_slot->prev);
	STARPU_ASSERT(!post_sync_task_dependency_slot->next);
	post_sync_task_dependency_slot->task = post_sync_task;
	post_sync_task_dependency_slot->next = handle->last_submitted_accessors.next;
	post_sync_task_dependency_slot->prev = &handle->last_submitted_accessors;
	post_sync_task_dependency_slot->next->prev = post_sync_task_dependency_slot;
	handle->last_submitted_accessors.next = post_sync_task_dependency_slot;

	/* This task depends on the previous writer if any */
	if (handle->last_sync_task && handle->last_sync_task != post_sync_task)
	{
		_STARPU_DEP_DEBUG("RAW %p\n", handle);
		struct starpu_task *task_array[1] = {handle->last_sync_task};
		_starpu_task_declare_deps_array(pre_sync_task, 1, task_array, 0);
		_starpu_add_dependency(handle, handle->last_sync_task, pre_sync_task);
		_STARPU_DEP_DEBUG("dep %p -> %p\n", handle->last_sync_task, pre_sync_task);
	}
        else
        {
		_STARPU_DEP_DEBUG("No dep\n");
        }

	/* There was perhaps no last submitted writer but a
	 * ghost one, we should report that here, and keep the
	 * ghost writer valid */
	if (
		(
#ifdef STARPU_USE_FXT
		1
#else
		_starpu_bound_recording
#endif
#ifdef HAVE_AYUDAME_H
		|| AYU_event
#endif
		) && handle->last_submitted_ghost_sync_id_is_valid)
	{
		_STARPU_TRACE_GHOST_TASK_DEPS(handle->last_submitted_ghost_sync_id,
			_starpu_get_job_associated_to_task(pre_sync_task)->job_id);
		_starpu_add_ghost_dependency(handle, handle->last_submitted_ghost_sync_id, pre_sync_task);
		_STARPU_DEP_DEBUG("dep ID%lu -> %p\n", handle->last_submitted_ghost_sync_id, pre_sync_task);
	}

	if (!pre_sync_task->cl) {
		/* Add a reference to be released in _starpu_handle_job_termination */
		_starpu_spin_lock(&handle->header_lock);
		handle->busy_count++;
		_starpu_spin_unlock(&handle->header_lock);
		_starpu_get_job_associated_to_task(pre_sync_task)->implicit_dep_handle = handle;
	}
}

/* Write after Read (WAR) */
static void _starpu_add_writer_after_readers(starpu_data_handle_t handle, struct starpu_task *pre_sync_task, struct starpu_task *post_sync_task)
{
	/* Count the existing accessors */
	unsigned naccessors = 0;
	struct _starpu_task_wrapper_dlist *l;
	l = handle->last_submitted_accessors.next;
	while (l != &handle->last_submitted_accessors)
	{
		if (l->task == post_sync_task)
		{
			/* Don't make pre_sync_task depend on post_sync_task!
			 * but still drop from the list.
			 * This happens notably when a task accesses several
			 * times to the same data.
			 */
			struct _starpu_task_wrapper_dlist *next;
			l->prev->next = l->next;
			l->next->prev = l->prev;
			l->task = NULL;
			l->prev = NULL;
			next = l->next;
			l->next = NULL;
			l = next;
		}
		else
		{
			naccessors++;
			l = l->next;
		}
	}
	_STARPU_DEP_DEBUG("%d accessors\n", naccessors);

	if (naccessors > 0)
	{
		/* Put all tasks in the list into task_array */
		struct starpu_task *task_array[naccessors];
		unsigned i = 0;
		l = handle->last_submitted_accessors.next;
		while (l != &handle->last_submitted_accessors)
		{
			STARPU_ASSERT(l->task);
			STARPU_ASSERT(l->task != post_sync_task);
			task_array[i++] = l->task;
			_starpu_add_dependency(handle, l->task, pre_sync_task);
			_STARPU_DEP_DEBUG("dep %p -> %p\n", l->task, pre_sync_task);

			struct _starpu_task_wrapper_dlist *prev = l;
			l = l->next;
			prev->task = NULL;
			prev->next = NULL;
			prev->prev = NULL;
		}
		_starpu_task_declare_deps_array(pre_sync_task, naccessors, task_array, 0);
	}
#ifndef STARPU_USE_FXT
	if (_starpu_bound_recording)
#endif
	{
		/* Declare all dependencies with ghost accessors */
		struct _starpu_jobid_list *ghost_accessors_id = handle->last_submitted_ghost_accessors_id;
		while (ghost_accessors_id)
		{
			unsigned long id = ghost_accessors_id->id;
			_STARPU_TRACE_GHOST_TASK_DEPS(id,
				_starpu_get_job_associated_to_task(pre_sync_task)->job_id);
			_starpu_add_ghost_dependency(handle, id, pre_sync_task);
			_STARPU_DEP_DEBUG("dep ID%lu -> %p\n", id, pre_sync_task);

			struct _starpu_jobid_list *prev = ghost_accessors_id;
			ghost_accessors_id = ghost_accessors_id->next;
			free(prev);
		}
		handle->last_submitted_ghost_accessors_id = NULL;
	}

	handle->last_submitted_accessors.next = &handle->last_submitted_accessors;
	handle->last_submitted_accessors.prev = &handle->last_submitted_accessors;
	handle->last_sync_task = post_sync_task;

	if (!post_sync_task->cl) {
		/* Add a reference to be released in _starpu_handle_job_termination */
		_starpu_spin_lock(&handle->header_lock);
		handle->busy_count++;
		_starpu_spin_unlock(&handle->header_lock);
		_starpu_get_job_associated_to_task(post_sync_task)->implicit_dep_handle = handle;
	}
}

/* Write after Write (WAW) */
static void _starpu_add_writer_after_writer(starpu_data_handle_t handle, struct starpu_task *pre_sync_task, struct starpu_task *post_sync_task)
{
	/* (Read) Write */
	/* This task depends on the previous writer */
	if (handle->last_sync_task && handle->last_sync_task != post_sync_task)
	{
		struct starpu_task *task_array[1] = {handle->last_sync_task};
		_starpu_task_declare_deps_array(pre_sync_task, 1, task_array, 0);
		_starpu_add_dependency(handle, handle->last_sync_task, pre_sync_task);
		_STARPU_DEP_DEBUG("dep %p -> %p\n", handle->last_sync_task, pre_sync_task);
	}
        else
        {
		_STARPU_DEP_DEBUG("No dep\n");
        }

	/* If there is a ghost writer instead, we
	 * should declare a ghost dependency here, and
	 * invalidate the ghost value. */
#ifndef STARPU_USE_FXT
	if (_starpu_bound_recording)
#endif
	{
		if (handle->last_submitted_ghost_sync_id_is_valid)
		{
			_STARPU_TRACE_GHOST_TASK_DEPS(handle->last_submitted_ghost_sync_id, 
				_starpu_get_job_associated_to_task(pre_sync_task)->job_id);
			_starpu_add_ghost_dependency(handle, handle->last_submitted_ghost_sync_id, pre_sync_task);
			_STARPU_DEP_DEBUG("dep ID%lu -> %p\n", handle->last_submitted_ghost_sync_id, pre_sync_task);
			handle->last_submitted_ghost_sync_id_is_valid = 0;
		}
                else
                {
			_STARPU_DEP_DEBUG("No dep ID\n");
                }
	}

	handle->last_sync_task = post_sync_task;

	if (!post_sync_task->cl) {
		/* Add a reference to be released in _starpu_handle_job_termination */
		_starpu_spin_lock(&handle->header_lock);
		handle->busy_count++;
		_starpu_spin_unlock(&handle->header_lock);
		_starpu_get_job_associated_to_task(post_sync_task)->implicit_dep_handle = handle;
	}
}

/* This function adds the implicit task dependencies introduced by data
 * sequential consistency. Two tasks are provided: pre_sync and post_sync which
 * respectively indicates which task is going to depend on the previous deps
 * and on which task future deps should wait. In the case of a dependency
 * introduced by a task submission, both tasks are just the submitted task, but
 * in the case of user interactions with the DSM, these may be different tasks.
 * */
/* NB : handle->sequential_consistency_mutex must be hold by the caller;
 * returns a task, to be submitted after releasing that mutex. */
struct starpu_task *_starpu_detect_implicit_data_deps_with_handle(struct starpu_task *pre_sync_task, struct starpu_task *post_sync_task, struct _starpu_task_wrapper_dlist *post_sync_task_dependency_slot,
						   starpu_data_handle_t handle, enum starpu_data_access_mode mode)
{
	struct starpu_task *task = NULL;

	/* Do not care about some flags */
	mode &= ~ STARPU_SSEND;

	STARPU_ASSERT(!(mode & STARPU_SCRATCH));
        _STARPU_LOG_IN();

	if (handle->sequential_consistency)
	{
		struct _starpu_job *pre_sync_job = _starpu_get_job_associated_to_task(pre_sync_task);
		struct _starpu_job *post_sync_job = _starpu_get_job_associated_to_task(post_sync_task);

		/* Skip tasks that are associated to a reduction phase so that
		 * they do not interfere with the application. */
		if (pre_sync_job->reduction_task || post_sync_job->reduction_task)
			return NULL;

		_STARPU_DEP_DEBUG("Tasks %p %p\n", pre_sync_task, post_sync_task);
		/* In case we are generating the DAG, we add an implicit
		 * dependency between the pre and the post sync tasks in case
		 * they are not the same. */
		if (pre_sync_task != post_sync_task
#ifndef STARPU_USE_FXT
			&& _starpu_bound_recording
#endif
		)
		{
			_STARPU_TRACE_GHOST_TASK_DEPS(pre_sync_job->job_id, post_sync_job->job_id);
			_starpu_bound_task_dep(post_sync_job, pre_sync_job);
		}

		enum starpu_data_access_mode previous_mode = handle->last_submitted_mode;

		if (mode & STARPU_W)
		{
			_STARPU_DEP_DEBUG("W %p\n", handle);
			if (previous_mode & STARPU_W)
			{
				_STARPU_DEP_DEBUG("WAW %p\n", handle);
				_starpu_add_writer_after_writer(handle, pre_sync_task, post_sync_task);
			}
			else
			{
				/* The task submitted previously were in read-only
				 * mode: this task must depend on all those read-only
				 * tasks and we get rid of the list of readers */
				_STARPU_DEP_DEBUG("WAR %p\n", handle);
				_starpu_add_writer_after_readers(handle, pre_sync_task, post_sync_task);
			}
		}
		else
		{
			_STARPU_DEP_DEBUG("R %p %d -> %d\n", handle, previous_mode, mode);
			/* Add a reader, after a writer or a reader. */
			STARPU_ASSERT(pre_sync_task);
			STARPU_ASSERT(post_sync_task);

			STARPU_ASSERT(mode & (STARPU_R|STARPU_REDUX));

			if (!(previous_mode & STARPU_W) && (mode != previous_mode))
			{
				/* Read after Redux or Redux after Read: we
				 * insert a dummy synchronization task so that
				 * we don't need to have a gigantic number of
				 * dependencies between all readers and all
				 * redux tasks. */

				/* Create an empty task */
				struct starpu_task *new_sync_task;
				new_sync_task = starpu_task_create();
				STARPU_ASSERT(new_sync_task);
				new_sync_task->cl = NULL;
#ifdef STARPU_USE_FXT
				_starpu_get_job_associated_to_task(new_sync_task)->model_name = "sync_task_redux";
#endif

				_starpu_add_writer_after_readers(handle, new_sync_task, new_sync_task);

				task = new_sync_task;
			}
			_starpu_add_reader_after_writer(handle, pre_sync_task, post_sync_task, post_sync_task_dependency_slot);
		}
		handle->last_submitted_mode = mode;
	}
        _STARPU_LOG_OUT();
	return task;
}

/* Create the implicit dependencies for a newly submitted task */
void _starpu_detect_implicit_data_deps(struct starpu_task *task)
{
	STARPU_ASSERT(task->cl);
        _STARPU_LOG_IN();

	if (!task->sequential_consistency)
		return;

	/* We don't want to enforce a sequential consistency for tasks that are
	 * not visible to the application. */
	struct _starpu_job *j = _starpu_get_job_associated_to_task(task);
	if (j->reduction_task)
		return;

	unsigned nbuffers = task->cl->nbuffers;
	struct _starpu_task_wrapper_dlist *dep_slots = _STARPU_JOB_GET_DEP_SLOTS(j);

	unsigned buffer;
	for (buffer = 0; buffer < nbuffers; buffer++)
	{
		starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(task, buffer);
		enum starpu_data_access_mode mode = STARPU_CODELET_GET_MODE(task->cl, buffer);
		struct starpu_task *new_task;

		/* Scratch memory does not introduce any deps */
		if (mode & STARPU_SCRATCH)
			continue;

		STARPU_PTHREAD_MUTEX_LOCK(&handle->sequential_consistency_mutex);
		new_task = _starpu_detect_implicit_data_deps_with_handle(task, task, &dep_slots[buffer], handle, mode);
		STARPU_PTHREAD_MUTEX_UNLOCK(&handle->sequential_consistency_mutex);
		if (new_task)
		{
			int ret = _starpu_task_submit_internally(new_task);
			STARPU_ASSERT(!ret);
		}
	}
        _STARPU_LOG_OUT();
}

/* This function is called when a task has been executed so that we don't
 * create dependencies to task that do not exist anymore. */
/* NB: We maintain a list of "ghost deps" in case FXT is enabled. Ghost
 * dependencies are the dependencies that are implicitely enforced by StarPU
 * even if they do not imply a real dependency. For instance in the following
 * sequence, f(Ar) g(Ar) h(Aw), we expect to have h depend on both f and g, but
 * if h is submitted after the termination of f or g, StarPU will not create a
 * dependency as this is not needed anymore. */
/* the sequential_consistency_mutex of the handle has to be already held */
void _starpu_release_data_enforce_sequential_consistency(struct starpu_task *task, struct _starpu_task_wrapper_dlist *task_dependency_slot, starpu_data_handle_t handle)
{
	STARPU_PTHREAD_MUTEX_LOCK(&handle->sequential_consistency_mutex);

	if (handle->sequential_consistency)
	{

		/* If this is the last writer, there is no point in adding
		 * extra deps to that tasks that does not exists anymore */
		if (task == handle->last_sync_task)
		{
			handle->last_sync_task = NULL;

#ifndef STARPU_USE_FXT
			if (_starpu_bound_recording)
#endif
			{
				/* Save the previous writer as the ghost last writer */
				handle->last_submitted_ghost_sync_id_is_valid = 1;
				struct _starpu_job *ghost_job = _starpu_get_job_associated_to_task(task);
				handle->last_submitted_ghost_sync_id = ghost_job->job_id;
			}
		}

		/* Same if this is one of the readers: we go through the list
		 * of readers and remove the task if it is found. */
		if (task_dependency_slot && task_dependency_slot->next)
		{
#ifdef STARPU_DEBUG
			/* Make sure we are removing ourself from the proper handle */
			struct _starpu_task_wrapper_dlist *l;
			for (l = task_dependency_slot->prev; l->task; l = l->prev)
				;
			STARPU_ASSERT(l == &handle->last_submitted_accessors);
			for (l = task_dependency_slot->next; l->task; l = l->next)
				;
			STARPU_ASSERT(l == &handle->last_submitted_accessors);
#endif
			STARPU_ASSERT(task_dependency_slot->task == task);

			task_dependency_slot->next->prev = task_dependency_slot->prev;
			task_dependency_slot->prev->next = task_dependency_slot->next;
			task_dependency_slot->task = NULL;
			task_dependency_slot->next = NULL;
			task_dependency_slot->prev = NULL;
#ifndef STARPU_USE_FXT
			if (_starpu_bound_recording)
#endif
			{
				/* Save the job id of the reader task in the ghost reader linked list list */
				struct _starpu_job *ghost_reader_job = _starpu_get_job_associated_to_task(task);
				struct _starpu_jobid_list *link = (struct _starpu_jobid_list *) malloc(sizeof(struct _starpu_jobid_list));
				STARPU_ASSERT(link);
				link->next = handle->last_submitted_ghost_accessors_id;
				link->id = ghost_reader_job->job_id;
				handle->last_submitted_ghost_accessors_id = link;
			}
		}
	}

	STARPU_PTHREAD_MUTEX_UNLOCK(&handle->sequential_consistency_mutex);
}

/* This is the same as _starpu_release_data_enforce_sequential_consistency, but
 * for all data of a task */
void _starpu_release_task_enforce_sequential_consistency(struct _starpu_job *j)
{
	struct starpu_task *task = j->task;
        struct _starpu_data_descr *descrs = _STARPU_JOB_GET_ORDERED_BUFFERS(j);
	struct _starpu_task_wrapper_dlist *slots = _STARPU_JOB_GET_DEP_SLOTS(j);

	if (!task->cl)
		return;

        unsigned nbuffers = task->cl->nbuffers;
	unsigned index;

	/* Release all implicit dependencies */
	for (index = 0; index < nbuffers; index++)
	{
		starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(task, index);

		_starpu_release_data_enforce_sequential_consistency(task, &slots[index], handle);
	}

	for (index = 0; index < nbuffers; index++)
	{
		starpu_data_handle_t handle = descrs[index].handle;

		if (index && descrs[index-1].handle == descrs[index].handle)
			/* We have already released this data, skip it. This
			 * depends on ordering putting writes before reads, see
			 * _starpu_compar_handles */
			continue;

		/* Release the reference acquired in _starpu_push_task_output */
		_starpu_spin_lock(&handle->header_lock);
		STARPU_ASSERT(handle->busy_count > 0);
		handle->busy_count--;
		if (!_starpu_data_check_not_busy(handle))
			_starpu_spin_unlock(&handle->header_lock);

	}
}


void _starpu_add_post_sync_tasks(struct starpu_task *post_sync_task, starpu_data_handle_t handle)
{
        _STARPU_LOG_IN();
	STARPU_PTHREAD_MUTEX_LOCK(&handle->sequential_consistency_mutex);

	if (handle->sequential_consistency)
	{
		handle->post_sync_tasks_cnt++;

		struct _starpu_task_wrapper_list *link = (struct _starpu_task_wrapper_list *) malloc(sizeof(struct _starpu_task_wrapper_list));
		link->task = post_sync_task;
		link->next = handle->post_sync_tasks;
		handle->post_sync_tasks = link;
	}

	STARPU_PTHREAD_MUTEX_UNLOCK(&handle->sequential_consistency_mutex);
        _STARPU_LOG_OUT();
}

void _starpu_unlock_post_sync_tasks(starpu_data_handle_t handle)
{
	struct _starpu_task_wrapper_list *post_sync_tasks = NULL;
	unsigned do_submit_tasks = 0;

	/* Here helgrind would shout that this is an unprotected access, but
	 * count can only be zero if we don't have to care about
	 * post_sync_tasks_cnt at all.  */
	if (STARPU_RUNNING_ON_VALGRIND || handle->post_sync_tasks_cnt)
	{
		STARPU_PTHREAD_MUTEX_LOCK(&handle->sequential_consistency_mutex);
		if (--handle->post_sync_tasks_cnt == 0)
		{
			/* unlock all tasks : we need not hold the lock while unlocking all these tasks */
			do_submit_tasks = 1;
			post_sync_tasks = handle->post_sync_tasks;
			handle->post_sync_tasks = NULL;
		}
		STARPU_PTHREAD_MUTEX_UNLOCK(&handle->sequential_consistency_mutex);
	}

	if (do_submit_tasks)
	{
		struct _starpu_task_wrapper_list *link = post_sync_tasks;

		while (link)
		{
			/* There is no need to depend on that task now, since it was already unlocked */
			_starpu_release_data_enforce_sequential_consistency(link->task, &_starpu_get_job_associated_to_task(link->task)->implicit_dep_slot, handle);

			int ret = _starpu_task_submit_internally(link->task);
			STARPU_ASSERT(!ret);
			struct _starpu_task_wrapper_list *tmp = link;
			link = link->next;
			free(tmp);
		}
	}
}

/* If sequential consistency mode is enabled, this function blocks until the
 * handle is available in the requested access mode. */
int _starpu_data_wait_until_available(starpu_data_handle_t handle, enum starpu_data_access_mode mode, const char *sync_name)
{
	/* If sequential consistency is enabled, wait until data is available */
	STARPU_PTHREAD_MUTEX_LOCK(&handle->sequential_consistency_mutex);
	int sequential_consistency = handle->sequential_consistency;
	if (sequential_consistency)
	{
		struct starpu_task *sync_task, *new_task;
		sync_task = starpu_task_create();
		sync_task->detach = 0;
		sync_task->destroy = 1;
#ifdef STARPU_USE_FXT
		_starpu_get_job_associated_to_task(sync_task)->model_name = sync_name;
#endif

		/* It is not really a RW access, but we want to make sure that
		 * all previous accesses are done */
		new_task = _starpu_detect_implicit_data_deps_with_handle(sync_task, sync_task, &_starpu_get_job_associated_to_task(sync_task)->implicit_dep_slot, handle, mode);
		STARPU_PTHREAD_MUTEX_UNLOCK(&handle->sequential_consistency_mutex);

		if (new_task)
		{
			int ret = _starpu_task_submit_internally(new_task);
			STARPU_ASSERT(!ret);
		}

		/* TODO detect if this is superflous */
		int ret = _starpu_task_submit_internally(sync_task);
		STARPU_ASSERT(!ret);
		ret = starpu_task_wait(sync_task);
		STARPU_ASSERT(ret == 0);
	}
	else
	{
		STARPU_PTHREAD_MUTEX_UNLOCK(&handle->sequential_consistency_mutex);
	}

	return 0;
}

/* This data is about to be freed, clean our stuff */
void _starpu_data_clear_implicit(starpu_data_handle_t handle)
{
	struct _starpu_jobid_list *list;

	STARPU_PTHREAD_MUTEX_LOCK(&handle->sequential_consistency_mutex);
	list = handle->last_submitted_ghost_accessors_id;
	while (list)
	{
		struct _starpu_jobid_list *next = list->next;
		free(list);
		list = next;
	}
	STARPU_PTHREAD_MUTEX_UNLOCK(&handle->sequential_consistency_mutex);
}
