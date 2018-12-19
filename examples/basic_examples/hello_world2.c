/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2010  Université de Bordeaux
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
 * This examples demonstrates how to construct and submit a task to StarPU and
 * more precisely:
 *  - how to allocate a new task structure (starpu_task_create)
 *  - how to describe a multi-versionned computational kernel (ie. a codelet) 
 *  - how to pass an argument to the codelet (task->cl_arg)
 *  - how to declare a callback function that is called once the task has been
 *    executed
 *  - how to specify if starpu_task_submit is a blocking or non-blocking
 *    operation (task->synchronous)
 */

#include <stdio.h>
#include <stdint.h>
#include <starpu.h>

/* Get argo allocators */
#include "argo.h"

#define FPRINTF(ofile, fmt, ...) do { if (!getenv("STARPU_SSILENT")) {fprintf(ofile, fmt, ## __VA_ARGS__); }} while(0)

/* When the task is done, task->callback_func(task->callback_arg) is called. Any
 * callback function must have the prototype void (*)(void *).
 * NB: Callback are NOT allowed to perform potentially blocking operations */
void callback_func(void *callback_arg)
{
        FPRINTF(stdout, "Callback function got argument %p\n", callback_arg);
}

/* Every implementation of a codelet must have this prototype, the first
 * argument (buffers) describes the buffers/streams that are managed by the
 * DSM; the second arguments references read-only data that is passed as an
 * argument of the codelet (task->cl_arg). Here, "buffers" is unused as there
 * are no data input/output managed by the DSM (cl.nbuffers = 0) */
struct params
{
	int i;
	float f;
};

void cpu_func(void *buffers[], void *cl_arg)
{
	struct params *params = (struct params *) cl_arg;

	FPRINTF(stdout, "Hello world (params = {%i, %f} )\n", params->i, params->f);
}

int main(int argc, char **argv)
{
	struct starpu_codelet cl;
	struct starpu_task **tasks;
	struct params params = {1, 2.0f};
	int ret;

	/* initialize StarPU : passing a NULL argument means that we use
 	* default configuration for the scheduling policies and the number of
	* processors/accelerators */
	ret = starpu_init(NULL);
	if (ret == -ENODEV)
		return 77;
	STARPU_CHECK_RETURN_VALUE(ret, "starpu_init");
	printf("%d: StarPU init complete\n",argo_node_id());

	char *A = NULL;
	unsigned long alloc_size = 800UL*1024UL*1024UL;

	printf("%d: StarPU init complete 2\n",argo_node_id());
//	printf("%d: Available mempool: %lu MB\n",argo_node_id(),argo_get_avail()>>20);
	printf("%d: Calling starpu_malloc with size %lu MB\n",argo_node_id(),alloc_size>>20);
	starpu_malloc((void **)(&A), alloc_size);//(size_t)dim*dim*sizeof(char));
	printf("%d: Starpu_malloc complete, A = 0x%lx\n",argo_node_id(), A);
//	printf("%d: Available mempool: %lu MB\n",argo_node_id(),argo_get_avail()>>20);
	argo_barrier(1);
	if(argo_node_id()==0)
		A[0] = 42;
	argo_barrier(1);
	printf("%d: A[0]=%d\n",argo_node_id(),A[0]);
	//assert(A[0] == 42);



	starpu_free(A);
	printf("%d: Starpu_free complete.\n",argo_node_id());
	//printf("%d: Available mempool: %lu MB\n",argo_node_id(),argo_get_avail()>>20);

	/* create a new task that is non-blocking by default : the task is not
	 * submitted to the scheduler until the starpu_task_submit function is
	 * called */
	int num_nodes = argo_number_of_nodes();
	int i = 0;
	tasks = malloc(num_nodes*sizeof(struct starpu_task *));
	for(i=0; i<num_nodes; ++i)
		tasks[i] = starpu_task_create();

	starpu_codelet_init(&cl);
	/* this codelet may only be executed on a CPU, and its cpu
 	 * implementation is function "cpu_func" */
	cl.cpu_funcs[0] = cpu_func;
	/* the codelet does not manipulate any data that is managed
	 * by our DSM */
	cl.nbuffers = 0;
	cl.name="hello";

	for(i=0; i<num_nodes; ++i){
		/* the task uses codelet "cl" */
		tasks[i]->cl = &cl;

		/* It is possible to pass buffers that are not managed by the DSM to the
		 * kernels: the second argument of the "cpu_func" function is a pointer to a
		 * buffer that contains information for the codelet (cl_arg stands for
		 * codelet argument). In the case of accelerators, it is possible that
		 * the codelet is given a pointer to a copy of that buffer: this buffer
		 * is read-only so that any modification is not passed to other copies
		 * of the buffer.  For this reason, a buffer passed as a codelet
		 * argument (cl_arg) is NOT a valid synchronization medium! */
		tasks[i]->cl_arg = &params;
		tasks[i]->cl_arg_size = sizeof(params);

		/* once the task has been executed, callback_func(0x42)
		 * will be called on a CPU */
		tasks[i]->callback_func = callback_func;
		tasks[i]->callback_arg = (void*) (uintptr_t) 0x42+i;

		/* starpu_task_submit will be a blocking call */
		tasks[i]->synchronous = 1;

		/* submit the task to StarPU */
		//if(argo_node_id() == i)
			ret = starpu_task_submit(tasks[i]);
		if (ret == -ENODEV) goto enodev;
		STARPU_CHECK_RETURN_VALUE(ret, "starpu_task_submit");
	}

	/* terminate StarPU: statistics and other debug outputs are not
	 * guaranteed to be generated unless this function is called. Once it
	 * is called, it is not possible to submit tasks anymore, and the user
	 * is responsible for making sure all tasks have already been executed:
	 * calling starpu_shutdown() before the termination of all the tasks
	 * results in an undefined behaviour */
	starpu_shutdown();

	return 0;

enodev:
	starpu_shutdown();
	return 77;
}
