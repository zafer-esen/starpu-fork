# StarPU --- Runtime system for heterogeneous multicore architectures.
#
# Copyright (C) 2009-2011  Université de Bordeaux
# Copyright (C) 2010-2013  Centre National de la Recherche Scientifique
#
# StarPU is free software; you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation; either version 2.1 of the License, or (at
# your option) any later version.
#
# StarPU is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
#
# See the GNU Lesser General Public License in COPYING.LGPL for more details.

CFLAGS          +=      $$(pkg-config --cflags starpu-1.1)
LDFLAGS         +=      $$(pkg-config --libs starpu-1.1)

HAS_CUDA	=	$(shell pkg-config --libs starpu-1.1 |grep -i cuda)
NVCC		?=	nvcc
HAS_OPENCL	=	$(shell pkg-config --libs starpu-1.1 |grep -i opencl)

%.o: %.cu
	nvcc $(CFLAGS) $< -c

TARGETS = hello_world vector_scal hello_world_plugin vector_scal_plugin

all: $(TARGETS)

VECTOR_SCAL_PREREQUISITES	=	vector_scal.o vector_scal_cpu.o
ifneq ($(strip $(HAS_CUDA)),)
VECTOR_SCAL_PREREQUISITES	+=	vector_scal_cuda.o
VECTOR_SCAL_COMPILER		=	$(NVCC)
else
VECTOR_SCAL_COMPILER		=	$(CC)
endif
ifneq ($(strip $(HAS_OPENCL)),)
VECTOR_SCAL_PREREQUISITES += vector_scal_opencl.o
endif

vector_scal: $(VECTOR_SCAL_PREREQUISITES)
	$(VECTOR_SCAL_COMPILER) $(LDFLAGS) $^ -o $@

hello_world_plugin: hello_world_plugin.c
	$(CC) $(CFLAGS) -fplugin=`pkg-config starpu-1.1 --variable=gccplugin` $(LDFLAGS) $^ -o $@

VECTOR_SCAL_PLUGIN_PREREQUISITES	=	vector_scal_plugin.o
ifneq ($(strip $(HAS_CUDA)),)
VECTOR_SCAL_PLUGIN_PREREQUISITES	+=	vector_scal_plugin_cuda.o
VECTOR_SCAL_PLUGIN_COMPILER		=	$(NVCC)
else
VECTOR_SCAL_PLUGIN_COMPILER		=	$(CC)
endif

vector_scal_plugin.o: vector_scal_plugin.c
	$(CC) -c $(CFLAGS) -fplugin=`pkg-config starpu-1.1 --variable=gccplugin` $^ -o $@

vector_scal_plugin: $(VECTOR_SCAL_PLUGIN_PREREQUISITES)
	$(CC) -fplugin=`pkg-config starpu-1.1 --variable=gccplugin` $(LDFLAGS) $(VECTOR_SCAL_PLUGIN_PREREQUISITES) -o $@

clean:
	rm -f $(TARGETS) *.o
