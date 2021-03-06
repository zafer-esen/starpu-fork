/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2011-2013, 2016  Universite de Bordeaux 1
 * Copyright (C) 2012-2013  Centre National de la Recherche Scientifique
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
 * This program should be used to parse the log generated by FxT 
 */

#include <config.h>
#include <starpu.h>

#define PROGNAME "starpu_fxt_tool"

static void usage(char **argv)
{
	fprintf(stderr, "Generate a trace in the Paje format\n\n");
	fprintf(stderr, "Usage: %s [ options ]\n", PROGNAME);
        fprintf(stderr, "\n");
        fprintf(stderr, "Options:\n");
	fprintf(stderr, "   -i <input file>     specify the input file. This can be specified several\n");
	fprintf(stderr, "                       times for MPI execution case\n");
        fprintf(stderr, "   -o <output file>    specify the output file\n");
        fprintf(stderr, "   -c                  use a different colour for every type of task\n");
        fprintf(stderr, "   -no-counter         set the FxT no counter option\n");
        fprintf(stderr, "   -no-bus             set the FxT no bus option\n");
	fprintf(stderr, "   -h, --help          display this help and exit\n");
	fprintf(stderr, "   -v, --version       output version information and exit\n\n");
        fprintf(stderr, "Reports bugs to <"PACKAGE_BUGREPORT">.");
        fprintf(stderr, "\n");
}

static struct starpu_fxt_options options;

static int parse_args(int argc, char **argv)
{
	/* Default options */
	starpu_fxt_options_init(&options);

	/* We want to support arguments such as "fxt_tool -i trace_*" */
	unsigned reading_input_filenames = 0;

	int i;
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-c") == 0) {
			options.per_task_colour = 1;
			reading_input_filenames = 0;
			continue;
		}

		if (strcmp(argv[i], "-o") == 0) {
			options.out_paje_path = argv[++i];
			reading_input_filenames = 0;
			continue;
		}

		if (strcmp(argv[i], "-i") == 0) {
			options.filenames[options.ninputfiles++] = argv[++i];
			reading_input_filenames = 1;
			continue;
		}

		if (strcmp(argv[i], "-no-counter") == 0) {
			options.no_counter = 1;
			reading_input_filenames = 0;
			continue;
		}

		if (strcmp(argv[i], "-no-bus") == 0) {
			options.no_bus = 1;
			reading_input_filenames = 0;
			continue;
		}

		if (strcmp(argv[i], "-h") == 0
		 || strcmp(argv[i], "--help") == 0) {
			usage(argv);
			return EXIT_SUCCESS;
		}

		if (strcmp(argv[i], "-v") == 0
		 || strcmp(argv[i], "--version") == 0) {
		        fputs(PROGNAME " (" PACKAGE_NAME ") " PACKAGE_VERSION "\n", stderr);
			return EXIT_SUCCESS;
		}

		/* That's pretty dirty: if the reading_input_filenames flag is
		 * set, and that the argument does not match an option, we
		 * assume this may be another filename */
		if (reading_input_filenames)
		{
			options.filenames[options.ninputfiles++] = argv[i];
			continue;
		}
	}

	if (!options.ninputfiles)
	{
		fprintf(stderr, "Incorrect usage, aborting\n");
                usage(argv);
		return 77;
	}
	return 0;
}

int main(int argc, char **argv)
{
	int ret = parse_args(argc, argv);
	if (ret) return ret;

	starpu_fxt_generate_trace(&options);

	return 0;
}
