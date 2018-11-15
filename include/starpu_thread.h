/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2010, 2012-2014  Université de Bordeaux
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

#ifndef __STARPU_THREAD_H__
#define __STARPU_THREAD_H__

#include <starpu_config.h>
#ifdef STARPU_SIMGRID
#include <xbt/synchro_core.h>
#ifdef STARPU_HAVE_SIMGRID_MSG_H
#include <simgrid/msg.h>
#else
#include <msg/msg.h>
#endif
#elif !defined(_MSC_VER) || defined(BUILDING_STARPU)
#include <pthread.h>
#endif

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * Encapsulation of the pthread_create function.
 */

#ifdef STARPU_SIMGRID

typedef int starpu_pthread_t;
typedef int starpu_pthread_attr_t;

int starpu_pthread_create_on(char *name, starpu_pthread_t *thread, const starpu_pthread_attr_t *attr, void *(*start_routine) (void *), void *arg, msg_host_t host);
int starpu_pthread_create(starpu_pthread_t *thread, const starpu_pthread_attr_t *attr, void *(*start_routine) (void *), void *arg);
int starpu_pthread_join(starpu_pthread_t thread, void **retval);
int starpu_pthread_attr_init(starpu_pthread_attr_t *attr);
int starpu_pthread_attr_destroy(starpu_pthread_attr_t *attr);
int starpu_pthread_attr_setdetachstate(starpu_pthread_attr_t *attr, int detachstate);

#elif !defined(_MSC_VER) || defined(BUILDING_STARPU) /* STARPU_SIMGRID */

typedef pthread_t starpu_pthread_t;
typedef pthread_attr_t starpu_pthread_attr_t;

#define starpu_pthread_create pthread_create
#define starpu_pthread_create_on(name, thread, attr, routine, arg, where) starpu_pthread_create(thread, attr, routine, arg)
#define starpu_pthread_join pthread_join
#define starpu_pthread_attr_init pthread_attr_init
#define starpu_pthread_attr_destroy pthread_attr_destroy
#define starpu_pthread_attr_setdetachstate pthread_attr_setdetachstate

#endif /* STARPU_SIMGRID, _MSC_VER */

/*
 * Encapsulation of the pthread_mutex_* functions.
 */

#ifdef STARPU_SIMGRID
typedef xbt_mutex_t starpu_pthread_mutex_t;
typedef int starpu_pthread_mutexattr_t;

#define STARPU_PTHREAD_MUTEX_INITIALIZER NULL

int starpu_pthread_mutex_init(starpu_pthread_mutex_t *mutex, const starpu_pthread_mutexattr_t *mutexattr);
int starpu_pthread_mutex_destroy(starpu_pthread_mutex_t *mutex);
int starpu_pthread_mutex_lock(starpu_pthread_mutex_t *mutex);
int starpu_pthread_mutex_unlock(starpu_pthread_mutex_t *mutex);
int starpu_pthread_mutex_trylock(starpu_pthread_mutex_t *mutex);

#elif !defined(_MSC_VER) || defined(BUILDING_STARPU) /* !STARPU_SIMGRID */

typedef pthread_mutex_t starpu_pthread_mutex_t;
typedef pthread_mutexattr_t starpu_pthread_mutexattr_t;

#define starpu_pthread_mutex_init pthread_mutex_init
#define starpu_pthread_mutex_destroy pthread_mutex_destroy
#define starpu_pthread_mutex_lock pthread_mutex_lock
#define starpu_pthread_mutex_unlock pthread_mutex_unlock
#define starpu_pthread_mutex_trylock pthread_mutex_trylock

#define STARPU_PTHREAD_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER

#endif /* STARPU_SIMGRID, _MSC_VER */

/*
 * Encapsulation of the pthread_key_* functions.
 */
#ifdef STARPU_SIMGRID

typedef int starpu_pthread_key_t;
int starpu_pthread_key_create(starpu_pthread_key_t *key, void (*destr_function) (void *));
int starpu_pthread_key_delete(starpu_pthread_key_t key);
int starpu_pthread_setspecific(starpu_pthread_key_t key, const void *pointer);
void *starpu_pthread_getspecific(starpu_pthread_key_t key);

#elif !defined(_MSC_VER) || defined(BUILDING_STARPU) /* !STARPU_SIMGRID */

typedef pthread_key_t starpu_pthread_key_t;

#define starpu_pthread_key_create pthread_key_create
#define starpu_pthread_key_delete pthread_key_delete
#define starpu_pthread_setspecific pthread_setspecific
#define starpu_pthread_getspecific pthread_getspecific

#endif /* STARPU_SIMGRID, _MSC_VER */

/*
 * Encapsulation of the pthread_cond_* functions.
 */

#ifdef STARPU_SIMGRID

typedef xbt_cond_t starpu_pthread_cond_t;
typedef int starpu_pthread_condattr_t;
#define STARPU_PTHREAD_COND_INITIALIZER NULL

