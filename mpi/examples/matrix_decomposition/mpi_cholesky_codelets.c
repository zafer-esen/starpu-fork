/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2009, 2010, 2014  Université de Bordeaux
 * Copyright (C) 2010, 2011, 2012, 2013, 2015  Centre National de la Recherche Scientifique
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
#include <common/blas.h>
#include "mpi_decomposition_params.h"
#include "mpi_decomposition_matrix.h"
#include "mpi_cholesky_models.h"
#include "mpi_cholesky_codelets.h"
#include "mpi_cholesky_kernels.h"
#include <sys/time.h>

/*
 *	Create the codelets
 */

static struct starpu_codelet cl11 =
{
	.cpu_funcs = {chol_cpu_codelet_update_u11},
#ifdef STARPU_USE_CUDA
	.cuda_funcs = {chol_cublas_codelet_update_u11},
#endif
	.nbuffers = 1,
	.modes = {STARPU_RW},
	.model = &chol_model_11
};

static struct starpu_codelet cl21 =
{
	.cpu_funcs = {chol_cpu_codelet_update_u21},
#ifdef STARPU_USE_CUDA
	.cuda_funcs = {chol_cublas_codelet_update_u21},
#endif
	.nbuffers = 2,
	.modes = {STARPU_R, STARPU_RW},
	.model = &chol_model_21
};

static struct starpu_codelet cl22 =
{
	.cpu_funcs = {chol_cpu_codelet_update_u22},
#ifdef STARPU_USE_CUDA
	.cuda_funcs = {chol_cublas_codelet_update_u22},
#endif
	.nbuffers = 3,
	.modes = {STARPU_R, STARPU_R, STARPU_RW},
	.model = &chol_model_22
};

/*
 *	code to bootstrap the factorization
 *	and construct the DAG
 */
void dw_cholesky(float ***matA, unsigned ld, int rank, int nodes, double *timing, double *flops)
{
	double start;
	double end;
	starpu_data_handle_t **data_handles;
	unsigned x,y,i,j,k;

	/* create all the DAG nodes */

	data_handles = malloc(nblocks*sizeof(starpu_data_handle_t *));
	for(x=0 ; x<nblocks ; x++) data_handles[x] = malloc(nblocks*sizeof(starpu_data_handle_t));

	for(x = 0; x < nblocks ; x++)
	{
		for (y = 0; y < nblocks; y++)
		{
			int mpi_rank = my_distrib(x, y, nodes);
			if (mpi_rank == rank)
			{
				//fprintf(stderr, "[%d] Owning data[%d][%d]\n", rank, x, y);
				starpu_matrix_data_register(&data_handles[x][y], 0, (uintptr_t)matA[x][y],
						ld, size/nblocks, size/nblocks, sizeof(float));
			}
#warning TODO: make better test to only register what is needed
			else
			{
				/* I don't own that index, but will need it for my computations */
				//fprintf(stderr, "[%d] Neighbour of data[%d][%d]\n", rank, x, y);
				starpu_matrix_data_register(&data_handles[x][y], -1, (uintptr_t)NULL,
						ld, size/nblocks, size/nblocks, sizeof(float));
			}
			if (data_handles[x][y])
			{
				starpu_mpi_data_register(data_handles[x][y], (y*nblocks)+x, mpi_rank);
			}
		}
	}

	starpu_mpi_barrier(MPI_COMM_WORLD);
	start = starpu_timing_now();

	for (k = 0; k < nblocks; k++)
	{
		int prio = STARPU_DEFAULT_PRIO;
		if (!noprio) prio = STARPU_MAX_PRIO;

		starpu_mpi_insert_task(MPI_COMM_WORLD, &cl11,
				STARPU_PRIORITY, prio,
				STARPU_RW, data_handles[k][k],
				0);

		for (j = k+1; j<nblocks; j++)
		{
			prio = STARPU_DEFAULT_PRIO;
			if (!noprio&& (j == k+1)) prio = STARPU_MAX_PRIO;
			starpu_mpi_insert_task(MPI_COMM_WORLD, &cl21,
					STARPU_PRIORITY, prio,
					STARPU_R, data_handles[k][k],
					STARPU_RW, data_handles[k][j],
					0);

			starpu_mpi_cache_flush(MPI_COMM_WORLD, data_handles[k][k]);

			for (i = k+1; i<nblocks; i++)
			{
				if (i <= j)
				{
					prio = STARPU_DEFAULT_PRIO;
					if (!noprio && (i == k + 1) && (j == k +1) ) prio = STARPU_MAX_PRIO;
					starpu_mpi_insert_task(MPI_COMM_WORLD, &cl22,
							STARPU_PRIORITY, prio,
							STARPU_R, data_handles[k][i],
							STARPU_R, data_handles[k][j],
							STARPU_RW, data_handles[i][j],
							0);
				}
			}

			starpu_mpi_cache_flush(MPI_COMM_WORLD, data_handles[k][j]);
		}
	}

	starpu_task_wait_for_all();

	for(x = 0; x < nblocks ; x++)
	{
		for (y = 0; y < nblocks; y++)
		{
			if (data_handles[x][y])
				starpu_data_unregister(data_handles[x][y]);
		}
		free(data_handles[x]);
	}
	free(data_handles);

	starpu_mpi_barrier(MPI_COMM_WORLD);
	end = starpu_timing_now();

	if (rank == 0)
	{
		*timing = end - start;
		*flops = (1.0f*size*size*size)/3.0f;
	}
}

