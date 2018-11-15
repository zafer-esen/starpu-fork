/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2009, 2010, 2014  Université de Bordeaux
 * Copyright (C) 2010, 2011, 2012, 2013, 2014, 2015  Centre National de la Recherche Scientifique
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

#include <starpu_mpi.h>
#include "helper.h"

#ifdef STARPU_QUICK_CHECK
#  define NITER	32
#else
#  define NITER	2048
#endif

#ifdef STARPU_USE_CUDA
extern void increment_cuda(void *descr[], STARPU_ATTRIBUTE_UNUSED void *_args);
#endif

void increment_cpu(void *descr[], STARPU_ATTRIBUTE_UNUSED void *_args)
{
	int *tokenptr = (int *)STARPU_VECTOR_GET_PTR(descr[0]);
	(*tokenptr)++;
}

static struct starpu_codelet increment_cl =
{
#ifdef STARPU_USE_CUDA
	.cuda_funcs = {increment_cuda},
#endif
	.cpu_funcs = {increment_cpu},
	.nbuffers = 1,
	.modes = {STARPU_RW}
};

void increment_token(starpu_data_handle_t handle)
{
	struct starpu_task *task = starpu_task_create();

	task->cl = &increment_cl;
	task->handles[0] = handle;
	task->synchronous = 1;

	int ret = starpu_task_submit(task);
	STARPU_CHECK_RETURN_VALUE(ret, "starpu_task_submit");
}

static starpu_pthread_mutex_t mutex = STARPU_PTHREAD_MUTEX_INITIALIZER;
static starpu_pthread_cond_t cond = STARPU_PTHREAD_COND_INITIALIZER;

void callback(void *arg)
{
	unsigned *completed = arg;

	STARPU_PTHREAD_MUTEX_LOCK(&mutex);
	*completed = 1;
	STARPU_PTHREAD_COND_SIGNAL(&cond);
	STARPU_PTHREAD_MUTEX_UNLOCK(&mutex);
}

int main(int argc, char **argv)
{
	int ret, rank, size;
	int token = 42;
	starpu_data_handle_t token_handle;

	MPI_Init(&argc, &argv);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &size);

	if (size < 2)
	{
		if (rank == 0)
			FPRINTF(stderr, "We need at least 2 processes.\n");

		MPI_Finalize();
		return STARPU_TEST_SKIPPED;
	}

	ret = starpu_init(NULL);
	STARPU_CHECK_RETURN_VALUE(ret, "starpu_init");
	ret = starpu_mpi_init(NULL, NULL, 0);
	STARPU_CHECK_RETURN_VALUE(ret, "starpu_mpi_init");

	starpu_vector_data_register(&token_handle, 0, (uintptr_t)&token, 1, sizeof(token));

	int nloops = NITER;
	int loop;

	int last_loop = nloops - 1;
	int last_rank = size - 1;

	for (loop = 0; loop < nloops; loop++)
	{
		int tag = loop*size + rank;

		if (loop == 0 && rank == 0)
		{
			token = 0;
			FPRINTF_MPI(stderr, "Start with token value %d\n", token);
		}
		else
		{
			MPI_Status status;
			starpu_mpi_recv(token_handle, (rank+size-1)%size, tag, MPI_COMM_WORLD, &status);
		}

		increment_token(token_handle);

		if (loop == last_loop && rank == last_rank)
		{
			starpu_data_acquire(token_handle, STARPU_R);
			FPRINTF_MPI(stderr, "Finished : token value %d\n", token);
			starpu_data_release(token_handle);
		}
		else
		{
			int sent = 0;
			starpu_mpi_issend_detached(token_handle, (rank+1)%size, tag+1, MPI_COMM_WORLD, callback, &sent);

			STARPU_PTHREAD_MUTEX_LOCK(&mutex);
			while (!sent)
				STARPU_PTHREAD_COND_WAIT(&cond, &mutex);
			STARPU_PTHREAD_MUTEX_UNLOCK(&mutex);
		}
	}

	starpu_data_unregister(token_handle);
	starpu_mpi_shutdown();
	starpu_shutdown();

	FPRINTF_MPI(stderr, "Final value for token %d\n", token);
	MPI_Finalize();

	if (rank == last_rank)
	{
		STARPU_ASSERT(token == nloops*size);
	}


	return 0;
}
