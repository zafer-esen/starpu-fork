/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2009-2013  Université de Bordeaux
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

#include <starpu.h>
#include <common/config.h>
#include <common/utils.h>
#include <core/dependencies/tags.h>
#include <core/jobs.h>
#include <core/sched_policy.h>
#include <core/dependencies/data_concurrency.h>
#include <profiling/bound.h>
#include <common/uthash.h>
#include <core/debug.h>

#define AYUDAME_OFFSET 4000000000000000000ULL

struct _starpu_tag_table
{
	UT_hash_handle hh;
	starpu_tag_t id;
	struct _starpu_tag *tag;
};

#define HASH_ADD_UINT64_T(head,field,add) HASH_ADD(hh,head,field,sizeof(uint64_t),add)
#define HASH_FIND_UINT64_T(head,find,out) HASH_FIND(hh,head,find,sizeof(uint64_t),out)

static struct _starpu_tag_table *tag_htbl = NULL;
static starpu_pthread_rwlock_t tag_global_rwlock;

static struct _starpu_cg *create_cg_apps(unsigned ntags)
{
	struct _starpu_cg *cg = (struct _starpu_cg *) malloc(sizeof(struct _starpu_cg));
	STARPU_ASSERT(cg);

	cg->ntags = ntags;
	cg->remaining = ntags;
	cg->cg_type = STARPU_CG_APPS;

	cg->succ.succ_apps.completed = 0;
	STARPU_PTHREAD_MUTEX_INIT(&cg->succ.succ_apps.cg_mutex, NULL);
	STARPU_PTHREAD_COND_INIT(&cg->succ.succ_apps.cg_cond, NULL);

	return cg;
}

static struct _starpu_cg *create_cg_tag(unsigned ntags, struct _starpu_tag *tag)
{
	struct _starpu_cg *cg = (struct _starpu_cg *) malloc(sizeof(struct _starpu_cg));
	STARPU_ASSERT(cg);

	cg->ntags = ntags;
	cg->remaining = ntags;
	cg->cg_type = STARPU_CG_TAG;

	cg->succ.tag = tag;
	tag->tag_successors.ndeps++;

	return cg;
}

static struct _starpu_tag *_starpu_tag_init(starpu_tag_t id)
{
	struct _starpu_tag *tag;
	tag = (struct _starpu_tag *) malloc(sizeof(struct _starpu_tag));
	STARPU_ASSERT(tag);

	tag->job = NULL;
	tag->is_assigned = 0;
	tag->is_submitted = 0;

	tag->id = id;
	tag->state = STARPU_INVALID_STATE;

	_starpu_cg_list_init(&tag->tag_successors);

	_starpu_spin_init(&tag->lock);

	return tag;
}

static void _starpu_tag_free(void *_tag)
{
	struct _starpu_tag *tag = (struct _starpu_tag *) _tag;

	if (tag)
	{
		_starpu_spin_lock(&tag->lock);

		unsigned nsuccs = tag->tag_successors.nsuccs;
		unsigned succ;

		for (succ = 0; succ < nsuccs; succ++)
		{
			struct _starpu_cg *cg = tag->tag_successors.succ[succ];

			unsigned ntags = STARPU_ATOMIC_ADD(&cg->ntags, -1);
			unsigned remaining STARPU_ATTRIBUTE_UNUSED = STARPU_ATOMIC_ADD(&cg->remaining, -1);

			if (!ntags && (cg->cg_type == STARPU_CG_TAG))
				/* Last tag this cg depends on, cg becomes unreferenced */
				free(cg);
		}

#ifdef STARPU_DYNAMIC_DEPS_SIZE
		free(tag->tag_successors.succ);
#endif

		_starpu_spin_unlock(&tag->lock);
		_starpu_spin_destroy(&tag->lock);

		free(tag);
	}
}

/*
 * Staticly initializing tag_global_rwlock seems to lead to weird errors
 * on Darwin, so we do it dynamically.
 */
void _starpu_init_tags(void)
{
	STARPU_PTHREAD_RWLOCK_INIT(&tag_global_rwlock, NULL);
}

void starpu_tag_remove(starpu_tag_t id)
{
	struct _starpu_tag_table *entry;

#ifdef HAVE_AYUDAME_H
	if (AYU_event)
		AYU_event(AYU_REMOVETASK, id + AYUDAME_OFFSET, NULL);
#endif

	STARPU_PTHREAD_RWLOCK_WRLOCK(&tag_global_rwlock);

	HASH_FIND_UINT64_T(tag_htbl, &id, entry);
	if (entry) HASH_DEL(tag_htbl, entry);

	STARPU_PTHREAD_RWLOCK_UNLOCK(&tag_global_rwlock);

	if (entry)
	{
		_starpu_tag_free(entry->tag);
		free(entry);
	}
}

