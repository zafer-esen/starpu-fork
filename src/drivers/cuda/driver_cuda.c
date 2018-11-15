/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2009-2015, 2017  Université de Bordeaux
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

#include <starpu.h>
#include <starpu_cuda.h>
#include <starpu_profiling.h>
#include <common/utils.h>
#include <common/config.h>
#include <core/debug.h>
#include <drivers/driver_common/driver_common.h>
#include "driver_cuda.h"
#include <core/sched_policy.h>
#ifdef HAVE_CUDA_GL_INTEROP_H
#include <cuda_gl_interop.h>
#endif
#include <datawizard/memory_manager.h>
#include <datawizard/memory_nodes.h>
#include <datawizard/malloc.h>

#ifdef STARPU_SIMGRID
#include <core/simgrid.h>
#endif

#ifdef STARPU_USE_CUDA
#if CUDART_VERSION >= 5000
/* Avoid letting our streams spuriously synchonize with the NULL stream */
#define starpu_cudaStreamCreate(stream) cudaStreamCreateWithFlags(stream, cudaStreamNonBlocking)
#else
#define starpu_cudaStreamCreate(stream) cudaStreamCreate(stream)
#endif
#endif

/* the number of CUDA devices */
static int ncudagpus = -1;

static size_t global_mem[STARPU_NMAXWORKERS];
#ifdef STARPU_USE_CUDA
static cudaStream_t streams[STARPU_NMAXWORKERS];
static cudaStream_t out_transfer_streams[STARPU_MAXCUDADEVS];
static cudaStream_t in_transfer_streams[STARPU_MAXCUDADEVS];
/* Note: streams are not thread-safe, so we define them for each CUDA worker
 * emitting a GPU-GPU transfer */
static cudaStream_t in_peer_transfer_streams[STARPU_MAXCUDADEVS][STARPU_MAXCUDADEVS];
static cudaStream_t out_peer_transfer_streams[STARPU_MAXCUDADEVS][STARPU_MAXCUDADEVS];
static struct cudaDeviceProp props[STARPU_MAXCUDADEVS];
#endif /* STARPU_USE_CUDA */

void
_starpu_cuda_discover_devices (struct _starpu_machine_config *config)
{
	/* Discover the number of CUDA devices. Fill the result in CONFIG. */

#ifdef STARPU_SIMGRID
	config->topology.nhwcudagpus = _starpu_simgrid_get_nbhosts("CUDA");
#else
	int cnt;
	cudaError_t cures;

	cures = cudaGetDeviceCount (&cnt);
	if (STARPU_UNLIKELY(cures != cudaSuccess))
		cnt = 0;
	config->topology.nhwcudagpus = cnt;
#endif
}

/* In case we want to cap the amount of memory available on the GPUs by the
 * mean of the STARPU_LIMIT_CUDA_MEM, we decrease the value of
 * global_mem[devid] which is the value returned by
 * _starpu_cuda_get_global_mem_size() to indicate how much memory can
 * be allocated on the device
 */
static void _starpu_cuda_limit_gpu_mem_if_needed(unsigned devid)
{
	starpu_ssize_t limit;
	size_t STARPU_ATTRIBUTE_UNUSED totalGlobalMem = 0;
	size_t STARPU_ATTRIBUTE_UNUSED to_waste = 0;
	char name[30];

#ifdef STARPU_SIMGRID
	totalGlobalMem = _starpu_simgrid_get_memsize("CUDA", devid);
#elif defined(STARPU_USE_CUDA)
	/* Find the size of the memory on the device */
	totalGlobalMem = props[devid].totalGlobalMem;
#endif

	limit = starpu_get_env_number("STARPU_LIMIT_CUDA_MEM");
	if (limit == -1)
	{
		sprintf(name, "STARPU_LIMIT_CUDA_%u_MEM", devid);
		limit = starpu_get_env_number(name);
	}
#if defined(STARPU_USE_CUDA) || defined(STARPU_SIMGRID)
	if (limit == -1)
	{
		/* Use 90% of the available memory by default.  */
		limit = totalGlobalMem / (1024*1024) * 0.9;
	}
#endif

	global_mem[devid] = limit * 1024*1024;

#ifdef STARPU_USE_CUDA
	/* How much memory to waste ? */
	to_waste = totalGlobalMem - global_mem[devid];

	props[devid].totalGlobalMem -= to_waste;
#endif /* STARPU_USE_CUDA */

	_STARPU_DEBUG("CUDA device %u: Wasting %ld MB / Limit %ld MB / Total %ld MB / Remains %ld MB\n",
			devid, (long) to_waste/(1024*1024), (long) limit, (long) totalGlobalMem/(1024*1024),
			(long) (totalGlobalMem - to_waste)/(1024*1024));
}

