/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2010, 2012-2013, 2015-2016  Université de Bordeaux
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

#include <starpu.h>
#include <common/config.h>
#include <common/utils.h>
#include <common/thread.h>
#include <core/workers.h>
#include <errno.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <fcntl.h>

#if defined(_WIN32) && !defined(__CYGWIN__)
#include <io.h>
#include <sys/locking.h>
#define mkdir(path, mode) mkdir(path)
#if !defined(__MINGW32__)
#define ftruncate(fd, length) _chsize(fd, length)
#endif
#endif

int _starpu_silent;

void _starpu_util_init(void)
{
	_starpu_silent = starpu_get_env_number_default("STARPU_SILENT", 0);
	STARPU_HG_DISABLE_CHECKING(_starpu_silent);
}

#if defined(_WIN32) && !defined(__CYGWIN__) && !defined(__MINGW32__)
#include <direct.h>
static char * dirname(char * path)
{
   char drive[_MAX_DRIVE];
   char dir[_MAX_DIR];
   /* Remove trailing slash */
   while (strlen(path) > 0 && (*(path+strlen(path)-1) == '/' || *(path+strlen(path)-1) == '\\'))
      *(path+strlen(path)-1) = '\0';
   _splitpath(path, drive, dir, NULL, NULL);
   _makepath(path, drive, dir, NULL, NULL);
   return path;
}
#else
#include <libgen.h>
#endif

/* Function with behaviour like `mkdir -p'. This function was adapted from
 * http://niallohiggins.com/2009/01/08/mkpath-mkdir-p-alike-in-c-for-unix/ */

int _starpu_mkpath(const char *s, mode_t mode)
{
	int olderrno;
	char *q, *r = NULL, *path = NULL, *up = NULL;
	int rv;

	rv = -1;
	if (strcmp(s, ".") == 0 || strcmp(s, "/") == 0
#if defined(_WIN32)
		/* C:/ or C:\ */
		|| (s[0] && s[1] == ':' && (s[2] == '/' || s[2] == '\\') && !s[3])
#endif
		)
		return 0;

	if ((path = strdup(s)) == NULL)
		STARPU_ABORT();

	if ((q = strdup(s)) == NULL)
		STARPU_ABORT();

	if ((r = dirname(q)) == NULL)
		goto out;

	if ((up = strdup(r)) == NULL)
		STARPU_ABORT();

	if ((_starpu_mkpath(up, mode) == -1) && (errno != EEXIST))
		goto out;

	struct stat sb;
	if (stat(path, &sb) == 0)
	{
		if (!S_ISDIR(sb.st_mode))
		{
			fprintf(stderr,"Error: %s is not a directory:\n", path);
			STARPU_ABORT();
		}
		/* It already exists and is a directory.  */
		rv = 0;
	}
	else
	{
		if ((mkdir(path, mode) == -1) && (errno != EEXIST))
			rv = -1;
		else
			rv = 0;
	}

out:
	olderrno = errno;
	if (up)
		free(up);

	free(q);
	free(path);
	errno = olderrno;
	return rv;
}

void _starpu_mkpath_and_check(const char *path, mode_t mode)
{
	int ret;

	ret = _starpu_mkpath(path, mode);

	if (ret == -1 && errno != EEXIST)
	{
		fprintf(stderr,"Error making StarPU directory %s:\n", path);
		perror("mkdir");
		STARPU_ABORT();
	}
}

int _starpu_ftruncate(int fd, size_t length)
{
	return ftruncate(fd, length);
}

int _starpu_fftruncate(FILE *file, size_t length)
{
	return ftruncate(fileno(file), length);
}

int _starpu_frdlock(FILE *file)
{
	int ret;
#if defined(_WIN32) && !defined(__CYGWIN__)
	do {
		ret = _locking(fileno(file), _LK_RLCK, 10);
	} while (ret == EDEADLOCK);
#else
	struct flock lock = {
		.l_type = F_RDLCK,
		.l_whence = SEEK_SET,
		.l_start = 0,
		.l_len = 0
	};
	ret = fcntl(fileno(file), F_SETLKW, &lock);
#endif
	STARPU_ASSERT(ret == 0);
	return ret;
}