void _starpu_tag_clear(void)
{
	STARPU_PTHREAD_RWLOCK_WRLOCK(&tag_global_rwlock);

	/* XXX: _starpu_tag_free takes the tag spinlocks while we are keeping
	 * the global rwlock. This contradicts the lock order of
	 * starpu_tag_wait_array. Should not be a problem in practice since
	 * _starpu_tag_clear is called at shutdown only. */
	struct _starpu_tag_table *entry, *tmp;

	HASH_ITER(hh, tag_htbl, entry, tmp)
	{
		HASH_DEL(tag_htbl, entry);
		_starpu_tag_free(entry->tag);
		free(entry);
	}

	STARPU_PTHREAD_RWLOCK_UNLOCK(&tag_global_rwlock);
}

static struct _starpu_tag *_gettag_struct(starpu_tag_t id)
{
	/* search if the tag is already declared or not */
	struct _starpu_tag_table *entry;
	struct _starpu_tag *tag;

	HASH_FIND_UINT64_T(tag_htbl, &id, entry);
	if (entry != NULL)
	     tag = entry->tag;
	else
	{
		/* the tag does not exist yet : create an entry */
		tag = _starpu_tag_init(id);

		struct _starpu_tag_table *entry2;
		entry2 = (struct _starpu_tag_table *) malloc(sizeof(*entry2));
		STARPU_ASSERT(entry2 != NULL);
		entry2->id = id;
		entry2->tag = tag;

		HASH_ADD_UINT64_T(tag_htbl, id, entry2);

#ifdef HAVE_AYUDAME_H
		if (AYU_event)
		{
			int64_t AYU_data[2] = {0, 0};
			STARPU_ASSERT(id < AYUDAME_OFFSET);
			AYU_event(AYU_ADDTASK, id + AYUDAME_OFFSET, AYU_data);
		}
#endif
	}

	return tag;
}

static struct _starpu_tag *gettag_struct(starpu_tag_t id)
{
	struct _starpu_tag *tag;
	STARPU_PTHREAD_RWLOCK_WRLOCK(&tag_global_rwlock);
	tag = _gettag_struct(id);
	STARPU_PTHREAD_RWLOCK_UNLOCK(&tag_global_rwlock);
	return tag;
}

/* lock should be taken */
void _starpu_tag_set_ready(struct _starpu_tag *tag)
{
	/* mark this tag as ready to run */
	tag->state = STARPU_READY;
	/* declare it to the scheduler ! */
	struct _starpu_job *j = tag->job;

	/* In case the task job is going to be scheduled immediately, and if
	 * the task is "empty", calling _starpu_push_task would directly try to enforce
	 * the dependencies of the task, and therefore it would try to grab the
	 * lock again, resulting in a deadlock. */
	_starpu_spin_unlock(&tag->lock);

	/* enforce data dependencies */
	STARPU_PTHREAD_MUTEX_LOCK(&j->sync_mutex);
	_starpu_enforce_deps_starting_from_task(j);

	_starpu_spin_lock(&tag->lock);
#ifdef HAVE_AYUDAME_H
	if (AYU_event)
	{
		intptr_t id = 0;
		AYU_event(AYU_PRERUNTASK, tag->id + AYUDAME_OFFSET, &id);
		AYU_event(AYU_POSTRUNTASK, tag->id + AYUDAME_OFFSET, NULL);
	}
#endif
}

/* the lock must be taken ! */
static void _starpu_tag_add_succ(struct _starpu_tag *tag, struct _starpu_cg *cg)
{
	STARPU_ASSERT(tag);

	_starpu_add_successor_to_cg_list(&tag->tag_successors, cg);

	if (tag->state == STARPU_DONE)
	{
		/* the tag was already completed sooner */
		_starpu_notify_cg(cg);
	}
}

void _starpu_notify_tag_dependencies(struct _starpu_tag *tag)
{
	_starpu_spin_lock(&tag->lock);

	if (tag->state == STARPU_DONE)
	{
		_starpu_spin_unlock(&tag->lock);
		return;
	}

	tag->state = STARPU_DONE;
	_STARPU_TRACE_TAG_DONE(tag);

	_starpu_notify_cg_list(&tag->tag_successors);

	_starpu_spin_unlock(&tag->lock);
}

