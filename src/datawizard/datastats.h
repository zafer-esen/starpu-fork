/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2009, 2010  Université de Bordeaux
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

#ifndef __DATASTATS_H__
#define __DATASTATS_H__

#include <starpu.h>
#include <common/config.h>
#include <stdint.h>
#include <stdlib.h>

void _starpu_msi_cache_hit(unsigned node);
void _starpu_msi_cache_miss(unsigned node);

void _starpu_display_msi_stats(void);

void _starpu_allocation_cache_hit(unsigned node STARPU_ATTRIBUTE_UNUSED);
void _starpu_data_allocation_inc_stats(unsigned node STARPU_ATTRIBUTE_UNUSED);

void _starpu_comm_amounts_inc(unsigned src, unsigned dst, size_t size);
void _starpu_display_comm_amounts(void);
void _starpu_display_alloc_cache_stats(void);

#endif // __DATASTATS_H__
