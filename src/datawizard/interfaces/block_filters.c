/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2010, 2011  Centre National de la Recherche Scientifique
 * Copyright (C) 2013  Université de Bordeaux
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

void starpu_block_filter_block(void *father_interface, void *child_interface, STARPU_ATTRIBUTE_UNUSED struct starpu_data_filter *f,
                                    unsigned id, unsigned nparts)
{
        struct starpu_block_interface *block_father = (struct starpu_block_interface *) father_interface;
        struct starpu_block_interface *block_child = (struct starpu_block_interface *) child_interface;

	uint32_t nx = block_father->nx;
        uint32_t ny = block_father->ny;
        uint32_t nz = block_father->nz;
	size_t elemsize = block_father->elemsize;

	STARPU_ASSERT_MSG(nparts <= nx, "%u parts for %u elements", nparts, nx);

	uint32_t chunk_size;
	size_t offset;
	_starpu_filter_nparts_compute_chunk_size_and_offset(nx, nparts, elemsize, id, 1,
				       &chunk_size, &offset);

	block_child->nx = chunk_size;
	block_child->ny = ny;
	block_child->nz = nz;
	block_child->elemsize = elemsize;

	if (block_father->dev_handle)
	{
		if (block_father->ptr)
                	block_child->ptr = block_father->ptr + offset;
                block_child->ldy = block_father->ldy;
                block_child->ldz = block_father->ldz;
                block_child->dev_handle = block_father->dev_handle;
                block_child->offset = block_father->offset + offset;
	}
}

void starpu_block_filter_block_shadow(void *father_interface, void *child_interface, STARPU_ATTRIBUTE_UNUSED struct starpu_data_filter *f,
                                    unsigned id, unsigned nparts)
{
        struct starpu_block_interface *block_father = (struct starpu_block_interface *) father_interface;
        struct starpu_block_interface *block_child = (struct starpu_block_interface *) child_interface;

        uintptr_t shadow_size = (uintptr_t) f->filter_arg_ptr;

	/* actual number of elements */
	uint32_t nx = block_father->nx - 2 * shadow_size;
        uint32_t ny = block_father->ny;
        uint32_t nz = block_father->nz;
	size_t elemsize = block_father->elemsize;

	STARPU_ASSERT_MSG(nparts <= nx, "%u parts for %u elements", nparts, nx);

	uint32_t child_nx;
	size_t offset;
	_starpu_filter_nparts_compute_chunk_size_and_offset(nx, nparts, elemsize, id, 1,
						     &child_nx, &offset);
	

	block_child->nx = child_nx + 2 * shadow_size;
	block_child->ny = ny;
	block_child->nz = nz;
	block_child->elemsize = elemsize;

	if (block_father->dev_handle)
	{
		if (block_father->ptr)
                	block_child->ptr = block_father->ptr + offset;
                block_child->ldy = block_father->ldy;
                block_child->ldz = block_father->ldz;
                block_child->dev_handle = block_father->dev_handle;
                block_child->offset = block_father->offset + offset;
	}
}

void starpu_block_filter_vertical_block(void *father_interface, void *child_interface, STARPU_ATTRIBUTE_UNUSED struct starpu_data_filter *f,
                                    unsigned id, unsigned nparts)
{
        struct starpu_block_interface *block_father = (struct starpu_block_interface *) father_interface;
        struct starpu_block_interface *block_child = (struct starpu_block_interface *) child_interface;

	uint32_t nx = block_father->nx;
        uint32_t ny = block_father->ny;
        uint32_t nz = block_father->nz;
	size_t elemsize = block_father->elemsize;

	STARPU_ASSERT_MSG(nparts <= ny, "%u parts for %u elements", nparts, ny);

	uint32_t child_ny;
	size_t offset;
	_starpu_filter_nparts_compute_chunk_size_and_offset(ny, nparts, elemsize, id, block_father->ldy,
				       &child_ny, &offset);

	block_child->nx = nx;
	block_child->ny = child_ny;
	block_child->nz = nz;
	block_child->elemsize = elemsize;

	if (block_father->dev_handle)
	{
		if (block_father->ptr)
                	block_child->ptr = block_father->ptr + offset;
                block_child->ldy = block_father->ldy;
                block_child->ldz = block_father->ldz;
                block_child->dev_handle = block_father->dev_handle;
                block_child->offset = block_father->offset + offset;
	}
}