int starpu_pthread_cond_init(starpu_pthread_cond_t *cond, starpu_pthread_condattr_t *cond_attr);
int starpu_pthread_cond_signal(starpu_pthread_cond_t *cond);
int starpu_pthread_cond_broadcast(starpu_pthread_cond_t *cond);
int starpu_pthread_cond_wait(starpu_pthread_cond_t *cond, starpu_pthread_mutex_t *mutex);
int starpu_pthread_cond_timedwait(starpu_pthread_cond_t *cond, starpu_pthread_mutex_t *mutex, const struct timespec *abstime);
int starpu_pthread_cond_destroy(starpu_pthread_cond_t *cond);

#elif !defined(_MSC_VER) || defined(BUILDING_STARPU) /* !STARPU_SIMGRID */

typedef pthread_cond_t starpu_pthread_cond_t;
typedef pthread_condattr_t starpu_pthread_condattr_t;
#define STARPU_PTHREAD_COND_INITIALIZER PTHREAD_COND_INITIALIZER

#define starpu_pthread_cond_init pthread_cond_init
#define starpu_pthread_cond_signal pthread_cond_signal
#define starpu_pthread_cond_broadcast pthread_cond_broadcast
#define starpu_pthread_cond_wait pthread_cond_wait
#define starpu_pthread_cond_timedwait pthread_cond_timedwait
#define starpu_pthread_cond_destroy pthread_cond_destroy

#endif /* STARPU_SIMGRID, _MSC_VER */

/*
 * Encapsulation of the pthread_rwlock_* functions.
 */

#ifdef STARPU_SIMGRID

typedef xbt_mutex_t starpu_pthread_rwlock_t;
typedef int starpu_pthread_rwlockattr_t;

int starpu_pthread_rwlock_init(starpu_pthread_rwlock_t *rwlock, const starpu_pthread_rwlockattr_t *attr);
int starpu_pthread_rwlock_destroy(starpu_pthread_rwlock_t *rwlock);
int starpu_pthread_rwlock_rdlock(starpu_pthread_rwlock_t *rwlock);
int starpu_pthread_rwlock_tryrdlock(starpu_pthread_rwlock_t *rwlock);
int starpu_pthread_rwlock_wrlock(starpu_pthread_rwlock_t *rwlock);
int starpu_pthread_rwlock_trywrlock(starpu_pthread_rwlock_t *rwlock);
int starpu_pthread_rwlock_unlock(starpu_pthread_rwlock_t *rwlock);

#elif !defined(_MSC_VER) || defined(BUILDING_STARPU) /* !STARPU_SIMGRID */

typedef pthread_rwlock_t starpu_pthread_rwlock_t;
typedef pthread_rwlockattr_t starpu_pthread_rwlockattr_t;

#define starpu_pthread_rwlock_init pthread_rwlock_init
#define starpu_pthread_rwlock_destroy pthread_rwlock_destroy
#define starpu_pthread_rwlock_rdlock pthread_rwlock_rdlock
#define starpu_pthread_rwlock_tryrdlock pthread_rwlock_tryrdlock
#define starpu_pthread_rwlock_wrlock pthread_rwlock_wrlock
#define starpu_pthread_rwlock_trywrlock pthread_rwlock_trywrlock
#define starpu_pthread_rwlock_unlock pthread_rwlock_unlock

#endif /* STARPU_SIMGRID, _MSC_VER */

/*
 * Encapsulation of the pthread_barrier_* functions.
 */

#if defined(STARPU_SIMGRID) || (!defined(STARPU_HAVE_PTHREAD_BARRIER) && (!defined(_MSC_VER) || defined(BUILDING_STARPU)))

typedef struct {
	starpu_pthread_mutex_t mutex;
	starpu_pthread_cond_t cond;
	starpu_pthread_cond_t cond_destroy;
	unsigned count;
	unsigned done;
	unsigned busy;
} starpu_pthread_barrier_t;
typedef int starpu_pthread_barrierattr_t;
#define STARPU_PTHREAD_BARRIER_SERIAL_THREAD -1

int starpu_pthread_barrier_init(starpu_pthread_barrier_t *barrier, const starpu_pthread_barrierattr_t *attr, unsigned count);
int starpu_pthread_barrier_destroy(starpu_pthread_barrier_t *barrier);
int starpu_pthread_barrier_wait(starpu_pthread_barrier_t *barrier);

#elif !defined(_MSC_VER) /* STARPU_SIMGRID, !STARPU_HAVE_PTHREAD_BARRIER */

typedef pthread_barrier_t starpu_pthread_barrier_t;
typedef pthread_barrierattr_t starpu_pthread_barrierattr_t;

#define starpu_pthread_barrier_init pthread_barrier_init
#define starpu_pthread_barrier_destroy pthread_barrier_destroy
#define starpu_pthread_barrier_wait pthread_barrier_wait
#define STARPU_PTHREAD_BARRIER_SERIAL_THREAD PTHREAD_BARRIER_SERIAL_THREAD

#endif /* STARPU_SIMGRID, !STARPU_HAVE_PTHREAD_BARRIER, _MSC_VER */

#if defined(_MSC_VER) && !defined(BUILDING_STARPU)
typedef void* starpu_pthread_rwlock_t;
typedef void* starpu_pthread_mutex_t;
typedef void* starpu_pthread_cond_t;
typedef void* starpu_pthread_barrier_t;
#endif /* _MSC_VER */

#ifdef __cplusplus
}
#endif

#endif /* __STARPU_THREAD_H__ */


