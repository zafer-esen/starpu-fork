/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2010-2013, 2015-2016  Université de Bordeaux
 * Copyright (C) 2010, 2011, 2012, 2013, 2014  Centre National de la Recherche Scientifique
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
#include <datawizard/coherency.h>
#include <datawizard/copy_driver.h>
#include <datawizard/filters.h>
#include <datawizard/memory_nodes.h>
#include <starpu_hash.h>
#include <starpu_cuda.h>
#include <starpu_opencl.h>
#include <drivers/opencl/driver_opencl.h>

static int copy_ram_to_ram(void *src_interface, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *dst_interface, unsigned dst_node STARPU_ATTRIBUTE_UNUSED);

#ifdef STARPU_USE_CUDA
/* At least CUDA 4.2 still didn't have working memcpy3D */
#if CUDART_VERSION < 5000
#define BUGGED_MEMCPY3D
#endif

static int copy_ram_to_cuda(void *src_interface, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *dst_interface, unsigned dst_node STARPU_ATTRIBUTE_UNUSED);
static int copy_cuda_to_ram(void *src_interface, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *dst_interface, unsigned dst_node STARPU_ATTRIBUTE_UNUSED);
static int copy_cuda_to_cuda(void *src_interface, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *dst_interface, unsigned dst_node STARPU_ATTRIBUTE_UNUSED);
static int copy_ram_to_cuda_async(void *src_interface, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *dst_interface, unsigned dst_node STARPU_ATTRIBUTE_UNUSED, cudaStream_t stream);
static int copy_cuda_to_ram_async(void *src_interface, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *dst_interface, unsigned dst_node STARPU_ATTRIBUTE_UNUSED, cudaStream_t stream);
#ifndef BUGGED_MEMCPY3D
static int copy_cuda_to_cuda_async(void *src_interface, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *dst_interface, unsigned dst_node STARPU_ATTRIBUTE_UNUSED, cudaStream_t stream);
#endif
#endif
#ifdef STARPU_USE_OPENCL
static int copy_ram_to_opencl(void *src_interface, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *dst_interface, unsigned dst_node STARPU_ATTRIBUTE_UNUSED);
static int copy_opencl_to_ram(void *src_interface, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *dst_interface, unsigned dst_node STARPU_ATTRIBUTE_UNUSED);
static int copy_opencl_to_opencl(void *src_interface, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *dst_interface, unsigned dst_node STARPU_ATTRIBUTE_UNUSED);
static int copy_ram_to_opencl_async(void *src_interface, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *dst_interface, unsigned dst_node STARPU_ATTRIBUTE_UNUSED, cl_event *event);
static int copy_opencl_to_ram_async(void *src_interface, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *dst_interface, unsigned dst_node STARPU_ATTRIBUTE_UNUSED, cl_event *event);
static int copy_opencl_to_opencl_async(void *src_interface, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *dst_interface, unsigned dst_node STARPU_ATTRIBUTE_UNUSED, cl_event *event);
#endif

static const struct starpu_data_copy_methods matrix_copy_data_methods_s =
{
	.ram_to_ram = copy_ram_to_ram,
#ifdef STARPU_USE_CUDA
	.ram_to_cuda = copy_ram_to_cuda,
	.cuda_to_ram = copy_cuda_to_ram,
	.ram_to_cuda_async = copy_ram_to_cuda_async,
	.cuda_to_ram_async = copy_cuda_to_ram_async,
	.cuda_to_cuda = copy_cuda_to_cuda,
#ifndef BUGGED_MEMCPY3D
	.cuda_to_cuda_async = copy_cuda_to_cuda_async,
#endif
#else
#ifdef STARPU_SIMGRID
#ifndef BUGGED_MEMCPY3D
	/* Enable GPU-GPU transfers in simgrid */
	.cuda_to_cuda_async = 1,
#endif
#endif
#endif
#ifdef STARPU_USE_OPENCL
	.ram_to_opencl = copy_ram_to_opencl,
	.opencl_to_ram = copy_opencl_to_ram,
	.opencl_to_opencl = copy_opencl_to_opencl,
        .ram_to_opencl_async = copy_ram_to_opencl_async,
	.opencl_to_ram_async = copy_opencl_to_ram_async,
	.opencl_to_opencl_async = copy_opencl_to_opencl_async,
#endif
};

