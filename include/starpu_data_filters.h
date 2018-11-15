/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2010-2012  Université de Bordeaux
 * Copyright (C) 2010  Mehdi Juhoor <mjuhoor@gmail.com>
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

#ifndef __STARPU_DATA_FILTERS_H__
#define __STARPU_DATA_FILTERS_H__

#include <starpu.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C"
{
#endif

struct starpu_data_interface_ops;

struct starpu_data_filter
{
	void (*filter_func)(void *father_interface, void *child_interface, struct starpu_data_filter *, unsigned id, unsigned nparts);
	unsigned nchildren;
	unsigned (*get_nchildren)(struct starpu_data_filter *, starpu_data_handle_t initial_handle);
	struct starpu_data_interface_ops *(*get_child_ops)(struct starpu_data_filter *, unsigned id);
	unsigned filter_arg;
	void *filter_arg_ptr;
};

void starpu_data_partition(starpu_data_handle_t initial_handle, struct starpu_data_filter *f);
void starpu_data_unpartition(starpu_data_handle_t root_data, unsigned gathering_node);

int starpu_data_get_nb_children(starpu_data_handle_t handle);
starpu_data_handle_t starpu_data_get_child(starpu_data_handle_t handle, unsigned i);

starpu_data_handle_t starpu_data_get_sub_data(starpu_data_handle_t root_data, unsigned depth, ... );
starpu_data_handle_t starpu_data_vget_sub_data(starpu_data_handle_t root_data, unsigned depth, va_list pa);

void starpu_data_map_filters(starpu_data_handle_t root_data, unsigned nfilters, ...);
void starpu_data_vmap_filters(starpu_data_handle_t root_data, unsigned nfilters, va_list pa);

void starpu_bcsr_filter_canonical_block(void *father_interface, void *child_interface, struct starpu_data_filter *f, unsigned id, unsigned nparts);
void starpu_csr_filter_vertical_block(void *father_interface, void *child_interface, struct starpu_data_filter *f, unsigned id, unsigned nparts);

void starpu_matrix_filter_block(void *father_interface, void *child_interface, struct starpu_data_filter *f, unsigned id, unsigned nparts);
void starpu_matrix_filter_block_shadow(void *father_interface, void *child_interface, struct starpu_data_filter *f, unsigned id, unsigned nparts);
void starpu_matrix_filter_vertical_block(void *father_interface, void *child_interface, struct starpu_data_filter *f, unsigned id, unsigned nparts);
void starpu_matrix_filter_vertical_block_shadow(void *father_interface, void *child_interface, struct starpu_data_filter *f, unsigned id, unsigned nparts);

void starpu_vector_filter_block(void *father_interface, void *child_interface, struct starpu_data_filter *f, unsigned id, unsigned nparts);
void starpu_vector_filter_block_shadow(void *father_interface, void *child_interface, struct starpu_data_filter *f, unsigned id, unsigned nparts);
void starpu_vector_filter_list(void *father_interface, void *child_interface, struct starpu_data_filter *f, unsigned id, unsigned nparts);
void starpu_vector_filter_divide_in_2(void *father_interface, void *child_interface, struct starpu_data_filter *f, unsigned id, unsigned nparts);

void starpu_block_filter_block(void *father_interface, void *child_interface, struct starpu_data_filter *f, unsigned id, unsigned nparts);
void starpu_block_filter_block_shadow(void *father_interface, void *child_interface, struct starpu_data_filter *f, unsigned id, unsigned nparts);
void starpu_block_filter_vertical_block(void *father_interface, void *child_interface, struct starpu_data_filter *f, unsigned id, unsigned nparts);
void starpu_block_filter_vertical_block_shadow(void *father_interface, void *child_interface, struct starpu_data_filter *f, unsigned id, unsigned nparts);
void starpu_block_filter_depth_block(void *father_interface, void *child_interface, struct starpu_data_filter *f, unsigned id, unsigned nparts);
void starpu_block_filter_depth_block_shadow(void *father_interface, void *child_interface, struct starpu_data_filter *f, unsigned id, unsigned nparts);

#ifdef __cplusplus
}
#endif

#endif
