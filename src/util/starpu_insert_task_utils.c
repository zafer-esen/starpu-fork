/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2011, 2012, 2013, 2014  Centre National de la Recherche Scientifique
 * Copyright (C) 2011, 2014        INRIA
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

#include <util/starpu_insert_task_utils.h>
#include <common/config.h>
#include <common/utils.h>
#include <core/task.h>

typedef void (*_starpu_callback_func_t)(void *);

size_t _starpu_insert_task_get_arg_size(va_list varg_list)
{
	int arg_type;
	size_t arg_buffer_size;

	arg_buffer_size = 0;

	arg_buffer_size += sizeof(char);

	while ((arg_type = va_arg(varg_list, int)) != 0)
	{
		if (arg_type & STARPU_R || arg_type & STARPU_W || arg_type & STARPU_RW || arg_type & STARPU_SCRATCH || arg_type & STARPU_REDUX)
		{
			(void)va_arg(varg_list, starpu_data_handle_t);
		}
		else if (arg_type==STARPU_DATA_ARRAY)
		{
			(void)va_arg(varg_list, starpu_data_handle_t*);
			(void)va_arg(varg_list, int);
		}
		else if (arg_type==STARPU_VALUE)
		{
			(void)va_arg(varg_list, void *);
			size_t cst_size = va_arg(varg_list, size_t);

			arg_buffer_size += sizeof(size_t);
			arg_buffer_size += cst_size;
		}
		else if (arg_type==STARPU_CALLBACK)
		{
			(void)va_arg(varg_list, _starpu_callback_func_t);
		}
		else if (arg_type==STARPU_CALLBACK_WITH_ARG)
		{
			va_arg(varg_list, _starpu_callback_func_t);
			va_arg(varg_list, void *);
		}
		else if (arg_type==STARPU_PROLOGUE_CALLBACK)
		{
			(void)va_arg(varg_list, _starpu_callback_func_t);
		}
		else if (arg_type==STARPU_PROLOGUE_CALLBACK_ARG)
		{
			(void)va_arg(varg_list, void *);
		}

		else if (arg_type==STARPU_CALLBACK_ARG)
		{
			(void)va_arg(varg_list, void *);
		}
		else if (arg_type==STARPU_PRIORITY)
		{
			(void)va_arg(varg_list, int);
		}
		else if (arg_type==STARPU_EXECUTE_ON_NODE)
		{
			(void)va_arg(varg_list, int);
		}
		else if (arg_type==STARPU_EXECUTE_ON_DATA)
		{
			(void)va_arg(varg_list, starpu_data_handle_t);
		}
		else if (arg_type==STARPU_EXECUTE_ON_WORKER)
		{
			va_arg(varg_list, int);
		}
		else if (arg_type==STARPU_SCHED_CTX)
		{
			(void)va_arg(varg_list, unsigned);
		}
		else if (arg_type==STARPU_HYPERVISOR_TAG)
		{
			(void)va_arg(varg_list, int);
		}
		else if (arg_type==STARPU_FLOPS)
		{
			(void)va_arg(varg_list, double);
		}

		else if (arg_type==STARPU_TAG)
		{
			(void)va_arg(varg_list, starpu_tag_t);
		}
		else
		{
			STARPU_ABORT_MSG("Unrecognized argument %d\n", arg_type);
		}
	}

	va_end(varg_list);
	return arg_buffer_size;
}

