/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2010-2016  Université de Bordeaux
 * Copyright (C) 2010  Mehdi Juhoor <mjuhoor@gmail.com>
 * Copyright (C) 2010, 2011, 2012, 2013  Centre National de la Recherche Scientifique
 * Copyright (C) 2011  Télécom-SudParis
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

#include <math.h>
#include <starpu.h>
#include <starpu_profiling.h>
#include <common/config.h>
#include <common/utils.h>
#include <core/debug.h>
#include <starpu_opencl.h>
#include <drivers/driver_common/driver_common.h>
#include "driver_opencl.h"
#include "driver_opencl_utils.h"
#include <common/utils.h>
#include <datawizard/memory_manager.h>
#include <datawizard/memory_nodes.h>
#include <datawizard/malloc.h>

#ifdef STARPU_SIMGRID
#include <core/simgrid.h>
#endif

static int nb_devices = -1;
static int init_done = 0;

static starpu_pthread_mutex_t big_lock = STARPU_PTHREAD_MUTEX_INITIALIZER;

static size_t global_mem[STARPU_MAXOPENCLDEVS];

#ifdef STARPU_USE_OPENCL
static cl_context contexts[STARPU_MAXOPENCLDEVS];
static cl_device_id devices[STARPU_MAXOPENCLDEVS];
static cl_command_queue queues[STARPU_MAXOPENCLDEVS];
static cl_command_queue in_transfer_queues[STARPU_MAXOPENCLDEVS];
static cl_command_queue out_transfer_queues[STARPU_MAXOPENCLDEVS];
static cl_command_queue peer_transfer_queues[STARPU_MAXOPENCLDEVS];
static cl_command_queue alloc_queues[STARPU_MAXOPENCLDEVS];
#endif

void
_starpu_opencl_discover_devices(struct _starpu_machine_config *config)
{
	/* Discover the number of OpenCL devices. Fill the result in CONFIG. */
	/* As OpenCL must have been initialized before calling this function,
	 * `nb_device' is ensured to be correctly set. */
	STARPU_ASSERT(init_done == 1);
	config->topology.nhwopenclgpus = nb_devices;
}

static void _starpu_opencl_limit_gpu_mem_if_needed(unsigned devid)
{
	starpu_ssize_t limit;
	size_t STARPU_ATTRIBUTE_UNUSED totalGlobalMem = 0;
	size_t STARPU_ATTRIBUTE_UNUSED to_waste = 0;
	char name[30];

#ifdef STARPU_SIMGRID
	totalGlobalMem = _starpu_simgrid_get_memsize("OpenCL", devid);
#elif defined(STARPU_USE_OPENCL)
	/* Request the size of the current device's memory */
	cl_int err;
	cl_ulong size;
	err = clGetDeviceInfo(devices[devid], CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(size), &size, NULL);
	if (STARPU_UNLIKELY(err != CL_SUCCESS))
		STARPU_OPENCL_REPORT_ERROR(err);
	totalGlobalMem = size;
#endif

	limit = starpu_get_env_number("STARPU_LIMIT_OPENCL_MEM");
	if (limit == -1)
	{
	     sprintf(name, "STARPU_LIMIT_OPENCL_%u_MEM", devid);
	     limit = starpu_get_env_number(name);
	}
#if defined(STARPU_USE_OPENCL) || defined(STARPU_SIMGRID)
	if (limit == -1)
	{
		/* Use 90% of the available memory by default.  */
		limit = totalGlobalMem / (1024*1024) * 0.9;
	}
#endif

	global_mem[devid] = limit * 1024*1024;

#ifdef STARPU_USE_OPENCL
	/* How much memory to waste ? */
	to_waste = totalGlobalMem - global_mem[devid];
#endif

	_STARPU_DEBUG("OpenCL device %d: Wasting %ld MB / Limit %ld MB / Total %ld MB / Remains %ld MB\n",
			devid, (long)to_waste/(1024*1024), (long) limit, (long)totalGlobalMem/(1024*1024),
			(long)(totalGlobalMem - to_waste)/(1024*1024));

}

#ifdef STARPU_USE_OPENCL
void starpu_opencl_get_context(int devid, cl_context *context)
{
        *context = contexts[devid];
}