static void register_matrix_handle(starpu_data_handle_t handle, unsigned home_node, void *data_interface);
static void *matrix_handle_to_pointer(starpu_data_handle_t data_handle, unsigned node);
static starpu_ssize_t allocate_matrix_buffer_on_node(void *data_interface_, unsigned dst_node);
static void free_matrix_buffer_on_node(void *data_interface, unsigned node);
static size_t matrix_interface_get_size(starpu_data_handle_t handle);
static uint32_t footprint_matrix_interface_crc32(starpu_data_handle_t handle);
static int matrix_compare(void *data_interface_a, void *data_interface_b);
static void display_matrix_interface(starpu_data_handle_t handle, FILE *f);

struct starpu_data_interface_ops starpu_interface_matrix_ops =
{
	.register_data_handle = register_matrix_handle,
	.allocate_data_on_node = allocate_matrix_buffer_on_node,
	.handle_to_pointer = matrix_handle_to_pointer,
	.free_data_on_node = free_matrix_buffer_on_node,
	.copy_methods = &matrix_copy_data_methods_s,
	.get_size = matrix_interface_get_size,
	.footprint = footprint_matrix_interface_crc32,
	.compare = matrix_compare,
	.interfaceid = STARPU_MATRIX_INTERFACE_ID,
	.interface_size = sizeof(struct starpu_matrix_interface),
	.display = display_matrix_interface,
};

static void register_matrix_handle(starpu_data_handle_t handle, unsigned home_node, void *data_interface)
{
	struct starpu_matrix_interface *matrix_interface = (struct starpu_matrix_interface *) data_interface;

	unsigned node;
	for (node = 0; node < STARPU_MAXNODES; node++)
	{
		struct starpu_matrix_interface *local_interface = (struct starpu_matrix_interface *)
			starpu_data_get_interface_on_node(handle, node);

		if (node == home_node)
		{
			local_interface->ptr = matrix_interface->ptr;
                        local_interface->dev_handle = matrix_interface->dev_handle;
                        local_interface->offset = matrix_interface->offset;
			local_interface->ld  = matrix_interface->ld;
		}
		else
		{
			local_interface->ptr = 0;
			local_interface->dev_handle = 0;
			local_interface->offset = 0;
			local_interface->ld  = 0;
		}

		local_interface->nx = matrix_interface->nx;
		local_interface->ny = matrix_interface->ny;
		local_interface->elemsize = matrix_interface->elemsize;
	}
}

static void *matrix_handle_to_pointer(starpu_data_handle_t handle, unsigned node)
{
	STARPU_ASSERT(starpu_data_test_if_allocated_on_node(handle, node));

	struct starpu_matrix_interface *matrix_interface = (struct starpu_matrix_interface *)
		starpu_data_get_interface_on_node(handle, node);

	return (void*) matrix_interface->ptr;
}


/* declare a new data with the matrix interface */
void starpu_matrix_data_register(starpu_data_handle_t *handleptr, int home_node,
			uintptr_t ptr, uint32_t ld, uint32_t nx,
			uint32_t ny, size_t elemsize)
{
	struct starpu_matrix_interface matrix_interface =
	{
		.ptr = ptr,
		.ld = ld,
		.nx = nx,
		.ny = ny,
		.elemsize = elemsize,
                .dev_handle = ptr,
                .offset = 0
	};
#ifndef STARPU_SIMGRID
	if (home_node == 0)
	{
		STARPU_ASSERT_ACCESSIBLE(ptr);
		STARPU_ASSERT_ACCESSIBLE(ptr + (ny-1)*ld*elemsize + nx*elemsize - 1);
	}
#endif

	starpu_data_register(handleptr, home_node, &matrix_interface, &starpu_interface_matrix_ops);
}

