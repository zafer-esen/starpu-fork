/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2009, 2010, 2012  Université de Bordeaux
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

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <starpu.h>
#include <stdlib.h>
#include "../helper.h"

#if !defined(STARPU_HAVE_UNSETENV)
#warning unsetenv is not defined. Skipping test
int main(int argc, char **argv)
{
	return STARPU_TEST_SKIPPED;
}
#else
static void unset_env_variables(void)
{
	(void) unsetenv("STARPU_NCPUS");
	(void) unsetenv("STARPU_NCUDA");
	(void) unsetenv("STARPU_NOPENCL");
}

int main(int argc, char **argv)
{
	int ret;

	unset_env_variables();

	/* We try to initialize StarPU without any worker */
	struct starpu_conf conf;
	starpu_conf_init(&conf);
	conf.ncpus = 0;
	conf.ncuda = 0;
	conf.nopencl = 0;

	/* starpu_init should return -ENODEV */
	ret = starpu_init(&conf);
	if (ret == -ENODEV)
	     return EXIT_SUCCESS;
	else
	{
	     	unsigned ncpu = starpu_cpu_worker_get_count();
		unsigned ncuda = starpu_cuda_worker_get_count();
		unsigned nopencl = starpu_opencl_worker_get_count();
		FPRINTF(stderr, "StarPU has found :\n");
		FPRINTF(stderr, "\t%u CPU cores\n", ncpu);
		FPRINTF(stderr, "\t%u CUDA devices\n", ncuda);
		FPRINTF(stderr, "\t%u OpenCL devices\n", nopencl);
		return EXIT_FAILURE;
	}


}
#endif
