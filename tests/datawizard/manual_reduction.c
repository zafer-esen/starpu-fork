/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2010, 2012  Université de Bordeaux
 * Copyright (C) 2012, 2013  Centre National de la Recherche Scientifique
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

#include <config.h>
#include <starpu.h>
#include "../helper.h"

#define INIT_VALUE	42
#define NTASKS		10000

static unsigned variable;
static starpu_data_handle_t variable_handle;

static uintptr_t per_worker[STARPU_NMAXWORKERS];
static starpu_data_handle_t per_worker_handle[STARPU_NMAXWORKERS];

/* Create per-worker handles */
static void initialize_per_worker_handle(void *arg STARPU_ATTRIBUTE_UNUSED)
{
	int workerid = starpu_worker_get_id();

	/* Allocate memory on the worker, and initialize it to 0 */
	switch (starpu_worker_get_type(workerid))
	{
		case STARPU_CPU_WORKER:
			per_worker[workerid] = (uintptr_t)calloc(1, sizeof(variable));
			break;
#ifdef STARPU_USE_OPENCL
		case STARPU_OPENCL_WORKER:
			{
			cl_context context;
			cl_command_queue queue;
			starpu_opencl_get_current_context(&context);
			starpu_opencl_get_current_queue(&queue);

			cl_mem ptr = clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(variable), NULL, NULL);
			/* Poor's man memset */
			unsigned zero = 0;
			clEnqueueWriteBuffer(queue, ptr, CL_FALSE, 0, sizeof(variable), (void *)&zero, 0, NULL, NULL);
			clFinish(queue);
			per_worker[workerid] = (uintptr_t)ptr;
			}

			break;
#endif
#ifdef STARPU_USE_CUDA
		case STARPU_CUDA_WORKER:
			cudaMalloc((void **)&per_worker[workerid], sizeof(variable));
			cudaMemset((void *)per_worker[workerid], 0, sizeof(variable));
			break;
#endif
		default:
			STARPU_ABORT();
			break;
	}

	STARPU_ASSERT(per_worker[workerid]);
}

/*
 *	Implement reduction method
 */

static void cpu_redux_func(void *descr[], void *cl_arg STARPU_ATTRIBUTE_UNUSED)
{
	unsigned *a = (unsigned *)STARPU_VARIABLE_GET_PTR(descr[0]);
	unsigned *b = (unsigned *)STARPU_VARIABLE_GET_PTR(descr[1]);

	FPRINTF(stderr, "%u = %u + %u\n", *a + *b, *a, *b);

	*a = *a + *b;
}

static struct starpu_codelet reduction_codelet =
{
	.cpu_funcs = {cpu_redux_func},
	.cuda_funcs = {NULL},
	.nbuffers = 2,
	.modes = {STARPU_RW, STARPU_R},
	.model = NULL
};

/*
 *	Use per-worker local copy
 */

static void cpu_func_incr(void *descr[], void *cl_arg STARPU_ATTRIBUTE_UNUSED)
{
	unsigned *val = (unsigned *)STARPU_VARIABLE_GET_PTR(descr[0]);
	*val = *val + 1;
}

#ifdef STARPU_USE_CUDA
/* dummy CUDA implementation */
static void cuda_func_incr(void *descr[], void *cl_arg STARPU_ATTRIBUTE_UNUSED)
{
	STARPU_SKIP_IF_VALGRIND;

	unsigned *val = (unsigned *)STARPU_VARIABLE_GET_PTR(descr[0]);

	unsigned h_val;
	cudaMemcpyAsync(&h_val, val, sizeof(unsigned), cudaMemcpyDeviceToHost, starpu_cuda_get_local_stream());
	cudaStreamSynchronize(starpu_cuda_get_local_stream());
	h_val++;
	cudaMemcpyAsync(val, &h_val, sizeof(unsigned), cudaMemcpyHostToDevice, starpu_cuda_get_local_stream());
	cudaStreamSynchronize(starpu_cuda_get_local_stream());
}
#endif

#ifdef STARPU_USE_OPENCL
/* dummy OpenCL implementation */
static void opencl_func_incr(void *descr[], void *cl_arg STARPU_ATTRIBUTE_UNUSED)
{
	STARPU_SKIP_IF_VALGRIND;

	cl_mem d_val = (cl_mem)STARPU_VARIABLE_GET_PTR(descr[0]);
	unsigned h_val;

	cl_command_queue queue;
	starpu_opencl_get_current_queue(&queue);

	clEnqueueReadBuffer(queue, d_val, CL_FALSE, 0, sizeof(unsigned), (void *)&h_val, 0, NULL, NULL);
	clFinish(queue);
	h_val++;
	clEnqueueWriteBuffer(queue, d_val, CL_FALSE, 0, sizeof(unsigned), (void *)&h_val, 0, NULL, NULL);
	clFinish(queue);
}
#endif

