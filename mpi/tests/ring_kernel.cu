/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2009, 2010  Université de Bordeaux
 * Copyright (C) 2010, 2012  Centre National de la Recherche Scientifique
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

static __global__ void cuda_incrementer(unsigned *token)
{
	(*token)++;
}

extern "C" void increment_cuda(void *descr[], void *_args)
{
	(void) _args;
	unsigned *tokenptr = (unsigned *)STARPU_VECTOR_GET_PTR(descr[0]);

	cuda_incrementer<<<1,1, 0, starpu_cuda_get_local_stream()>>>(tokenptr);
	cudaStreamSynchronize(starpu_cuda_get_local_stream());
}