void dw_cholesky_check_computation(float ***matA, int rank, int nodes, int *correctness, double *flops)
{
	unsigned i,j,x,y;
	float *rmat = malloc(size*size*sizeof(float));

	for(x=0 ; x<nblocks ; x++)
	{
		for(y=0 ; y<nblocks ; y++)
		{
			for (i = 0; i < BLOCKSIZE; i++)
			{
				for (j = 0; j < BLOCKSIZE; j++)
				{
					rmat[j+(y*BLOCKSIZE)+(i+(x*BLOCKSIZE))*size] = matA[x][y][j +i*BLOCKSIZE];
				}
			}
		}
	}

	fprintf(stderr, "[%d] compute explicit LLt ...\n", rank);
	for (j = 0; j < size; j++)
	{
		for (i = 0; i < size; i++)
		{
			if (i > j)
			{
				rmat[j+i*size] = 0.0f; // debug
			}
		}
	}
	float *test_mat = malloc(size*size*sizeof(float));
	STARPU_ASSERT(test_mat);

	STARPU_SSYRK("L", "N", size, size, 1.0f,
			rmat, size, 0.0f, test_mat, size);

	fprintf(stderr, "[%d] comparing results ...\n", rank);
	if (display)
	{
		for (j = 0; j < size; j++)
		{
			for (i = 0; i < size; i++)
			{
				if (i <= j)
				{
					printf("%2.2f\t", test_mat[j +i*size]);
				}
				else
				{
					printf(".\t");
				}
			}
			printf("\n");
		}
	}

	*correctness = 1;
	for(x = 0; x < nblocks ; x++)
	{
		for (y = 0; y < nblocks; y++)
		{
			int mpi_rank = my_distrib(x, y, nodes);
			if (mpi_rank == rank)
			{
				for (i = (size/nblocks)*x ; i < (size/nblocks)*x+(size/nblocks); i++)
				{
					for (j = (size/nblocks)*y ; j < (size/nblocks)*y+(size/nblocks); j++)
					{
						if (i <= j)
						{
							float orig = (1.0f/(1.0f+i+j)) + ((i == j)?1.0f*size:0.0f);
							float err = abs(test_mat[j +i*size] - orig);
							if (err > 0.00001)
							{
								fprintf(stderr, "[%d] Error[%u, %u] --> %2.2f != %2.2f (err %2.2f)\n", rank, i, j, test_mat[j +i*size], orig, err);
								*correctness = 0;
								*flops = 0;
								break;
							}
						}
					}
				}
			}
		}
	}
	free(rmat);
	free(test_mat);
}
