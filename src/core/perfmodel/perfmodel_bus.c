/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2009-2014, 2016  Université de Bordeaux
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

#ifdef STARPU_USE_CUDA
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>
#endif
#include <stdlib.h>
#include <math.h>

#include <starpu.h>
#include <starpu_cuda.h>
#include <starpu_opencl.h>
#include <common/config.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <core/workers.h>
#include <core/perfmodel/perfmodel.h>
#include <core/simgrid.h>
#include <common/utils.h>

#ifdef STARPU_USE_OPENCL
#include <starpu_opencl.h>
#endif

#ifdef STARPU_HAVE_WINDOWS
#include <windows.h>
#endif

#define SIZE	(32*1024*1024*sizeof(char))
#define NITER	32

#ifndef STARPU_SIMGRID
static void _starpu_bus_force_sampling(void);
#endif

/* timing is in µs per byte (i.e. slowness, inverse of bandwidth) */
struct dev_timing
{
	int cpu_id;
	double timing_htod;
	double latency_htod;
	double timing_dtoh;
	double latency_dtoh;
};

/* TODO: measure latency */
static double bandwidth_matrix[STARPU_MAXNODES][STARPU_MAXNODES];
static double latency_matrix[STARPU_MAXNODES][STARPU_MAXNODES];
static unsigned was_benchmarked = 0;
static unsigned ncpus = 0;
static unsigned ncuda = 0;
static unsigned nopencl = 0;

/* Benchmarking the performance of the bus */

#ifdef STARPU_USE_CUDA
static uint64_t cuda_size[STARPU_MAXCUDADEVS];
static int cuda_affinity_matrix[STARPU_MAXCUDADEVS][STARPU_MAXCPUS];
static double cudadev_timing_htod[STARPU_MAXNODES] = {0.0};
static double cudadev_latency_htod[STARPU_MAXNODES] = {0.0};
static double cudadev_timing_dtoh[STARPU_MAXNODES] = {0.0};
static double cudadev_latency_dtoh[STARPU_MAXNODES] = {0.0};
#ifdef HAVE_CUDA_MEMCPY_PEER
static double cudadev_timing_dtod[STARPU_MAXNODES][STARPU_MAXNODES] = {{0.0}};
static double cudadev_latency_dtod[STARPU_MAXNODES][STARPU_MAXNODES] = {{0.0}};
#endif
static struct dev_timing cudadev_timing_per_cpu[STARPU_MAXNODES*STARPU_MAXCPUS];
#endif
#ifdef STARPU_USE_OPENCL
static uint64_t opencl_size[STARPU_MAXCUDADEVS];
static int opencl_affinity_matrix[STARPU_MAXOPENCLDEVS][STARPU_MAXCPUS];
static double opencldev_timing_htod[STARPU_MAXNODES] = {0.0};
static double opencldev_latency_htod[STARPU_MAXNODES] = {0.0};
static double opencldev_timing_dtoh[STARPU_MAXNODES] = {0.0};
static double opencldev_latency_dtoh[STARPU_MAXNODES] = {0.0};
static struct dev_timing opencldev_timing_per_cpu[STARPU_MAXNODES*STARPU_MAXCPUS];
#endif

#ifdef STARPU_HAVE_HWLOC
static hwloc_topology_t hwtopology;
#endif

#if (defined(STARPU_USE_CUDA) || defined(STARPU_USE_OPENCL)) && !defined(STARPU_SIMGRID)

#ifdef STARPU_USE_CUDA

static void measure_bandwidth_between_host_and_dev_on_cpu_with_cuda(int dev, int cpu, struct dev_timing *dev_timing_per_cpu)
{
	struct _starpu_machine_config *config = _starpu_get_machine_config();
	_starpu_bind_thread_on_cpu(config, cpu, STARPU_NOWORKERID);
	size_t size = SIZE;

	/* Initialize CUDA context on the device */
	/* We do not need to enable OpenGL interoperability at this point,
	 * since we cleanly shutdown CUDA before returning. */
	cudaSetDevice(dev);

	/* hack to avoid third party libs to rebind threads */
	_starpu_bind_thread_on_cpu(config, cpu, STARPU_NOWORKERID);

	/* hack to force the initialization */
	cudaFree(0);

	/* hack to avoid third party libs to rebind threads */
	_starpu_bind_thread_on_cpu(config, cpu, STARPU_NOWORKERID);

        /* Get the maximum size which can be allocated on the device */
	struct cudaDeviceProp prop;
	cudaError_t cures;
	cures = cudaGetDeviceProperties(&prop, dev);
	if (STARPU_UNLIKELY(cures)) STARPU_CUDA_REPORT_ERROR(cures);
	cuda_size[dev] = prop.totalGlobalMem;
        if (size > prop.totalGlobalMem/4) size = prop.totalGlobalMem/4;

	/* Allocate a buffer on the device */
	unsigned char *d_buffer;
	cures = cudaMalloc((void **)&d_buffer, size);
	STARPU_ASSERT(cures == cudaSuccess);

	/* hack to avoid third party libs to rebind threads */
	_starpu_bind_thread_on_cpu(config, cpu, STARPU_NOWORKERID);

	/* Allocate a buffer on the host */
	unsigned char *h_buffer;
	cures = cudaHostAlloc((void **)&h_buffer, size, 0);
	STARPU_ASSERT(cures == cudaSuccess);

	/* hack to avoid third party libs to rebind threads */
	_starpu_bind_thread_on_cpu(config, cpu, STARPU_NOWORKERID);

	/* Fill them */
	memset(h_buffer, 0, size);
	cudaMemset(d_buffer, 0, size);

	/* hack to avoid third party libs to rebind threads */
	_starpu_bind_thread_on_cpu(config, cpu, STARPU_NOWORKERID);

	unsigned iter;
	double timing;
	double start;
	double end;

	/* Measure upload bandwidth */
	start = starpu_timing_now();
	for (iter = 0; iter < NITER; iter++)
	{
		cudaMemcpy(d_buffer, h_buffer, size, cudaMemcpyHostToDevice);
		cudaThreadSynchronize();
	}
	end = starpu_timing_now();
	timing = end - start;

	dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS+cpu].timing_htod = timing/NITER/size;

	/* Measure download bandwidth */
	start = starpu_timing_now();
	for (iter = 0; iter < NITER; iter++)
	{
		cudaMemcpy(h_buffer, d_buffer, size, cudaMemcpyDeviceToHost);
		cudaThreadSynchronize();
	}
	end = starpu_timing_now();
	timing = end - start;

	dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS+cpu].timing_dtoh = timing/NITER/size;

	/* Measure upload latency */
	start = starpu_timing_now();
	for (iter = 0; iter < NITER; iter++)
	{
		cudaMemcpy(d_buffer, h_buffer, 1, cudaMemcpyHostToDevice);
		cudaThreadSynchronize();
	}
	end = starpu_timing_now();
	timing = end - start;

	dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS+cpu].latency_htod = timing/NITER;

	/* Measure download latency */
	start = starpu_timing_now();
	for (iter = 0; iter < NITER; iter++)
	{
		cudaMemcpy(h_buffer, d_buffer, 1, cudaMemcpyDeviceToHost);
		cudaThreadSynchronize();
	}
	end = starpu_timing_now();
	timing = end - start;

	dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS+cpu].latency_dtoh = timing/NITER;

	/* Free buffers */
	cudaFreeHost(h_buffer);
	cudaFree(d_buffer);

	cudaThreadExit();
}

