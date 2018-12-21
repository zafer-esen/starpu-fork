/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2009-2013, 2015  Université de Bordeaux
 * Copyright (C) 2010  Mehdi Juhoor <mjuhoor@gmail.com>
 * Copyright (C) 2010, 2011, 2012  Centre National de la Recherche Scientifique
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

/*
 * This version of the Cholesky factorization uses explicit dependency
 * declaration through dependency tags.
 * It also uses data partitioning to split the matrix into submatrices
 */

#include "cholesky.h"
#include "argo.h"

#if defined(STARPU_USE_CUDA) && defined(STARPU_HAVE_MAGMA)
#include "magma.h"
#endif

/*
 *	Some useful functions
 */

static struct starpu_task *create_task(starpu_tag_t id)
{
	struct starpu_task *task = starpu_task_create();
		task->cl_arg = NULL;
		task->use_tag = 1;
		task->tag_id = id;

	return task;
}

/*
 *	Create the codelets
 */

static struct starpu_codelet cl11 =
{
	.modes = { STARPU_RW },
	.cpu_funcs = {chol_cpu_codelet_update_u11},
#ifdef STARPU_USE_CUDA
	.cuda_funcs = {chol_cublas_codelet_update_u11},
#endif
	.nbuffers = 1,
	.model = &chol_model_11
};

static struct starpu_task * create_task_11(starpu_data_handle_t dataA, unsigned k)
{
/*	FPRINTF(stdout, "task 11 k = %d TAG = %llx\n", k, (TAG11(k))); */

	struct starpu_task *task = create_task(TAG11(k));

	task->cl = &cl11;

	/* which sub-data is manipulated ? */
	task->handles[0] = starpu_data_get_sub_data(dataA, 2, k, k);

	/* this is an important task */
	if (!noprio)
		task->priority = STARPU_MAX_PRIO;

	/* enforce dependencies ... */
	if (k > 0)
	{
		starpu_tag_declare_deps(TAG11(k), 1, TAG22(k-1, k, k));
	}

	int n = starpu_matrix_get_nx(task->handles[0]);
	task->flops = FLOPS_SPOTRF(n);

	return task;
}

static struct starpu_codelet cl21 =
{
	.modes = { STARPU_R, STARPU_RW },
	.cpu_funcs = {chol_cpu_codelet_update_u21},
#ifdef STARPU_USE_CUDA
	.cuda_funcs = {chol_cublas_codelet_update_u21},
#endif
	.nbuffers = 2,
	.model = &chol_model_21
};

static void create_task_21(starpu_data_handle_t dataA, unsigned k, unsigned j)
{
	struct starpu_task *task = create_task(TAG21(k, j));

	task->cl = &cl21;

	/* which sub-data is manipulated ? */
	task->handles[0] = starpu_data_get_sub_data(dataA, 2, k, k);
	task->handles[1] = starpu_data_get_sub_data(dataA, 2, k, j);

	if (!noprio && (j == k+1))
	{
		task->priority = STARPU_MAX_PRIO;
	}

	/* enforce dependencies ... */
	if (k > 0)
	{
		starpu_tag_declare_deps(TAG21(k, j), 2, TAG11(k), TAG22(k-1, k, j));
	}
	else
	{
		starpu_tag_declare_deps(TAG21(k, j), 1, TAG11(k));
	}

	int n = starpu_matrix_get_nx(task->handles[0]);
	task->flops = FLOPS_STRSM(n, n);

	int ret = starpu_task_submit(task);
        if (STARPU_UNLIKELY(ret == -ENODEV))
	{
                FPRINTF(stderr, "No worker may execute this task\n");
                exit(0);
        }

}

static struct starpu_codelet cl22 =
{
	.modes = { STARPU_R, STARPU_R, STARPU_RW },
	.cpu_funcs = {chol_cpu_codelet_update_u22},
#ifdef STARPU_USE_CUDA
	.cuda_funcs = {chol_cublas_codelet_update_u22},
#endif
	.nbuffers = 3,
	.model = &chol_model_22
};

