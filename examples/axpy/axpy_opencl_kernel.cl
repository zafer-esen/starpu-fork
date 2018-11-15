/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2012 inria
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

/* OpenCL kernel implementing axpy */

#include "axpy.h"

__kernel void _axpy_opencl(__global TYPE *x,
			   __global TYPE *y,
			   unsigned nx,
			   TYPE alpha)
{
        const int i = get_global_id(0);
        if (i < nx)
                y[i] = alpha * x[i] + y[i];
}
