/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2010, 2014-2015  Université de Bordeaux
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

#include <common/config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>

#if defined(_WIN32) && !defined(__MINGW32__) && !defined(__CYGWIN__)
#include <windows.h>
#else
#include <sys/time.h>
#endif

#ifdef STARPU_QUICK_CHECK
/* Quick checks are supposed to be real quick, typically less than 1s each, sometimes 10s */
#define  DEFAULT_TIMEOUT       30
#elif !defined(STARPU_LONG_CHECK)
/* Normal checks are supposed to be short enough, typically less than 10s each, sometimes 1-2m */
#define  DEFAULT_TIMEOUT       300
#else
/* Long checks can be very long */
#define  DEFAULT_TIMEOUT       1800
#endif
#define  AUTOTEST_SKIPPED_TEST 77

static pid_t child_pid = 0;
static int   timeout;

#if defined(_WIN32) && !defined(__MINGW32__) && !defined(__CYGWIN__)
static int mygettimeofday(struct timeval *tv, void *tz)
{
	if (tv)
	{
		FILETIME ft;
		unsigned long long res;
		GetSystemTimeAsFileTime(&ft);
		/* 100-nanosecond intervals since January 1, 1601 */
		res = ft.dwHighDateTime;
		res <<= 32;
		res |= ft.dwLowDateTime;
		res /= 10;
		/* Now we have microseconds */
		res -= (((1970-1601)*365) + 89) * 24ULL * 3600ULL * 1000000ULL;
		/* Now we are based on epoch */
		tv->tv_sec = res / 1000000ULL;
		tv->tv_usec = res % 1000000ULL;
	}
}
#else
#define mygettimeofday(tv,tz) gettimeofday(tv,tz)
#endif

static void launch_gdb(const char *exe)
{
#ifdef STARPU_GDB_PATH
# define CORE_FILE "core"
# define GDB_ALL_COMMAND "thread apply all bt full"
# define GDB_COMMAND "bt full"
	int err;
	pid_t pid;
	struct stat st;
	const char *top_builddir;
	char *gdb;

	err = stat(CORE_FILE, &st);
	if (err != 0)
	{
		fprintf(stderr, "while looking for core file of %s: %s: %m\n",
			exe, CORE_FILE);
		return;
	}

	if (!(st.st_mode & S_IFREG))
	{
		fprintf(stderr, CORE_FILE ": not a regular file\n");
		return;
	}

	top_builddir = getenv("top_builddir");

	pid = fork();
	switch (pid)
	{
	case 0:					  /* kid */
		if (top_builddir != NULL)
		{
			/* Run gdb with Libtool.  */
			gdb = alloca(strlen(top_builddir)
				     + sizeof("/libtool") + 1);
			strcpy(gdb, top_builddir);
			strcat(gdb, "/libtool");
			err = execl(gdb, "gdb", "--mode=execute",
				    STARPU_GDB_PATH, "--batch",
				    "-ex", GDB_COMMAND,
				    "-ex", GDB_ALL_COMMAND,
				    exe, CORE_FILE, NULL);
		}
		else
		{
			/* Run gdb directly  */
			gdb = STARPU_GDB_PATH;
			err = execl(gdb, "gdb", "--batch",
				    "-ex", GDB_COMMAND,
				    "-ex", GDB_ALL_COMMAND,
				    exe, CORE_FILE, NULL);
		}
		if (err != 0)
		{
			fprintf(stderr, "while launching `%s': %m\n", gdb);
			exit(EXIT_FAILURE);
		}
		exit(EXIT_SUCCESS);
		break;

	case -1:
		fprintf(stderr, "fork: %m\n");
		return;

	default:				  /* parent */
		{
			pid_t who;
			int status;
			who = waitpid(pid, &status, 0);
			if (who != pid)
				fprintf(stderr, "while waiting for gdb "
					"process %d: %m\n", pid);
		}
	}
# undef GDB_COMMAND
# undef GDB_ALL_COMMAND
# undef CORE_FILE
#endif	/* STARPU_GDB_PATH */
}

static char *test_name;

static void test_cleaner(int sig)
{
	pid_t child_gid;
	int status;

	// send signal to all loader family members
	fprintf(stderr, "[error] test %s has been blocked for %d seconds. Mark it as failed\n", test_name, timeout);
	child_gid = getpgid(child_pid);
	kill(-child_gid, SIGQUIT);
	waitpid(child_pid, &status, 0);
	launch_gdb(test_name);
	exit(EXIT_FAILURE);
}

static void decode(char **src, char *motif, const char *value)
{
	if (*src)
	{
		char *y = strstr(*src, motif);
		if (y && value == NULL)
		{
			fprintf(stderr, "error: $%s undefined\n", motif);
			exit(EXIT_FAILURE);
		}
		while (y)
		{
			char *neo = calloc(strlen(*src)-strlen(motif)+strlen(value)+1, sizeof(char));
			char *to = neo;

			to = strncpy(to, *src, y - *src); to += y - *src;
			to = strcpy(to, value); to += strlen(value);
			strcpy(to, y+strlen(motif));

			*src = neo;
			y = strstr(*src, motif);
		}
	}
}