void starpu_opencl_get_device(int devid, cl_device_id *device)
{
        *device = devices[devid];
}

void starpu_opencl_get_queue(int devid, cl_command_queue *queue)
{
        *queue = queues[devid];
}

void starpu_opencl_get_current_queue(cl_command_queue *queue)
{
	struct _starpu_worker *worker = _starpu_get_local_worker_key();
	STARPU_ASSERT(queue);
        *queue = queues[worker->devid];
}

void starpu_opencl_get_current_context(cl_context *context)
{
	struct _starpu_worker *worker = _starpu_get_local_worker_key();
	STARPU_ASSERT(context);
        *context = contexts[worker->devid];
}

#ifndef STARPU_SIMGRID
cl_int _starpu_opencl_init_context(int devid)
{
	cl_int err;
	cl_uint uint;

	STARPU_PTHREAD_MUTEX_LOCK(&big_lock);

        _STARPU_DEBUG("Initialising context for dev %d\n", devid);

        // Create a compute context
	err = 0;
        contexts[devid] = clCreateContext(NULL, 1, &devices[devid], NULL, NULL, &err);
        if (STARPU_UNLIKELY(err != CL_SUCCESS)) STARPU_OPENCL_REPORT_ERROR(err);

        err = clGetDeviceInfo(devices[devid], CL_DEVICE_MEM_BASE_ADDR_ALIGN, sizeof(uint), &uint, NULL);
	if (STARPU_UNLIKELY(err != CL_SUCCESS))
		STARPU_OPENCL_REPORT_ERROR(err);
	starpu_malloc_set_align(uint/8);

        // Create execution queue for the given device
        queues[devid] = clCreateCommandQueue(contexts[devid], devices[devid], 0, &err);
        if (STARPU_UNLIKELY(err != CL_SUCCESS)) STARPU_OPENCL_REPORT_ERROR(err);

        // Create transfer queue for the given device
        cl_command_queue_properties props;
        err = clGetDeviceInfo(devices[devid], CL_DEVICE_QUEUE_PROPERTIES, sizeof(props), &props, NULL);
	if (STARPU_UNLIKELY(err != CL_SUCCESS))
		STARPU_OPENCL_REPORT_ERROR(err);
        props &= ~CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE;
        in_transfer_queues[devid] = clCreateCommandQueue(contexts[devid], devices[devid], props, &err);
        if (STARPU_UNLIKELY(err != CL_SUCCESS)) STARPU_OPENCL_REPORT_ERROR(err);
        out_transfer_queues[devid] = clCreateCommandQueue(contexts[devid], devices[devid], props, &err);
        if (STARPU_UNLIKELY(err != CL_SUCCESS)) STARPU_OPENCL_REPORT_ERROR(err);
        peer_transfer_queues[devid] = clCreateCommandQueue(contexts[devid], devices[devid], props, &err);
        if (STARPU_UNLIKELY(err != CL_SUCCESS)) STARPU_OPENCL_REPORT_ERROR(err);

        alloc_queues[devid] = clCreateCommandQueue(contexts[devid], devices[devid], 0, &err);
        if (STARPU_UNLIKELY(err != CL_SUCCESS)) STARPU_OPENCL_REPORT_ERROR(err);

	STARPU_PTHREAD_MUTEX_UNLOCK(&big_lock);

	return CL_SUCCESS;
}

cl_int _starpu_opencl_deinit_context(int devid)
{
        cl_int err;

	STARPU_PTHREAD_MUTEX_LOCK(&big_lock);

        _STARPU_DEBUG("De-initialising context for dev %d\n", devid);

        err = clReleaseContext(contexts[devid]);
        if (STARPU_UNLIKELY(err != CL_SUCCESS)) STARPU_OPENCL_REPORT_ERROR(err);

        err = clReleaseCommandQueue(queues[devid]);
        if (STARPU_UNLIKELY(err != CL_SUCCESS)) STARPU_OPENCL_REPORT_ERROR(err);

        err = clReleaseCommandQueue(in_transfer_queues[devid]);
        if (STARPU_UNLIKELY(err != CL_SUCCESS)) STARPU_OPENCL_REPORT_ERROR(err);
        err = clReleaseCommandQueue(out_transfer_queues[devid]);
        if (STARPU_UNLIKELY(err != CL_SUCCESS)) STARPU_OPENCL_REPORT_ERROR(err);
        err = clReleaseCommandQueue(peer_transfer_queues[devid]);
        if (STARPU_UNLIKELY(err != CL_SUCCESS)) STARPU_OPENCL_REPORT_ERROR(err);

        err = clReleaseCommandQueue(alloc_queues[devid]);
        if (STARPU_UNLIKELY(err != CL_SUCCESS)) STARPU_OPENCL_REPORT_ERROR(err);

        contexts[devid] = NULL;

	STARPU_PTHREAD_MUTEX_UNLOCK(&big_lock);

        return CL_SUCCESS;
}
#endif