static struct starpu_codelet use_data_on_worker_codelet =
{
	.cpu_funcs = {cpu_func_incr},
#ifdef STARPU_USE_CUDA
	.cuda_funcs = {cuda_func_incr},
#endif
#ifdef STARPU_USE_OPENCL
	.opencl_funcs = {opencl_func_incr},
#endif
	.nbuffers = 1,
	.modes = {STARPU_RW},
	.model = NULL
};

int main(int argc, char **argv)
{
	unsigned worker;
	unsigned i;
	int ret;

	variable = INIT_VALUE;

        ret = starpu_init(NULL);
	if (ret == -ENODEV) return STARPU_TEST_SKIPPED;
	STARPU_CHECK_RETURN_VALUE(ret, "starpu_init");

	unsigned nworkers = starpu_worker_get_count();

	starpu_variable_data_register(&variable_handle, 0, (uintptr_t)&variable, sizeof(unsigned));

	/* Allocate a per-worker handle on each worker (and initialize it to 0) */
	starpu_execute_on_each_worker(initialize_per_worker_handle, NULL, STARPU_CPU|STARPU_CUDA|STARPU_OPENCL);

	/* Register all per-worker handles */
	for (worker = 0; worker < nworkers; worker++)
	{
		STARPU_ASSERT(per_worker[worker]);

		unsigned memory_node = starpu_worker_get_memory_node(worker);
		starpu_variable_data_register(&per_worker_handle[worker], memory_node,
						per_worker[worker], sizeof(variable));
	}

	/* Submit NTASKS tasks to the different worker to simulate the usage of a data in reduction */
	for (i = 0; i < NTASKS; i++)
	{
		struct starpu_task *task = starpu_task_create();
		task->cl = &use_data_on_worker_codelet;

		int workerid = (i % nworkers);
		task->handles[0] = per_worker_handle[workerid];

		task->execute_on_a_specific_worker = 1;
		task->workerid = (unsigned)workerid;

		ret = starpu_task_submit(task);
		if (ret == -ENODEV) goto enodev;
		STARPU_CHECK_RETURN_VALUE(ret, "starpu_task_submit");
	}


	/* Perform the reduction of all per-worker handles into the variable_handle */
	for (worker = 0; worker < nworkers; worker++)
	{
		struct starpu_task *task = starpu_task_create();
		task->cl = &reduction_codelet;

		task->handles[0] = variable_handle;
		task->handles[1] = per_worker_handle[worker];

		ret = starpu_task_submit(task);
		if (ret == -ENODEV) goto enodev;
		STARPU_CHECK_RETURN_VALUE(ret, "starpu_task_submit");
	}

	starpu_data_unregister(variable_handle);

	/* Destroy all per-worker handles */
	for (worker = 0; worker < nworkers; worker++)
	{
		starpu_data_unregister_no_coherency(per_worker_handle[worker]);
		switch(starpu_worker_get_type(worker))
		{
		case STARPU_CPU_WORKER:
			free((void*)per_worker[worker]);
			break;
#ifdef STARPU_USE_CUDA
		case STARPU_CUDA_WORKER:
			cudaFree((void*)per_worker[worker]);
			break;
#endif /* !STARPU_USE_CUDA */
#ifdef STARPU_USE_OPENCL
		case STARPU_OPENCL_WORKER:
			clReleaseMemObject((void*)per_worker[worker]);
			break;
#endif /* !STARPU_USE_OPENCL */
		default:
			STARPU_ABORT();
		}
	}

	starpu_shutdown();

	if (variable == INIT_VALUE + NTASKS)
		ret = EXIT_SUCCESS;
	else
		ret = EXIT_FAILURE;
	STARPU_RETURN(ret);

enodev:
	fprintf(stderr, "WARNING: No one can execute this task\n");
	starpu_task_wait_for_all();
	/* yes, we do not perform the computation but we did detect that no one
 	 * could perform the kernel, so this is not an error from StarPU */
	starpu_shutdown();
	return STARPU_TEST_SKIPPED;
}
