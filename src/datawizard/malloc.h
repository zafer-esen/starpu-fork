/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2013  Université de Bordeaux
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

#ifndef __ALLOC_H__
#define __ALLOC_H__

void _starpu_malloc_init(unsigned dst_node);
void _starpu_malloc_shutdown(unsigned dst_node);

#endif