cl_int starpu_opencl_allocate_memory(int devid, cl_mem *mem STARPU_ATTRIBUTE_UNUSED, size_t size STARPU_ATTRIBUTE_UNUSED, cl_mem_flags flags STARPU_ATTRIBUTE_UNUSED)
{
#ifdef STARPU_SIMGRID
	STARPU_ABORT();
#else
	cl_int err;
        cl_mem memory;

	memory = clCreateBuffer(contexts[devid], flags, size, NULL, &err);
	if (err == CL_OUT_OF_HOST_MEMORY) return err;
	if (err == CL_MEM_OBJECT_ALLOCATION_FAILURE) return err;
        if (err != CL_SUCCESS) STARPU_OPENCL_REPORT_ERROR(err);

	/*
	 * OpenCL uses lazy memory allocation: we will only know if the
	 * allocation failed when trying to copy data onto the device. But we
	 * want to know this __now__, so we just perform a dummy copy.
	 */
	char dummy = 0;
	cl_event ev;
	err = clEnqueueWriteBuffer(alloc_queues[devid], memory, CL_TRUE,
				   0, sizeof(dummy), &dummy,
				   0, NULL, &ev);
	if (err == CL_MEM_OBJECT_ALLOCATION_FAILURE)
		return err;
	if (err == CL_OUT_OF_RESOURCES)
		return err;
	if (err != CL_SUCCESS)
		STARPU_OPENCL_REPORT_ERROR(err);

	clWaitForEvents(1, &ev);
	clReleaseEvent(ev);

        *mem = memory;
        return CL_SUCCESS;
#endif
}

cl_int starpu_opencl_copy_ram_to_opencl(void *ptr, unsigned src_node STARPU_ATTRIBUTE_UNUSED, cl_mem buffer, unsigned dst_node STARPU_ATTRIBUTE_UNUSED, size_t size, size_t offset, cl_event *event, int *ret)
{
	cl_int err;
	struct _starpu_worker *worker = _starpu_get_local_worker_key();

	if (event)
		_STARPU_TRACE_START_DRIVER_COPY_ASYNC(src_node, dst_node);

	cl_event ev;
	err = clEnqueueWriteBuffer(in_transfer_queues[worker->devid], buffer, CL_FALSE, offset, size, ptr, 0, NULL, &ev);

	if (event)
		_STARPU_TRACE_END_DRIVER_COPY_ASYNC(src_node, dst_node);

	if (STARPU_LIKELY(err == CL_SUCCESS))
	{
		if (event == NULL)
		{
			/* We want a synchronous copy, let's synchronise the queue */
			err = clWaitForEvents(1, &ev);
			if (STARPU_UNLIKELY(err))
				STARPU_OPENCL_REPORT_ERROR(err);
			err = clReleaseEvent(ev);
			if (STARPU_UNLIKELY(err))
				STARPU_OPENCL_REPORT_ERROR(err);
		}
		else
		{
			*event = ev;
		}

		if (ret)
		{
			*ret = (event == NULL) ? 0 : -EAGAIN;
		}
	}
	return err;
}