#ifdef HAVE_CUDA_MEMCPY_PEER
static void measure_bandwidth_between_dev_and_dev_cuda(int src, int dst)
{
	size_t size = SIZE;
	int can;

        /* Get the maximum size which can be allocated on the device */
	struct cudaDeviceProp prop;
	cudaError_t cures;

	cures = cudaGetDeviceProperties(&prop, src);
	if (STARPU_UNLIKELY(cures)) STARPU_CUDA_REPORT_ERROR(cures);
        if (size > prop.totalGlobalMem/4) size = prop.totalGlobalMem/4;
	cures = cudaGetDeviceProperties(&prop, dst);
	if (STARPU_UNLIKELY(cures)) STARPU_CUDA_REPORT_ERROR(cures);
        if (size > prop.totalGlobalMem/4) size = prop.totalGlobalMem/4;

	/* Initialize CUDA context on the source */
	/* We do not need to enable OpenGL interoperability at this point,
	 * since we cleanly shutdown CUDA before returning. */
	cudaSetDevice(src);

	if (starpu_get_env_number("STARPU_ENABLE_CUDA_GPU_GPU_DIRECT") != 0)
	{
		cures = cudaDeviceCanAccessPeer(&can, src, dst);
		if (!cures && can)
		{
			cures = cudaDeviceEnablePeerAccess(dst, 0);
			if (!cures)
				_STARPU_DISP("GPU-Direct %d -> %d\n", dst, src);
		}
	}

	/* Allocate a buffer on the device */
	unsigned char *s_buffer;
	cures = cudaMalloc((void **)&s_buffer, size);
	STARPU_ASSERT(cures == cudaSuccess);
	cudaMemset(s_buffer, 0, size);

	/* Initialize CUDA context on the destination */
	/* We do not need to enable OpenGL interoperability at this point,
	 * since we cleanly shutdown CUDA before returning. */
	cudaSetDevice(dst);

	if (starpu_get_env_number("STARPU_ENABLE_CUDA_GPU_GPU_DIRECT") != 0)
	{
		cures = cudaDeviceCanAccessPeer(&can, dst, src);
		if (!cures && can)
		{
			cures = cudaDeviceEnablePeerAccess(src, 0);
			if (!cures)
				_STARPU_DISP("GPU-Direct %d -> %d\n", src, dst);
		}
	}

	/* Allocate a buffer on the device */
	unsigned char *d_buffer;
	cures = cudaMalloc((void **)&d_buffer, size);
	STARPU_ASSERT(cures == cudaSuccess);
	cudaMemset(d_buffer, 0, size);

	unsigned iter;
	double timing;
	double start;
	double end;

	/* Measure upload bandwidth */
	start = starpu_timing_now();
	for (iter = 0; iter < NITER; iter++)
	{
		cudaMemcpyPeer(d_buffer, dst, s_buffer, src, size);
		cudaThreadSynchronize();
	}
	end = starpu_timing_now();
	timing = end - start;

	cudadev_timing_dtod[src+1][dst+1] = timing/NITER/size;

	/* Measure upload latency */
	start = starpu_timing_now();
	for (iter = 0; iter < NITER; iter++)
	{
		cudaMemcpyPeer(d_buffer, dst, s_buffer, src, 1);
		cudaThreadSynchronize();
	}
	end = starpu_timing_now();
	timing = end - start;

	cudadev_latency_dtod[src+1][dst+1] = timing/NITER;

	/* Free buffers */
	cudaFree(d_buffer);
	cudaSetDevice(src);
	cudaFree(s_buffer);

	cudaThreadExit();
}
#endif
#endif

#ifdef STARPU_USE_OPENCL
static void measure_bandwidth_between_host_and_dev_on_cpu_with_opencl(int dev, int cpu, struct dev_timing *dev_timing_per_cpu)
{
        cl_context context;
        cl_command_queue queue;
        cl_int err=0;
	size_t size = SIZE;
	int not_initialized;

        struct _starpu_machine_config *config = _starpu_get_machine_config();
	_starpu_bind_thread_on_cpu(config, cpu, STARPU_NOWORKERID);

	/* Is the context already initialised ? */
        starpu_opencl_get_context(dev, &context);
	not_initialized = (context == NULL);
	if (not_initialized == 1)
	     _starpu_opencl_init_context(dev);

	/* Get context and queue */
        starpu_opencl_get_context(dev, &context);
        starpu_opencl_get_queue(dev, &queue);

        /* Get the maximum size which can be allocated on the device */
        cl_device_id device;
	cl_ulong maxMemAllocSize, totalGlobalMem;
        starpu_opencl_get_device(dev, &device);
	err = clGetDeviceInfo(device, CL_DEVICE_MAX_MEM_ALLOC_SIZE, sizeof(maxMemAllocSize), &maxMemAllocSize, NULL);
        if (STARPU_UNLIKELY(err != CL_SUCCESS)) STARPU_OPENCL_REPORT_ERROR(err);
        if (size > (size_t)maxMemAllocSize/4) size = maxMemAllocSize/4;

	err = clGetDeviceInfo(device, CL_DEVICE_GLOBAL_MEM_SIZE , sizeof(totalGlobalMem), &totalGlobalMem, NULL);
        if (STARPU_UNLIKELY(err != CL_SUCCESS)) STARPU_OPENCL_REPORT_ERROR(err);
	opencl_size[dev] = totalGlobalMem;

	if (_starpu_opencl_get_device_type(dev) == CL_DEVICE_TYPE_CPU)
	{
		/* Let's not use too much RAM when running OpenCL on a CPU: it
		 * would make the OS swap like crazy. */
		size /= 2;
	}

	/* hack to avoid third party libs to rebind threads */
	_starpu_bind_thread_on_cpu(config, cpu, STARPU_NOWORKERID);

	/* Allocate a buffer on the device */
	cl_mem d_buffer;
	d_buffer = clCreateBuffer(context, CL_MEM_READ_WRITE, size, NULL, &err);
	if (STARPU_UNLIKELY(err != CL_SUCCESS)) STARPU_OPENCL_REPORT_ERROR(err);

	/* hack to avoid third party libs to rebind threads */
	_starpu_bind_thread_on_cpu(config, cpu, STARPU_NOWORKERID);
        /* Allocate a buffer on the host */
	unsigned char *h_buffer;
        h_buffer = (unsigned char *)malloc(size);
	STARPU_ASSERT(h_buffer);

	/* hack to avoid third party libs to rebind threads */
	_starpu_bind_thread_on_cpu(config, cpu, STARPU_NOWORKERID);
        /* Fill them */
	memset(h_buffer, 0, size);
        err = clEnqueueWriteBuffer(queue, d_buffer, CL_TRUE, 0, size, h_buffer, 0, NULL, NULL);
        if (STARPU_UNLIKELY(err != CL_SUCCESS)) STARPU_OPENCL_REPORT_ERROR(err);
        clFinish(queue);
	/* hack to avoid third party libs to rebind threads */
	_starpu_bind_thread_on_cpu(config, cpu, STARPU_NOWORKERID);

        unsigned iter;
	double timing;
	double start;
	double end;

	/* Measure upload bandwidth */
	start = starpu_timing_now();
	for (iter = 0; iter < NITER; iter++)
	{
                err = clEnqueueWriteBuffer(queue, d_buffer, CL_TRUE, 0, size, h_buffer, 0, NULL, NULL);
                if (STARPU_UNLIKELY(err != CL_SUCCESS)) STARPU_OPENCL_REPORT_ERROR(err);
                clFinish(queue);
	}
	end = starpu_timing_now();
	timing = end - start;

	dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS+cpu].timing_htod = timing/NITER/size;

	/* Measure download bandwidth */
	start = starpu_timing_now();
	for (iter = 0; iter < NITER; iter++)
	{
                err = clEnqueueReadBuffer(queue, d_buffer, CL_TRUE, 0, size, h_buffer, 0, NULL, NULL);
                if (STARPU_UNLIKELY(err != CL_SUCCESS)) STARPU_OPENCL_REPORT_ERROR(err);
                clFinish(queue);
	}
	end = starpu_timing_now();
	timing = end - start;

	dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS+cpu].timing_dtoh = timing/NITER/size;

	/* Measure upload latency */
	start = starpu_timing_now();
	for (iter = 0; iter < NITER; iter++)
	{
		err = clEnqueueWriteBuffer(queue, d_buffer, CL_TRUE, 0, 1, h_buffer, 0, NULL, NULL);
                if (STARPU_UNLIKELY(err != CL_SUCCESS)) STARPU_OPENCL_REPORT_ERROR(err);
                clFinish(queue);
	}
	end = starpu_timing_now();
	timing = end - start;

	dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS+cpu].latency_htod = timing/NITER;

	/* Measure download latency */
	start = starpu_timing_now();
	for (iter = 0; iter < NITER; iter++)
	{
		err = clEnqueueReadBuffer(queue, d_buffer, CL_TRUE, 0, 1, h_buffer, 0, NULL, NULL);
                if (STARPU_UNLIKELY(err != CL_SUCCESS)) STARPU_OPENCL_REPORT_ERROR(err);
                clFinish(queue);
	}
	end = starpu_timing_now();
	timing = end - start;

	dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS+cpu].latency_dtoh = timing/NITER;

	/* Free buffers */
	err = clReleaseMemObject(d_buffer);
	if (STARPU_UNLIKELY(err != CL_SUCCESS))
		STARPU_OPENCL_REPORT_ERROR(err);
	free(h_buffer);

	/* Uninitiliaze OpenCL context on the device */
	if (not_initialized == 1)
	     _starpu_opencl_deinit_context(dev);
}
#endif