#ifdef STARPU_USE_CUDA
cudaStream_t starpu_cuda_get_local_in_transfer_stream()
{
	int worker = starpu_worker_get_id();
	int devid = starpu_worker_get_devid(worker);
	cudaStream_t stream;

	stream = in_transfer_streams[devid];
	STARPU_ASSERT(stream);
	return stream;
}

cudaStream_t starpu_cuda_get_local_out_transfer_stream()
{
	int worker = starpu_worker_get_id();
	int devid = starpu_worker_get_devid(worker);
	cudaStream_t stream;

	stream = out_transfer_streams[devid];
	STARPU_ASSERT(stream);
	return stream;
}

cudaStream_t starpu_cuda_get_peer_transfer_stream(unsigned src_node, unsigned dst_node)
{
	int worker = starpu_worker_get_id();
	int devid = starpu_worker_get_devid(worker);
	int src_devid = _starpu_memory_node_get_devid(src_node);
	int dst_devid = _starpu_memory_node_get_devid(dst_node);
	cudaStream_t stream;

	STARPU_ASSERT(devid == src_devid || devid == dst_devid);

	if (devid == dst_devid)
		stream = in_peer_transfer_streams[src_devid][dst_devid];
	else
		stream = out_peer_transfer_streams[src_devid][dst_devid];
	STARPU_ASSERT(stream);
	return stream;
}

cudaStream_t starpu_cuda_get_local_stream(void)
{
	int worker = starpu_worker_get_id();

	return streams[worker];
}

const struct cudaDeviceProp *starpu_cuda_get_device_properties(unsigned workerid)
{
	struct _starpu_machine_config *config = _starpu_get_machine_config();
	unsigned devid = config->workers[workerid].devid;
	return &props[devid];
}
#endif /* STARPU_USE_CUDA */

void starpu_cuda_set_device(unsigned devid STARPU_ATTRIBUTE_UNUSED)
{
#ifdef STARPU_SIMGRID
	STARPU_ABORT();
#else
	cudaError_t cures;
	struct starpu_conf *conf = &_starpu_get_machine_config()->conf;
#if !defined(HAVE_CUDA_MEMCPY_PEER) && defined(HAVE_CUDA_GL_INTEROP_H)
	unsigned i;
#endif

#ifdef HAVE_CUDA_MEMCPY_PEER
	if (conf->n_cuda_opengl_interoperability)
	{
		fprintf(stderr, "OpenGL interoperability was requested, but StarPU was built with multithread GPU control support, please reconfigure with --disable-cuda-memcpy-peer but that will disable the memcpy-peer optimizations\n");
		STARPU_ABORT();
	}
#elif !defined(HAVE_CUDA_GL_INTEROP_H)
	if (conf->n_cuda_opengl_interoperability)
	{
		fprintf(stderr,"OpenGL interoperability was requested, but cuda_gl_interop.h could not be compiled, please make sure that OpenGL headers were available before ./configure run.");
		STARPU_ABORT();
	}
#else
	for (i = 0; i < conf->n_cuda_opengl_interoperability; i++)
		if (conf->cuda_opengl_interoperability[i] == devid)
		{
			cures = cudaGLSetGLDevice(devid);
			goto done;
		}
#endif

	cures = cudaSetDevice(devid);

#if !defined(HAVE_CUDA_MEMCPY_PEER) && defined(HAVE_CUDA_GL_INTEROP_H)
done:
#endif
	if (STARPU_UNLIKELY(cures))
		STARPU_CUDA_REPORT_ERROR(cures);
#endif
}