void starpu_tag_restart(starpu_tag_t id)
{
	struct _starpu_tag *tag = gettag_struct(id);

	_starpu_spin_lock(&tag->lock);
	STARPU_ASSERT_MSG(tag->state == STARPU_DONE || tag->state == STARPU_INVALID_STATE || tag->state == STARPU_ASSOCIATED || tag->state == STARPU_BLOCKED, "Only completed tags can be restarted (%llu was %d)", (unsigned long long) id, tag->state);
	tag->state = STARPU_BLOCKED;
	_starpu_spin_unlock(&tag->lock);
}

void starpu_tag_notify_from_apps(starpu_tag_t id)
{
	struct _starpu_tag *tag = gettag_struct(id);

	_starpu_notify_tag_dependencies(tag);
}

void _starpu_tag_declare(starpu_tag_t id, struct _starpu_job *job)
{
	_STARPU_TRACE_TAG(id, job);
	job->task->use_tag = 1;

	struct _starpu_tag *tag= gettag_struct(id);

	_starpu_spin_lock(&tag->lock);

	/* Note: a tag can be shared by several tasks, when it is used to
	 * detect when either of them are finished. We however don't allow
	 * several tasks to share a tag when it is used to wake them by
	 * dependency */
	if (tag->job != job)
		tag->is_assigned++;
	tag->job = job;

	job->tag = tag;
	/* the tag is now associated to a job */

	/* When the same tag may be signaled several times by different tasks,
	 * and it's already done, we should not reset the "done" state.
	 * When the tag is simply used by the same task several times, we have
	 * to do so. */
	if (job->task->regenerate || job->submitted == 2 ||
			tag->state != STARPU_DONE)
		tag->state = STARPU_ASSOCIATED;
#ifdef HAVE_AYUDAME_H
	if (AYU_event)
	{
		uintptr_t AYU_data1[3] = {id+AYUDAME_OFFSET, 0, 0};
		uintptr_t AYU_data2[3] = {job->job_id, 0, 0};
		AYU_event(AYU_ADDDEPENDENCY, job->job_id, AYU_data1);
		AYU_event(AYU_ADDDEPENDENCY, id+AYUDAME_OFFSET, AYU_data2);
	}
#endif
	_starpu_spin_unlock(&tag->lock);
}

void starpu_tag_declare_deps_array(starpu_tag_t id, unsigned ndeps, starpu_tag_t *array)
{
	if (!ndeps)
		return;

	unsigned i;

	/* create the associated completion group */
	struct _starpu_tag *tag_child = gettag_struct(id);

	_starpu_spin_lock(&tag_child->lock);
	struct _starpu_cg *cg = create_cg_tag(ndeps, tag_child);
	_starpu_spin_unlock(&tag_child->lock);

	for (i = 0; i < ndeps; i++)
	{
		starpu_tag_t dep_id = array[i];

		/* id depends on dep_id
		 * so cg should be among dep_id's successors*/
		_STARPU_TRACE_TAG_DEPS(id, dep_id);
		_starpu_bound_tag_dep(id, dep_id);
		struct _starpu_tag *tag_dep = gettag_struct(dep_id);
		STARPU_ASSERT(tag_dep != tag_child);
		_starpu_spin_lock(&tag_dep->lock);
		_starpu_spin_lock(&tag_child->lock);
		_starpu_tag_add_succ(tag_dep, cg);
#ifdef HAVE_AYUDAME_H
		if (AYU_event)
		{
			uintptr_t AYU_data[3] = {dep_id+AYUDAME_OFFSET, 0, 0};
			AYU_event(AYU_ADDDEPENDENCY, id+AYUDAME_OFFSET, AYU_data);
		}
#endif
		_starpu_spin_unlock(&tag_child->lock);
		_starpu_spin_unlock(&tag_dep->lock);
	}
}