/* NB: we want to sort the bandwidth by DECREASING order */
static int compar_dev_timing(const void *left_dev_timing, const void *right_dev_timing)
{
	const struct dev_timing *left = (const struct dev_timing *)left_dev_timing;
	const struct dev_timing *right = (const struct dev_timing *)right_dev_timing;

	double left_dtoh = left->timing_dtoh;
	double left_htod = left->timing_htod;
	double right_dtoh = right->timing_dtoh;
	double right_htod = right->timing_htod;

	double timing_sum2_left = left_dtoh*left_dtoh + left_htod*left_htod;
	double timing_sum2_right = right_dtoh*right_dtoh + right_htod*right_htod;

	/* it's for a decreasing sorting */
	return (timing_sum2_left > timing_sum2_right);
}

#ifdef STARPU_HAVE_HWLOC
static int find_numa_node(hwloc_obj_t obj)
{
	STARPU_ASSERT(obj);
	hwloc_obj_t current = obj;

	while (current->depth != HWLOC_OBJ_NODE)
	{
		current = current->parent;

		/* If we don't find a "node" obj before the root, this means
		 * hwloc does not know whether there are numa nodes or not, so
		 * we should not use a per-node sampling in that case. */
		STARPU_ASSERT(current);
	}

	STARPU_ASSERT(current->depth == HWLOC_OBJ_NODE);

	return current->logical_index;
}
#endif

static void measure_bandwidth_between_cpus_and_dev(int dev, struct dev_timing *dev_timing_per_cpu, char *type)
{
	/* Either we have hwloc and we measure the bandwith between each GPU
	 * and each NUMA node, or we don't have such NUMA information and we
	 * measure the bandwith for each pair of (CPU, GPU), which is slower.
	 * */
#ifdef STARPU_HAVE_HWLOC
	int cpu_depth = hwloc_get_type_depth(hwtopology, HWLOC_OBJ_CORE);
	int nnuma_nodes = hwloc_get_nbobjs_by_depth(hwtopology, HWLOC_OBJ_NODE);

	/* If no NUMA node was found, we assume that we have a single memory
	 * bank. */
	const unsigned no_node_obj_was_found = (nnuma_nodes == 0);

	unsigned *is_available_per_numa_node = NULL;
	double *dev_timing_htod_per_numa_node = NULL;
	double *dev_latency_htod_per_numa_node = NULL;
	double *dev_timing_dtoh_per_numa_node = NULL;
	double *dev_latency_dtoh_per_numa_node = NULL;

	if (!no_node_obj_was_found)
	{
		is_available_per_numa_node = (unsigned *)malloc(nnuma_nodes * sizeof(unsigned));
		STARPU_ASSERT(is_available_per_numa_node);

		dev_timing_htod_per_numa_node = (double *)malloc(nnuma_nodes * sizeof(double));
		STARPU_ASSERT(dev_timing_htod_per_numa_node);
		dev_latency_htod_per_numa_node = (double *)malloc(nnuma_nodes * sizeof(double));
		STARPU_ASSERT(dev_latency_htod_per_numa_node);

		dev_timing_dtoh_per_numa_node = (double *)malloc(nnuma_nodes * sizeof(double));
		STARPU_ASSERT(dev_timing_dtoh_per_numa_node);
		dev_latency_dtoh_per_numa_node = (double *)malloc(nnuma_nodes * sizeof(double));
		STARPU_ASSERT(dev_latency_dtoh_per_numa_node);

		memset(is_available_per_numa_node, 0, nnuma_nodes*sizeof(unsigned));
	}
#endif

	unsigned cpu;
	for (cpu = 0; cpu < ncpus; cpu++)
	{
		dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS+cpu].cpu_id = cpu;

#ifdef STARPU_HAVE_HWLOC
		int numa_id = 0;

		if (!no_node_obj_was_found)
		{
			hwloc_obj_t obj = hwloc_get_obj_by_depth(hwtopology, cpu_depth, cpu);

			numa_id = find_numa_node(obj);

			if (is_available_per_numa_node[numa_id])
			{
				/* We reuse the previous numbers for that NUMA node */
				dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS+cpu].timing_htod =
					dev_timing_htod_per_numa_node[numa_id];
				dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS+cpu].latency_htod =
					dev_latency_htod_per_numa_node[numa_id];
				dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS+cpu].timing_dtoh =
					dev_timing_dtoh_per_numa_node[numa_id];
				dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS+cpu].latency_dtoh =
					dev_latency_dtoh_per_numa_node[numa_id];
				continue;
			}
		}
#endif

#ifdef STARPU_USE_CUDA
                if (strncmp(type, "CUDA", 4) == 0)
                        measure_bandwidth_between_host_and_dev_on_cpu_with_cuda(dev, cpu, dev_timing_per_cpu);
#endif
#ifdef STARPU_USE_OPENCL
                if (strncmp(type, "OpenCL", 6) == 0)
                        measure_bandwidth_between_host_and_dev_on_cpu_with_opencl(dev, cpu, dev_timing_per_cpu);
#endif

#ifdef STARPU_HAVE_HWLOC
		if (!no_node_obj_was_found && !is_available_per_numa_node[numa_id])
		{
			/* Save the results for that NUMA node */
			dev_timing_htod_per_numa_node[numa_id] =
				dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS+cpu].timing_htod;
			dev_latency_htod_per_numa_node[numa_id] =
				dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS+cpu].latency_htod;
			dev_timing_dtoh_per_numa_node[numa_id] =
				dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS+cpu].timing_dtoh;
			dev_latency_dtoh_per_numa_node[numa_id] =
				dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS+cpu].latency_dtoh;

			is_available_per_numa_node[numa_id] = 1;
		}
#endif
        }

#ifdef STARPU_HAVE_HWLOC
	if (!no_node_obj_was_found)
	{
		free(is_available_per_numa_node);
		free(dev_timing_htod_per_numa_node);
		free(dev_latency_htod_per_numa_node);
		free(dev_timing_dtoh_per_numa_node);
		free(dev_latency_dtoh_per_numa_node);
	}
#endif /* STARPU_HAVE_HWLOC */
}

static void measure_bandwidth_between_host_and_dev(int dev, double *dev_timing_htod, double *dev_latency_htod,
                                                   double *dev_timing_dtoh, double *dev_latency_dtoh,
                                                   struct dev_timing *dev_timing_per_cpu, char *type)
{
	measure_bandwidth_between_cpus_and_dev(dev, dev_timing_per_cpu, type);

	/* sort the results */
	qsort(&(dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS]), ncpus,
              sizeof(struct dev_timing),
			compar_dev_timing);

#ifdef STARPU_VERBOSE
        unsigned cpu;
	for (cpu = 0; cpu < ncpus; cpu++)
	{
		unsigned current_cpu = dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS+cpu].cpu_id;
		double bandwidth_dtoh = dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS+cpu].timing_dtoh;
		double bandwidth_htod = dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS+cpu].timing_htod;

		double bandwidth_sum2 = bandwidth_dtoh*bandwidth_dtoh + bandwidth_htod*bandwidth_htod;

		_STARPU_DISP("(%10s) BANDWIDTH GPU %d CPU %u - htod %f - dtoh %f - %f\n", type, dev, current_cpu, bandwidth_htod, bandwidth_dtoh, sqrt(bandwidth_sum2));
	}

	unsigned best_cpu = dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS+0].cpu_id;

	_STARPU_DISP("(%10s) BANDWIDTH GPU %d BEST CPU %u\n", type, dev, best_cpu);
