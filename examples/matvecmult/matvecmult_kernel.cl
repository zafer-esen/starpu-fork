/*
 * StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2014  Université de Bordeaux
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

__kernel void matVecMult(const __global float *A, const __global float *X, int n, int m, __global float *Y)
{
	const int i = get_global_id(0);
	if (i < m)
	{
		float val = 0;
		int j;

		for (j = 0; j < n; j++)
		       val += A[i*n+j] * X[j];

		Y[i] = val;
	}
}