int _starpu_codelet_pack_args(void **arg_buffer, size_t arg_buffer_size, va_list varg_list)
{
	int arg_type;
	unsigned current_arg_offset = 0;
	unsigned char nargs = 0;
	char *_arg_buffer;

	/* The buffer will contain : nargs, {size, content} (x nargs)*/

	_arg_buffer = malloc(arg_buffer_size);

	/* We will begin the buffer with the number of args (which is stored as a char) */
	current_arg_offset += sizeof(char);

	while((arg_type = va_arg(varg_list, int)) != 0)
	{
		if (arg_type & STARPU_R || arg_type & STARPU_W || arg_type & STARPU_RW || arg_type & STARPU_SCRATCH || arg_type & STARPU_REDUX)
		{
			(void)va_arg(varg_list, starpu_data_handle_t);
		}
		else if (arg_type==STARPU_DATA_ARRAY)
		{
			(void)va_arg(varg_list, starpu_data_handle_t*);
			(void)va_arg(varg_list, int);
		}
		else if (arg_type==STARPU_VALUE)
		{
			/* We have a constant value: this should be followed by a pointer to the cst value and the size of the constant */
			void *ptr = va_arg(varg_list, void *);
			size_t cst_size = va_arg(varg_list, size_t);

			*(size_t *)(&(_arg_buffer)[current_arg_offset]) = cst_size;
			current_arg_offset += sizeof(size_t);

			memcpy(&_arg_buffer[current_arg_offset], ptr, cst_size);
			current_arg_offset += cst_size;

			nargs++;
			STARPU_ASSERT(current_arg_offset <= arg_buffer_size);
		}
		else if (arg_type==STARPU_CALLBACK)
		{
			(void)va_arg(varg_list, _starpu_callback_func_t);
		}
		else if (arg_type==STARPU_CALLBACK_WITH_ARG)
		{
			va_arg(varg_list, _starpu_callback_func_t);
			va_arg(varg_list, void *);
		}
		else if (arg_type==STARPU_CALLBACK_ARG)
		{
			(void)va_arg(varg_list, void *);
		}
		else if (arg_type==STARPU_PROLOGUE_CALLBACK)
		{
			va_arg(varg_list, _starpu_callback_func_t);
		}
		else if (arg_type==STARPU_PROLOGUE_CALLBACK_ARG)
		{
			(void)va_arg(varg_list, void *);
		}
		else if (arg_type==STARPU_PRIORITY)
		{
			(void)va_arg(varg_list, int);
		}
		else if (arg_type==STARPU_EXECUTE_ON_NODE)
		{
			(void)va_arg(varg_list, int);
		}
		else if (arg_type==STARPU_EXECUTE_ON_DATA)
		{
			(void)va_arg(varg_list, starpu_data_handle_t);
		}
		else if (arg_type==STARPU_EXECUTE_ON_WORKER)
		{
			va_arg(varg_list, int);
		}
		else if (arg_type==STARPU_SCHED_CTX)
		{
			(void)va_arg(varg_list, unsigned);
		}
		else if (arg_type==STARPU_HYPERVISOR_TAG)
		{
			(void)va_arg(varg_list, int);
		}
		else if (arg_type==STARPU_FLOPS)
		{
			(void)va_arg(varg_list, double);
		}
		else if (arg_type==STARPU_TAG)
		{
			(void)va_arg(varg_list, starpu_tag_t);
		}
		else
		{
			STARPU_ABORT_MSG("Unrecognized argument %d\n", arg_type);
		}
	}

	if (nargs)
	{
		_arg_buffer[0] = nargs;
	}
	else
	{
		free(_arg_buffer);
		_arg_buffer = NULL;
	}

	*arg_buffer = _arg_buffer;
	va_end(varg_list);
	return 0;
}