#endif

	/* The results are sorted in a decreasing order, so that the best
	 * measurement is currently the first entry. */
	dev_timing_dtoh[dev+1] = dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS+0].timing_dtoh;
	dev_latency_dtoh[dev+1] = dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS+0].latency_dtoh;
	dev_timing_htod[dev+1] = dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS+0].timing_htod;
	dev_latency_htod[dev+1] = dev_timing_per_cpu[(dev+1)*STARPU_MAXCPUS+0].latency_htod;
}
#endif /* defined(STARPU_USE_CUDA) || defined(STARPU_USE_OPENCL) */

static void benchmark_all_gpu_devices(void)
{
#ifdef STARPU_SIMGRID
	_STARPU_DISP("can not measure bus in simgrid mode, please run starpu_calibrate_bus in non-simgrid mode to make sure the bus performance model was calibrated\n");
	STARPU_ABORT();
#else /* !SIMGRID */
#if defined(STARPU_USE_CUDA) || defined(STARPU_USE_OPENCL)
	unsigned i;
#endif
#ifdef HAVE_CUDA_MEMCPY_PEER
	unsigned j;
#endif

	_STARPU_DEBUG("Benchmarking the speed of the bus\n");

#ifdef STARPU_HAVE_HWLOC
	hwloc_topology_init(&hwtopology);
	hwloc_topology_load(hwtopology);
#endif

#ifdef STARPU_HAVE_HWLOC
	hwloc_bitmap_t former_cpuset = hwloc_bitmap_alloc();
	hwloc_get_cpubind(hwtopology, former_cpuset, HWLOC_CPUBIND_THREAD);
#elif __linux__
	/* Save the current cpu binding */
	cpu_set_t former_process_affinity;
	int ret;
	ret = sched_getaffinity(0, sizeof(former_process_affinity), &former_process_affinity);
	if (ret)
	{
		perror("sched_getaffinity");
		STARPU_ABORT();
	}
#else
#warning Missing binding support, StarPU will not be able to properly benchmark NUMA topology
#endif

	struct _starpu_machine_config *config = _starpu_get_machine_config();
	ncpus = _starpu_topology_get_nhwcpu(config);

#ifdef STARPU_USE_CUDA
	ncuda = _starpu_get_cuda_device_count();
	for (i = 0; i < ncuda; i++)
	{
		_STARPU_DISP("CUDA %d...\n", i);
		/* measure bandwidth between Host and Device i */
		measure_bandwidth_between_host_and_dev(i, cudadev_timing_htod, cudadev_latency_htod, cudadev_timing_dtoh, cudadev_latency_dtoh, cudadev_timing_per_cpu, "CUDA");
	}
#ifdef HAVE_CUDA_MEMCPY_PEER
	for (i = 0; i < ncuda; i++)
		for (j = 0; j < ncuda; j++)
			if (i != j)
			{
				_STARPU_DISP("CUDA %d -> %d...\n", i, j);
				/* measure bandwidth between Host and Device i */
				measure_bandwidth_between_dev_and_dev_cuda(i, j);
			}
#endif
#endif
#ifdef STARPU_USE_OPENCL
        nopencl = _starpu_opencl_get_device_count();
	for (i = 0; i < nopencl; i++)
	{
		_STARPU_DISP("OpenCL %d...\n", i);
		/* measure bandwith between Host and Device i */
		measure_bandwidth_between_host_and_dev(i, opencldev_timing_htod, opencldev_latency_htod, opencldev_timing_dtoh, opencldev_latency_dtoh, opencldev_timing_per_cpu, "OpenCL");
	}
#endif

#ifdef STARPU_HAVE_HWLOC
	hwloc_set_cpubind(hwtopology, former_cpuset, HWLOC_CPUBIND_THREAD);
	hwloc_bitmap_free(former_cpuset);
#elif __linux__
	/* Restore the former affinity */
	ret = sched_setaffinity(0, sizeof(former_process_affinity), &former_process_affinity);
	if (ret)
	{
		perror("sched_setaffinity");
		STARPU_ABORT();
	}
#endif

#ifdef STARPU_HAVE_HWLOC
	hwloc_topology_destroy(hwtopology);
#endif

	_STARPU_DEBUG("Benchmarking the speed of the bus is done.\n");

	was_benchmarked = 1;
#endif /* !SIMGRID */
}

static void get_bus_path(const char *type, char *path, size_t maxlen)
{
	char hostname[65];

	_starpu_gethostname(hostname, sizeof(hostname));
	snprintf(path, maxlen, "%s%s.%s", _starpu_get_perf_model_dir_bus(), hostname, type);
}

/*
 *	Affinity
 */

#ifndef STARPU_SIMGRID
static void get_affinity_path(char *path, size_t maxlen)
{
	get_bus_path("affinity", path, maxlen);
}

static void load_bus_affinity_file_content(void)
{
#if defined(STARPU_USE_CUDA) || defined(STARPU_USE_OPENCL)
	FILE *f;

	char path[256];
	get_affinity_path(path, sizeof(path));

	_STARPU_DEBUG("loading affinities from %s\n", path);

	f = fopen(path, "r");
	STARPU_ASSERT(f);

	_starpu_frdlock(f);

	struct _starpu_machine_config *config = _starpu_get_machine_config();
	ncpus = _starpu_topology_get_nhwcpu(config);
        unsigned gpu;

#ifdef STARPU_USE_CUDA
	ncuda = _starpu_get_cuda_device_count();
	for (gpu = 0; gpu < ncuda; gpu++)
	{
		int ret;
		unsigned dummy;

		_starpu_drop_comments(f);
		ret = fscanf(f, "%d\t", &dummy);
		STARPU_ASSERT(ret == 1);

		STARPU_ASSERT(dummy == gpu);

		unsigned cpu;
		for (cpu = 0; cpu < ncpus; cpu++)
		{
			ret = fscanf(f, "%d\t", &cuda_affinity_matrix[gpu][cpu]);
			STARPU_ASSERT(ret == 1);
		}

		ret = fscanf(f, "\n");
		STARPU_ASSERT(ret == 0);
	}
#endif /* !STARPU_USE_CUDA */
#ifdef STARPU_USE_OPENCL
        nopencl = _starpu_opencl_get_device_count();
	for (gpu = 0; gpu < nopencl; gpu++)
	{
		int ret;
		unsigned dummy;

		_starpu_drop_comments(f);
		ret = fscanf(f, "%d\t", &dummy);
		STARPU_ASSERT(ret == 1);

		STARPU_ASSERT(dummy == gpu);

		unsigned cpu;
		for (cpu = 0; cpu < ncpus; cpu++)
		{
			ret = fscanf(f, "%d\t", &opencl_affinity_matrix[gpu][cpu]);
			STARPU_ASSERT(ret == 1);
		}

		ret = fscanf(f, "\n");
		STARPU_ASSERT(ret == 0);
	}
#endif /* !STARPU_USE_OPENCL */
	_starpu_frdunlock(f);

	fclose(f);
#endif /* !(STARPU_USE_CUDA_ || STARPU_USE_OPENCL */

}

#ifndef STARPU_SIMGRID
static void write_bus_affinity_file_content(void)
{
	STARPU_ASSERT(was_benchmarked);

#if defined(STARPU_USE_CUDA) || defined(STARPU_USE_OPENCL)
	FILE *f;
	char path[256];
	get_affinity_path(path, sizeof(path));

	_STARPU_DEBUG("writing affinities to %s\n", path);

	f = fopen(path, "w+");
	if (!f)
	{
		perror("fopen write_buf_affinity_file_content");
		_STARPU_DISP("path '%s'\n", path);
		fflush(stderr);
		STARPU_ABORT();
	}

	_starpu_frdlock(f);
	unsigned cpu;
        unsigned gpu;

        fprintf(f, "# GPU\t");
	for (cpu = 0; cpu < ncpus; cpu++)
		fprintf(f, "CPU%u\t", cpu);
	fprintf(f, "\n");

#ifdef STARPU_USE_CUDA
	for (gpu = 0; gpu < ncuda; gpu++)
	{
		fprintf(f, "%d\t", gpu);

		for (cpu = 0; cpu < ncpus; cpu++)
		{
			fprintf(f, "%d\t", cudadev_timing_per_cpu[(gpu+1)*STARPU_MAXCPUS+cpu].cpu_id);
		}

		fprintf(f, "\n");
	}
#endif
#ifdef STARPU_USE_OPENCL
	for (gpu = 0; gpu < nopencl; gpu++)
	{
		fprintf(f, "%d\t", gpu);

		for (cpu = 0; cpu < ncpus; cpu++)
		{
                        fprintf(f, "%d\t", opencldev_timing_per_cpu[(gpu+1)*STARPU_MAXCPUS+cpu].cpu_id);
		}

		fprintf(f, "\n");
	}
#endif

	_starpu_frdunlock(f);
	fclose(f);
#endif
}
#endif /* STARPU_SIMGRID */