void starpu_matrix_ptr_register(starpu_data_handle_t handle, unsigned node,
			uintptr_t ptr, uintptr_t dev_handle, size_t offset, uint32_t ld)
{
	struct starpu_matrix_interface *matrix_interface = starpu_data_get_interface_on_node(handle, node);
	starpu_data_ptr_register(handle, node);
	matrix_interface->ptr = ptr;
	matrix_interface->dev_handle = dev_handle;
	matrix_interface->offset = offset;
	matrix_interface->ld = ld;
}

static uint32_t footprint_matrix_interface_crc32(starpu_data_handle_t handle)
{
	return starpu_hash_crc32c_be(starpu_matrix_get_nx(handle), starpu_matrix_get_ny(handle));
}

static int matrix_compare(void *data_interface_a, void *data_interface_b)
{
	struct starpu_matrix_interface *matrix_a = (struct starpu_matrix_interface *) data_interface_a;
	struct starpu_matrix_interface *matrix_b = (struct starpu_matrix_interface *) data_interface_b;

	/* Two matricess are considered compatible if they have the same size */
	return ((matrix_a->nx == matrix_b->nx)
			&& (matrix_a->ny == matrix_b->ny)
			&& (matrix_a->elemsize == matrix_b->elemsize));
}

static void display_matrix_interface(starpu_data_handle_t handle, FILE *f)
{
	struct starpu_matrix_interface *matrix_interface = (struct starpu_matrix_interface *)
		starpu_data_get_interface_on_node(handle, 0);

	fprintf(f, "%u\t%u\t", matrix_interface->nx, matrix_interface->ny);
}

static size_t matrix_interface_get_size(starpu_data_handle_t handle)
{
	struct starpu_matrix_interface *matrix_interface = (struct starpu_matrix_interface *)
		starpu_data_get_interface_on_node(handle, 0);

	size_t size;
	size = (size_t)matrix_interface->nx*matrix_interface->ny*matrix_interface->elemsize;

	return size;
}

/* offer an access to the data parameters */
uint32_t starpu_matrix_get_nx(starpu_data_handle_t handle)
{
	struct starpu_matrix_interface *matrix_interface = (struct starpu_matrix_interface *)
		starpu_data_get_interface_on_node(handle, 0);

	return matrix_interface->nx;
}

uint32_t starpu_matrix_get_ny(starpu_data_handle_t handle)
{
	struct starpu_matrix_interface *matrix_interface = (struct starpu_matrix_interface *)
		starpu_data_get_interface_on_node(handle, 0);

	return matrix_interface->ny;
}

uint32_t starpu_matrix_get_local_ld(starpu_data_handle_t handle)
{
	unsigned node;
	node = _starpu_memory_node_get_local_key();

	STARPU_ASSERT(starpu_data_test_if_allocated_on_node(handle, node));

	struct starpu_matrix_interface *matrix_interface = (struct starpu_matrix_interface *)
		starpu_data_get_interface_on_node(handle, node);

	return matrix_interface->ld;
}

uintptr_t starpu_matrix_get_local_ptr(starpu_data_handle_t handle)
{
	unsigned node;
	node = _starpu_memory_node_get_local_key();

	STARPU_ASSERT(starpu_data_test_if_allocated_on_node(handle, node));

	struct starpu_matrix_interface *matrix_interface = (struct starpu_matrix_interface *)
		starpu_data_get_interface_on_node(handle, node);

	return matrix_interface->ptr;
}

size_t starpu_matrix_get_elemsize(starpu_data_handle_t handle)
{
	struct starpu_matrix_interface *matrix_interface = (struct starpu_matrix_interface *)
		starpu_data_get_interface_on_node(handle, 0);

	return matrix_interface->elemsize;
}

/* memory allocation/deallocation primitives for the matrix interface */