cl_int starpu_opencl_copy_opencl_to_ram(cl_mem buffer, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *ptr, unsigned dst_node STARPU_ATTRIBUTE_UNUSED, size_t size, size_t offset, cl_event *event, int *ret)
{
	cl_int err;
	struct _starpu_worker *worker = _starpu_get_local_worker_key();

	if (event)
		_STARPU_TRACE_START_DRIVER_COPY_ASYNC(src_node, dst_node);
	cl_event ev;
	err = clEnqueueReadBuffer(out_transfer_queues[worker->devid], buffer, CL_FALSE, offset, size, ptr, 0, NULL, &ev);
	if (event)
		_STARPU_TRACE_END_DRIVER_COPY_ASYNC(src_node, dst_node);
	if (STARPU_LIKELY(err == CL_SUCCESS))
	{
		if (event == NULL)
		{
			/* We want a synchronous copy, let's synchronise the queue */
			err = clWaitForEvents(1, &ev);
			if (STARPU_UNLIKELY(err))
				STARPU_OPENCL_REPORT_ERROR(err);
			err = clReleaseEvent(ev);
			if (STARPU_UNLIKELY(err))
				STARPU_OPENCL_REPORT_ERROR(err);
		}
		else
		{
			*event = ev;
		}

		if (ret)
		{
			*ret = (event == NULL) ? 0 : -EAGAIN;
		}
	}
	return err;
}

cl_int starpu_opencl_copy_opencl_to_opencl(cl_mem src, unsigned src_node STARPU_ATTRIBUTE_UNUSED, size_t src_offset, cl_mem dst, unsigned dst_node STARPU_ATTRIBUTE_UNUSED, size_t dst_offset, size_t size, cl_event *event, int *ret)
{
	cl_int err;
	struct _starpu_worker *worker = _starpu_get_local_worker_key();

	if (event)
		_STARPU_TRACE_START_DRIVER_COPY_ASYNC(src_node, dst_node);
	cl_event ev;
	err = clEnqueueCopyBuffer(peer_transfer_queues[worker->devid], src, dst, src_offset, dst_offset, size, 0, NULL, &ev);
	if (event)
		_STARPU_TRACE_END_DRIVER_COPY_ASYNC(src_node, dst_node);
	if (STARPU_LIKELY(err == CL_SUCCESS))
	{
		if (event == NULL)
		{
			/* We want a synchronous copy, let's synchronise the queue */
			err = clWaitForEvents(1, &ev);
			if (STARPU_UNLIKELY(err))
				STARPU_OPENCL_REPORT_ERROR(err);
			err = clReleaseEvent(ev);
			if (STARPU_UNLIKELY(err))
				STARPU_OPENCL_REPORT_ERROR(err);
		}
		else
		{
			*event = ev;
		}

		if (ret)
		{
			*ret = (event == NULL) ? 0 : -EAGAIN;
		}
	}
	return err;
}

#ifdef STARPU_USE_OPENCL
cl_int starpu_opencl_copy_async_sync(uintptr_t src, size_t src_offset, unsigned src_node, uintptr_t dst, size_t dst_offset, unsigned dst_node, size_t size, cl_event *event)
{
	enum starpu_node_kind src_kind = starpu_node_get_kind(src_node);
	enum starpu_node_kind dst_kind = starpu_node_get_kind(dst_node);
	cl_int err;
	int ret;

	switch (_STARPU_MEMORY_NODE_TUPLE(src_kind,dst_kind))
	{
	case _STARPU_MEMORY_NODE_TUPLE(STARPU_OPENCL_RAM,STARPU_CPU_RAM):
		err = starpu_opencl_copy_opencl_to_ram(
				(cl_mem) src, src_node,
				(void*) (dst + dst_offset), dst_node,
				size, src_offset, event, &ret);
		if (STARPU_UNLIKELY(err))
			STARPU_OPENCL_REPORT_ERROR(err);
		return ret;

	case _STARPU_MEMORY_NODE_TUPLE(STARPU_CPU_RAM,STARPU_OPENCL_RAM):
		err = starpu_opencl_copy_ram_to_opencl(
				(void*) (src + src_offset), src_node,
				(cl_mem) dst, dst_node,
				size, dst_offset, event, &ret);
		if (STARPU_UNLIKELY(err))
			STARPU_OPENCL_REPORT_ERROR(err);
		return ret;

	case _STARPU_MEMORY_NODE_TUPLE(STARPU_OPENCL_RAM,STARPU_OPENCL_RAM):
		err = starpu_opencl_copy_opencl_to_opencl(
				(cl_mem) src, src_node, src_offset,
				(cl_mem) dst, dst_node, dst_offset,
				size, event, &ret);
		if (STARPU_UNLIKELY(err))
			STARPU_OPENCL_REPORT_ERROR(err);
		return ret;

	default:
		STARPU_ABORT();
		break;
	}
}
#endif

