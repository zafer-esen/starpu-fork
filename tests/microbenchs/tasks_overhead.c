/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2010-2011, 2013-2014, 2016  Université de Bordeaux
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

#include <starpu.h>
#include "../helper.h"

starpu_data_handle_t data_handles[8];
float *buffers[8];

#ifdef STARPU_QUICK_CHECK
static unsigned ntasks = 128;
#else
static unsigned ntasks = 65536;
#endif
static unsigned nbuffers = 0;

struct starpu_task *tasks;

static void dummy_func(void *descr[] STARPU_ATTRIBUTE_UNUSED, void *arg STARPU_ATTRIBUTE_UNUSED)
{
}

static struct starpu_codelet dummy_codelet =
{
	.cpu_funcs = {dummy_func},
	.cuda_funcs = {dummy_func},
	.opencl_funcs = {dummy_func},
	.model = NULL,
	.nbuffers = 0,
	.modes = {STARPU_RW, STARPU_RW, STARPU_RW, STARPU_RW, STARPU_RW, STARPU_RW, STARPU_RW, STARPU_RW}
};

int inject_one_task(void)
{
	struct starpu_task *task = starpu_task_create();

	task->cl = &dummy_codelet;
	task->cl_arg = NULL;
	task->callback_func = NULL;
	task->synchronous = 1;

	int ret;
	ret = starpu_task_submit(task);
	return ret;
}

static void parse_args(int argc, char **argv)
{
	int c;
	while ((c = getopt(argc, argv, "i:b:h")) != -1)
	switch(c)
	{
		case 'i':
			ntasks = atoi(optarg);
			break;
		case 'b':
			nbuffers = atoi(optarg);
			dummy_codelet.nbuffers = nbuffers;
			break;
		case 'h':
			fprintf(stderr, "Usage: %s [-i ntasks] [-b nbuffers] [-h]\n", argv[0]);
			break;
	}
}

int main(int argc, char **argv)
{
	int ret;
	unsigned i;

	double timing_submit;
	double start_submit;
	double end_submit;

	double timing_exec;
	double start_exec;
	double end_exec;
	struct starpu_conf conf;
	starpu_conf_init(&conf);
	conf.ncpus = 2;

	parse_args(argc, argv);

	ret = starpu_init(&conf);
	if (ret == -ENODEV) return STARPU_TEST_SKIPPED;
	STARPU_CHECK_RETURN_VALUE(ret, "starpu_init");

	unsigned buffer;
	for (buffer = 0; buffer < nbuffers; buffer++)
	{
		buffers[buffer] = (float *) malloc(16*sizeof(float));
		starpu_vector_data_register(&data_handles[buffer], 0, (uintptr_t)buffers[buffer], 16, sizeof(float));
	}

	fprintf(stderr, "#tasks : %u\n#buffers : %u\n", ntasks, nbuffers);

	/* submit tasks (but don't execute them yet !) */
	tasks = (struct starpu_task *) calloc(1, ntasks*sizeof(struct starpu_task));

	start_submit = starpu_timing_now();
	for (i = 0; i < ntasks; i++)
	{
		starpu_task_init(&tasks[i]);
		tasks[i].callback_func = NULL;
		tasks[i].cl = &dummy_codelet;
		tasks[i].cl_arg = NULL;
		tasks[i].synchronous = 0;
		tasks[i].use_tag = 1;
		tasks[i].tag_id = (starpu_tag_t)i;

		/* we have 8 buffers at most */
		for (buffer = 0; buffer < nbuffers; buffer++)
		{
			tasks[i].handles[buffer] = data_handles[buffer];
		}
	}
	tasks[ntasks-1].detach = 0;

	start_submit = starpu_timing_now();
	for (i = 1; i < ntasks; i++)
	{
		starpu_tag_declare_deps((starpu_tag_t)i, 1, (starpu_tag_t)(i-1));

		ret = starpu_task_submit(&tasks[i]);
		if (ret == -ENODEV) goto enodev;
		STARPU_CHECK_RETURN_VALUE(ret, "starpu_task_submit");
	}

	/* submit the first task */
	ret = starpu_task_submit(&tasks[0]);
	if (ret == -ENODEV) goto enodev;
	STARPU_CHECK_RETURN_VALUE(ret, "starpu_task_submit");

	end_submit = starpu_timing_now();

	/* wait for the execution of the tasks */
	start_exec = starpu_timing_now();
	ret = starpu_task_wait(&tasks[ntasks-1]);
	STARPU_CHECK_RETURN_VALUE(ret, "starpu_tag_wait");
	end_exec = starpu_timing_now();

	starpu_task_wait_for_all();

	for (i = 0; i < ntasks; i++)
		starpu_task_clean(&tasks[i]);

	for (buffer = 0; buffer < nbuffers; buffer++)
		starpu_data_unregister(data_handles[buffer]);

	timing_submit = end_submit - start_submit;
	timing_exec = end_exec - start_exec;

	fprintf(stderr, "Total submit: %f secs\n", timing_submit/1000000);
	fprintf(stderr, "Per task submit: %f usecs\n", timing_submit/ntasks);
	fprintf(stderr, "\n");
	fprintf(stderr, "Total execution: %f secs\n", timing_exec/1000000);
	fprintf(stderr, "Per task execution: %f usecs\n", timing_exec/ntasks);
	fprintf(stderr, "\n");
	fprintf(stderr, "Total: %f secs\n", (timing_submit+timing_exec)/1000000);
	fprintf(stderr, "Per task: %f usecs\n", (timing_submit+timing_exec)/ntasks);

        {
                char *output_dir = getenv("STARPU_BENCH_DIR");
                char *bench_id = getenv("STARPU_BENCH_ID");

                if (output_dir && bench_id)
		{
                        char file[1024];
                        FILE *f;

                        sprintf(file, "%s/tasks_overhead_total_submit.dat", output_dir);
                        f = fopen(file, "a");
                        fprintf(f, "%s\t%f\n", bench_id, timing_submit/1000000);
                        fclose(f);

                        sprintf(file, "%s/tasks_overhead_per_task_submit.dat", output_dir);
                        f = fopen(file, "a");
                        fprintf(f, "%s\t%f\n", bench_id, timing_submit/ntasks);
                        fclose(f);

                        sprintf(file, "%s/tasks_overhead_total_execution.dat", output_dir);
                        f = fopen(file, "a");
                        fprintf(f, "%s\t%f\n", bench_id, timing_exec/1000000);
                        fclose(f);

                        sprintf(file, "%s/tasks_overhead_per_task_execution.dat", output_dir);
                        f = fopen(file, "a");
                        fprintf(f, "%s\t%f\n", bench_id, timing_exec/ntasks);
                        fclose(f);

                        sprintf(file, "%s/tasks_overhead_total_submit_execution.dat", output_dir);
                        f = fopen(file, "a");
                        fprintf(f, "%s\t%f\n", bench_id, (timing_submit+timing_exec)/1000000);
                        fclose(f);

                        sprintf(file, "%s/tasks_overhead_per_task_submit_execution.dat", output_dir);
                        f = fopen(file, "a");
                        fprintf(f, "%s\t%f\n", bench_id, (timing_submit+timing_exec)/ntasks);
                        fclose(f);
                }
        }

	starpu_shutdown();
	free(tasks);
	return EXIT_SUCCESS;

enodev:
	fprintf(stderr, "WARNING: No one can execute this task\n");
	/* yes, we do not perform the computation but we did detect that no one
 	 * could perform the kernel, so this is not an error from StarPU */
	starpu_shutdown();
	free(tasks);
	return STARPU_TEST_SKIPPED;
}