/* returns the size of the allocated area */
static starpu_ssize_t allocate_matrix_buffer_on_node(void *data_interface_, unsigned dst_node)
{
	uintptr_t addr = 0, handle;

	struct starpu_matrix_interface *matrix_interface = (struct starpu_matrix_interface *) data_interface_;

	uint32_t nx = matrix_interface->nx;
	uint32_t ny = matrix_interface->ny;
	uint32_t ld = nx; // by default
	size_t elemsize = matrix_interface->elemsize;

	starpu_ssize_t allocated_memory;

	handle = starpu_malloc_on_node(dst_node, nx*ny*elemsize);

	if (!handle)
		return -ENOMEM;

	if (starpu_node_get_kind(dst_node) != STARPU_OPENCL_RAM)
		addr = handle;

	allocated_memory = (size_t)nx*ny*elemsize;

	/* update the data properly in consequence */
	matrix_interface->ptr = addr;
	matrix_interface->dev_handle = handle;
	matrix_interface->offset = 0;
	matrix_interface->ld = ld;

	return allocated_memory;
}

static void free_matrix_buffer_on_node(void *data_interface, unsigned node)
{
	struct starpu_matrix_interface *matrix_interface = (struct starpu_matrix_interface *) data_interface;
	uint32_t nx = matrix_interface->nx;
	uint32_t ny = matrix_interface->ny;
	size_t elemsize = matrix_interface->elemsize;

	starpu_free_on_node(node, matrix_interface->dev_handle, nx*ny*elemsize);
}

#ifdef STARPU_USE_CUDA
static int copy_cuda_common(void *src_interface, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *dst_interface, unsigned dst_node STARPU_ATTRIBUTE_UNUSED, enum cudaMemcpyKind kind, int is_async, cudaStream_t stream)
{
	struct starpu_matrix_interface *src_matrix = src_interface;
	struct starpu_matrix_interface *dst_matrix = dst_interface;

	size_t elemsize = src_matrix->elemsize;
	cudaError_t cures;

	if (is_async)
	{
		_STARPU_TRACE_START_DRIVER_COPY_ASYNC(src_node, dst_node);
		cures = cudaMemcpy2DAsync((char *)dst_matrix->ptr, dst_matrix->ld*elemsize,
			(char *)src_matrix->ptr, src_matrix->ld*elemsize,
			src_matrix->nx*elemsize, src_matrix->ny, kind, stream);
		_STARPU_TRACE_END_DRIVER_COPY_ASYNC(src_node, dst_node);
		if (!cures)
			return -EAGAIN;
	}

	cures = cudaMemcpy2D((char *)dst_matrix->ptr, dst_matrix->ld*elemsize,
		(char *)src_matrix->ptr, src_matrix->ld*elemsize,
		src_matrix->nx*elemsize, src_matrix->ny, kind);
	if (STARPU_UNLIKELY(cures))
	{
		int ret = 0;
		unsigned y;
		uint32_t nx = dst_matrix->nx;
		uint32_t ny = dst_matrix->ny;
		uint32_t ld_src = src_matrix->ld;
		uint32_t ld_dst = dst_matrix->ld;

		if (ld_src == nx && ld_dst == nx)
		{
			/* Optimize unpartitioned and y-partitioned cases */
			if (starpu_interface_copy(src_matrix->dev_handle, src_matrix->offset, src_node,
						  dst_matrix->dev_handle, dst_matrix->offset, dst_node,
						  nx*ny*elemsize, (void*)(uintptr_t)is_async))
				ret = -EAGAIN;
		}
		else
		{
			for (y = 0; y < ny; y++)
			{
				uint32_t src_offset = y*ld_src*elemsize;
				uint32_t dst_offset = y*ld_dst*elemsize;

				if (starpu_interface_copy(src_matrix->dev_handle, src_matrix->offset + src_offset, src_node,
							  dst_matrix->dev_handle, dst_matrix->offset + dst_offset, dst_node,
							  nx*elemsize, (void*)(uintptr_t)is_async))
					ret = -EAGAIN;
			}
		}

		if (ret == -EAGAIN) return ret;
		if (ret) STARPU_CUDA_REPORT_ERROR(cures);
	}

	_STARPU_TRACE_DATA_COPY(src_node, dst_node, (size_t)src_matrix->nx*src_matrix->ny*src_matrix->elemsize);

	return 0;
}

