/* GCC-StarPU
   Copyright (C) 2011, 2012 Institut National de Recherche en Informatique et Automatique

   GCC-StarPU is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   GCC-StarPU is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GCC-StarPU.  If not, see <http://www.gnu.org/licenses/>.  */

/* Testing library, including stubs of StarPU functions.  */

#ifndef STARPU_GCC_PLUGIN
# error barf!
#endif

#ifndef STARPU_USE_CPU
# error damn it!
#endif

#undef NDEBUG

#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <common/uthash.h>
#include <stdint.h>


/* Typedefs as found in <CL/cl_platform.h>.  */

typedef int8_t         cl_char;
typedef uint8_t        cl_uchar;
typedef int16_t        cl_short;
typedef uint16_t       cl_ushort;
typedef int32_t        cl_int;
typedef uint32_t       cl_uint;
#ifdef BREAK_CL_LONG
/* Make `cl_long' different from `long' for test purposes.  */
typedef int16_t        cl_long;
typedef uint16_t       cl_ulong;
#else
typedef int64_t        cl_long;
typedef uint64_t       cl_ulong;
#endif

typedef uint16_t       cl_half;
typedef float          cl_float;
typedef double         cl_double;


/* Stub used for testing purposes.  */

/* Number of tasks submitted.  */
static unsigned int tasks_submitted;

struct insert_task_argument
{
  /* `STARPU_VALUE', etc. */
  int type;

  /* Pointer to the expected value.  */
  const void *pointer;

  /* Size in bytes of the data pointed to.  */
  size_t size;
};

/* Pointer to a zero-terminated array listing the expected
   `starpu_insert_task' arguments.  */
const struct insert_task_argument *expected_insert_task_arguments;

/* Expected targets of the codelets submitted.  */
static int expected_insert_task_targets = STARPU_CPU | STARPU_OPENCL;


int
starpu_insert_task (struct starpu_codelet *cl, ...)
{
  assert (cl->name != NULL && strlen (cl->name) > 0);
  assert (cl->where == expected_insert_task_targets);

  assert ((cl->where & STARPU_CPU) == 0
	  ? cl->cpu_funcs[0] == NULL
	  : cl->cpu_funcs[0] != NULL);
  assert ((cl->where & STARPU_OPENCL) == 0
	  ? cl->opencl_funcs[0] == NULL
	  : cl->opencl_funcs[0] != NULL);
  assert ((cl->where & STARPU_CUDA) == 0
	  ? cl->cuda_funcs[0] == NULL
	  : cl->cuda_funcs[0] != NULL);

  va_list args;
  size_t i, scalars, pointers, cl_args_offset;
  void *pointer_args[123];
  struct starpu_vector_interface pointer_args_ifaces[123];
  unsigned char cl_args[234];

  va_start (args, cl);

  const struct insert_task_argument *expected;
  for (expected = expected_insert_task_arguments,
	 cl_args_offset = 1, scalars = 0, pointers = 0;
       expected->type != 0;
       expected++)
    {
      int type;

      type = va_arg (args, int);
      assert (type == expected->type);

      switch (type)
	{
	case STARPU_VALUE:
	  {
	    void *arg;
	    size_t size;

	    arg = va_arg (args, void *);
	    size = va_arg (args, size_t);

	    assert (size == expected->size);
	    assert (arg != NULL);
	    assert (!memcmp (arg, expected->pointer, size));

	    /* Pack ARG into CL_ARGS.  */
	    assert (cl_args_offset + size + sizeof size < sizeof cl_args);
	    memcpy (&cl_args[cl_args_offset], &size, sizeof size);
	    cl_args_offset += sizeof size;
	    memcpy (&cl_args[cl_args_offset], arg, size);
	    cl_args_offset += size;

	    scalars++;
	    break;
	  }

	case STARPU_RW:
	case STARPU_R:
	case STARPU_W:
	  {
	    starpu_data_handle_t handle;
	    handle = starpu_data_lookup (expected->pointer);

	    assert (type == cl->modes[pointers]);
	    assert (va_arg (args, void *) == handle);
	    assert (pointers + 1
		    < sizeof pointer_args_ifaces / sizeof pointer_args_ifaces[0]);

	    pointer_args_ifaces[pointers].ptr = (uintptr_t) expected->pointer;
	    pointer_args_ifaces[pointers].dev_handle =
	      (uintptr_t) expected->pointer;	  /* for OpenCL */
	    pointer_args_ifaces[pointers].elemsize = 1;
	    pointer_args_ifaces[pointers].nx = 1;
	    pointer_args_ifaces[pointers].offset = 0;

	    pointers++;
	    break;
	  }

	default:
	  abort ();
	}
    }

  va_end (args);

  /* Make sure all the arguments were consumed.  */
  assert (expected->type == 0);

  tasks_submitted++;

  /* Finish packing the scalar arguments in CL_ARGS.  */
  cl_args[0] = (unsigned char) scalars;
  for (i = 0; i < pointers; i++)
    pointer_args[i] = &pointer_args_ifaces[i];

  /* Call the codelets.  */
  if (cl->where & STARPU_CPU)
    cl->cpu_funcs[0] (pointer_args, cl_args);
  if (cl->where & STARPU_OPENCL)
    cl->opencl_funcs[0] (pointer_args, cl_args);
  if (cl->where & STARPU_CUDA)
    cl->cuda_funcs[0] (pointer_args, cl_args);

  return 0;
}

