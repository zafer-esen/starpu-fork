/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2010 - 2012  INRIA
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

#ifndef __STARPU_SCHED_CTX_H__
#define __STARPU_SCHED_CTX_H__

#include <starpu.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define STARPU_SCHED_CTX_POLICY_NAME		 (1<<16)
#define STARPU_SCHED_CTX_POLICY_STRUCT		 (2<<16)
#define STARPU_SCHED_CTX_POLICY_MIN_PRIO	 (3<<16)
#define STARPU_SCHED_CTX_POLICY_MAX_PRIO	 (4<<16)

unsigned starpu_sched_ctx_create(int *workerids_ctx, int nworkers_ctx, const char *sched_ctx_name, ...);

unsigned starpu_sched_ctx_create_inside_interval(const char *policy_name, const char *sched_name, int min_ncpus, int max_ncpus, int min_ngpus, int max_ngpus, unsigned allow_overlap);

void starpu_sched_ctx_register_close_callback(unsigned sched_ctx_id, void (*close_callback)(unsigned sched_ctx_id, void* args), void *args);

void starpu_sched_ctx_add_workers(int *workerids_ctx, int nworkers_ctx, unsigned sched_ctx_id);

void starpu_sched_ctx_remove_workers(int *workerids_ctx, int nworkers_ctx, unsigned sched_ctx_id);

void starpu_sched_ctx_delete(unsigned sched_ctx_id);

void starpu_sched_ctx_set_inheritor(unsigned sched_ctx_id, unsigned inheritor);

void starpu_sched_ctx_set_context(unsigned *sched_ctx_id);

unsigned starpu_sched_ctx_get_context(void);

void starpu_sched_ctx_stop_task_submission(void);

void starpu_sched_ctx_finished_submit(unsigned sched_ctx_id);

unsigned starpu_sched_ctx_get_workers_list(unsigned sched_ctx_id, int **workerids);

unsigned starpu_sched_ctx_get_nworkers(unsigned sched_ctx_id);

unsigned starpu_sched_ctx_get_nshared_workers(unsigned sched_ctx_id, unsigned sched_ctx_id2);

unsigned starpu_sched_ctx_contains_worker(int workerid, unsigned sched_ctx_id);

unsigned starpu_sched_ctx_contains_type_of_worker(enum starpu_worker_archtype arch, unsigned sched_ctx_id);

unsigned starpu_sched_ctx_worker_get_id(unsigned sched_ctx_id);

unsigned starpu_sched_ctx_overlapping_ctxs_on_worker(int workerid);

int starpu_sched_get_min_priority(void);

int starpu_sched_get_max_priority(void);

int starpu_sched_set_min_priority(int min_prio);

int starpu_sched_set_max_priority(int max_prio);

int starpu_sched_ctx_get_min_priority(unsigned sched_ctx_id);

int starpu_sched_ctx_get_max_priority(unsigned sched_ctx_id);

int starpu_sched_ctx_set_min_priority(unsigned sched_ctx_id, int min_prio);

int starpu_sched_ctx_set_max_priority(unsigned sched_ctx_id, int max_prio);

int starpu_sched_ctx_min_priority_is_set(unsigned sched_ctx_id);

int starpu_sched_ctx_max_priority_is_set(unsigned sched_ctx_id);

#define STARPU_MIN_PRIO		(starpu_sched_get_min_priority())
#define STARPU_MAX_PRIO		(starpu_sched_get_max_priority())

#define STARPU_DEFAULT_PRIO	0

struct starpu_worker_collection *starpu_sched_ctx_create_worker_collection(unsigned sched_ctx_id, enum starpu_worker_collection_type type) STARPU_ATTRIBUTE_MALLOC;

void starpu_sched_ctx_delete_worker_collection(unsigned sched_ctx_id);

struct starpu_worker_collection *starpu_sched_ctx_get_worker_collection(unsigned sched_ctx_id);

void starpu_sched_ctx_set_policy_data(unsigned sched_ctx_id, void *policy_data);

void *starpu_sched_ctx_get_policy_data(unsigned sched_ctx_id);

int starpu_sched_ctx_get_nready_tasks(unsigned sched_ctx_id);

double starpu_sched_ctx_get_nready_flops(unsigned sched_ctx_id);

#ifdef STARPU_USE_SC_HYPERVISOR
void starpu_sched_ctx_call_pushed_task_cb(int workerid, unsigned sched_ctx_id);
#endif /* STARPU_USE_SC_HYPERVISOR */

#ifdef __cplusplus
}
#endif

#endif /* __STARPU_SCHED_CTX_H__ */
