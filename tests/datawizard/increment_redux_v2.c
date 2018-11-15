/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2011-2013  Université de Bordeaux
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

static unsigned var = 0;
static starpu_data_handle_t handle;

/*
 *	Reduction methods
 */

#ifdef STARPU_USE_CUDA
static void redux_cuda_kernel(void *descr[], void *arg)
{
	STARPU_SKIP_IF_VALGRIND;

	unsigned *dst = (unsigned *)STARPU_VARIABLE_GET_PTR(descr[0]);
	unsigned *src = (unsigned *)STARPU_VARIABLE_GET_PTR(descr[1]);

	unsigned host_dst, host_src;

	/* This is a dummy technique of course */
	cudaMemcpyAsync(&host_src, src, sizeof(unsigned), cudaMemcpyDeviceToHost, starpu_cuda_get_local_stream());
	cudaMemcpyAsync(&host_dst, dst, sizeof(unsigned), cudaMemcpyDeviceToHost, starpu_cuda_get_local_stream());
	cudaStreamSynchronize(starpu_cuda_get_local_stream());

	host_dst += host_src;

	cudaMemcpyAsync(dst, &host_dst, sizeof(unsigned), cudaMemcpyHostToDevice, starpu_cuda_get_local_stream());
	cudaStreamSynchronize(starpu_cuda_get_local_stream());
}

static void neutral_cuda_kernel(void *descr[], void *arg)
{
	STARPU_SKIP_IF_VALGRIND;

	unsigned *dst = (unsigned *)STARPU_VARIABLE_GET_PTR(descr[0]);

	/* This is a dummy technique of course */
	unsigned host_dst = 0;
	cudaMemcpyAsync(dst, &host_dst, sizeof(unsigned), cudaMemcpyHostToDevice, starpu_cuda_get_local_stream());
	cudaStreamSynchronize(starpu_cuda_get_local_stream());
}
#endif

#ifdef STARPU_USE_OPENCL
static void redux_opencl_kernel(void *descr[], void *arg)
{
	STARPU_SKIP_IF_VALGRIND;

	unsigned h_dst, h_src;

	cl_mem d_dst = (cl_mem)STARPU_VARIABLE_GET_PTR(descr[0]);
	cl_mem d_src = (cl_mem)STARPU_VARIABLE_GET_PTR(descr[1]);

	cl_command_queue queue;
	starpu_opencl_get_current_queue(&queue);

	/* This is a dummy technique of course */
	clEnqueueReadBuffer(queue, d_dst, CL_TRUE, 0, sizeof(unsigned), (void *)&h_dst, 0, NULL, NULL);
	clEnqueueReadBuffer(queue, d_src, CL_TRUE, 0, sizeof(unsigned), (void *)&h_src, 0, NULL, NULL);

	h_dst += h_src;

	clEnqueueWriteBuffer(queue, d_dst, CL_TRUE, 0, sizeof(unsigned), (void *)&h_dst, 0, NULL, NULL);
	clFinish(queue);
}

static void neutral_opencl_kernel(void *descr[], void *arg)
{
	STARPU_SKIP_IF_VALGRIND;

	unsigned h_dst = 0;
	cl_mem d_dst = (cl_mem)STARPU_VARIABLE_GET_PTR(descr[0]);

	cl_command_queue queue;
	starpu_opencl_get_current_queue(&queue);

	clEnqueueWriteBuffer(queue, d_dst, CL_TRUE, 0, sizeof(unsigned), (void *)&h_dst, 0, NULL, NULL);
	clFinish(queue);
}
#endif



static void redux_cpu_kernel(void *descr[], void *arg)
{
	STARPU_SKIP_IF_VALGRIND;

	unsigned *dst = (unsigned *)STARPU_VARIABLE_GET_PTR(descr[0]);
	unsigned *src = (unsigned *)STARPU_VARIABLE_GET_PTR(descr[1]);
	*dst = *dst + *src;
}

static void neutral_cpu_kernel(void *descr[], void *arg)
{
	STARPU_SKIP_IF_VALGRIND;

	unsigned *dst = (unsigned *)STARPU_VARIABLE_GET_PTR(descr[0]);
	*dst = 0;
}

static struct starpu_codelet redux_cl =
{
#ifdef STARPU_USE_CUDA
	.cuda_funcs = {redux_cuda_kernel},
#endif
#ifdef STARPU_USE_OPENCL
	.opencl_funcs = {redux_opencl_kernel},
#endif
	.cpu_funcs = {redux_cpu_kernel},
	.modes = {STARPU_RW, STARPU_R},
	.nbuffers = 2
};

static struct starpu_codelet neutral_cl =
{
#ifdef STARPU_USE_CUDA
	.cuda_funcs = {neutral_cuda_kernel},
#endif
#ifdef STARPU_USE_OPENCL
	.opencl_funcs = {neutral_opencl_kernel},
#endif
	.cpu_funcs = {neutral_cpu_kernel},
	.modes = {STARPU_W},
	.nbuffers = 1
};

/*
 *	Increment codelet
 */