#ifndef STARPU_SIMGRID
static void init_context(unsigned devid)
{
	cudaError_t cures;
	int workerid;
	int i;

	/* TODO: cudaSetDeviceFlag(cudaDeviceMapHost) */

	starpu_cuda_set_device(devid);

#ifdef HAVE_CUDA_MEMCPY_PEER
	if (starpu_get_env_number("STARPU_ENABLE_CUDA_GPU_GPU_DIRECT") != 0)
	{
		int nworkers = starpu_worker_get_count();
		for (workerid = 0; workerid < nworkers; workerid++)
		{
			struct _starpu_worker *worker = _starpu_get_worker_struct(workerid);
			if (worker->arch == STARPU_CUDA_WORKER && worker->devid != devid)
			{
				int can;
				cures = cudaDeviceCanAccessPeer(&can, devid, worker->devid);
				if (!cures && can)
				{
					cures = cudaDeviceEnablePeerAccess(worker->devid, 0);
					if (!cures)
						_STARPU_DEBUG("Enabled GPU-Direct %d -> %d\n", worker->devid, devid);
				}
			}
		}
	}
#endif

	/* force CUDA to initialize the context for real */
	cures = cudaFree(0);
	if (STARPU_UNLIKELY(cures))
	{
		if (cures == cudaErrorDevicesUnavailable)
		{
			fprintf(stderr,"All CUDA-capable devices are busy or unavailable\n");
			exit(77);
		}
		STARPU_CUDA_REPORT_ERROR(cures);
	}

	cures = cudaGetDeviceProperties(&props[devid], devid);
	if (STARPU_UNLIKELY(cures))
		STARPU_CUDA_REPORT_ERROR(cures);
#ifdef HAVE_CUDA_MEMCPY_PEER
	if (props[devid].computeMode == cudaComputeModeExclusive)
	{
		fprintf(stderr, "CUDA is in EXCLUSIVE-THREAD mode, but StarPU was built with multithread GPU control support, please either ask your administrator to use EXCLUSIVE-PROCESS mode (which should really be fine), or reconfigure with --disable-cuda-memcpy-peer but that will disable the memcpy-peer optimizations\n");
		STARPU_ABORT();
	}
#endif

	workerid = starpu_worker_get_id();

	cures = starpu_cudaStreamCreate(&streams[workerid]);
	if (STARPU_UNLIKELY(cures))
		STARPU_CUDA_REPORT_ERROR(cures);

	cures = starpu_cudaStreamCreate(&in_transfer_streams[devid]);
	if (STARPU_UNLIKELY(cures))
		STARPU_CUDA_REPORT_ERROR(cures);

	cures = starpu_cudaStreamCreate(&out_transfer_streams[devid]);
	if (STARPU_UNLIKELY(cures))
		STARPU_CUDA_REPORT_ERROR(cures);

	for (i = 0; i < ncudagpus; i++)
	{
		cures = starpu_cudaStreamCreate(&in_peer_transfer_streams[i][devid]);
		if (STARPU_UNLIKELY(cures))
			STARPU_CUDA_REPORT_ERROR(cures);
		cures = starpu_cudaStreamCreate(&out_peer_transfer_streams[devid][i]);
		if (STARPU_UNLIKELY(cures))
			STARPU_CUDA_REPORT_ERROR(cures);
	}
}

static void deinit_context(int workerid)
{
	int devid = starpu_worker_get_devid(workerid);
	int i;

	cudaStreamDestroy(streams[workerid]);
	cudaStreamDestroy(in_transfer_streams[devid]);
	cudaStreamDestroy(out_transfer_streams[devid]);
	for (i = 0; i < ncudagpus; i++)
	{
		cudaStreamDestroy(in_peer_transfer_streams[i][devid]);
		cudaStreamDestroy(out_peer_transfer_streams[devid][i]);
	}
}
#endif /* !SIMGRID */