void _starpu_insert_task_create(void *arg_buffer, size_t arg_buffer_size, struct starpu_codelet *cl, struct starpu_task **task, va_list varg_list)
{
	int arg_type;
	unsigned current_buffer = 0;

	while((arg_type = va_arg(varg_list, int)) != 0)
	{
		if (arg_type & STARPU_R || arg_type & STARPU_W || arg_type & STARPU_RW || arg_type & STARPU_SCRATCH || arg_type & STARPU_REDUX)
		{
			/* We have an access mode : we expect to find a handle */
			starpu_data_handle_t handle = va_arg(varg_list, starpu_data_handle_t);
			enum starpu_data_access_mode mode = (enum starpu_data_access_mode) arg_type & ~STARPU_SSEND;

			STARPU_ASSERT(cl != NULL);

			STARPU_TASK_SET_HANDLE((*task), handle, current_buffer);
			if (STARPU_CODELET_GET_MODE(cl, current_buffer))
			{
				STARPU_ASSERT_MSG(STARPU_CODELET_GET_MODE(cl, current_buffer) == mode,
						   "The codelet <%s> defines the access mode %d for the buffer %d which is different from the mode %d given to starpu_insert_task\n",
						  cl->name, STARPU_CODELET_GET_MODE(cl, current_buffer),
						  current_buffer, mode);
			}
			else
			{
#ifdef STARPU_DEVEL
#  warning shall we print a warning to the user
#endif
				STARPU_CODELET_SET_MODE(cl, mode, current_buffer);
			}

			current_buffer++;
		}
		else if (arg_type == STARPU_DATA_ARRAY)
		{
			// Expect to find a array of handles and its size
			starpu_data_handle_t *handles = va_arg(varg_list, starpu_data_handle_t *);
			int nb_handles = va_arg(varg_list, int);

			int i;
			for(i=0 ; i<nb_handles ; i++)
			{
				STARPU_TASK_SET_HANDLE((*task), handles[i], current_buffer);
				current_buffer++;
			}

		}
		else if (arg_type==STARPU_VALUE)
		{
			(void)va_arg(varg_list, void *);
			(void)va_arg(varg_list, size_t);
		}
		else if (arg_type==STARPU_CALLBACK)
		{
			(*task)->callback_func = va_arg(varg_list, _starpu_callback_func_t);
		}
		else if (arg_type==STARPU_CALLBACK_WITH_ARG)
		{
			(*task)->callback_func = va_arg(varg_list, _starpu_callback_func_t);
			(*task)->callback_arg = va_arg(varg_list, void *);
		}
		else if (arg_type==STARPU_CALLBACK_ARG)
		{
			(*task)->callback_arg = va_arg(varg_list, void *);
		}
		else if (arg_type==STARPU_PROLOGUE_CALLBACK)
		{
			(*task)->prologue_callback_func = va_arg(varg_list, _starpu_callback_func_t);
		}
		else if (arg_type==STARPU_PROLOGUE_CALLBACK_ARG)
		{
			(*task)->prologue_callback_arg = va_arg(varg_list, void *);
		}
		else if (arg_type==STARPU_PRIORITY)
		{
			/* Followed by a priority level */
			int prio = va_arg(varg_list, int);
			(*task)->priority = prio;
		}
		else if (arg_type==STARPU_EXECUTE_ON_NODE)
		{
			(void)va_arg(varg_list, int);
		}
		else if (arg_type==STARPU_EXECUTE_ON_DATA)
		{
			(void)va_arg(varg_list, starpu_data_handle_t);
		}
		else if (arg_type==STARPU_EXECUTE_ON_WORKER)
		{
			int worker = va_arg(varg_list, int);
			if (worker != -1)
			{
				(*task)->workerid = worker;
				(*task)->execute_on_a_specific_worker = 1;
			}
		}
		else if (arg_type==STARPU_SCHED_CTX)
		{
			unsigned sched_ctx = va_arg(varg_list, unsigned);
			(*task)->sched_ctx = sched_ctx;
		}
		else if (arg_type==STARPU_HYPERVISOR_TAG)
		{
			int hypervisor_tag = va_arg(varg_list, int);
			(*task)->hypervisor_tag = hypervisor_tag;
		}
		else if (arg_type==STARPU_FLOPS)
		{
			double flops = va_arg(varg_list, double);
			(*task)->flops = flops;
		}
		else if (arg_type==STARPU_TAG)
		{
			starpu_tag_t tag = va_arg(varg_list, starpu_tag_t);
			(*task)->tag_id = tag;
			(*task)->use_tag = 1;
		}
		else
		{
			STARPU_ABORT_MSG("Unrecognized argument %d\n", arg_type);
		}
	}

	va_end(varg_list);

	STARPU_ASSERT(cl == NULL || current_buffer == cl->nbuffers);

	(*task)->cl = cl;
	(*task)->cl_arg = arg_buffer;
	(*task)->cl_arg_size = arg_buffer_size;

}

int _starpu_insert_task_create_and_submit(void *arg_buffer, size_t arg_buffer_size, struct starpu_codelet *cl, struct starpu_task **task, va_list varg_list)
{
	_starpu_insert_task_create(arg_buffer, arg_buffer_size, cl, task, varg_list);

	int ret = starpu_task_submit(*task);

	if (STARPU_UNLIKELY(ret == -ENODEV))
	{
		fprintf(stderr, "submission of task %p wih codelet %p failed (symbol `%s') (err: ENODEV)\n",
			*task, (*task)->cl,
			(cl == NULL) ? "none" :
			(*task)->cl->name ? (*task)->cl->name :
			((*task)->cl->model && (*task)->cl->model->symbol)?(*task)->cl->model->symbol:"none");
	}

	return ret;
}