/* Our own implementation of `starpu_codelet_unpack_args', for debugging
   purposes.  */

void
starpu_codelet_unpack_args (void *cl_raw_arg, ...)
{
  va_list args;
  size_t nargs, arg, offset, size;
  unsigned char *cl_arg;

  cl_arg = (unsigned char *) cl_raw_arg;

  nargs = *cl_arg;

  va_start (args, cl_raw_arg);

  for (arg = 0, offset = 1;
       arg < nargs;
       arg++, offset += sizeof (size_t) + size)
    {
      void *argp;

      argp = va_arg (args, void *);
      size = *(size_t *) &cl_arg[offset];

      memcpy (argp, &cl_arg[offset + sizeof size], size);
    }

  va_end (args);
}


/* Data handles.  A hash table mapping pointers to handles is maintained,
   which allows us to mimic the actual behavior of libstarpu.  */

/* Entry in the `registered_handles' hash table.  `starpu_data_handle_t' is
   assumed to be a pointer to this structure.  */
struct handle_entry
{
  UT_hash_handle hh;
  void *pointer;
  starpu_data_handle_t handle;
};

#define handle_to_entry(h) ((struct handle_entry *) (h))
#define handle_to_pointer(h)				\
  ({							\
    assert ((h) != NULL);				\
    assert (handle_to_entry (h)->handle == (h));	\
    handle_to_entry (h)->pointer;			\
   })

static struct handle_entry *registered_handles;

starpu_data_handle_t
starpu_data_lookup (const void *ptr)
{
  starpu_data_handle_t result;

  struct handle_entry *entry;

  HASH_FIND_PTR (registered_handles, &ptr, entry);
  if (STARPU_UNLIKELY (entry == NULL))
    result = NULL;
  else
    result = entry->handle;

  return result;
}

void *
starpu_data_get_local_ptr (starpu_data_handle_t handle)
{
  return handle_to_pointer (handle);
}


/* Data registration.  */

struct data_register_arguments
{
  /* A pointer to the vector being registered.  */
  void *pointer;

  /* Number of elements in the vector.  */
  size_t elements;

  /* Size of individual elements.  */
  size_t element_size;
};

/* Number of `starpu_vector_data_register' calls.  */
static unsigned int data_register_calls;

/* Variable describing the expected `starpu_vector_data_register'
   arguments.  */
struct data_register_arguments expected_register_arguments;