#ifndef BUGGED_MEMCPY3D
static int copy_cuda_peer(void *src_interface, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *dst_interface, unsigned dst_node STARPU_ATTRIBUTE_UNUSED, int is_async, cudaStream_t stream)
{
#ifdef HAVE_CUDA_MEMCPY_PEER
	struct starpu_matrix_interface *src_matrix = src_interface;
	struct starpu_matrix_interface *dst_matrix = dst_interface;

	size_t elemsize = src_matrix->elemsize;
	cudaError_t cures;

	int src_dev = _starpu_memory_node_get_devid(src_node);
	int dst_dev = _starpu_memory_node_get_devid(dst_node);

	struct cudaMemcpy3DPeerParms p;
	memset(&p, 0, sizeof(p));

	p.srcDevice = src_dev;
	p.dstDevice = dst_dev;
	p.srcPtr = make_cudaPitchedPtr((char *)src_matrix->ptr, src_matrix->ld * elemsize, src_matrix->nx, src_matrix->ny);
	p.dstPtr = make_cudaPitchedPtr((char *)dst_matrix->ptr, dst_matrix->ld * elemsize, dst_matrix->nx, dst_matrix->ny);
	p.extent = make_cudaExtent(src_matrix->nx * elemsize, src_matrix->ny, 1);

	if (is_async)
	{
		_STARPU_TRACE_START_DRIVER_COPY_ASYNC(src_node, dst_node);
		cures = cudaMemcpy3DPeerAsync(&p, stream);
		_STARPU_TRACE_END_DRIVER_COPY_ASYNC(src_node, dst_node);
		if (!cures)
			return -EAGAIN;
	}

	cures = cudaMemcpy3DPeer(&p);
	if (STARPU_UNLIKELY(cures))
		STARPU_CUDA_REPORT_ERROR(cures);

	_STARPU_TRACE_DATA_COPY(src_node, dst_node, (size_t)src_matrix->nx*src_matrix->ny*src_matrix->elemsize);

	return 0;
#else
	STARPU_ABORT_MSG("CUDA memcpy 3D peer not available, but core triggered one ?!");
#endif
}
#endif

static int copy_cuda_to_ram(void *src_interface, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *dst_interface, unsigned dst_node STARPU_ATTRIBUTE_UNUSED)
{
	return copy_cuda_common(src_interface, src_node, dst_interface, dst_node, cudaMemcpyDeviceToHost, 0, 0);
}

static int copy_ram_to_cuda(void *src_interface, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *dst_interface, unsigned dst_node STARPU_ATTRIBUTE_UNUSED)
{
	return copy_cuda_common(src_interface, src_node, dst_interface, dst_node, cudaMemcpyHostToDevice, 0, 0);
}

static int copy_cuda_to_cuda(void *src_interface, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *dst_interface, unsigned dst_node STARPU_ATTRIBUTE_UNUSED)
{
	if (src_node == dst_node)
		return copy_cuda_common(src_interface, src_node, dst_interface, dst_node, cudaMemcpyDeviceToDevice, 0, 0);
	else
#ifdef BUGGED_MEMCPY3D
		STARPU_ABORT_MSG("CUDA memcpy 3D peer not available, but core triggered one?!");
#else
		return copy_cuda_peer(src_interface, src_node, dst_interface, dst_node, 0, 0);
#endif
}

static int copy_cuda_to_ram_async(void *src_interface, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *dst_interface, unsigned dst_node STARPU_ATTRIBUTE_UNUSED, cudaStream_t stream)
{
	return copy_cuda_common(src_interface, src_node, dst_interface, dst_node, cudaMemcpyDeviceToHost, 1, stream);
}

static int copy_ram_to_cuda_async(void *src_interface, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *dst_interface, unsigned dst_node STARPU_ATTRIBUTE_UNUSED, cudaStream_t stream)
{
	return copy_cuda_common(src_interface, src_node, dst_interface, dst_node, cudaMemcpyHostToDevice, 1, stream);
}

