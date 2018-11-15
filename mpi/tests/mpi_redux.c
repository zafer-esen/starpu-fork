/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2013  Centre National de la Recherche Scientifique
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

static starpu_pthread_mutex_t mutex = STARPU_PTHREAD_MUTEX_INITIALIZER;
static starpu_pthread_cond_t cond = STARPU_PTHREAD_COND_INITIALIZER;

void callback(void *arg)
{
	unsigned *received = arg;

	STARPU_PTHREAD_MUTEX_LOCK(&mutex);
	*received = *received + 1;
	fprintf(stderr, "received = %d\n", *received);
	STARPU_PTHREAD_COND_SIGNAL(&cond);
	STARPU_PTHREAD_MUTEX_UNLOCK(&mutex);
}

int main(int argc, char **argv)
{
	int ret, rank, size;
	int value=0;
	starpu_data_handle_t *handles;

	MPI_Init(&argc, &argv);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &size);

	ret = starpu_init(NULL);
	STARPU_CHECK_RETURN_VALUE(ret, "starpu_init");
	ret = starpu_mpi_init(NULL, NULL, 0);
	STARPU_CHECK_RETURN_VALUE(ret, "starpu_mpi_init");

	if (rank == 0)
	{
		int src, sum;
		int received = 1;

		handles = malloc(size * sizeof(starpu_data_handle_t));

		for(src=1 ; src<size ; src++)
		{
			starpu_variable_data_register(&handles[src], -1, (uintptr_t)NULL, sizeof(int));
			starpu_mpi_irecv_detached(handles[src], src, 12, MPI_COMM_WORLD, callback, &received);
		}

		STARPU_PTHREAD_MUTEX_LOCK(&mutex);
		while (received != size)
			STARPU_PTHREAD_COND_WAIT(&cond, &mutex);
		STARPU_PTHREAD_MUTEX_UNLOCK(&mutex);

		for(src=1 ; src<size ; src++)
		{
			void *ptr = starpu_data_get_local_ptr(handles[src]);
			value += *((int *)ptr);
			starpu_data_unregister(handles[src]);
		}
		sum = ((size-1) * (size) / 2);
		STARPU_ASSERT_MSG(sum == value, "Sum of first %d integers is %d, not %d\n", size-1, sum, value);
	}
	else
	{
		value = rank;
		handles = malloc(sizeof(starpu_data_handle_t));
		starpu_variable_data_register(&handles[0], 0, (uintptr_t)&value, sizeof(int));
		starpu_mpi_send(handles[0], 0, 12, MPI_COMM_WORLD);
		starpu_data_unregister_submit(handles[0]);
	}

	starpu_task_wait_for_all();
	free(handles);

	starpu_mpi_shutdown();
	starpu_shutdown();

	MPI_Finalize();

	return 0;
}