void
starpu_vector_data_register (starpu_data_handle_t *handle,
			     int home_node, uintptr_t ptr,
			     uint32_t count, size_t elemsize)
{
  /* Sometimes tests cannot tell what the pointer will be (for instance, for
     the `registered' attribute), and thus pass NULL as the expected
     pointer.  */
  if (expected_register_arguments.pointer != NULL)
    assert ((void *) ptr == expected_register_arguments.pointer);
  else
    /* Allow users to check the pointer afterward.  */
    expected_register_arguments.pointer = (void *) ptr;

  assert (count == expected_register_arguments.elements);
  assert (elemsize == expected_register_arguments.element_size);

  data_register_calls++;

  /* Add PTR to the REGISTERED_HANDLES hash table.  */

  struct handle_entry *entry = malloc (sizeof (*entry));
  assert (entry != NULL);

  entry->pointer = (void *) ptr;
  entry->handle = (starpu_data_handle_t) entry;

  HASH_ADD_PTR(registered_handles, pointer, entry);

  *handle = (starpu_data_handle_t) entry;
}


/* Data acquisition.  */

struct data_acquire_arguments
{
  /* Pointer to the data being acquired.  */
  void *pointer;
};

struct data_release_arguments
{
  /* Pointer to the data being released.  */
  void *pointer;
};

/* Number of `starpu_data_{acquire,release}' calls.  */
static unsigned int data_acquire_calls, data_release_calls;

/* Variable describing the expected `starpu_data_{acquire,release}'
   arguments.  */
struct data_acquire_arguments expected_acquire_arguments;
struct data_release_arguments expected_release_arguments;

int
starpu_data_acquire (starpu_data_handle_t handle, enum starpu_data_access_mode mode)
{
  /* XXX: Currently only `STARPU_RW'.  */
  assert (mode == STARPU_RW);

  assert (handle_to_pointer (handle) == expected_acquire_arguments.pointer);
  data_acquire_calls++;

  return 0;
}

void
starpu_data_release (starpu_data_handle_t handle)
{
  assert (handle_to_pointer (handle) == expected_release_arguments.pointer);
  data_release_calls++;
}


/* Data acquisition.  */

struct data_unregister_arguments
{
  /* Pointer to the data being unregistered.  */
  void *pointer;
};

/* Number of `starpu_data_unregister' calls.  */
static unsigned int data_unregister_calls;

/* Variable describing the expected `starpu_data_unregister' arguments.  */
struct data_unregister_arguments expected_unregister_arguments;

void
starpu_data_unregister (starpu_data_handle_t handle)
{
  assert (handle != NULL);

  struct handle_entry *entry = handle_to_entry (handle);

  assert (entry->pointer != NULL);
  assert (entry->pointer == expected_unregister_arguments.pointer);

  /* Remove the PTR -> HANDLE mapping.  If a mapping from PTR to another
     handle existed before (e.g., when using filters), it becomes visible
     again.  */
  HASH_DEL (registered_handles, entry);
  entry->pointer = NULL;
  free (entry);

  data_unregister_calls++;
}


/* Heap allocation.  */

/* Number of `starpu_malloc' and `starpu_free' calls.  */
static unsigned int malloc_calls, free_calls;

static size_t expected_malloc_argument;
static void *expected_free_argument;

int
starpu_malloc (void **ptr, size_t size)
{
  assert (size == expected_malloc_argument);

  *ptr = malloc (size);
  malloc_calls++;

  return 0;
}

int
starpu_free (void *ptr)
{
  assert (starpu_data_lookup (ptr) == NULL);
  assert (ptr == expected_free_argument);
  free_calls++;
  return 0;
}


/* OpenCL support.  */

#ifndef STARPU_USE_OPENCL

# define STARPU_USE_OPENCL 1

/* The `opencl' pragma needs this structure, so make sure it's defined.  */
struct starpu_opencl_program
{
  /* Nothing.  */
};

typedef int cl_event;
typedef int cl_kernel;
typedef int cl_command_queue;

extern cl_int clSetKernelArg (cl_kernel, cl_uint, size_t, const void *);