int _starpu_frdunlock(FILE *file)
{
	int ret;
#if defined(_WIN32) && !defined(__CYGWIN__)
#  ifndef _LK_UNLCK
#    define _LK_UNLCK _LK_UNLOCK
#  endif
	ret = _lseek(fileno(file), 0, SEEK_SET);
	STARPU_ASSERT(ret == 0);
	ret = _locking(fileno(file), _LK_UNLCK, 10);
#else
	struct flock lock = {
		.l_type = F_UNLCK,
		.l_whence = SEEK_SET,
		.l_start = 0,
		.l_len = 0
	};
	ret = fcntl(fileno(file), F_SETLKW, &lock);
#endif
	STARPU_ASSERT(ret == 0);
	return ret;
}

int _starpu_fwrlock(FILE *file)
{
	int ret;
#if defined(_WIN32) && !defined(__CYGWIN__)
	ret = _lseek(fileno(file), 0, SEEK_SET);
	STARPU_ASSERT(ret == 0);
	do {
		ret = _locking(fileno(file), _LK_LOCK, 10);
	} while (ret == EDEADLOCK);
#else
	struct flock lock = {
		.l_type = F_WRLCK,
		.l_whence = SEEK_SET,
		.l_start = 0,
		.l_len = 0
	};
	ret = fcntl(fileno(file), F_SETLKW, &lock);
#endif
	STARPU_ASSERT(ret == 0);
	return ret;
}

int _starpu_fwrunlock(FILE *file)
{
	return _starpu_frdunlock(file);
}

int _starpu_check_mutex_deadlock(starpu_pthread_mutex_t *mutex)
{
	int ret;
	ret = starpu_pthread_mutex_trylock(mutex);
	if (!ret)
	{
		STARPU_PTHREAD_MUTEX_UNLOCK(mutex);
		return 0;
	}

	if (ret == EBUSY)
		return 0;

	STARPU_ASSERT (ret != EDEADLK);

	return 1;
}

char *_starpu_get_home_path(void)
{
	char *path = starpu_getenv("XDG_CACHE_HOME");
	if (!path)
		path = starpu_getenv("STARPU_HOME");
#ifdef _WIN32
	if (!path)
		path = starpu_getenv("LOCALAPPDATA");
	if (!path)
		path = starpu_getenv("USERPROFILE");
#endif
	if (!path)
		path = starpu_getenv("HOME");
	if (!path)
	{
		static int warn;
		path = starpu_getenv("TMPDIR");
		if (!path)
			path = "/tmp";
		if (!warn)
		{
			warn = 1;
			_STARPU_DISP("couldn't find a $STARPU_HOME place to put .starpu data, using %s\n", path);
		}
	}
	return path;
}

void _starpu_gethostname(char *hostname, size_t size)
{
	char *forced_hostname = starpu_getenv("STARPU_HOSTNAME");
	if (forced_hostname && forced_hostname[0])
	{
		snprintf(hostname, size-1, "%s", forced_hostname);
		hostname[size-1] = 0;
	}
	else
	{
		char *c;
		gethostname(hostname, size-1);
		hostname[size-1] = 0;
		c = strchr(hostname, '.');
		if (c)
			*c = 0;
	}
}

char *starpu_getenv(const char *str)
{
#ifndef STARPU_SIMGRID
#if defined(STARPU_DEVEL) || defined(STARPU_DEBUG)
	struct _starpu_worker * worker;

	worker = _starpu_get_local_worker_key();

	if (worker && worker->worker_is_initialized)
		_STARPU_DISP( "getenv should not be called from running workers, only for main() or worker initialization, since it is not reentrant\n");
#endif
#endif
	return getenv(str);
}
