/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2010-2014  Université de Bordeaux
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

#ifndef __STARPU_OPENCL_H__
#define __STARPU_OPENCL_H__

#include <starpu_config.h>
#ifdef STARPU_USE_OPENCL
#ifdef __APPLE__
#include <OpenCL/cl.h>
#else
#include <CL/cl.h>
#endif
#include <assert.h>

#ifdef __cplusplus
extern "C"
{
#endif

const char *starpu_opencl_error_string(cl_int status);
void starpu_opencl_display_error(const char *func, const char *file, int line, const char *msg, cl_int status);
#define STARPU_OPENCL_DISPLAY_ERROR(status) \
	starpu_opencl_display_error(__starpu_func__, __FILE__, __LINE__, NULL, status)

static __starpu_inline void starpu_opencl_report_error(const char *func, const char *file, int line, const char *msg, cl_int status)
{
	starpu_opencl_display_error(func, file, line, msg, status);
	assert(0);
}
#define STARPU_OPENCL_REPORT_ERROR(status)			\
	starpu_opencl_report_error(__starpu_func__, __FILE__, __LINE__, NULL, status)

#define STARPU_OPENCL_REPORT_ERROR_WITH_MSG(msg, status)			\
	starpu_opencl_report_error(__starpu_func__, __FILE__, __LINE__, msg, status)

struct starpu_opencl_program
{
	cl_program programs[STARPU_MAXOPENCLDEVS];
};

void starpu_opencl_get_context(int devid, cl_context *context);
void starpu_opencl_get_device(int devid, cl_device_id *device);
void starpu_opencl_get_queue(int devid, cl_command_queue *queue);
void starpu_opencl_get_current_context(cl_context *context);
void starpu_opencl_get_current_queue(cl_command_queue *queue);

void starpu_opencl_load_program_source(const char *source_file_name, char *located_file_name, char *located_dir_name, char *opencl_program_source);
int starpu_opencl_compile_opencl_from_file(const char *source_file_name, const char *build_options);
int starpu_opencl_compile_opencl_from_string(const char *opencl_program_source, const char *file_name, const char *build_options);

int starpu_opencl_load_binary_opencl(const char *kernel_id, struct starpu_opencl_program *opencl_programs);

int starpu_opencl_load_opencl_from_file(const char *source_file_name, struct starpu_opencl_program *opencl_programs, const char *build_options);
int starpu_opencl_load_opencl_from_string(const char *opencl_program_source, struct starpu_opencl_program *opencl_programs, const char *build_options);
int starpu_opencl_unload_opencl(struct starpu_opencl_program *opencl_programs);

int starpu_opencl_load_kernel(cl_kernel *kernel, cl_command_queue *queue, struct starpu_opencl_program *opencl_programs, const char *kernel_name, int devid);
int starpu_opencl_release_kernel(cl_kernel kernel);

int starpu_opencl_collect_stats(cl_event event);

int starpu_opencl_set_kernel_args(cl_int *err, cl_kernel *kernel, ...);

cl_int starpu_opencl_allocate_memory(int devid, cl_mem *addr, size_t size, cl_mem_flags flags);

cl_int starpu_opencl_copy_ram_to_opencl(void *ptr, unsigned src_node, cl_mem buffer, unsigned dst_node, size_t size, size_t offset, cl_event *event, int *ret);

cl_int starpu_opencl_copy_opencl_to_ram(cl_mem buffer, unsigned src_node, void *ptr, unsigned dst_node, size_t size, size_t offset, cl_event *event, int *ret);

cl_int starpu_opencl_copy_opencl_to_opencl(cl_mem src, unsigned src_node, size_t src_offset, cl_mem dst, unsigned dst_node, size_t dst_offset, size_t size, cl_event *event, int *ret);

cl_int starpu_opencl_copy_async_sync(uintptr_t src, size_t src_offset, unsigned src_node, uintptr_t dst, size_t dst_offset, unsigned dst_node, size_t size, cl_event *event);

#ifdef __cplusplus
}
#endif

#endif /* STARPU_USE_OPENCL */
#endif /* __STARPU_OPENCL_H__ */