static void generate_bus_affinity_file(void)
{
	if (!was_benchmarked)
		benchmark_all_gpu_devices();

	write_bus_affinity_file_content();
}

static void load_bus_affinity_file(void)
{
	int res;

	char path[256];
	get_affinity_path(path, sizeof(path));

	res = access(path, F_OK);
	if (res)
	{
		/* File does not exist yet */
		generate_bus_affinity_file();
	}

	load_bus_affinity_file_content();
}

#ifdef STARPU_USE_CUDA
int *_starpu_get_cuda_affinity_vector(unsigned gpuid)
{
        return cuda_affinity_matrix[gpuid];
}
#endif /* STARPU_USE_CUDA */

#ifdef STARPU_USE_OPENCL
int *_starpu_get_opencl_affinity_vector(unsigned gpuid)
{
        return opencl_affinity_matrix[gpuid];
}
#endif /* STARPU_USE_OPENCL */

void starpu_bus_print_affinity(FILE *f)
{
#if defined(STARPU_USE_CUDA) || defined(STARPU_USE_OPENCL)
	unsigned cpu;
	unsigned gpu;
#endif

	fprintf(f, "# GPU\tCPU in preference order (logical index)\n");

#ifdef STARPU_USE_CUDA
	fprintf(f, "# CUDA\n");
	for(gpu = 0 ; gpu<ncuda ; gpu++)
	{
		fprintf(f, "%d\t", gpu);
		for (cpu = 0; cpu < ncpus; cpu++)
		{
			fprintf(f, "%d\t", cuda_affinity_matrix[gpu][cpu]);
		}
		fprintf(f, "\n");
	}
#endif
#ifdef STARPU_USE_OPENCL
	fprintf(f, "# OpenCL\n");
	for(gpu = 0 ; gpu<nopencl ; gpu++)
	{
		fprintf(f, "%d\t", gpu);
		for (cpu = 0; cpu < ncpus; cpu++)
		{
			fprintf(f, "%d\t", opencl_affinity_matrix[gpu][cpu]);
		}
		fprintf(f, "\n");
	}
#endif
}
#endif /* STARPU_SIMGRID */

/*
 *	Latency
 */

static void get_latency_path(char *path, size_t maxlen)
{
	get_bus_path("latency", path, maxlen);
}

static int load_bus_latency_file_content(void)
{
	int n;
	unsigned src, dst;
	FILE *f;
	double latency;

	char path[256];
	get_latency_path(path, sizeof(path));

	_STARPU_DEBUG("loading latencies from %s\n", path);

	f = fopen(path, "r");
	if (!f)
	{
		perror("fopen load_bus_latency_file_content");
		_STARPU_DISP("path '%s'\n", path);
		fflush(stderr);
		STARPU_ABORT();
	}
	_starpu_frdlock(f);

	for (src = 0; src < STARPU_MAXNODES; src++)
	{
		_starpu_drop_comments(f);
		for (dst = 0; dst < STARPU_MAXNODES; dst++)
		{
			n = _starpu_read_double(f, "%le", &latency);
			if (n != 1)
			{
				_STARPU_DISP("Error while reading latency file <%s>. Expected a number\n", path);
				fclose(f);
				return 0;
			}
			n = getc(f);
			if (n == '\n')
				break;
			if (n != '\t')
			{
				_STARPU_DISP("bogus character %c in latency file %s\n", n, path);
				fclose(f);
				return 0;
			}

			latency_matrix[src][dst] = latency;

			/* Look out for \t\n */
			n = getc(f);
			if (n == '\n')
				break;
			ungetc(n, f);
			n = '\t';
		}

		/* No more values, take NAN */
		for ( ; dst < STARPU_MAXNODES; dst++)
			latency_matrix[src][dst] = NAN;

		while (n == '\t')
		{
			/* Look out for \t\n */
			n = getc(f);
			if (n == '\n')
				break;
			ungetc(n, f);

			n = _starpu_read_double(f, "%le", &latency);
			if (n && !isnan(latency))
			{
				_STARPU_DISP("Too many nodes in latency file %s for this configuration (%d), use --enable-maxnodes to increase it\n", path, STARPU_MAXNODES);
				fclose(f);
				return 0;
			}
			n = getc(f);
		}
		if (n != '\n')
		{
			_STARPU_DISP("Bogus character %c in latency file %s\n", n, path);
			fclose(f);
			return 0;
		}

		/* Look out for EOF */
		n = getc(f);
		if (n == EOF)
			break;
		ungetc(n, f);
	}
	_starpu_frdunlock(f);
	fclose(f);

	/* No more values, take NAN */
	for ( ; src < STARPU_MAXNODES; src++)
		for (dst = 0; dst < STARPU_MAXNODES; dst++)
			latency_matrix[src][dst] = NAN;

	return 1;
}

#ifndef STARPU_SIMGRID
static void write_bus_latency_file_content(void)
{
        unsigned src, dst, maxnode;
	FILE *f;

	STARPU_ASSERT(was_benchmarked);

	char path[256];
	get_latency_path(path, sizeof(path));

	_STARPU_DEBUG("writing latencies to %s\n", path);

	f = fopen(path, "w+");
	if (!f)
	{
		perror("fopen write_bus_latency_file_content");
		_STARPU_DISP("path '%s'\n", path);
		fflush(stderr);
		STARPU_ABORT();
	}
	_starpu_fwrlock(f);
	_starpu_fftruncate(f, 0);

	fprintf(f, "# ");
	for (dst = 0; dst < STARPU_MAXNODES; dst++)
		fprintf(f, "to %d\t\t", dst);
	fprintf(f, "\n");

        maxnode = ncuda;
#ifdef STARPU_USE_OPENCL
        maxnode += nopencl;
#endif
        for (src = 0; src < STARPU_MAXNODES; src++)
	{
		for (dst = 0; dst < STARPU_MAXNODES; dst++)
		{
			double latency = 0.0;

			if ((src > maxnode) || (dst > maxnode))
			{
				/* convention */
				latency = NAN;
			}
			else if (src == dst)
			{
				latency = 0.0;
			}
			else
			{
				/* µs */
#ifdef STARPU_USE_CUDA
#ifdef HAVE_CUDA_MEMCPY_PEER
				if (src && src < ncuda && dst && dst <= ncuda)
					latency = cudadev_latency_dtod[src][dst];
				else
#endif
				{
					if (src && src <= ncuda)
						latency += cudadev_latency_dtoh[src];
					if (dst && dst <= ncuda)
						latency += cudadev_latency_htod[dst];
				}
#endif
#ifdef STARPU_USE_OPENCL
				if (src > ncuda)
					latency += opencldev_latency_dtoh[src-ncuda];
				if (dst > ncuda)
					latency += opencldev_latency_htod[dst-ncuda];
#endif
			}

			if (dst)
				fputc('\t', f);
			_starpu_write_double(f, "%e", latency);
		}

		fprintf(f, "\n");
	}
	_starpu_fwrunlock(f);

	fclose(f);
}
#endif

static void generate_bus_latency_file(void)
{
	if (!was_benchmarked)
		benchmark_all_gpu_devices();

#ifndef STARPU_SIMGRID
	write_bus_latency_file_content();
#endif
}

