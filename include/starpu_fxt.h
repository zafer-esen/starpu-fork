/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2010-2011, 2013  Université de Bordeaux
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

#ifndef __STARPU_FXT_H__
#define __STARPU_FXT_H__

#include <starpu_perfmodel.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define STARPU_FXT_MAX_FILES	64

struct starpu_fxt_codelet_event
{
	char symbol[256];
	int workerid;
	enum starpu_perfmodel_archtype archtype;
	uint32_t hash;
	size_t size;
	float time;
};

struct starpu_fxt_options
{
	unsigned per_task_colour;
	unsigned no_counter;
	unsigned no_bus;
	unsigned ninputfiles;
	char *filenames[STARPU_FXT_MAX_FILES];
	char *out_paje_path;
	char *distrib_time_path;
	char *activity_path;
	char *dag_path;

	char *file_prefix;
	uint64_t file_offset;
	int file_rank;

	char worker_names[STARPU_NMAXWORKERS][256];
	enum starpu_perfmodel_archtype worker_archtypes[STARPU_NMAXWORKERS];
	int nworkers;

	struct starpu_fxt_codelet_event **dumped_codelets;
	long dumped_codelets_count;
};

void starpu_fxt_options_init(struct starpu_fxt_options *options);
void starpu_fxt_generate_trace(struct starpu_fxt_options *options);
void starpu_fxt_start_profiling(void);
void starpu_fxt_stop_profiling(void);

#ifdef __cplusplus
}
#endif

#endif /* __STARPU_FXT_H__ */
