/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2009-2013  Université de Bordeaux
 * Copyright (C) 2010  Mehdi Juhoor <mjuhoor@gmail.com>
 * Copyright (C) 2010, 2011  Centre National de la Recherche Scientifique
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
#include <datawizard/filters.h>

void starpu_vector_filter_block(void *father_interface, void *child_interface, STARPU_ATTRIBUTE_UNUSED struct starpu_data_filter *f, unsigned id, unsigned nchunks)
{
        struct starpu_vector_interface *vector_father = (struct starpu_vector_interface *) father_interface;
        struct starpu_vector_interface *vector_child = (struct starpu_vector_interface *) child_interface;

	uint32_t nx = vector_father->nx;
	size_t elemsize = vector_father->elemsize;

	STARPU_ASSERT_MSG(nchunks <= nx, "%u parts for %u elements", nchunks, nx);

	uint32_t child_nx;
	size_t offset;
	_starpu_filter_nparts_compute_chunk_size_and_offset(nx, nchunks, elemsize, id, 1,
						     &child_nx, &offset);

	vector_child->nx = child_nx;
	vector_child->elemsize = elemsize;

	if (vector_father->dev_handle)
	{
		if (vector_father->ptr)
			vector_child->ptr = vector_father->ptr + offset;
		vector_child->dev_handle = vector_father->dev_handle;
		vector_child->offset = vector_father->offset + offset;
	}
}


void starpu_vector_filter_block_shadow(void *father_interface, void *child_interface, STARPU_ATTRIBUTE_UNUSED struct starpu_data_filter *f, unsigned id, unsigned nchunks)
{
        struct starpu_vector_interface *vector_father = (struct starpu_vector_interface *) father_interface;
        struct starpu_vector_interface *vector_child = (struct starpu_vector_interface *) child_interface;

        uintptr_t shadow_size = (uintptr_t) f->filter_arg_ptr;

	/* actual number of elements */
	uint32_t nx = vector_father->nx - 2 * shadow_size;
	size_t elemsize = vector_father->elemsize;

	STARPU_ASSERT_MSG(nchunks <= nx, "%u parts for %u elements", nchunks, nx);

	uint32_t child_nx;
	size_t offset;
	_starpu_filter_nparts_compute_chunk_size_and_offset(nx, nchunks, elemsize, id, 1,
						     &child_nx, &offset);
	child_nx += 2*shadow_size;

	vector_child->nx = child_nx;
	vector_child->elemsize = elemsize;

	if (vector_father->dev_handle)
	{
		if (vector_father->ptr)
			vector_child->ptr = vector_father->ptr + offset;
		vector_child->dev_handle = vector_father->dev_handle;
		vector_child->offset = vector_father->offset + offset;
	}
}


void starpu_vector_filter_divide_in_2(void *father_interface, void *child_interface, struct starpu_data_filter *f, unsigned id, STARPU_ATTRIBUTE_UNUSED unsigned nchunks)
{
        /* there cannot be more than 2 chunks */
	STARPU_ASSERT_MSG(id < 2, "Only %d parts", id);

	struct starpu_vector_interface *vector_father = (struct starpu_vector_interface *) father_interface;
	struct starpu_vector_interface *vector_child = (struct starpu_vector_interface *) child_interface;

	uint32_t length_first = f->filter_arg;

	uint32_t nx = vector_father->nx;
	size_t elemsize = vector_father->elemsize;

	STARPU_ASSERT_MSG(length_first < nx, "First part is too long: %u vs %u", length_first, nx);

	/* this is the first child */
	if (id == 0)
	{
		vector_child->nx = length_first;
		vector_child->elemsize = elemsize;

		if (vector_father->dev_handle)
		{
			if (vector_father->ptr)
				vector_child->ptr = vector_father->ptr;
			vector_child->offset = vector_father->offset;
			vector_child->dev_handle = vector_father->dev_handle;
		}
	}
	else /* the second child */
	{
		vector_child->nx = nx - length_first;
		vector_child->elemsize = elemsize;

		if (vector_father->dev_handle)
		{
			if (vector_father->ptr)
				vector_child->ptr = vector_father->ptr + length_first*elemsize;
			vector_child->offset = vector_father->offset + length_first*elemsize;
			vector_child->dev_handle = vector_father->dev_handle;
		}
	}
}


void starpu_vector_filter_list(void *father_interface, void *child_interface, struct starpu_data_filter *f, unsigned id, STARPU_ATTRIBUTE_UNUSED unsigned nchunks)
{
        struct starpu_vector_interface *vector_father = (struct starpu_vector_interface *) father_interface;
        struct starpu_vector_interface *vector_child = (struct starpu_vector_interface *) child_interface;

        uint32_t *length_tab = (uint32_t *) f->filter_arg_ptr;

	size_t elemsize = vector_father->elemsize;

	unsigned current_pos = 0;

	uint32_t chunk_size = length_tab[id];

	vector_child->nx = chunk_size;
	vector_child->elemsize = elemsize;

	if (vector_father->dev_handle)
	{
		/* compute the current position */
		unsigned i;
		for (i = 0; i < id; i++)
			current_pos += length_tab[i];

		if (vector_father->ptr)
			vector_child->ptr = vector_father->ptr + current_pos*elemsize;
		vector_child->offset = vector_father->offset + current_pos*elemsize;
		vector_child->dev_handle = vector_father->dev_handle;
	}
}