static void load_bus_latency_file(void)
{
	int res;

	char path[256];
	get_latency_path(path, sizeof(path));

	res = access(path, F_OK);
	if (res || !load_bus_latency_file_content())
	{
		/* File does not exist yet or is bogus */
		generate_bus_latency_file();
	}

}


/*
 *	Bandwidth
 */
static void get_bandwidth_path(char *path, size_t maxlen)
{
	get_bus_path("bandwidth", path, maxlen);
}

static int load_bus_bandwidth_file_content(void)
{
	int n;
	unsigned src, dst;
	FILE *f;
	double bandwidth;

	char path[256];
	get_bandwidth_path(path, sizeof(path));

	_STARPU_DEBUG("loading bandwidth from %s\n", path);

	f = fopen(path, "r");
	if (!f)
	{
		perror("fopen load_bus_bandwidth_file_content");
		_STARPU_DISP("path '%s'\n", path);
		fflush(stderr);
		STARPU_ABORT();
	}
	_starpu_frdlock(f);

	for (src = 0; src < STARPU_MAXNODES; src++)
	{
		_starpu_drop_comments(f);
		for (dst = 0; dst < STARPU_MAXNODES; dst++)
		{
			n = _starpu_read_double(f, "%le", &bandwidth);
			if (n != 1)
			{
				_STARPU_DISP("Error while reading bandwidth file <%s>. Expected a number\n", path);
				fclose(f);
				return 0;
			}
			n = getc(f);
			if (n == '\n')
				break;
			if (n != '\t')
			{
				_STARPU_DISP("bogus character %c in bandwidth file %s\n", n, path);
				fclose(f);
				return 0;
			}

			bandwidth_matrix[src][dst] = bandwidth;

			/* Look out for \t\n */
			n = getc(f);
			if (n == '\n')
				break;
			ungetc(n, f);
			n = '\t';
		}

		/* No more values, take NAN */
		for ( ; dst < STARPU_MAXNODES; dst++)
			bandwidth_matrix[src][dst] = NAN;

		while (n == '\t')
		{
			/* Look out for \t\n */
			n = getc(f);
			if (n == '\n')
				break;
			ungetc(n, f);

			n = _starpu_read_double(f, "%le", &bandwidth);
			if (n && !isnan(bandwidth))
			{
				_STARPU_DISP("Too many nodes in bandwidth file %s for this configuration (%d), use --enable-maxnodes to increase it\n", path, STARPU_MAXNODES);
				fclose(f);
				return 0;
			}
			n = getc(f);
		}
		if (n != '\n')
		{
			_STARPU_DISP("Bogus character %c in bandwidth file %s\n", n, path);
			fclose(f);
			return 0;
		}

		/* Look out for EOF */
		n = getc(f);
		if (n == EOF)
			break;
		ungetc(n, f);
	}
	_starpu_frdunlock(f);
	fclose(f);

	/* No more values, take NAN */
	for ( ; src < STARPU_MAXNODES; src++)
		for (dst = 0; dst < STARPU_MAXNODES; dst++)
			latency_matrix[src][dst] = NAN;

	return 1;
}

#ifndef STARPU_SIMGRID
static void write_bus_bandwidth_file_content(void)
{
	unsigned src, dst, maxnode;
	FILE *f;

	STARPU_ASSERT(was_benchmarked);

	char path[256];
	get_bandwidth_path(path, sizeof(path));

	_STARPU_DEBUG("writing bandwidth to %s\n", path);

	f = fopen(path, "w+");
	STARPU_ASSERT(f);

	_starpu_fwrlock(f);
	_starpu_fftruncate(f, 0);

	fprintf(f, "# ");
	for (dst = 0; dst < STARPU_MAXNODES; dst++)
		fprintf(f, "to %d\t\t", dst);
	fprintf(f, "\n");

        maxnode = ncuda;
#ifdef STARPU_USE_OPENCL
        maxnode += nopencl;
#endif
	for (src = 0; src < STARPU_MAXNODES; src++)
	{
		for (dst = 0; dst < STARPU_MAXNODES; dst++)
		{
			double bandwidth;

			if ((src > maxnode) || (dst > maxnode))
			{
				bandwidth = NAN;
			}
#if defined(STARPU_USE_CUDA) || defined(STARPU_USE_OPENCL)
			else if (src != dst)
			{
				double slowness = 0.0;
				/* Total bandwidth is the harmonic mean of bandwidths */
#ifdef STARPU_USE_CUDA
#ifdef HAVE_CUDA_MEMCPY_PEER
				if (src && src <= ncuda && dst && dst <= ncuda)
					/* Direct GPU-GPU transfert */
					slowness = cudadev_timing_dtod[src][dst];
				else
#endif
				{
					if (src && src <= ncuda)
						slowness += cudadev_timing_dtoh[src];
					if (dst && dst <= ncuda)
						slowness += cudadev_timing_htod[dst];
				}
#endif
#ifdef STARPU_USE_OPENCL
				if (src > ncuda)
					slowness += opencldev_timing_dtoh[src-ncuda];
				if (dst > ncuda)
					slowness += opencldev_timing_htod[dst-ncuda];
#endif
				bandwidth = 1.0/slowness;
			}
#endif
			else
			{
			        /* convention */
			        bandwidth = 0.0;
			}

			if (dst)
				fputc('\t', f);
			_starpu_write_double(f, "%e", bandwidth);
		}

		fprintf(f, "\n");
	}

	_starpu_fwrunlock(f);
	fclose(f);
}
#endif /* STARPU_SIMGRID */

void starpu_bus_print_bandwidth(FILE *f)
{
	unsigned src, dst, maxnode;

        maxnode = ncuda;
#ifdef STARPU_USE_OPENCL
        maxnode += nopencl;
#endif

	fprintf(f, "from/to\t");
	fprintf(f, "RAM\t");
	for (dst = 0; dst < ncuda; dst++)
		fprintf(f, "CUDA %d\t", dst);
	for (dst = 0; dst < nopencl; dst++)
		fprintf(f, "OpenCL%d\t", dst);
	fprintf(f, "\n");

	for (src = 0; src <= maxnode; src++)
	{
		if (!src)
			fprintf(f, "RAM\t");
		else if (src <= ncuda)
			fprintf(f, "CUDA %d\t", src-1);
		else
			fprintf(f, "OpenCL%d\t", src-ncuda-1);
		for (dst = 0; dst <= maxnode; dst++)
			fprintf(f, "%.0f\t", bandwidth_matrix[src][dst]);

		fprintf(f, "\n");
	}
	fprintf(f, "\n");

	for (src = 0; src <= maxnode; src++)
	{
		if (!src)
			fprintf(f, "RAM\t");
		else if (src <= ncuda)
			fprintf(f, "CUDA %d\t", src-1);
		else
			fprintf(f, "OpenCL%d\t", src-ncuda-1);
		for (dst = 0; dst <= maxnode; dst++)
			fprintf(f, "%.0f\t", latency_matrix[src][dst]);

		fprintf(f, "\n");
	}

#if defined(STARPU_USE_CUDA) || defined(STARPU_USE_OPENCL)
	if (ncuda != 0 || nopencl != 0)
		fprintf(f, "\nGPU\tCPU in preference order (logical index), host-to-device, device-to-host\n");
	for (src = 1; src <= maxnode; src++)
	{
		struct dev_timing *timing;
		struct _starpu_machine_config *config = _starpu_get_machine_config();
		unsigned config_ncpus = _starpu_topology_get_nhwcpu(config);
		unsigned cpu;

#ifdef STARPU_USE_CUDA
		if (src <= ncuda)
		{
			fprintf(f, "CUDA %d\t", src-1);
			for (cpu = 0; cpu < config_ncpus; cpu++)
			{
				timing = &cudadev_timing_per_cpu[src*STARPU_MAXCPUS+cpu];
				if (timing->timing_htod)
					fprintf(f, "%2d %.0f %.0f\t", timing->cpu_id, 1/timing->timing_htod, 1/timing->timing_dtoh);
				else
					fprintf(f, "%2d\t", cuda_affinity_matrix[src-1][cpu]);
			}
		}
#ifdef STARPU_USE_OPENCL
		else
#endif
#endif
#ifdef STARPU_USE_OPENCL
		{
			fprintf(f, "OpenCL%d\t", src-ncuda-1);
			for (cpu = 0; cpu < config_ncpus; cpu++)
			{
				timing = &opencldev_timing_per_cpu[(src-ncuda)*STARPU_MAXCPUS+cpu];
				if (timing->timing_htod)
					fprintf(f, "%2d %.0f %.0f\t", timing->cpu_id, 1/timing->timing_htod, 1/timing->timing_dtoh);
				else
					fprintf(f, "%2d\t", opencl_affinity_matrix[src-1][cpu]);
			}
		}
#endif
		fprintf(f, "\n");
	}
#endif
}