static size_t _starpu_cuda_get_global_mem_size(unsigned devid)
{
	return global_mem[devid];
}


/* Return the number of devices usable in the system.
 * The value returned cannot be greater than MAXCUDADEVS */

unsigned _starpu_get_cuda_device_count(void)
{
	int cnt;
#ifdef STARPU_SIMGRID
	cnt = _starpu_simgrid_get_nbhosts("CUDA");
#else
	cudaError_t cures;
	cures = cudaGetDeviceCount(&cnt);
	if (STARPU_UNLIKELY(cures))
		 return 0;
#endif

	if (cnt > STARPU_MAXCUDADEVS)
	{
		fprintf(stderr, "# Warning: %d CUDA devices available. Only %d enabled. Use configure option --enable-maxcudadev=xxx to update the maximum value of supported CUDA devices.\n", cnt, STARPU_MAXCUDADEVS);
		cnt = STARPU_MAXCUDADEVS;
	}
	return (unsigned)cnt;
}

void _starpu_init_cuda(void)
{
	if (ncudagpus < 0)
	{
		ncudagpus = _starpu_get_cuda_device_count();
		STARPU_ASSERT(ncudagpus <= STARPU_MAXCUDADEVS);
	}
}

static int execute_job_on_cuda(struct _starpu_job *j, struct _starpu_worker *args)
{
	int ret;
	uint32_t mask = 0;

	STARPU_ASSERT(j);
	struct starpu_task *task = j->task;

	struct timespec codelet_start, codelet_end;

	int profiling = starpu_profiling_status_get();

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

#if defined(HAVE_CUDA_MEMCPY_PEER) && !defined(STARPU_SIMGRID)
	/* We make sure we do manipulate the proper device */
	starpu_cuda_set_device(args->devid);
#endif

	starpu_cuda_func_t func = _starpu_task_get_cuda_nth_implementation(cl, j->nimpl);
	STARPU_ASSERT_MSG(func, "when STARPU_CUDA is defined in 'where', cuda_func or cuda_funcs has to be defined");

	if (_starpu_get_disable_kernels() <= 0)
	{
#ifdef STARPU_SIMGRID
		_starpu_simgrid_execute_job(j, args->perf_arch, NAN);
#else
		func(_STARPU_TASK_GET_INTERFACES(task), task->cl_arg);
#ifdef STARPU_DEBUG
		STARPU_ASSERT_MSG(cudaStreamQuery(starpu_cuda_get_local_stream()) == cudaSuccess, "CUDA codelets have to wait for termination of their kernels on the starpu_cuda_get_local_stream() stream");
#endif
#endif
	}

	_starpu_driver_end_job(args, j, args->perf_arch, &codelet_end, 0, profiling);

	_starpu_driver_update_job_feedback(j, args, args->perf_arch, &codelet_start, &codelet_end, profiling);

	_starpu_push_task_output(j, mask);

	return 0;
}

