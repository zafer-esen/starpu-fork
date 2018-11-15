/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2011, 2014-2015  Université de Bordeaux
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

#ifndef _STARPU_PARAMETERS_H
#define _STARPU_PARAMETERS_H

/* Parameters which are not worth being added to ./configure options, but
 * still interesting to easily change */

/* Assumed relative performance ratios */
/* TODO: benchmark a bit instead */
#define _STARPU_CPU_ALPHA	1.0f
#define _STARPU_CUDA_ALPHA	13.33f
#define _STARPU_OPENCL_ALPHA	12.22f

#endif /* _STARPU_PARAMETERS_H */
