/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2012 Institut National de Recherche en Informatique et Automatique
 * Copyright (C) 2010, 2011, 2013  Centre National de la Recherche Scientifique
 * Copyright (C) 2010  Université de Bordeaux
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

/* CUDA implementation of the `vector_scal' task.  */

#include <starpu.h>
#include <starpu_cuda.h>
#include <stdlib.h>

static __global__ void
vector_mult_cuda (unsigned int n, float *val, float factor)
{
  unsigned i = blockIdx.x * blockDim.x + threadIdx.x;

  if (i < n)
    val[i] *= factor;
}

extern "C" void
vector_scal_cuda (unsigned int size, float vector[], float factor)
{
  unsigned threads_per_block = 64;
  unsigned nblocks = (size + threads_per_block - 1) / threads_per_block;

  vector_mult_cuda <<< nblocks, threads_per_block, 0,
       starpu_cuda_get_local_stream () >>> (size, vector, factor);

  cudaStreamSynchronize (starpu_cuda_get_local_stream ());
}
