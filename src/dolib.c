/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2010, 2012, 2014  Université de Bordeaux
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

/* Wrapper to avoid msys' tendency to turn / into \ and : into ;  */

#include <stdio.h>
#include <stdlib.h>
#if defined(_WIN32) && !defined(__MINGW32__) && !defined(__CYGWIN__)
#include <unistd.h>
#endif

int main(int argc, char *argv[])
{
	char *prog, *arch, *def, *effective_version, *version, *lib;
	char s[1024];
	char name[16];
	int current, age, revision;

	if (argc != 7)
	{
		fprintf(stderr,"bad number of arguments");
		exit(EXIT_FAILURE);
	}

	prog = argv[1];
	arch = argv[2];
	def = argv[3];
	effective_version = argv[4];
	version = argv[5];
	lib = argv[6];

	if (sscanf(version, "%d:%d:%d", &current, &revision, &age) != 3)
		exit(EXIT_FAILURE);

	_snprintf(name, sizeof(name), "libstarpu-%s-%d", effective_version, current - age);
	printf("using soname %s\n", name);

	_snprintf(s, sizeof(s), "\"%s\" /machine:%s /def:%s /name:%s /out:%s",
		 prog, arch, def, name, lib);
	if (system(s))
	{
		fprintf(stderr, "%s failed\n", s);
		exit(EXIT_FAILURE);
	}

	exit(EXIT_SUCCESS);
}