int main(int argc, char *argv[])
{
	int   child_exit_status;
	char *test_args;
	int   status;
	char *launcher;
	char *launcher_args;
	struct sigaction sa;
	int   ret;
	struct timeval start;
	struct timeval end;
	double timing;

	test_args = NULL;
	timeout = 0;
	test_name = argv[1];

	if (!test_name)
	{
		fprintf(stderr, "[error] Need name of program to start\n");
		exit(EXIT_FAILURE);
	}

	if (strstr(test_name, "spmv/dw_block_spmv"))
	{
		test_args = (char *) calloc(150, sizeof(char));
		sprintf(test_args, "%s/examples/spmv/matrix_market/examples/fidapm05.mtx", STARPU_SRC_DIR);
	}

	if (strstr(test_name, "starpu_perfmodel_display"))
	{
		test_args = (char *) calloc(5, sizeof(char));
		sprintf(test_args, "-l");
	}
	if (strstr(test_name, "starpu_perfmodel_plot"))
	{
		test_args = (char *) calloc(5, sizeof(char));
		sprintf(test_args, "-l");
	}

	/* get launcher program */
	launcher=getenv("STARPU_CHECK_LAUNCHER");
	launcher_args=getenv("STARPU_CHECK_LAUNCHER_ARGS");
	if (launcher_args)
		launcher_args=strdup(launcher_args);

	/* get user-defined iter_max value */
	if (getenv("STARPU_TIMEOUT_ENV"))
		timeout = strtol(getenv("STARPU_TIMEOUT_ENV"), NULL, 10);
	if (timeout <= 0)
		timeout = DEFAULT_TIMEOUT;

	/* set SIGALARM handler */
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = test_cleaner;
	if (-1 == sigaction(SIGALRM, &sa, NULL))
		perror("sigaction");

	child_pid = fork();
	if (child_pid == 0)
	{
		// get a new pgid
		if (setpgid(0, 0) == -1)
		{
			perror("setpgid");
			fprintf(stderr, "[error] setpgid. Mark test as failed\n");
			exit(EXIT_FAILURE);
		}
		if (launcher)
		{
			/* "Launchers" such as Valgrind need to be inserted
			 * after the Libtool-generated wrapper scripts, hence
			 * this special-case.  */
			const char *top_builddir = getenv ("top_builddir");
			const char *top_srcdir = getenv("top_srcdir");
			if (top_builddir != NULL)
			{
				char *launcher_argv[100];
				int i=3;
				char libtool[strlen(top_builddir)
					     + sizeof("libtool") + 1];
				strcpy(libtool, top_builddir);
				strcat(libtool, "/libtool");

				decode(&launcher_args, "@top_srcdir@", top_srcdir);

				launcher_argv[0] = libtool;
				launcher_argv[1] = "--mode=execute";
				launcher_argv[2] = launcher;
				launcher_argv[i] = strtok(launcher_args, " ");
				while (launcher_argv[i])
				{
					i++;
					launcher_argv[i] = strtok(NULL, " ");
				}
				launcher_argv[i] = test_name;
				launcher_argv[i+1] = test_args;
				launcher_argv[i+2] = NULL;
				execvp(*launcher_argv, launcher_argv);
			}
			else
			{
				fprintf(stderr,
					"warning: $top_builddir undefined, "
					"so $STARPU_CHECK_LAUNCHER ignored\n");
				execl(test_name, test_name, test_args, NULL);
			}
		}
		else
			execl(test_name, test_name, test_args, NULL);

		fprintf(stderr, "[error] '%s' failed to exec. test marked as failed\n", test_name);
		exit(EXIT_FAILURE);
	}
	if (child_pid == -1)
	{
		fprintf(stderr, "[error] fork. test marked as failed\n");
		exit(EXIT_FAILURE);
	}

	ret = EXIT_SUCCESS;
	gettimeofday(&start, NULL);
	alarm(timeout);
	if (child_pid == waitpid(child_pid, &child_exit_status, 0))
	{
		if (WIFEXITED(child_exit_status))
		{
			status = WEXITSTATUS(child_exit_status);
			if (status == EXIT_SUCCESS)
			{
				alarm(0);
			}
			else
			{
				if (status != AUTOTEST_SKIPPED_TEST)
					fprintf(stdout, "`%s' exited with return code %d\n",
						test_name, status);
				ret = status;
			}
		}
		else if (WIFSIGNALED(child_exit_status))
		{
			fprintf(stderr, "[error] `%s' killed with signal %d; test marked as failed\n",
				test_name, WTERMSIG(child_exit_status));
			launch_gdb(test_name);
			ret = EXIT_FAILURE;
		}
		else
		{
			fprintf(stderr, "[error] `%s' did not terminate normally; test marked as failed\n",
				test_name);
			ret = EXIT_FAILURE;
		}
	}

	gettimeofday(&end, NULL);
	timing = (double)((end.tv_sec - start.tv_sec)*1000000 + (end.tv_usec - start.tv_usec));
	fprintf(stderr, "#Execution_time_in_seconds %f %s\n", timing/1000000, test_name);

	return ret;
}