#if 0
cl_int _starpu_opencl_copy_rect_opencl_to_ram(cl_mem buffer, unsigned src_node STARPU_ATTRIBUTE_UNUSED, void *ptr, unsigned dst_node STARPU_ATTRIBUTE_UNUSED, const size_t buffer_origin[3], const size_t host_origin[3],
                                              const size_t region[3], size_t buffer_row_pitch, size_t buffer_slice_pitch,
                                              size_t host_row_pitch, size_t host_slice_pitch, cl_event *event)
{
        cl_int err;
        struct _starpu_worker *worker = _starpu_get_local_worker_key();
        cl_bool blocking;

        blocking = (event == NULL) ? CL_TRUE : CL_FALSE;
        if (event)
                _STARPU_TRACE_START_DRIVER_COPY_ASYNC(src_node, dst_node);
        err = clEnqueueReadBufferRect(out_transfer_queues[worker->devid], buffer, blocking, buffer_origin, host_origin, region, buffer_row_pitch,
                                      buffer_slice_pitch, host_row_pitch, host_slice_pitch, ptr, 0, NULL, event);
        if (event)
                _STARPU_TRACE_END_DRIVER_COPY_ASYNC(src_node, dst_node);
        if (err != CL_SUCCESS) STARPU_OPENCL_REPORT_ERROR(err);

        return CL_SUCCESS;
}

cl_int _starpu_opencl_copy_rect_ram_to_opencl(void *ptr, unsigned src_node STARPU_ATTRIBUTE_UNUSED, cl_mem buffer, unsigned dst_node STARPU_ATTRIBUTE_UNUSED, const size_t buffer_origin[3], const size_t host_origin[3],
                                              const size_t region[3], size_t buffer_row_pitch, size_t buffer_slice_pitch,
                                              size_t host_row_pitch, size_t host_slice_pitch, cl_event *event)
{
        cl_int err;
        struct _starpu_worker *worker = _starpu_get_local_worker_key();
        cl_bool blocking;

        blocking = (event == NULL) ? CL_TRUE : CL_FALSE;
        if (event)
                _STARPU_TRACE_START_DRIVER_COPY_ASYNC(src_node, dst_node);
        err = clEnqueueWriteBufferRect(in_transfer_queues[worker->devid], buffer, blocking, buffer_origin, host_origin, region, buffer_row_pitch,
                                       buffer_slice_pitch, host_row_pitch, host_slice_pitch, ptr, 0, NULL, event);
        if (event)
                _STARPU_TRACE_END_DRIVER_COPY_ASYNC(src_node, dst_node);
        if (err != CL_SUCCESS) STARPU_OPENCL_REPORT_ERROR(err);

        return CL_SUCCESS;
}
#endif
#endif /* STARPU_USE_OPENCL */

static size_t _starpu_opencl_get_global_mem_size(int devid)
{
	return global_mem[devid];
}

