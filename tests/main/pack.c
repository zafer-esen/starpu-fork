/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2014  Centre National de la Recherche Scientifique
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

#include <config.h>
#include <starpu.h>
#include "../helper.h"

void func_cpu(void *descr[], void *_args)
{
	int factor;
	char c;
	int x;

	starpu_codelet_unpack_args(_args, &factor, &c, &x);

        FPRINTF(stderr, "values: %d %c %d\n", factor, c, x);
	assert(factor == 12 && c == 'n' && x == 42);
}

struct starpu_codelet mycodelet =
{
	.cpu_funcs = {func_cpu},
        .nbuffers = 0
};

int main(int argc, char **argv)
{
        int i, ret;
        int x=42;
	int factor=12;
	char c='n';
	struct starpu_task *task;

	ret = starpu_init(NULL);
	if (ret == -ENODEV) return STARPU_TEST_SKIPPED;
	STARPU_CHECK_RETURN_VALUE(ret, "starpu_init");

        FPRINTF(stderr, "values: %d %c %d\n", factor, c, x);

	task = starpu_task_create();
	task->synchronous = 1;
	task->cl = &mycodelet;
	task->cl_arg_free = 1;
	starpu_codelet_pack_args(&task->cl_arg, &task->cl_arg_size,
				 STARPU_VALUE, &factor, sizeof(factor),
				 STARPU_VALUE, &c, sizeof(c),
				 STARPU_VALUE, &x, sizeof(x),
				 0);
	ret = starpu_task_submit(task);
	if (ret != -ENODEV)
		STARPU_CHECK_RETURN_VALUE(ret, "starpu_task_submit");

	starpu_shutdown();
	if (ret == -ENODEV)
	{
		fprintf(stderr, "WARNING: No one can execute this task\n");
		/* yes, we do not perform the computation but we did detect that no one
		 * could perform the kernel, so this is not an error from StarPU */
		return STARPU_TEST_SKIPPED;
	}
	else
		return 0;
}
