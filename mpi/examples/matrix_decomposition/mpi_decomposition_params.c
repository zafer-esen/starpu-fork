/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2009, 2010, 2015  Université de Bordeaux
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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

unsigned size = 4*960;
unsigned nblocks = 16;
unsigned nbigblocks = 2;
unsigned noprio = 0;
unsigned display = 0;
int dblockx = -1;
int dblocky = -1;

void parse_args(int argc, char **argv, int nodes)
{
        int i;
        for (i = 1; i < argc; i++)
        {
                if (strcmp(argv[i], "-size") == 0)
                {
                        char *argptr;
                        size = strtol(argv[++i], &argptr, 10);
                }

                if (strcmp(argv[i], "-dblockx") == 0)
                {
                        char *argptr;
                        dblockx = strtol(argv[++i], &argptr, 10);
                }

                if (strcmp(argv[i], "-dblocky") == 0)
                {
                        char *argptr;
                        dblocky = strtol(argv[++i], &argptr, 10);
                }

                if (strcmp(argv[i], "-nblocks") == 0)
                {
                        char *argptr;
                        nblocks = strtol(argv[++i], &argptr, 10);
                }

                if (strcmp(argv[i], "-nbigblocks") == 0)
                {
                        char *argptr;
                        nbigblocks = strtol(argv[++i], &argptr, 10);
                }

                if (strcmp(argv[i], "-no-prio") == 0)
                {
                        noprio = 1;
                }

                if (strcmp(argv[i], "-display") == 0)
                {
                        display = 1;
                }

                if (strcmp(argv[i], "-h") == 0)
                {
                        printf("usage : %s [-display] [-size size] [-nblocks nblocks]\n", argv[0]);
                }
        }

        if (nblocks > size) nblocks = size;

	if (dblockx == -1 || dblocky == -1)
	{
		int factor;
		dblockx = nodes;
		dblocky = 1;
		for(factor=sqrt(nodes) ; factor>1 ; factor--)
		{
			if (nodes % factor == 0)
			{
				dblockx = nodes/factor;
				dblocky = factor;
				break;
			}
		}
	}
}

