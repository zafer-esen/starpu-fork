/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2009, 2010, 2015  Université de Bordeaux
 * Copyright (C) 2010  Centre National de la Recherche Scientifique
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

/* Real float macros */

#define TYPE float
#define CUBLAS_TYPE TYPE

#define STARPU_LU(name)       starpu_slu_##name

#ifdef STARPU_HAVE_MAGMA
#include <magmablas.h>
#define CUBLAS_GEMM	magmablas_sgemm
#define CUBLAS_TRSM	magmablas_strsm
#else
#define CUBLAS_GEMM	cublasSgemm
#define CUBLAS_TRSM	cublasStrsm
#endif

#define CUBLAS_SCAL	cublasSscal
#define CUBLAS_GER	cublasSger
#define CUBLAS_SWAP	cublasSswap
#define CUBLAS_IAMAX	cublasIsamax

#define CPU_GEMM	STARPU_SGEMM
#define CPU_TRSM	STARPU_STRSM
#define CPU_SCAL	STARPU_SSCAL
#define CPU_GER		STARPU_SGER
#define CPU_SWAP	STARPU_SSWAP

#define CPU_TRMM	STARPU_STRMM
#define CPU_AXPY	STARPU_SAXPY
#define CPU_ASUM	STARPU_SASUM
#define CPU_IAMAX	STARPU_ISAMAX

#define PIVOT_THRESHHOLD	10e-5

#define CAN_EXECUTE