static void create_task_22(starpu_data_handle_t dataA, unsigned k, unsigned i, unsigned j)
{
/*	FPRINTF(stdout, "task 22 k,i,j = %d,%d,%d TAG = %llx\n", k,i,j, TAG22(k,i,j)); */

	struct starpu_task *task = create_task(TAG22(k, i, j));

	task->cl = &cl22;

	/* which sub-data is manipulated ? */
	task->handles[0] = starpu_data_get_sub_data(dataA, 2, k, i);
	task->handles[1] = starpu_data_get_sub_data(dataA, 2, k, j);
	task->handles[2] = starpu_data_get_sub_data(dataA, 2, i, j);

	if (!noprio && (i == k + 1) && (j == k +1) )
	{
		task->priority = STARPU_MAX_PRIO;
	}

	/* enforce dependencies ... */
	if (k > 0)
	{
		starpu_tag_declare_deps(TAG22(k, i, j), 3, TAG22(k-1, i, j), TAG21(k, i), TAG21(k, j));
	}
	else
	{
		starpu_tag_declare_deps(TAG22(k, i, j), 2, TAG21(k, i), TAG21(k, j));
	}

	int n = starpu_matrix_get_nx(task->handles[0]);
	task->flops = FLOPS_SGEMM(n, n, n);

	int ret = starpu_task_submit(task);
        if (STARPU_UNLIKELY(ret == -ENODEV))
	{
                FPRINTF(stderr, "No worker may execute this task\n");
                exit(0);
        }
}



/*
 *	code to bootstrap the factorization
 *	and construct the DAG
 */

static void _cholesky(starpu_data_handle_t dataA, unsigned nblocks)
{
	double start;
	double end;

	struct starpu_task *entry_task = NULL;

	/* create all the DAG nodes */
	unsigned i,j,k;

	printf("in _cholesky...\n");
	start = starpu_timing_now();

	for (k = 0; k < nblocks; k++)
	{
		struct starpu_task *task = create_task_11(dataA, k);
		/* we defer the launch of the first task */
		if (k == 0)
		{
			entry_task = task;
		}
		else
		{
			int ret = starpu_task_submit(task);
                        if (STARPU_UNLIKELY(ret == -ENODEV))
			{
                                FPRINTF(stderr, "No worker may execute this task\n");
                                exit(0);
                        }

		}

		for (j = k+1; j<nblocks; j++)
		{
			create_task_21(dataA, k, j);

			for (i = k+1; i<nblocks; i++)
			{
				if (i <= j)
					create_task_22(dataA, k, i, j);
			}
		}
	}
	printf("created tasks...\n");
	
	/* schedule the codelet */
	int ret = starpu_task_submit(entry_task);
        if (STARPU_UNLIKELY(ret == -ENODEV))
	{
                FPRINTF(stderr, "No worker may execute this task\n");
                exit(0);
        }

	/* stall the application until the end of computations */
	printf("tag_wait 1\n");
	starpu_tag_wait(TAG11(nblocks-1));
	printf("tag_wait 2\n");
	starpu_data_unpartition(dataA, 0);
	printf("%d: data unpartition 2\n",argo_node_id());
	end = starpu_timing_now();


	double timing = end - start;

	unsigned n = starpu_matrix_get_nx(dataA);

	double flop = (1.0f*n*n*n)/3.0f;

	PRINTF("# size\tms\tGFlops\n");
	PRINTF("%u\t%.0f\t%.1f\n", n, timing/1000, (flop/timing/1000.0f));
}

static int initialize_system(float **A, unsigned dim, unsigned pinned)
{
	int ret;

#ifdef STARPU_HAVE_MAGMA
	magma_init();
#endif

	ret = starpu_init(NULL);
	if (ret == -ENODEV)
		return 77;
	STARPU_CHECK_RETURN_VALUE(ret, "starpu_init");

#ifndef STARPU_SIMGRID
	if (pinned)
	{
		printf("starpu_malloc 1\n");
		starpu_malloc((void **)A, (size_t)dim*dim*sizeof(float));
		printf("starpu_malloc 2\n");
	}
	else
	{
		*A = malloc(dim*dim*sizeof(float));
	}
#endif
	return 0;
}