/* XXX Should this be merged with _starpu_init_cuda ? */
int _starpu_cuda_driver_init(struct _starpu_worker *args)
{
	unsigned devid = args->devid;

	_starpu_worker_start(args, _STARPU_FUT_CUDA_KEY);

#ifndef STARPU_SIMGRID
	init_context(devid);
#endif

	_starpu_cuda_limit_gpu_mem_if_needed(devid);
	_starpu_memory_manager_set_global_memory_size(args->memory_node, _starpu_cuda_get_global_mem_size(devid));

	_starpu_malloc_init(args->memory_node);

	/* one more time to avoid hacks from third party lib :) */
	_starpu_bind_thread_on_cpu(args->config, args->bindid, args->workerid);

	args->status = STATUS_UNKNOWN;

	float size = (float) global_mem[devid] / (1<<30);
#ifdef STARPU_SIMGRID
	const char *devname = "Simgrid";
#else
	/* get the device's name */
	char devname[128];
	strncpy(devname, props[devid].name, 127);
	devname[127] = 0;
#endif

#if defined(STARPU_HAVE_BUSID) && !defined(STARPU_SIMGRID)
#if defined(STARPU_HAVE_DOMAINID) && !defined(STARPU_SIMGRID)
	if (props[devid].pciDomainID)
		snprintf(args->name, sizeof(args->name), "CUDA %u (%s %.1f GiB %04x:%02x:%02x.0)", devid, devname, size, props[devid].pciDomainID, props[devid].pciBusID, props[devid].pciDeviceID);
	else
#endif
		snprintf(args->name, sizeof(args->name), "CUDA %u (%s %.1f GiB %02x:%02x.0)", devid, devname, size, props[devid].pciBusID, props[devid].pciDeviceID);
#else
	snprintf(args->name, sizeof(args->name), "CUDA %u (%s %.1f GiB)", devid, devname, size);
#endif
	snprintf(args->short_name, sizeof(args->short_name), "CUDA %u", devid);
	_STARPU_DEBUG("cuda (%s) dev id %u thread is ready to run on CPU %d !\n", devname, devid, args->bindid);

	_STARPU_TRACE_WORKER_INIT_END;

	/* tell the main thread that this one is ready */
	STARPU_PTHREAD_MUTEX_LOCK(&args->mutex);
	args->worker_is_initialized = 1;
	STARPU_PTHREAD_COND_SIGNAL(&args->ready_cond);
	STARPU_PTHREAD_MUTEX_UNLOCK(&args->mutex);

	return 0;
}

int _starpu_cuda_driver_run_once(struct _starpu_worker *args)
{
	unsigned memnode = args->memory_node;
	int workerid = args->workerid;

	_STARPU_TRACE_START_PROGRESS(memnode);
	_starpu_datawizard_progress(memnode, 1);
	_STARPU_TRACE_END_PROGRESS(memnode);

	struct starpu_task *task;
	struct _starpu_job *j = NULL;

	task = _starpu_get_worker_task(args, workerid, memnode);

	if (!task)
		return 0;

	j = _starpu_get_job_associated_to_task(task);

	/* can CUDA do that task ? */
	if (!_STARPU_CUDA_MAY_PERFORM(j))
	{
		/* this is neither a cuda or a cublas task */
		_starpu_push_task_to_workers(task);
		return 0;
	}

	_starpu_set_current_task(task);
	args->current_task = j->task;

	int res = execute_job_on_cuda(j, args);

	_starpu_set_current_task(NULL);
	args->current_task = NULL;

	if (res)
	{
		switch (res)
		{
			case -EAGAIN:
				_STARPU_DISP("ouch, CUDA could not actually run task %p, putting it back...\n", task);
				_starpu_push_task_to_workers(task);
				STARPU_ABORT();
			default:
				STARPU_ABORT();
		}
	}

	_starpu_handle_job_termination(j);

	return 0;
}

int _starpu_cuda_driver_deinit(struct _starpu_worker *args)
{
	unsigned memnode = args->memory_node;
	_STARPU_TRACE_WORKER_DEINIT_START;

	_starpu_handle_all_pending_node_data_requests(memnode);

	/* In case there remains some memory that was automatically
	 * allocated by StarPU, we release it now. Note that data
	 * coherency is not maintained anymore at that point ! */
	_starpu_free_all_automatically_allocated_buffers(memnode);

	_starpu_malloc_shutdown(memnode);

#ifndef STARPU_SIMGRID
	deinit_context(args->workerid);
#endif

	args->worker_is_initialized = 0;
	_STARPU_TRACE_WORKER_DEINIT_END(_STARPU_FUT_CUDA_KEY);

	return 0;
}

void *_starpu_cuda_worker(void *arg)
{
	struct _starpu_worker* args = arg;

	_starpu_cuda_driver_init(args);
	while (_starpu_machine_is_running())
	{
		_starpu_may_pause();
		_starpu_cuda_driver_run_once(args);
	}
	_starpu_cuda_driver_deinit(args);

	return NULL;
}