void starpu_block_filter_vertical_block_shadow(void *father_interface, void *child_interface, STARPU_ATTRIBUTE_UNUSED struct starpu_data_filter *f,
                                    unsigned id, unsigned nparts)
{
        struct starpu_block_interface *block_father = (struct starpu_block_interface *) father_interface;
        struct starpu_block_interface *block_child = (struct starpu_block_interface *) child_interface;

        uintptr_t shadow_size = (uintptr_t) f->filter_arg_ptr;

	uint32_t nx = block_father->nx;
	/* actual number of elements */
        uint32_t ny = block_father->ny - 2 * shadow_size;
        uint32_t nz = block_father->nz;
	size_t elemsize = block_father->elemsize;

	STARPU_ASSERT_MSG(nparts <= ny, "%u parts for %u elements", nparts, ny);

	uint32_t child_ny;
	size_t offset;

	_starpu_filter_nparts_compute_chunk_size_and_offset(ny, nparts, elemsize, id,
						     block_father->ldy,
						     &child_ny, &offset);

	block_child->nx = nx;
	block_child->ny = child_ny + 2 * shadow_size;
	block_child->nz = nz;
	block_child->elemsize = elemsize;

	if (block_father->dev_handle)
	{
		if (block_father->ptr)
                	block_child->ptr = block_father->ptr + offset;
                block_child->ldy = block_father->ldy;
                block_child->ldz = block_father->ldz;
                block_child->dev_handle = block_father->dev_handle;
                block_child->offset = block_father->offset + offset;
	}
}

void starpu_block_filter_depth_block(void *father_interface, void *child_interface, STARPU_ATTRIBUTE_UNUSED struct starpu_data_filter *f,
                                    unsigned id, unsigned nparts)
{
        struct starpu_block_interface *block_father = (struct starpu_block_interface *) father_interface;
        struct starpu_block_interface *block_child = (struct starpu_block_interface *) child_interface;

	uint32_t nx = block_father->nx;
        uint32_t ny = block_father->ny;
        uint32_t nz = block_father->nz;
	size_t elemsize = block_father->elemsize;

	STARPU_ASSERT_MSG(nparts <= nz, "%u parts for %u elements", nparts, nz);

	uint32_t child_nz;
	size_t offset;

	_starpu_filter_nparts_compute_chunk_size_and_offset(nz, nparts, elemsize, id,
				       block_father->ldz, &child_nz, &offset);

	block_child->nx = nx;
	block_child->ny = ny;
	block_child->nz = child_nz;
	block_child->elemsize = elemsize;

	if (block_father->dev_handle)
	{
		if (block_father->ptr)
                	block_child->ptr = block_father->ptr + offset;
                block_child->ldy = block_father->ldy;
                block_child->ldz = block_father->ldz;
                block_child->dev_handle = block_father->dev_handle;
                block_child->offset = block_father->offset + offset;
	}
}

void starpu_block_filter_depth_block_shadow(void *father_interface, void *child_interface, STARPU_ATTRIBUTE_UNUSED struct starpu_data_filter *f,
                                    unsigned id, unsigned nparts)
{
        struct starpu_block_interface *block_father = (struct starpu_block_interface *) father_interface;
        struct starpu_block_interface *block_child = (struct starpu_block_interface *) child_interface;

        uintptr_t shadow_size = (uintptr_t) f->filter_arg_ptr;

	uint32_t nx = block_father->nx;
        uint32_t ny = block_father->ny;
	/* actual number of elements */
        uint32_t nz = block_father->nz - 2 * shadow_size;
	size_t elemsize = block_father->elemsize;

	STARPU_ASSERT_MSG(nparts <= nz, "%u parts for %u elements", nparts, nz);

	uint32_t child_nz;
	size_t offset;

	_starpu_filter_nparts_compute_chunk_size_and_offset(nz, nparts, elemsize, id,
						     block_father->ldz,
						     &child_nz, &offset);

	block_child->nx = nx;
	block_child->ny = ny;
	block_child->nz = child_nz + 2 * shadow_size;
	block_child->elemsize = elemsize;

	if (block_father->dev_handle)
	{
		if (block_father->ptr)
                	block_child->ptr = block_father->ptr + offset;
                block_child->ldy = block_father->ldy;
                block_child->ldz = block_father->ldz;
                block_child->dev_handle = block_father->dev_handle;
                block_child->offset = block_father->offset + offset;
	}
}