void starpu_tag_declare_deps(starpu_tag_t id, unsigned ndeps, ...)
{
	if (!ndeps)
		return;

	unsigned i;

	/* create the associated completion group */
	struct _starpu_tag *tag_child = gettag_struct(id);

	_starpu_spin_lock(&tag_child->lock);
	struct _starpu_cg *cg = create_cg_tag(ndeps, tag_child);
	_starpu_spin_unlock(&tag_child->lock);

	va_list pa;
	va_start(pa, ndeps);
	for (i = 0; i < ndeps; i++)
	{
		starpu_tag_t dep_id;
		dep_id = va_arg(pa, starpu_tag_t);

		/* id depends on dep_id
		 * so cg should be among dep_id's successors*/
		_STARPU_TRACE_TAG_DEPS(id, dep_id);
		_starpu_bound_tag_dep(id, dep_id);
		struct _starpu_tag *tag_dep = gettag_struct(dep_id);
		STARPU_ASSERT(tag_dep != tag_child);
		_starpu_spin_lock(&tag_dep->lock);
		_starpu_spin_lock(&tag_child->lock);
		_starpu_tag_add_succ(tag_dep, cg);
#ifdef HAVE_AYUDAME_H
		if (AYU_event)
		{
			uintptr_t AYU_data[3] = {dep_id+AYUDAME_OFFSET, 0, 0};
			AYU_event(AYU_ADDDEPENDENCY, id+AYUDAME_OFFSET, AYU_data);
		}
#endif
		_starpu_spin_unlock(&tag_child->lock);
		_starpu_spin_unlock(&tag_dep->lock);
	}
	va_end(pa);
}

/* this function may be called by the application (outside callbacks !) */
int starpu_tag_wait_array(unsigned ntags, starpu_tag_t *id)
{
	unsigned i;
	unsigned current;

	struct _starpu_tag *tag_array[ntags];

	_STARPU_LOG_IN();

	/* It is forbidden to block within callbacks or codelets */
	STARPU_ASSERT_MSG(_starpu_worker_may_perform_blocking_calls(), "starpu_tag_wait must not be called from a task or callback");

	STARPU_PTHREAD_RWLOCK_WRLOCK(&tag_global_rwlock);
	/* only wait the tags that are not done yet */
	printf("starpu_tag_wait_array waiting for tags 1\n");
	for (i = 0, current = 0; i < ntags; i++)
	{
		struct _starpu_tag *tag = _gettag_struct(id[i]);

		_starpu_spin_lock(&tag->lock);

		if (tag->state == STARPU_DONE)
		{
			/* that tag is done already */
			_starpu_spin_unlock(&tag->lock);
		}
		else
		{
			tag_array[current] = tag;
			current++;
		}
	}
	printf("starpu_tag_wait_array waiting for tags 2\n");
	STARPU_PTHREAD_RWLOCK_UNLOCK(&tag_global_rwlock);
	printf("starpu_tag_wait_array waiting for tags 3\n");
	if (current == 0)
	{
		/* all deps are already fulfilled */
		_STARPU_LOG_OUT_TAG("all deps are already fulfilled");
		return 0;
	}
	printf("starpu_tag_wait_array waiting for tags 4\n");
	/* there is at least one task that is not finished */
	struct _starpu_cg *cg = create_cg_apps(current);
	printf("starpu_tag_wait_array waiting for tags 5\n");
	for (i = 0; i < current; i++)
	{
		_starpu_tag_add_succ(tag_array[i], cg);
		_starpu_spin_unlock(&tag_array[i]->lock);
	}
	printf("starpu_tag_wait_array waiting for tags 6\n");
	STARPU_PTHREAD_MUTEX_LOCK(&cg->succ.succ_apps.cg_mutex);
	printf("starpu_tag_wait_array waiting for tags 7\n");
	while (!cg->succ.succ_apps.completed)
		STARPU_PTHREAD_COND_WAIT(&cg->succ.succ_apps.cg_cond, &cg->succ.succ_apps.cg_mutex);
	printf("starpu_tag_wait_array waiting for tags 8\n");
	STARPU_PTHREAD_MUTEX_UNLOCK(&cg->succ.succ_apps.cg_mutex);
	printf("starpu_tag_wait_array waiting for tags 9\n");
	STARPU_PTHREAD_MUTEX_DESTROY(&cg->succ.succ_apps.cg_mutex);
	printf("starpu_tag_wait_array waiting for tags 10\n");
	STARPU_PTHREAD_COND_DESTROY(&cg->succ.succ_apps.cg_cond);
	printf("starpu_tag_wait_array waiting for tags 11\n");
	free(cg);
	printf("starpu_tag_wait_array waiting for tags 12\n");
	_STARPU_LOG_OUT();
	printf("starpu_tag_wait_array waiting for tags 13\n");
	return 0;
}

int starpu_tag_wait(starpu_tag_t id)
{
	return starpu_tag_wait_array(1, &id);
}