static void generate_bus_bandwidth_file(void)
{
	if (!was_benchmarked)
		benchmark_all_gpu_devices();

#ifndef STARPU_SIMGRID
	write_bus_bandwidth_file_content();
#endif
}

static void load_bus_bandwidth_file(void)
{
	int res;

	char path[256];
	get_bandwidth_path(path, sizeof(path));

	res = access(path, F_OK);
	if (res || !load_bus_bandwidth_file_content())
	{
		/* File does not exist yet or is bogus */
		generate_bus_bandwidth_file();
	}
}

#ifndef STARPU_SIMGRID
/*
 *	Config
 */
static void get_config_path(char *path, size_t maxlen)
{
	get_bus_path("config", path, maxlen);
}

static void check_bus_config_file(void)
{
        int res;
        char path[256];
        struct _starpu_machine_config *config = _starpu_get_machine_config();

        get_config_path(path, sizeof(path));
        res = access(path, F_OK);
	if (res || config->conf.bus_calibrate > 0)
	{
		if (res)
			_STARPU_DISP("No performance model for the bus, calibrating...\n");
		_starpu_bus_force_sampling();
		if (res)
			_STARPU_DISP("... done\n");
        }
        else
	{
                FILE *f;
                int ret;
		unsigned read_cuda = -1, read_opencl = -1;
                unsigned read_cpus = -1;

                // Loading configuration from file
                f = fopen(path, "r");
                STARPU_ASSERT(f);
		_starpu_frdlock(f);
                _starpu_drop_comments(f);
                ret = fscanf(f, "%u\t", &read_cpus);
		STARPU_ASSERT(ret == 1);
                _starpu_drop_comments(f);
		ret = fscanf(f, "%d\t", &read_cuda);
		STARPU_ASSERT(ret == 1);
                _starpu_drop_comments(f);
		ret = fscanf(f, "%d\t", &read_opencl);
		STARPU_ASSERT(ret == 1);
                _starpu_drop_comments(f);
		_starpu_frdunlock(f);
                fclose(f);

                // Loading current configuration
                ncpus = _starpu_topology_get_nhwcpu(config);
#ifdef STARPU_USE_CUDA
		ncuda = _starpu_get_cuda_device_count();
#endif
#ifdef STARPU_USE_OPENCL
                nopencl = _starpu_opencl_get_device_count();
#endif

                // Checking if both configurations match
                if (read_cpus != ncpus)
		{
			_STARPU_DISP("Current configuration does not match the bus performance model (CPUS: (stored) %u != (current) %u), recalibrating...\n", read_cpus, ncpus);
                        _starpu_bus_force_sampling();
			_STARPU_DISP("... done\n");
                }
                else if (read_cuda != ncuda)
		{
                        _STARPU_DISP("Current configuration does not match the bus performance model (CUDA: (stored) %d != (current) %d), recalibrating...\n", read_cuda, ncuda);
                        _starpu_bus_force_sampling();
			_STARPU_DISP("... done\n");
                }
                else if (read_opencl != nopencl)
		{
                        _STARPU_DISP("Current configuration does not match the bus performance model (OpenCL: (stored) %d != (current) %d), recalibrating...\n", read_opencl, nopencl);
                        _starpu_bus_force_sampling();
			_STARPU_DISP("... done\n");
                }
        }
}

static void write_bus_config_file_content(void)
{
	FILE *f;
	char path[256];

	STARPU_ASSERT(was_benchmarked);
        get_config_path(path, sizeof(path));

	_STARPU_DEBUG("writing config to %s\n", path);

        f = fopen(path, "w+");
	STARPU_ASSERT(f);
	_starpu_fwrlock(f);
	_starpu_fftruncate(f, 0);

        fprintf(f, "# Current configuration\n");
        fprintf(f, "%u # Number of CPUs\n", ncpus);
        fprintf(f, "%d # Number of CUDA devices\n", ncuda);
        fprintf(f, "%d # Number of OpenCL devices\n", nopencl);

	_starpu_fwrunlock(f);
        fclose(f);
}

static void generate_bus_config_file(void)
{
	if (!was_benchmarked)
		benchmark_all_gpu_devices();

	write_bus_config_file_content();
}
#endif /* !SIMGRID */

void _starpu_simgrid_get_platform_path(int version, char *path, size_t maxlen)
{
	if (version == 3)
		get_bus_path("platform.xml", path, maxlen);
	else
		get_bus_path("platform.v4.xml", path, maxlen);
}