void _starpu_opencl_init(void)
{
	STARPU_PTHREAD_MUTEX_LOCK(&big_lock);
        if (!init_done)
	{
#ifdef STARPU_SIMGRID
		nb_devices = _starpu_simgrid_get_nbhosts("OpenCL");
#else /* STARPU_USE_OPENCL */
                cl_platform_id platform_id[_STARPU_OPENCL_PLATFORM_MAX];
                cl_uint nb_platforms;
                cl_int err;
                int i;
                cl_device_type device_type = CL_DEVICE_TYPE_GPU|CL_DEVICE_TYPE_ACCELERATOR;

                _STARPU_DEBUG("Initialising OpenCL\n");

                // Get Platforms
		if (starpu_get_env_number("STARPU_OPENCL_ON_CPUS") > 0)
		     device_type |= CL_DEVICE_TYPE_CPU;
		if (starpu_get_env_number("STARPU_OPENCL_ONLY_ON_CPUS") > 0)
		     device_type = CL_DEVICE_TYPE_CPU;
                err = clGetPlatformIDs(_STARPU_OPENCL_PLATFORM_MAX, platform_id, &nb_platforms);
                if (STARPU_UNLIKELY(err != CL_SUCCESS)) nb_platforms=0;
                _STARPU_DEBUG("Platforms detected: %u\n", nb_platforms);

                // Get devices
                nb_devices = 0;
                {
			unsigned j;
                        for (j=0; j<nb_platforms; j++)
			{
                                cl_uint num;
				int platform_valid = 1;
				char name[1024], vendor[1024];

				err = clGetPlatformInfo(platform_id[j], CL_PLATFORM_NAME, 1024, name, NULL);
				if (err != CL_SUCCESS)
				{
					STARPU_OPENCL_REPORT_ERROR_WITH_MSG("clGetPlatformInfo NAME", err);
					platform_valid = 0;
				}
				else
				{
					err = clGetPlatformInfo(platform_id[j], CL_PLATFORM_VENDOR, 1024, vendor, NULL);
					if (STARPU_UNLIKELY(err != CL_SUCCESS))
					{
						STARPU_OPENCL_REPORT_ERROR_WITH_MSG("clGetPlatformInfo VENDOR", err);
						platform_valid = 0;
					}
				}
				if(strcmp(name, "SOCL Platform") == 0)
				{
					platform_valid = 0;
					_STARPU_DEBUG("Skipping SOCL Platform\n");
				}
#ifdef STARPU_VERBOSE
				if (platform_valid)
					_STARPU_DEBUG("Platform: %s - %s\n", name, vendor);
				else
					_STARPU_DEBUG("Platform invalid\n");
#endif
				if (platform_valid && nb_devices <= STARPU_MAXOPENCLDEVS)
				{
					err = clGetDeviceIDs(platform_id[j], device_type, STARPU_MAXOPENCLDEVS-nb_devices, STARPU_MAXOPENCLDEVS == nb_devices ? NULL : &devices[nb_devices], &num);
					if (err == CL_DEVICE_NOT_FOUND)
					{
						_STARPU_DEBUG("  No devices detected on this platform\n");
					}
					else
					{
						if (STARPU_UNLIKELY(err != CL_SUCCESS)) STARPU_OPENCL_REPORT_ERROR(err);
						_STARPU_DEBUG("  %u devices detected\n", num);
						nb_devices += num;
					}
				}
			}
		}

                // Get location of OpenCl kernel source files
                _starpu_opencl_program_dir = starpu_getenv("STARPU_OPENCL_PROGRAM_DIR");

		if (nb_devices > STARPU_MAXOPENCLDEVS)
		{
			_STARPU_DISP("# Warning: %u OpenCL devices available. Only %d enabled. Use configure option --enable-maxopencldev=xxx to update the maximum value of supported OpenCL devices?\n", nb_devices, STARPU_MAXOPENCLDEVS);
			nb_devices = STARPU_MAXOPENCLDEVS;
		}

                // initialise internal structures
                for(i=0 ; i<nb_devices ; i++)
		{
                        contexts[i] = NULL;
                        queues[i] = NULL;
                        in_transfer_queues[i] = NULL;
                        out_transfer_queues[i] = NULL;
                        peer_transfer_queues[i] = NULL;
                        alloc_queues[i] = NULL;
                }
#endif /* STARPU_USE_OPENCL */

                init_done=1;
        }
	STARPU_PTHREAD_MUTEX_UNLOCK(&big_lock);
}

#ifndef STARPU_SIMGRID
static unsigned _starpu_opencl_get_device_name(int dev, char *name, int lname);
#endif
static int _starpu_opencl_execute_job(struct _starpu_job *j, struct _starpu_worker *args);