#ifdef STARPU_USE_CUDA
void starpu_cublas_report_error(const char *func, const char *file, int line, int status)
{
	char *errormsg;
	switch (status)
	{
		case CUBLAS_STATUS_SUCCESS:
			errormsg = "success";
			break;
		case CUBLAS_STATUS_NOT_INITIALIZED:
			errormsg = "not initialized";
			break;
		case CUBLAS_STATUS_ALLOC_FAILED:
			errormsg = "alloc failed";
			break;
		case CUBLAS_STATUS_INVALID_VALUE:
			errormsg = "invalid value";
			break;
		case CUBLAS_STATUS_ARCH_MISMATCH:
			errormsg = "arch mismatch";
			break;
		case CUBLAS_STATUS_EXECUTION_FAILED:
			errormsg = "execution failed";
			break;
		case CUBLAS_STATUS_INTERNAL_ERROR:
			errormsg = "internal error";
			break;
		default:
			errormsg = "unknown error";
			break;
	}
	fprintf(stderr, "oops in %s (%s:%d)... %d: %s \n", func, file, line, status, errormsg);
	STARPU_ABORT();
}

void starpu_cuda_report_error(const char *func, const char *file, int line, cudaError_t status)
{
	const char *errormsg = cudaGetErrorString(status);
	printf("oops in %s (%s:%d)... %d: %s \n", func, file, line, status, errormsg);
	STARPU_ABORT();
}
#endif /* STARPU_USE_CUDA */

#ifdef STARPU_USE_CUDA
int
starpu_cuda_copy_async_sync(void *src_ptr, unsigned src_node,
			    void *dst_ptr, unsigned dst_node,
			    size_t ssize, cudaStream_t stream,
			    enum cudaMemcpyKind kind)
{
#ifdef HAVE_CUDA_MEMCPY_PEER
	int peer_copy = 0;
	int src_dev = -1, dst_dev = -1;
#endif
	cudaError_t cures = 0;

	if (kind == cudaMemcpyDeviceToDevice && src_node != dst_node)
	{
#ifdef HAVE_CUDA_MEMCPY_PEER
		peer_copy = 1;
		src_dev = _starpu_memory_node_get_devid(src_node);
		dst_dev = _starpu_memory_node_get_devid(dst_node);
#else
		STARPU_ABORT();
#endif
	}

	if (stream)
	{
		_STARPU_TRACE_START_DRIVER_COPY_ASYNC(src_node, dst_node);
#ifdef HAVE_CUDA_MEMCPY_PEER
		if (peer_copy)
		{
			cures = cudaMemcpyPeerAsync((char *) dst_ptr, dst_dev,
						    (char *) src_ptr, src_dev,
						    ssize, stream);
		}
		else
#endif
		{
			cures = cudaMemcpyAsync((char *)dst_ptr, (char *)src_ptr, ssize, kind, stream);
		}
		_STARPU_TRACE_END_DRIVER_COPY_ASYNC(src_node, dst_node);
	}

	/* Test if the asynchronous copy has failed or if the caller only asked for a synchronous copy */
	if (stream == NULL || cures)
	{
		/* do it in a synchronous fashion */
#ifdef HAVE_CUDA_MEMCPY_PEER
		if (peer_copy)
		{
			cures = cudaMemcpyPeer((char *) dst_ptr, dst_dev,
					       (char *) src_ptr, src_dev,
					       ssize);
		}
		else
#endif
		{
			cures = cudaMemcpy((char *)dst_ptr, (char *)src_ptr, ssize, kind);
		}


		if (STARPU_UNLIKELY(cures))
			STARPU_CUDA_REPORT_ERROR(cures);

		return 0;
	}

	return -EAGAIN;
}
#endif /* STARPU_USE_CUDA */

int _starpu_run_cuda(struct _starpu_worker *workerarg)
{
	workerarg->set = NULL;
	workerarg->worker_is_initialized = 0;

	/* Let's go ! */
	_starpu_cuda_worker(workerarg);

	return 0;
}