#ifndef BUGGED_MEMCPY3D
static int copy_cuda_to_cuda_async(void *src_interface, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *dst_interface, unsigned dst_node STARPU_ATTRIBUTE_UNUSED, cudaStream_t stream)
{
	if (src_node == dst_node)
		return copy_cuda_common(src_interface, src_node, dst_interface, dst_node, cudaMemcpyDeviceToDevice, 1, stream);
	else
		return copy_cuda_peer(src_interface, src_node, dst_interface, dst_node, 1, stream);
}
#endif
#endif // STARPU_USE_CUDA

#ifdef STARPU_USE_OPENCL
static int copy_opencl_common(void *src_interface, unsigned src_node, void *dst_interface, unsigned dst_node, cl_event *event)
{
	struct starpu_matrix_interface *src_matrix = src_interface;
	struct starpu_matrix_interface *dst_matrix = dst_interface;
        int ret;

	STARPU_ASSERT_MSG((src_matrix->ld == src_matrix->nx) && (dst_matrix->ld == dst_matrix->nx), "XXX non contiguous buffers are not properly supported in OpenCL yet. (TODO)");

	ret = starpu_opencl_copy_async_sync(src_matrix->dev_handle, src_matrix->offset, src_node,
					    dst_matrix->dev_handle, dst_matrix->offset, dst_node,
					    src_matrix->nx*src_matrix->ny*src_matrix->elemsize,
					    event);

	_STARPU_TRACE_DATA_COPY(src_node, dst_node, src_matrix->nx*src_matrix->ny*src_matrix->elemsize);

	return ret;
}

static int copy_ram_to_opencl_async(void *src_interface, unsigned src_node, void *dst_interface, unsigned dst_node, cl_event *event)
{
	return copy_opencl_common(src_interface, src_node, dst_interface, dst_node, event);
}

static int copy_opencl_to_ram_async(void *src_interface, unsigned src_node, void *dst_interface, unsigned dst_node, cl_event *event)
{
	return copy_opencl_common(src_interface, src_node, dst_interface, dst_node, event);
}

static int copy_opencl_to_opencl_async(void *src_interface, unsigned src_node, void *dst_interface, unsigned dst_node, cl_event *event)
{
	return copy_opencl_common(src_interface, src_node, dst_interface, dst_node, event);
}

static int copy_ram_to_opencl(void *src_interface, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *dst_interface, unsigned dst_node STARPU_ATTRIBUTE_UNUSED)
{
        return copy_ram_to_opencl_async(src_interface, src_node, dst_interface, dst_node, NULL);
}

static int copy_opencl_to_ram(void *src_interface, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *dst_interface, unsigned dst_node STARPU_ATTRIBUTE_UNUSED)
{
        return copy_opencl_to_ram_async(src_interface, src_node, dst_interface, dst_node, NULL);
}

static int copy_opencl_to_opencl(void *src_interface, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *dst_interface, unsigned dst_node STARPU_ATTRIBUTE_UNUSED)
{
	return copy_opencl_to_opencl_async(src_interface, src_node, dst_interface, dst_node, NULL);
}

#endif

/* as not all platform easily have a  lib installed ... */
static int copy_ram_to_ram(void *src_interface, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *dst_interface, unsigned dst_node STARPU_ATTRIBUTE_UNUSED)
{
	struct starpu_matrix_interface *src_matrix = (struct starpu_matrix_interface *) src_interface;
	struct starpu_matrix_interface *dst_matrix = (struct starpu_matrix_interface *) dst_interface;

	unsigned y;
	uint32_t nx = dst_matrix->nx;
	uint32_t ny = dst_matrix->ny;
	size_t elemsize = dst_matrix->elemsize;

	uint32_t ld_src = src_matrix->ld;
	uint32_t ld_dst = dst_matrix->ld;

	uintptr_t ptr_src = src_matrix->ptr;
	uintptr_t ptr_dst = dst_matrix->ptr;


	for (y = 0; y < ny; y++)
	{
		uint32_t src_offset = y*ld_src*elemsize;
		uint32_t dst_offset = y*ld_dst*elemsize;

		memcpy((void *)(ptr_dst + dst_offset),
			(void *)(ptr_src + src_offset), nx*elemsize);
	}

	_STARPU_TRACE_DATA_COPY(src_node, dst_node, (size_t)nx*ny*elemsize);

	return 0;
}