int _starpu_opencl_driver_init(struct _starpu_worker *args)
{
	int devid = args->devid;

	_starpu_worker_start(args, _STARPU_FUT_OPENCL_KEY);

#ifndef STARPU_SIMGRID
	_starpu_opencl_init_context(devid);
#endif

	/* one more time to avoid hacks from third party lib :) */
	_starpu_bind_thread_on_cpu(args->config, args->bindid, args->workerid);

	_starpu_opencl_limit_gpu_mem_if_needed(devid);
	_starpu_memory_manager_set_global_memory_size(args->memory_node, _starpu_opencl_get_global_mem_size(devid));

	_starpu_malloc_init(args->memory_node);

	args->status = STATUS_UNKNOWN;

#ifdef STARPU_SIMGRID
	const char *devname = "Simgrid";
#else
	/* get the device's name */
	char devname[128];
	_starpu_opencl_get_device_name(devid, devname, 128);
#endif
	snprintf(args->name, sizeof(args->name), "OpenCL %u (%s)", devid, devname);
	snprintf(args->short_name, sizeof(args->short_name), "OpenCL %u", devid);

	_STARPU_DEBUG("OpenCL (%s) dev id %d thread is ready to run on CPU %d !\n", devname, devid, args->bindid);

	_STARPU_TRACE_WORKER_INIT_END;

	/* tell the main thread that this one is ready */
	STARPU_PTHREAD_MUTEX_LOCK(&args->mutex);
	args->worker_is_initialized = 1;
	STARPU_PTHREAD_COND_SIGNAL(&args->ready_cond);
	STARPU_PTHREAD_MUTEX_UNLOCK(&args->mutex);

	return 0;
}

int _starpu_opencl_driver_run_once(struct _starpu_worker *args)
{
	int workerid = args->workerid;
	unsigned memnode = args->memory_node;

	struct _starpu_job *j;
	struct starpu_task *task;
	int res;

	_STARPU_TRACE_START_PROGRESS(memnode);
	_starpu_datawizard_progress(memnode, 1);
	_STARPU_TRACE_END_PROGRESS(memnode);

	task = _starpu_get_worker_task(args, workerid, memnode);

	if (task == NULL)
		return 0;

	j = _starpu_get_job_associated_to_task(task);

	/* can OpenCL do that task ? */
	if (!_STARPU_OPENCL_MAY_PERFORM(j))
	{
		/* this is not a OpenCL task */
		_starpu_push_task_to_workers(task);
		return 0;
	}

	_starpu_set_current_task(j->task);
	args->current_task = j->task;

	res = _starpu_opencl_execute_job(j, args);

	_starpu_set_current_task(NULL);
	args->current_task = NULL;

	if (res)
	{
		switch (res)
		{
			case -EAGAIN:
				_STARPU_DISP("ouch, OpenCL could not actually run task %p, putting it back...\n", task);
				_starpu_push_task_to_workers(task);
				STARPU_ABORT();
				return 0;
			default:
				STARPU_ABORT();
		}
	}

	_starpu_handle_job_termination(j);
	return 0;
}

int _starpu_opencl_driver_deinit(struct _starpu_worker *args)
{
	_STARPU_TRACE_WORKER_DEINIT_START;

	unsigned memnode = args->memory_node;

	_starpu_handle_all_pending_node_data_requests(memnode);

	/* In case there remains some memory that was automatically
	 * allocated by StarPU, we release it now. Note that data
	 * coherency is not maintained anymore at that point ! */
	_starpu_free_all_automatically_allocated_buffers(memnode);

	_starpu_malloc_shutdown(memnode);

#ifndef STARPU_SIMGRID
	unsigned devid   = args->devid;
        _starpu_opencl_deinit_context(devid);
#endif

	args->worker_is_initialized = 0;
	_STARPU_TRACE_WORKER_DEINIT_END(_STARPU_FUT_OPENCL_KEY);

	return 0;
}

void *_starpu_opencl_worker(void *arg)
{
	struct _starpu_worker* args = arg;

	_starpu_opencl_driver_init(args);
	while (_starpu_machine_is_running())
	{
		_starpu_may_pause();
		_starpu_opencl_driver_run_once(args);
	}
	_starpu_opencl_driver_deinit(args);

	return NULL;
}

#ifdef STARPU_USE_OPENCL
#ifndef STARPU_SIMGRID
static unsigned _starpu_opencl_get_device_name(int dev, char *name, int lname)
{
	int err;

        if (!init_done)
	{
                _starpu_opencl_init();
        }

	// Get device name
	err = clGetDeviceInfo(devices[dev], CL_DEVICE_NAME, lname, name, NULL);
	if (STARPU_UNLIKELY(err != CL_SUCCESS)) STARPU_OPENCL_REPORT_ERROR(err);

	_STARPU_DEBUG("Device %d : [%s]\n", dev, name);
	return EXIT_SUCCESS;
}
#endif
#endif