#ifndef STARPU_SIMGRID
static void write_bus_platform_file_content(int version)
{
	FILE *f;
	char path[256];
	unsigned i;
	const char *speed, *flops, *Bps, *s;
	char dash;

	if (version == 3)
	{
		speed = "power";
		flops = "";
		Bps = "";
		s = "";
		dash = '_';
	}
	else
	{
		speed = "speed";
		flops = "f";
		Bps = "Bps";
		s = "s";
		dash = '-';
	}

	STARPU_ASSERT(was_benchmarked);

	_starpu_simgrid_get_platform_path(version, path, sizeof(path));

	_STARPU_DEBUG("writing platform to %s\n", path);

	f = fopen(path, "w+");
	if (!f)
	{
		perror("fopen write_bus_platform_file_content");
		_STARPU_DISP("path '%s'\n", path);
		fflush(stderr);
		STARPU_ABORT();
	}
	_starpu_fwrlock(f);
	_starpu_fftruncate(f, 0);

	fprintf(f,
"<?xml version='1.0'?>\n"
"<!DOCTYPE platform SYSTEM '%s'>\n"
" <platform version=\"%u\">\n"
" <config id=\"General\">\n"
"   <prop id=\"network/TCP%cgamma\" value=\"-1\"></prop>\n"
"   <prop id=\"network/latency%cfactor\" value=\"1\"></prop>\n"
"   <prop id=\"network/bandwidth%cfactor\" value=\"1\"></prop>\n"
" </config>\n"
" <AS  id=\"AS0\"  routing=\"Full\">\n"
"   <host id=\"MAIN\" %s=\"1%s\"/>\n",
		version == 3
			? "http://simgrid.gforge.inria.fr/simgrid.dtd"
			: "http://simgrid.gforge.inria.fr/simgrid/simgrid.dtd",
		version, dash, dash, dash, speed, flops);

	for (i = 0; i < ncpus; i++)
		/* TODO: host memory for out-of-core simulation */
		fprintf(f, "   <host id=\"CPU%d\" %s=\"2000000000%s\"/>\n", i, speed, flops);

#ifdef STARPU_USE_CUDA
	for (i = 0; i < ncuda; i++)
	{
		fprintf(f, "   <host id=\"CUDA%d\" %s=\"2000000000%s\">\n", i, speed, flops);
		fprintf(f, "     <prop id=\"memsize\" value=\"%llu\"/>\n", (unsigned long long) cuda_size[i]);
#ifdef HAVE_CUDA_MEMCPY_PEER
		fprintf(f, "     <prop id=\"memcpy_peer\" value=\"1\"/>\n");
#endif
		fprintf(f, "   </host>\n");
	}
#endif

#ifdef STARPU_USE_OPENCL
	for (i = 0; i < nopencl; i++)
	{
		fprintf(f, "   <host id=\"OpenCL%d\" %s=\"2000000000%s\">\n", i, speed, flops);
		fprintf(f, "     <prop id=\"memsize\" value=\"%llu\"/>\n", (unsigned long long) opencl_size[i]);;
		fprintf(f, "   </host>\n");
	}
#endif

	fprintf(f, "\n   <host id=\"RAM\" %s=\"1%s\"/>\n", speed, flops);

	/* Compute maximum bandwidth, taken as machine bandwidth */
	double max_bandwidth = 0;
#ifdef STARPU_USE_CUDA
	for (i = 0; i < ncuda; i++)
	{
		double down_bw = 1.0 / cudadev_timing_dtoh[1+i];
		double up_bw = 1.0 / cudadev_timing_htod[1+i];
		if (max_bandwidth < down_bw)
			max_bandwidth = down_bw;
		if (max_bandwidth < up_bw)
			max_bandwidth = up_bw;
	}
#endif
#ifdef STARPU_USE_OPENCL
	for (i = 0; i < nopencl; i++)
	{
		double down_bw = 1.0 / opencldev_timing_dtoh[1+i];
		double up_bw = 1.0 / opencldev_timing_htod[1+i];
		if (max_bandwidth < down_bw)
			max_bandwidth = down_bw;
		if (max_bandwidth < up_bw)
			max_bandwidth = up_bw;
	}
#endif
	fprintf(f, "\n   <link id=\"Share\" bandwidth=\"%f%s\" latency=\"0.000000%s\"/>\n\n", max_bandwidth*1000000, Bps, s);

	/* Write bandwidths & latencies */
#ifdef STARPU_USE_CUDA
	for (i = 0; i < ncuda; i++)
	{
		char i_name[16];
		snprintf(i_name, sizeof(i_name), "CUDA%d", i);
		fprintf(f, "   <link id=\"RAM-%s\" bandwidth=\"%f%s\" latency=\"%f%s\"/>\n",
			i_name,
			1000000. / cudadev_timing_htod[1+i], Bps,
			cudadev_latency_htod[1+i]/1000000., s);
		fprintf(f, "   <link id=\"%s-RAM\" bandwidth=\"%f%s\" latency=\"%f%s\"/>\n",
			i_name,
			1000000. / cudadev_timing_dtoh[1+i], Bps,
			cudadev_latency_dtoh[1+i]/1000000., s);
	}
#ifdef HAVE_CUDA_MEMCPY_PEER
	for (i = 0; i < ncuda; i++)
	{
		unsigned j;
		char i_name[16];
		snprintf(i_name, sizeof(i_name), "CUDA%d", i);
		for (j = 0; j < ncuda; j++)
		{
			char j_name[16];
			if (j == i)
				continue;
			snprintf(j_name, sizeof(j_name), "CUDA%d", j);
			fprintf(f, "   <link id=\"%s-%s\" bandwidth=\"%f%s\" latency=\"%f%s\"/>\n",
				i_name, j_name,
				1000000. / cudadev_timing_dtod[1+i][1+j], Bps,
				cudadev_latency_dtod[1+i][1+j]/1000000., s);
		}
	}
#endif
#endif

#ifdef STARPU_USE_OPENCL
	for (i = 0; i < nopencl; i++)
	{
		char i_name[16];
		snprintf(i_name, sizeof(i_name), "OpenCL%d", i);
		fprintf(f, "   <link id=\"RAM-%s\" bandwidth=\"%f%s\" latency=\"%f%s\"/>\n",
			i_name,
			1000000 / opencldev_timing_htod[1+i], Bps,
			opencldev_latency_htod[1+i]/1000000., s);
		fprintf(f, "   <link id=\"%s-RAM\" bandwidth=\"%f%s\" latency=\"%f%s\"/>\n",
			i_name,
			1000000 / opencldev_timing_dtoh[1+i], Bps,
			opencldev_latency_dtoh[1+i]/1000000., s);
	}
#endif

	/* Write routes */
#ifdef STARPU_USE_CUDA
	for (i = 0; i < ncuda; i++)
	{
		char i_name[16];
		snprintf(i_name, sizeof(i_name), "CUDA%d", i);
		fprintf(f, "   <route src=\"RAM\" dst=\"%s\" symmetrical=\"NO\"><link_ctn id=\"RAM-%s\"/><link_ctn id=\"Share\"/></route>\n", i_name, i_name);
		fprintf(f, "   <route src=\"%s\" dst=\"RAM\" symmetrical=\"NO\"><link_ctn id=\"%s-RAM\"/><link_ctn id=\"Share\"/></route>\n", i_name, i_name);
	}
#ifdef HAVE_CUDA_MEMCPY_PEER
	for (i = 0; i < ncuda; i++)
	{
		unsigned j;
		char i_name[16];
		snprintf(i_name, sizeof(i_name), "CUDA%d", i);
		for (j = 0; j < ncuda; j++)
		{
			char j_name[16];
			if (j == i)
				continue;
			snprintf(j_name, sizeof(j_name), "CUDA%d", j);
			fprintf(f, "   <route src=\"%s\" dst=\"%s\" symmetrical=\"NO\"><link_ctn id=\"%s-%s\"/><link_ctn id=\"Share\"/></route>\n", i_name, j_name, i_name, j_name);
		}
	}
#endif
#endif

#ifdef STARPU_USE_OPENCL
	for (i = 0; i < nopencl; i++)
	{
		char i_name[16];
		snprintf(i_name, sizeof(i_name), "OpenCL%d", i);
		fprintf(f, "   <route src=\"RAM\" dst=\"%s\" symmetrical=\"NO\"><link_ctn id=\"RAM-%s\"/><link_ctn id=\"Share\"/></route>\n", i_name, i_name);
		fprintf(f, "   <route src=\"%s\" dst=\"RAM\" symmetrical=\"NO\"><link_ctn id=\"%s-RAM\"/><link_ctn id=\"Share\"/></route>\n", i_name, i_name);
	}
#endif

	fprintf(f,
" </AS>\n"
" </platform>\n"
		);

	_starpu_fwrunlock(f);
	fclose(f);
}

static void generate_bus_platform_file(void)
{
	if (!was_benchmarked)
		benchmark_all_gpu_devices();

	write_bus_platform_file_content(3);
	write_bus_platform_file_content(4);
}

static void check_bus_platform_file(void)
{
	int res;

	char path[256];
	_starpu_simgrid_get_platform_path(4, path, sizeof(path));

	res = access(path, F_OK);

	if (!res)
	{
		_starpu_simgrid_get_platform_path(3, path, sizeof(path));
		res = access(path, F_OK);
	}

	if (res)
	{
		/* File does not exist yet */
		generate_bus_platform_file();
	}
}

/*
 *	Generic
 */

static void _starpu_bus_force_sampling(void)
{
	_STARPU_DEBUG("Force bus sampling ...\n");
	_starpu_create_sampling_directory_if_needed();

	generate_bus_affinity_file();
	generate_bus_latency_file();
	generate_bus_bandwidth_file();
	generate_bus_config_file();
	generate_bus_platform_file();
}
#endif /* !SIMGRID */

void _starpu_load_bus_performance_files(void)
{
	_starpu_create_sampling_directory_if_needed();

#if defined(STARPU_USE_CUDA) || defined(STARPU_USE_SIMGRID)
	ncuda = _starpu_get_cuda_device_count();
#endif
#if defined(STARPU_USE_OPENCL) || defined(STARPU_USE_SIMGRID)
	nopencl = _starpu_opencl_get_device_count();
#endif

#ifndef STARPU_SIMGRID
        check_bus_config_file();
	load_bus_affinity_file();
#endif
	load_bus_latency_file();
	load_bus_bandwidth_file();
#ifndef STARPU_SIMGRID
	check_bus_platform_file();
#endif
}

/* (in MB/s) */
double starpu_transfer_bandwidth(unsigned src_node, unsigned dst_node)
{
	return bandwidth_matrix[src_node][dst_node];
}

/* (in µs) */
double starpu_transfer_latency(unsigned src_node, unsigned dst_node)
{
	return latency_matrix[src_node][dst_node];
}

/* (in µs) */
double starpu_transfer_predict(unsigned src_node, unsigned dst_node, size_t size)
{
	double bandwidth = bandwidth_matrix[src_node][dst_node];
	double latency = latency_matrix[src_node][dst_node];
	struct _starpu_machine_topology *topology = &_starpu_get_machine_config()->topology;

	return latency + (size/bandwidth)*2*(topology->ncudagpus+topology->nopenclgpus);
}