#ifdef STARPU_USE_OPENCL
/* dummy OpenCL implementation */
static void increment_opencl_kernel(void *descr[], void *cl_arg STARPU_ATTRIBUTE_UNUSED)
{
	STARPU_SKIP_IF_VALGRIND;

	cl_mem d_token = (cl_mem)STARPU_VARIABLE_GET_PTR(descr[0]);
	unsigned h_token;

	cl_command_queue queue;
	starpu_opencl_get_current_queue(&queue);

	clEnqueueReadBuffer(queue, d_token, CL_TRUE, 0, sizeof(unsigned), (void *)&h_token, 0, NULL, NULL);
	h_token++;
	clEnqueueWriteBuffer(queue, d_token, CL_TRUE, 0, sizeof(unsigned), (void *)&h_token, 0, NULL, NULL);
	clFinish(queue);
}
#endif


#ifdef STARPU_USE_CUDA
static void increment_cuda_kernel(void *descr[], void *arg)
{
	STARPU_SKIP_IF_VALGRIND;

	unsigned *tokenptr = (unsigned *)STARPU_VARIABLE_GET_PTR(descr[0]);
	unsigned host_token;

	/* This is a dummy technique of course */
	cudaMemcpyAsync(&host_token, tokenptr, sizeof(unsigned), cudaMemcpyDeviceToHost, starpu_cuda_get_local_stream());
	cudaStreamSynchronize(starpu_cuda_get_local_stream());

	host_token++;

	cudaMemcpyAsync(tokenptr, &host_token, sizeof(unsigned), cudaMemcpyHostToDevice, starpu_cuda_get_local_stream());
	cudaStreamSynchronize(starpu_cuda_get_local_stream());
}
#endif

static void increment_cpu_kernel(void *descr[], void *arg)
{
	STARPU_SKIP_IF_VALGRIND;

	unsigned *tokenptr = (unsigned *)STARPU_VARIABLE_GET_PTR(descr[0]);
	*tokenptr = *tokenptr + 1;
}

static struct starpu_codelet increment_cl =
{
#ifdef STARPU_USE_CUDA
	.cuda_funcs = {increment_cuda_kernel},
#endif
#ifdef STARPU_USE_OPENCL
	.opencl_funcs = {increment_opencl_kernel},
#endif
	.cpu_funcs = {increment_cpu_kernel},
	.nbuffers = 1,
	.modes = {STARPU_RW}
};

static struct starpu_codelet increment_cl_redux =
{
#ifdef STARPU_USE_CUDA
	.cuda_funcs = {increment_cuda_kernel},
#endif
#ifdef STARPU_USE_OPENCL
	.opencl_funcs = {increment_opencl_kernel},
#endif
	.cpu_funcs = {increment_cpu_kernel},
	.nbuffers = 1,
	.modes = {STARPU_REDUX}
};

int main(int argc, char **argv)
{
	int ret;

	ret = starpu_init(NULL);
	if (ret == -ENODEV) return STARPU_TEST_SKIPPED;
	STARPU_CHECK_RETURN_VALUE(ret, "starpu_init");

	starpu_variable_data_register(&handle, 0, (uintptr_t)&var, sizeof(unsigned));

	starpu_data_set_reduction_methods(handle, &redux_cl, &neutral_cl);

#ifdef STARPU_QUICK_CHECK
	unsigned ntasks = 32;
	unsigned nloops = 4;
#else
	unsigned ntasks = 1024;
	unsigned nloops = 16;
#endif

	unsigned loop;
	unsigned t;

	for (loop = 0; loop < nloops; loop++)
	{
		for (t = 0; t < ntasks; t++)
		{
			struct starpu_task *task = starpu_task_create();

			if (t % 10 == 0)
			{
				task->cl = &increment_cl;
			}
			else
			{
				task->cl = &increment_cl_redux;
			}
			task->handles[0] = handle;

			ret = starpu_task_submit(task);
			if (ret == -ENODEV) goto enodev;
			STARPU_CHECK_RETURN_VALUE(ret, "starpu_task_submit");
		}

		ret = starpu_data_acquire(handle, STARPU_R);
		STARPU_CHECK_RETURN_VALUE(ret, "starpu_data_acquire");
		if (var != ntasks *(loop+1))
		{
			_STARPU_DEBUG("%u != %u\n", var, ntasks*(loop+1));
			starpu_data_release(handle);
			starpu_data_unregister(handle);
			goto err;
		}
		starpu_data_release(handle);
	}

	starpu_data_unregister(handle);
	if (var != ntasks *nloops)
	{
		_STARPU_DEBUG("%u != %u\n", var, ntasks*nloops);
		goto err;
	}

	starpu_shutdown();

	return EXIT_SUCCESS;

enodev:
	starpu_data_unregister(handle);
	fprintf(stderr, "WARNING: No one can execute this task\n");
	/* yes, we do not perform the computation but we did detect that no one
 	 * could perform the kernel, so this is not an error from StarPU */
	starpu_shutdown();
	return STARPU_TEST_SKIPPED;

err:
	starpu_shutdown();
	STARPU_RETURN(EXIT_FAILURE);
}