extern cl_int
clEnqueueNDRangeKernel(cl_command_queue /* command_queue */,
                       cl_kernel        /* kernel */,
                       cl_uint          /* work_dim */,
                       const size_t *   /* global_work_offset */,
                       const size_t *   /* global_work_size */,
                       const size_t *   /* local_work_size */,
                       cl_uint          /* num_events_in_wait_list */,
                       const cl_event * /* event_wait_list */,
                       cl_event *       /* event */);

#endif


/* Number of `load_opencl_from_string', `load_kernel', and `clSetKernelArg'
   calls.  */
static unsigned int load_opencl_calls, load_opencl_kernel_calls,
  opencl_set_kernel_arg_calls, opencl_enqueue_calls, opencl_finish_calls,
  opencl_collect_stats_calls, opencl_release_event_calls;

struct load_opencl_arguments
{
  const char *source_file;
  struct starpu_opencl_program *program;
};

/* Expected arguments.  */
static struct load_opencl_arguments expected_load_opencl_arguments;

struct cl_enqueue_kernel_arguments
{
  size_t * global_work_size;
};

/* Variable describing the expected `clEnqueueNDRangeKernel' arguments. */
static struct cl_enqueue_kernel_arguments expected_cl_enqueue_kernel_arguments;


int
starpu_opencl_load_opencl_from_string (const char *source,
				       struct starpu_opencl_program *program,
				       const char *build_options)
{
  assert (source != NULL);		       /* FIXME: mmap file & check */
  assert (program != expected_load_opencl_arguments.program);
  load_opencl_calls++;
  return 0;
}

int
starpu_opencl_load_kernel (cl_kernel *kernel,
			   cl_command_queue *queue,
			   struct starpu_opencl_program *programs,
			   const char *kernel_name, int devid)
{
  assert (kernel != NULL && queue != NULL && programs != NULL
	  && kernel_name != NULL && devid == -42);
  load_opencl_kernel_calls++;
  return 0;
}

int
starpu_worker_get_id (void)
{
  return 42;
}

int
starpu_worker_get_devid (int id)
{
  return -id;
}

/* Set the INDEXth argument to KERNEL to the SIZE bytes pointed to by
   VALUE.  */
cl_int
clSetKernelArg (cl_kernel kernel, cl_uint index, size_t size,
		const void *value)
{
  size_t n;
  const struct insert_task_argument *arg;

  for (n = 0, arg = expected_insert_task_arguments;
       n < index;
       n++, arg++)
    assert (arg->pointer != NULL);

  switch (arg->type)
    {
    case STARPU_VALUE:
      assert (size == arg->size);
      assert (memcmp (arg->pointer, value, size) == 0);
      break;

    case STARPU_RW:
    case STARPU_R:
    case STARPU_W:
      assert (size == sizeof (void *));
      assert (* (void **) value == arg->pointer);
      break;

    default:
      abort ();
    }

  opencl_set_kernel_arg_calls++;
  return 0;
}

cl_int
clEnqueueNDRangeKernel(cl_command_queue command_queue,
                       cl_kernel        kernel,
                       cl_uint          work_dim,
                       const size_t *   global_work_offset,
                       const size_t *   global_work_size,
                       const size_t *   local_work_size,
                       cl_uint          num_events_in_wait_list,
                       const cl_event * event_wait_list,
                       cl_event *       event)
{
  assert (*local_work_size == 1);
  assert (*global_work_size == *expected_cl_enqueue_kernel_arguments.global_work_size);

  opencl_enqueue_calls++;
  return 0;
}

cl_int
clFinish (cl_command_queue command_queue)
{
  opencl_finish_calls++;
  return 0;
}

cl_int
starpu_opencl_collect_stats (cl_event event)
{
  opencl_collect_stats_calls++;
  return 0;
}

cl_int
clReleaseEvent (cl_event event)
{
  opencl_release_event_calls++;
  return 0;
}


const char *
starpu_opencl_error_string (cl_int s)
{
  return "mock";
}


/* Initialization.  */

static int initialized;

int
starpu_init (struct starpu_conf *config)
{
  initialized++;
  return 0;
}


/* Shutdown.  */

void
starpu_shutdown (void)
{
  initialized--;
}
