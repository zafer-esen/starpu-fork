/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2009, 2010, 2011, 2013  Université de Bordeaux
 * Copyright (C) 2010, 2011  Centre National de la Recherche Scientifiqu
 * Copyright (C) 2011, 2012  INRIA
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

#ifndef __DW_CHOLESKY_H__
#define __DW_CHOLESKY_H__

#include <limits.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#ifdef STARPU_USE_CUDA
#include <cuda.h>
#include <cuda_runtime.h>
#include <cublas.h>
#endif

#include <common/blas.h>
#include <starpu.h>
#include <starpu_bound.h>

#define FPRINTF(ofile, fmt, ...) do { if (!getenv("STARPU_SSILENT")) {fprintf(ofile, fmt, ## __VA_ARGS__); }} while(0)
#define NMAXBLOCKS	32

#define TAG11(k)	((starpu_tag_t)( (1ULL<<60) | (unsigned long long)(k)))
#define TAG21(k,j)	((starpu_tag_t)(((3ULL<<60) | (((unsigned long long)(k))<<32)	\
					| (unsigned long long)(j))))
#define TAG22(k,i,j)	((starpu_tag_t)(((4ULL<<60) | ((unsigned long long)(k)<<32) 	\
					| ((unsigned long long)(i)<<16)	\
					| (unsigned long long)(j))))

#define TAG11_AUX(k, prefix)	((starpu_tag_t)( (((unsigned long long)(prefix))<<60)  |  (1ULL<<56) | (unsigned long long)(k)))
#define TAG21_AUX(k,j, prefix)	((starpu_tag_t)( (((unsigned long long)(prefix))<<60)  			\
					|  ((3ULL<<56) | (((unsigned long long)(k))<<32)	\
					| (unsigned long long)(j))))
#define TAG22_AUX(k,i,j, prefix)    ((starpu_tag_t)(  (((unsigned long long)(prefix))<<60)	\
					|  ((4ULL<<56) | ((unsigned long long)(k)<<32)  	\
					| ((unsigned long long)(i)<<16) 			\
					| (unsigned long long)(j))))

#define BLOCKSIZE	(size/nblocks)

#define BLAS3_FLOP(n1,n2,n3)    \
        (2*((uint64_t)n1)*((uint64_t)n2)*((uint64_t)n3))

static unsigned size = 4*1024;
static unsigned nblocks = 16;
static unsigned nbigblocks = 8;
static unsigned pinned = 0;
static unsigned noprio = 0;
static unsigned check = 0;
static unsigned bound = 0;
static unsigned with_ctxs = 0;
static unsigned with_noctxs = 0;
static unsigned chole1 = 0;
static unsigned chole2 = 0;

void chol_cpu_codelet_update_u11(void **, void *);
void chol_cpu_codelet_update_u21(void **, void *);
void chol_cpu_codelet_update_u22(void **, void *);

#ifdef STARPU_USE_CUDA
void chol_cublas_codelet_update_u11(void *descr[], void *_args);
void chol_cublas_codelet_update_u21(void *descr[], void *_args);
void chol_cublas_codelet_update_u22(void *descr[], void *_args);
#endif

extern struct starpu_perfmodel chol_model_11;
extern struct starpu_perfmodel chol_model_21;
extern struct starpu_perfmodel chol_model_22;

static void STARPU_ATTRIBUTE_UNUSED parse_args(int argc, char **argv)
{
	int i;
	for (i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "-with_ctxs") == 0) 
		{
			with_ctxs = 1;
			break;
		}
		if (strcmp(argv[i], "-with_noctxs") == 0) 
		{
			with_noctxs = 1;
			break;
		}
		
		if (strcmp(argv[i], "-chole1") == 0) 
		{
			chole1 = 1;
			break;
		}

		if (strcmp(argv[i], "-chole2") == 0) 
		{
			chole2 = 1;
			break;
		}

		if (strcmp(argv[i], "-size") == 0)
		{
		        char *argptr;
			size = strtol(argv[++i], &argptr, 10);
		}

		if (strcmp(argv[i], "-nblocks") == 0)
		{
		        char *argptr;
			nblocks = strtol(argv[++i], &argptr, 10);
		}

		if (strcmp(argv[i], "-nbigblocks") == 0)
		{
		        char *argptr;
			nbigblocks = strtol(argv[++i], &argptr, 10);
		}

		if (strcmp(argv[i], "-pin") == 0)
		{
			pinned = 1;
		}

		if (strcmp(argv[i], "-no-prio") == 0)
		{
			noprio = 1;
		}

		if (strcmp(argv[i], "-bound") == 0)
		{
			bound = 1;
		}

		if (strcmp(argv[i], "-check") == 0)
		{
			check = 1;
		}

		if (strcmp(argv[i], "-h") == 0)
		{
			printf("usage : %s [-pin] [-size size] [-nblocks nblocks] [-check]\n", argv[0]);
		}
	}
}

#endif /* __DW_CHOLESKY_H__ */
