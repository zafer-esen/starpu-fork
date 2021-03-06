/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2009, 2010, 2013, 2015  Université de Bordeaux
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

/*
 * This is an example of using a prologue callback. We submit a task, whose
 * prologue callback (i.e. before task gets scheduled) prints a value, and
 * whose pop_prologue callback (i.e. after task gets scheduled, but before task
 * execution) prints another value.
 */

#include <starpu.h>

#define FPRINTF(ofile, fmt, ...) do { if (!getenv("STARPU_SSILENT")) {fprintf(ofile, fmt, ## __VA_ARGS__); }} while(0)

starpu_data_handle_t handle;

void cpu_codelet(void *descr[], STARPU_ATTRIBUTE_UNUSED void *_args)
{
	int *val = (int *)STARPU_VARIABLE_GET_PTR(descr[0]);

	*val += 1;
}

struct starpu_codelet cl =
{
	.modes = { STARPU_RW },
	.cpu_funcs = {cpu_codelet},
	.nbuffers = 1,
	.name = "callback"
};

void prologue_callback_func(void *callback_arg)
{
	double *x = (double*)callback_arg;
	printf("x = %lf\n", *x);
	STARPU_ASSERT(*x == -999.0);
}


int main(int argc, char **argv)
{
	int v=40;
	int ret;

	ret = starpu_init(NULL);
	if (ret == -ENODEV)
		return 77;
	STARPU_CHECK_RETURN_VALUE(ret, "starpu_init");

	starpu_variable_data_register(&handle, 0, (uintptr_t)&v, sizeof(int));
	double x = -999.0;

	struct starpu_task *task = starpu_task_create();
	task->cl = &cl;
	task->prologue_callback_func = prologue_callback_func;
	task->prologue_callback_arg = &x;
	task->handles[0] = handle;

	ret = starpu_task_submit(task);
	if (ret == -ENODEV) goto enodev;
	STARPU_CHECK_RETURN_VALUE(ret, "starpu_task_submit");

	int ret2 = starpu_insert_task(&cl,
				      STARPU_RW, handle,
				      STARPU_PROLOGUE_CALLBACK, prologue_callback_func,
				      STARPU_PROLOGUE_CALLBACK_ARG, &x,
				      0);


	starpu_task_wait_for_all();
	starpu_data_unregister(handle);

	FPRINTF(stderr, "v -> %d\n", v);


	starpu_shutdown();

	return 0;

enodev:
	starpu_shutdown();
	return 77;
}
