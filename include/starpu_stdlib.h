/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2010-2015  Université de Bordeaux
 * Copyright (C) 2010, 2011, 2012, 2013  Centre National de la Recherche Scientifique
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

#ifndef __STARPU_STDLIB_H__
#define __STARPU_STDLIB_H__

#include <starpu.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define STARPU_MALLOC_PINNED	((1ULL)<<1)
#define STARPU_MALLOC_COUNT	((1ULL)<<3)

void starpu_malloc_set_align(size_t align);

int starpu_malloc(void **A, size_t dim) STARPU_ATTRIBUTE_ALLOC_SIZE(2);
int starpu_free(void *A);

int starpu_malloc_flags(void **A, size_t dim, int flags) STARPU_ATTRIBUTE_ALLOC_SIZE(2);
int starpu_free_flags(void *A, size_t dim, int flags);

int starpu_memory_pin(void *addr, size_t size);
int starpu_memory_unpin(void *addr, size_t size);

starpu_ssize_t starpu_memory_get_total(unsigned node);
starpu_ssize_t starpu_memory_get_available(unsigned node);
void starpu_memory_wait_available(unsigned node, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* __STARPU_STDLIB_H__ */