unsigned _starpu_opencl_get_device_count(void)
{
        if (!init_done)
	{
                _starpu_opencl_init();
        }
	return nb_devices;
}

#ifdef STARPU_USE_OPENCL
cl_device_type _starpu_opencl_get_device_type(int devid)
{
	int err;
	cl_device_type type;

	if (!init_done)
		_starpu_opencl_init();

	err = clGetDeviceInfo(devices[devid], CL_DEVICE_TYPE, sizeof(cl_device_type), &type, NULL);
	if (STARPU_UNLIKELY(err != CL_SUCCESS))
		STARPU_OPENCL_REPORT_ERROR(err);

	return type;
}
#endif /* STARPU_USE_OPENCL */

static int _starpu_opencl_execute_job(struct _starpu_job *j, struct _starpu_worker *args)
{
	int ret;
	uint32_t mask = 0;

	STARPU_ASSERT(j);
	struct starpu_task *task = j->task;

	int profiling = starpu_profiling_status_get();
	struct timespec codelet_start, codelet_end;

	STARPU_ASSERT(task);
	struct starpu_codelet *cl = task->cl;
	STARPU_ASSERT(cl);

	ret = _starpu_fetch_task_input(j, mask);
	if (ret != 0)
	{
		/* there was not enough memory, so the input of
		 * the codelet cannot be fetched ... put the
		 * codelet back, and try it later */
		return -EAGAIN;
	}

	_starpu_driver_start_job(args, j, &codelet_start, 0, profiling);

	starpu_opencl_func_t func = _starpu_task_get_opencl_nth_implementation(cl, j->nimpl);
	STARPU_ASSERT_MSG(func, "when STARPU_OPENCL is defined in 'where', opencl_func or opencl_funcs has to be defined");

	if (_starpu_get_disable_kernels() <= 0)
	{
#ifdef STARPU_SIMGRID
		double length = NAN;
	  #ifdef STARPU_OPENCL_SIMULATOR
		func(_STARPU_TASK_GET_INTERFACES(task), task->cl_arg);
	    #ifndef CL_PROFILING_CLOCK_CYCLE_COUNT
	      #ifdef CL_PROFILING_COMMAND_SHAVE_CYCLE_COUNT
		#define CL_PROFILING_CLOCK_CYCLE_COUNT CL_PROFILING_COMMAND_SHAVE_CYCLE_COUNT
	      #else
		#error The OpenCL simulator must provide CL_PROFILING_CLOCK_CYCLE_COUNT
	      #endif
	    #endif
		struct starpu_profiling_task_info *profiling_info = task->profiling_info;
		STARPU_ASSERT_MSG(profiling_info->used_cycles, "Application kernel must call starpu_opencl_collect_stats to collect simulated time");
#ifdef HAVE_MSG_HOST_GET_SPEED
		length = ((double) profiling_info->used_cycles)/MSG_host_get_speed(MSG_host_self());
#else
		length = ((double) profiling_info->used_cycles)/MSG_get_host_speed(MSG_host_self());
#endif
	  #endif
		_starpu_simgrid_execute_job(j, args->perf_arch, length);
#else
		func(_STARPU_TASK_GET_INTERFACES(task), task->cl_arg);
#endif
	}

	_starpu_driver_end_job(args, j, args->perf_arch, &codelet_end, 0, profiling);

	_starpu_driver_update_job_feedback(j, args, args->perf_arch,
					   &codelet_start, &codelet_end, profiling);

	_starpu_push_task_output(j, mask);

	return EXIT_SUCCESS;
}

#ifdef STARPU_USE_OPENCL
int _starpu_run_opencl(struct _starpu_worker *workerarg)
{
	_STARPU_DEBUG("Running OpenCL %u from the application\n", workerarg->devid);

	workerarg->set = NULL;
	workerarg->worker_is_initialized = 0;

	/* Let's go ! */
	_starpu_opencl_worker(workerarg);

	return 0;
}
#endif /* STARPU_USE_OPENCL */