static void cholesky(float *matA, unsigned size, unsigned ld, unsigned nblocks)
{
	starpu_data_handle_t dataA;

	/* monitor and partition the A matrix into blocks :
	 * one block is now determined by 2 unsigned (i,j) */
	printf("starpu_matrix_data_register 1...\n");
	starpu_matrix_data_register(&dataA, 0, (uintptr_t)matA, ld, size, size, sizeof(float));
	printf("starpu_matrix_data_register 2...\n");
	starpu_data_set_sequential_consistency_flag(dataA, 0);

	struct starpu_data_filter f =
	{
		.filter_func = starpu_matrix_filter_vertical_block,
		.nchildren = nblocks
	};

	struct starpu_data_filter f2 =
	{
		.filter_func = starpu_matrix_filter_block,
		.nchildren = nblocks
	};

	printf("starpu_data_map_filters 1...\n");
	starpu_data_map_filters(dataA, 2, &f, &f2);
	printf("starpu_data_map_filters 2...\n");

	_cholesky(dataA, nblocks);

	printf("starpu_data_unregister 1...\n");
	starpu_data_unregister(dataA);
	printf("starpu_data_unregister 2...\n");
}

static void shutdown_system(float **matA, unsigned pinned)
{
	if (pinned)
	{
		starpu_free(*matA);
	}
	else
	{
		free(*matA);
	}

	starpu_shutdown();
}

int main(int argc, char **argv)
{
	/* create a simple definite positive symetric matrix example
	 *
	 *	Hilbert matrix : h(i,j) = 1/(i+j+1)
	 * */

	parse_args(argc, argv);

	float *mat = NULL;
	int ret = initialize_system(&mat, size, pinned);
	if (ret) return ret;

#ifndef STARPU_SIMGRID
	unsigned i,j;
	printf("%d: init matrix data 1\n",argo_node_id());
	if (argo_node_id() == 0){ //initialization should only be done on a single node
		for (i = 0; i < size; i++)
		{
			for (j = 0; j < size; j++)
			{
				mat[j +i*size] = (1.0f/(1.0f+i+j)) + ((i == j)?1.0f*size:0.0f);
				/* mat[j +i*size] = ((i == j)?1.0f*size:0.0f); */
			}
		}
	}
	argo_barrier(1);
	printf("%d: init matrix data 2\n",argo_node_id());
#endif


#ifdef CHECK_OUTPUT
	FPRINTF(stdout, "Input :\n");

	for (j = 0; j < size; j++)
	{
		for (i = 0; i < size; i++)
		{
			if (i <= j)
			{
				FPRINTF(stdout, "%2.2f\t", mat[j +i*size]);
			}
			else
			{
				FPRINTF(stdout, ".\t");
			}
		}
		FPRINTF(stdout, "\n");
	}
#endif

	printf("%d: call cholesky 1\n",argo_node_id());
	cholesky(mat, size, size, nblocks);
	printf("%d: call cholesky 2\n",argo_node_id());

#ifdef CHECK_OUTPUT
	FPRINTF(stdout, "Results :\n");

	for (j = 0; j < size; j++)
	{
		for (i = 0; i < size; i++)
		{
			if (i <= j)
			{
				FPRINTF(stdout, "%2.2f\t", mat[j +i*size]);
			}
			else
			{
				FPRINTF(stdout, ".\t");
				mat[j+i*size] = 0.0f; /* debug */
			}
		}
		FPRINTF(stdout, "\n");
	}

	FPRINTF(stderr, "compute explicit LLt ...\n");
	float *test_mat = malloc(size*size*sizeof(float));
	STARPU_ASSERT(test_mat);

	STARPU_SSYRK("L", "N", size, size, 1.0f,
				mat, size, 0.0f, test_mat, size);

	FPRINTF(stderr, "comparing results ...\n");
	for (j = 0; j < size; j++)
	{
		for (i = 0; i < size; i++)
		{
			if (i <= j)
			{
				FPRINTF(stdout, "%2.2f\t", test_mat[j +i*size]);
			}
			else
			{
				FPRINTF(stdout, ".\t");
			}
		}
		FPRINTF(stdout, "\n");
	}
	free(test_mat);
#endif

	shutdown_system(&mat, pinned);
	return 0;
}
