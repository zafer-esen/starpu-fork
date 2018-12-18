/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2010-2016  Université de Bordeaux
 * Copyright (C) 2010, 2011, 2012, 2013  Centre National de la Recherche Scientifique
 * Copyright (C) 2011  Télécom-SudParis
 * Copyright (C) 2011-2012  INRIA
 *
 * StarPU is free software; you can rediribute it and/or modify
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

/* Distributed queues using performance modeling to assign tasks */

#include <starpu_config.h>
#include <starpu_scheduler.h>

#include <common/fxt.h>
#include <core/task.h>
#include <core/sched_ctx.h>

#include <sched_policies/fifo_queues.h>
#include <limits.h>

#ifdef HAVE_AYUDAME_H
#include <Ayudame.h>
#endif

#include "argo.h"


//const double argo_signal_penalty = 0.0008;
//const double argo_task_penalty = 0.02;

#ifndef DBL_MIN
#define DBL_MIN __DBL_MIN__
#endif

#ifndef DBL_MAX
#define DBL_MAX __DBL_MAX__
#endif

struct _starpu_dmda_data
{
		double alpha;
		double beta;
		double _gamma;
		double idle_power;

		struct _starpu_fifo_taskq **queue_array;

		long int total_task_cnt;
		long int ready_task_cnt;
};

/* The dmda scheduling policy uses
 *
 * alpha * T_computation + beta * T_communication + gamma * Consumption
 *
 * Here are the default values of alpha, beta, gamma
 */

#define _STARPU_SCHED_ALPHA_DEFAULT 1.0
#define _STARPU_SCHED_BETA_DEFAULT 1.0
#define _STARPU_SCHED_GAMMA_DEFAULT 1000.0

#ifdef STARPU_USE_TOP
static double alpha = _STARPU_SCHED_ALPHA_DEFAULT;
static double beta = _STARPU_SCHED_BETA_DEFAULT;
static double _gamma = _STARPU_SCHED_GAMMA_DEFAULT;
static double idle_power = 0.0;
static const float alpha_minimum=0;
static const float alpha_maximum=10.0;
static const float beta_minimum=0;
static const float beta_maximum=10.0;
static const float gamma_minimum=0;
static const float gamma_maximum=10000.0;
static const float idle_power_minimum=0;
static const float idle_power_maximum=10000.0;
#endif /* !STARPU_USE_TOP */

static int count_non_ready_buffers(struct starpu_task *task, unsigned node)
{
	int cnt = 0;
	unsigned nbuffers = task->cl->nbuffers;
	unsigned index;

	for (index = 0; index < nbuffers; index++)
		{
			starpu_data_handle_t handle;

			handle = STARPU_TASK_GET_HANDLE(task, index);

			int is_valid;
			starpu_data_query_status(handle, node, NULL, &is_valid, NULL);

			if (!is_valid)
				cnt++;
		}

	return cnt;
}

#ifdef STARPU_USE_TOP
static void param_modified(struct starpu_top_param* d)
{
#ifdef STARPU_DEVEL
#warning FIXME: get sched ctx to get alpha/beta/gamma/idle values
#endif
	/* Just to show parameter modification. */
	fprintf(stderr,
					"%s has been modified : "
					"alpha=%f|beta=%f|gamma=%f|idle_power=%f !\n",
					d->name, alpha,beta,_gamma, idle_power);
}
#endif /* !STARPU_USE_TOP */

static struct starpu_task *_starpu_fifo_pop_first_ready_task(struct _starpu_fifo_taskq *fifo_queue, unsigned node)
{
	struct starpu_task *task = NULL, *current;

	if (fifo_queue->ntasks == 0)
		return NULL;

	if (fifo_queue->ntasks > 0)
		{
			fifo_queue->ntasks--;

			task = starpu_task_list_front(&fifo_queue->taskq);
			if (STARPU_UNLIKELY(!task))
				return NULL;

			int first_task_priority = task->priority;

			current = task;

			int non_ready_best = INT_MAX;

			while (current)
				{
				int priority = current->priority;

					if (priority >= first_task_priority)
						{
							int non_ready = count_non_ready_buffers(current, node);
							if (non_ready < non_ready_best)
								{
									non_ready_best = non_ready;
									task = current;

									if (non_ready == 0)
										break;
								}
						}
					current = current->next;
				}
			starpu_task_list_erase(&fifo_queue->taskq, task);
		}
	return task;
}

static struct starpu_task *dmda_pop_ready_task(unsigned sched_ctx_id)
{
	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	struct starpu_task *task;

	int workerid = starpu_worker_get_id();
	struct _starpu_fifo_taskq *fifo = dt->queue_array[workerid];

	unsigned node = starpu_worker_get_memory_node(workerid);

	task = _starpu_fifo_pop_first_ready_task(fifo, node);
	if (task)
		{
			/* We now start the transfer, get rid of it in the completion
			 * prediction */
			double transfer_model = task->predicted_transfer;
			if(!isnan(transfer_model)) 
				{
					fifo->exp_len -= transfer_model;
					fifo->exp_start = starpu_timing_now() + transfer_model;
					fifo->exp_end = fifo->exp_start + fifo->exp_len;
				}

#ifdef STARPU_VERBOSE
			if (task->cl)
				{
					int non_ready = count_non_ready_buffers(task, node);
					if (non_ready == 0)
						dt->ready_task_cnt++;
				}

			dt->total_task_cnt++;
#endif
		}

	return task;
}

static struct starpu_task *dmda_pop_task(unsigned sched_ctx_id)
{
	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	struct starpu_task *task;

	int workerid = starpu_worker_get_id();
	struct _starpu_fifo_taskq *fifo = dt->queue_array[workerid];

	STARPU_ASSERT_MSG(fifo, "worker %d does not belong to ctx %d anymore.\n", workerid, sched_ctx_id);

	task = _starpu_fifo_pop_local_task(fifo);
	if (task)
		{
			/* while(task){ */
			/* 	if(task->priority==0){ */
			/* 		break; */
			/* 	} */
			/* 	else{ */
			/* 		unsigned nbuffers = task->cl->nbuffers; */
			/* 		unsigned index; */
			/* 		char cntpref = 0; */
			/* 		for (index = 0; index < nbuffers; index++){ */
			/* 			starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(task, index); */
			/* 			enum starpu_data_access_mode mode = STARPU_CODELET_GET_MODE(task->cl, index); */
			/* 			void *local_ptr; */
			/* 			size_t data_size; */
			/* 			local_ptr = starpu_data_get_local_ptr(handle); */
			/* 			data_size = starpu_data_get_size(handle); */
			/* 			int tmp = argo_in_cache(local_ptr,data_size); */
			/* 			if(tmp==0){ */
			/* 				cntpref++; //Not cached */
			/* 				break; */
			/* 			} */
			/* 		} */
			/* 		if(task->priority==1 && cntpref==0){ */
			/* 			task->priority=0; */
			/* 			starpu_task_list_push_back(&dt->queue_array[workerid]->taskq, task); */
			/* 			//_starpu_fifo_push_sorted_task(dt->queue_array[workerid], task); */
			/* 			//					printf("resort\n"); */
			/* 			fifo->resort++; */
			/* 			task = _starpu_fifo_pop_local_task(fifo); */
			/* 		} */
			/* 		else{ */
			/* 			break; */
			/* 		} */
			/* 	} */
			/* } */

			/* while(task){ */
			/* 	if(task->priority>1){ */
			/* 		unsigned nbuffers = task->cl->nbuffers; */
			/* 		unsigned index; */
			/* 		char cntpref = 0; */
			/* 		for (index = 0; index < nbuffers; index++){ */
			/* 			starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(task, index); */
			/* 			enum starpu_data_access_mode mode = STARPU_CODELET_GET_MODE(task->cl, index); */
			/* 			void *local_ptr; */
			/* 			size_t data_size; */
			/* 			local_ptr = starpu_data_get_local_ptr(handle); */
			/* 			data_size = starpu_data_get_size(handle); */
			/* 			int tmp = argo_in_cache(local_ptr,data_size); */
			/* 			if(tmp==0){ */
			/* 				cntpref++; //Not cached */
			/* 			} */
			/* 		} */
			/* 		int taskprio = cntpref; */
			/* 		if(cntpref==0){ */
			/* 			taskprio+=42; */
			/* 		} */
			/* 		if(task->priority > taskprio){ */
			/* 			task->priority=taskprio; */
			/* 			_starpu_fifo_push_sorted_task(dt->queue_array[workerid], task); */
			/* 			fifo->resort++; */
			/* 			task = _starpu_fifo_pop_local_task(fifo); */

			/* 		} */
			/* 		else{ */
			/* 			break; */
			/* 		} */
			/* 	} */
			/* 	else{ */
			/* 		break; */
			/* 	} */
			/* } */
			/* if(!task){ */
			/* 	printf("ERROR NULL TASK\n"); */
			/* 	return task; */
			/* } */

			double transfer_model = task->predicted_transfer;
			/* We now start the transfer, get rid of it in the completion
			 * prediction */
			if(!isnan(transfer_model)){
				double model = task->predicted+_starpu_get_job_associated_to_task(task)->argo_penalty;
				fifo->exp_len -= transfer_model;
				fifo->exp_start = starpu_timing_now() + transfer_model+model;
				fifo->exp_end = fifo->exp_start + fifo->exp_len;
			}


		  
#ifdef STARPU_VERBOSE
			if (task->cl)
				{
					int non_ready = count_non_ready_buffers(task, starpu_worker_get_memory_node(workerid));
					if (non_ready == 0)
						dt->ready_task_cnt++;
				}
		
			dt->total_task_cnt++;
#endif
		}

	return task;
}

static struct starpu_task *dmda_pop_every_task(unsigned sched_ctx_id)
{
	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	struct starpu_task *new_list;

	int workerid = starpu_worker_get_id();
	struct _starpu_fifo_taskq *fifo = dt->queue_array[workerid];

	starpu_pthread_mutex_t *sched_mutex;
	starpu_pthread_cond_t *sched_cond;
	starpu_worker_get_sched_condition(workerid, &sched_mutex, &sched_cond);
	STARPU_PTHREAD_MUTEX_LOCK(sched_mutex);
	new_list = _starpu_fifo_pop_every_task(fifo, workerid);
	STARPU_PTHREAD_MUTEX_UNLOCK(sched_mutex);
	while (new_list)
		{
			double transfer_model = new_list->predicted_transfer;
			/* We now start the transfer, get rid of it in the completion
			 * prediction */
			if(!isnan(transfer_model)) 
				{
					fifo->exp_len -= transfer_model;
					fifo->exp_start = starpu_timing_now() + transfer_model;
					fifo->exp_end = fifo->exp_start + fifo->exp_len;
				}

			new_list = new_list->next;
		}

	return new_list;
}

static int push_task_on_best_worker(struct starpu_task *task, int best_workerid,
																		double predicted, double predicted_transfer,
																		int prio, unsigned sched_ctx_id)
{


	task->argo_cached=0;
	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);
	/* make sure someone coule execute that task ! */
	STARPU_ASSERT(best_workerid != -1);

	struct _starpu_fifo_taskq *fifo = dt->queue_array[best_workerid];
	//	_STARPU_DISP("inb4 sched condition\n");
	starpu_pthread_mutex_t *sched_mutex;
	starpu_pthread_cond_t *sched_cond;
	starpu_worker_get_sched_condition(best_workerid, &sched_mutex, &sched_cond);

#ifdef STARPU_USE_SC_HYPERVISOR
	starpu_sched_ctx_call_pushed_task_cb(best_workerid, sched_ctx_id);
#endif //STARPU_USE_SC_HYPERVISOR

	STARPU_PTHREAD_MUTEX_LOCK(sched_mutex);
	int workerid = starpu_worker_get_id();
	struct _starpu_fifo_taskq *sched_fifo;
	
	float old_start,old_end,old_len,pred_start,pred_end,pred_len,pred_len2;
	if(workerid != -1){
		sched_fifo = dt->queue_array[workerid]; 
		old_start = sched_fifo->exp_start;
	}
	/* old_end = fifo->exp_end; */
	/* old_len = fifo->exp_len; */
	/* Sometimes workers didn't take the tasks as early as we expected */
	fifo->exp_start = STARPU_MAX(fifo->exp_start, starpu_timing_now());
	fifo->exp_end = fifo->exp_start + fifo->exp_len;

	if ((starpu_timing_now() + predicted_transfer) < fifo->exp_end)
		{
			/* We may hope that the transfer will be finished by
			 * the start of the task. */
			predicted_transfer = 0.0;
		}
	else
		{
			/* The transfer will not be finished by then, take the
			 * remainder into account */
			predicted_transfer = (starpu_timing_now() + predicted_transfer) - fifo->exp_end;
		}

	if(!isnan(predicted_transfer)) 
		{
			fifo->exp_end += predicted_transfer;
			fifo->exp_len += predicted_transfer;
		}

	if(!isnan(predicted))
		{
			fifo->exp_end += predicted;
			fifo->exp_len += predicted;
		}

	STARPU_PTHREAD_MUTEX_UNLOCK(sched_mutex);

	task->predicted = predicted;
	task->predicted_transfer = predicted_transfer;


#ifdef STARPU_USE_TOP
	starpu_top_task_prevision(task, best_workerid,
														(unsigned long long)(fifo->exp_end-predicted)/1000,
														(unsigned long long)fifo->exp_end/1000);
#endif /* !STARPU_USE_TOP */
	
	//_STARPU_DISP("inb4 ayudame\n");
#ifdef HAVE_AYUDAME_H
	if (AYU_event)
		{
			intptr_t id = best_workerid;
			AYU_event(AYU_ADDTASKTOQUEUE, _starpu_get_job_associated_to_task(task)->job_id, &id);
		}
#endif
	//_STARPU_DISP("inb4 check prio\n");
	int ret = 0;
	

			STARPU_PTHREAD_MUTEX_LOCK(sched_mutex);
			unsigned nbuffers = task->cl->nbuffers;
			char* iscached = (char*)malloc(nbuffers);
			unsigned index;
			//			int qsize = argo_prefetch_queue_size();
			char dopref = 0;
			char cntpref = 0;
			index=0;
			for (index = 0; index < nbuffers; index++){
				starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(task, index);
				enum starpu_data_access_mode mode = STARPU_CODELET_GET_MODE(task->cl, index);
				/* starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(task, index); */
				/* enum starpu_data_access_mode mode = STARPU_CODELET_GET_MODE(task->cl, index); */
				void *local_ptr;
				size_t data_size;
				local_ptr = starpu_data_get_local_ptr(handle);
				data_size = starpu_data_get_size(handle);
				/* iscached[index] = argo_in_cache(local_ptr,data_size); */
				/* if(iscached[index] == 0){ */
				/* 	cntpref++; */
				/* 	cntpref=nbuffers; */
				/* } */
				//	}
				int tmp = argo_in_cache(local_ptr,data_size);
				iscached[index]=tmp;
				if(tmp==0){ // Not cached
					if(index==0){
						cntpref+=1;
					} 
					if(index==1){
						cntpref+=2;
					} 
					if(index==2){
						cntpref+=4;
					} 
					dopref++;
				}
			}
			int taskprio = cntpref;
			//			if(cntpref==0){
			if(cntpref!=nbuffers){
				taskprio+=42-cntpref;
			}

			task->argo_cached=cntpref;
			task->priority = taskprio;

			const int back = 0;
			const int front_all_cached = 1;
			const int front_some_cached = 2;
			const int sorted=3;
			
			
			int pushpolicy=argo_get_sched_policy();

			if(pushpolicy == front_all_cached){
				if(fifo && (fifo->ntasks==0 || dopref==0)){
					starpu_task_list_push_front(&dt->queue_array[best_workerid]->taskq, task);
					ret=0;
					task->priority = 1;
				}
				else{
					starpu_task_list_push_back(&dt->queue_array[best_workerid]->taskq, task);
					ret=fifo->ntasks -1;
					task->priority = 0;
				}
			}
			else if(pushpolicy == front_some_cached){
				if(fifo && (fifo->ntasks==0 || dopref!=nbuffers)){
					starpu_task_list_push_front(&dt->queue_array[best_workerid]->taskq, task);
					ret=0;
					task->priority = 1;
				}
				else{
					starpu_task_list_push_back(&dt->queue_array[best_workerid]->taskq, task);
					ret=fifo->ntasks -1;
					task->priority = 0;
				}
			}
			else if(pushpolicy==back){
				starpu_task_list_push_back(&dt->queue_array[best_workerid]->taskq, task);
			}
			else if(pushpolicy==sorted){
				ret =_starpu_fifo_push_sorted_task(dt->queue_array[best_workerid], task);
			}
			task->argo_prefetched=0;
			//			qsize+=cntpref;


			float ratio, ratioend;
			if(pushpolicy==back){
			 ratio=1;
			 ratioend=1;
			} 
			else if(fifo->ntasks > 0 && ret <= fifo->ntasks){
				ratio = ((float)ret)/((float)fifo->ntasks);
				ratioend= ((float)ret+1)/((float)fifo->ntasks);
			}
			else{
				ratio = 0;
				ratioend = 0;
			}


			pred_start = fifo->exp_start + ratio*fifo->exp_len;
			pred_end = fifo->exp_start + ratioend*fifo->exp_len;
			pred_len = ratio*fifo->exp_len;
			pred_len2 = ratioend*fifo->exp_len;

			//			printf("ret:%d exp_cache:%lf best_workerid:%d sched_worker:%d\n",ret, fifo->exp_cachemiss,best_workerid,workerid);

			//	ratio=1;
			/* if( */
			/* 	 //cntpref==nbuffers */
			/* 	 cntpref>0 && */
			/* 	 //ret >2 && ret <53 */
			/* 	 //qsize<=nbuffers */
			/* 	 //				   qsize*argo_get_taskavg()*1000000 < fifo->exp_cachemiss) */
			/* 	 qsize*argo_get_taskavg()*1000000 < pred_len */
			/* 	 ){ */
			/* 	dopref=1; */
			/* 		//					printf("happens tasks%d exp qtime:%lf fifotime:%lf,qsize:%d\n",dt->queue_array[best_workerid]->ntasks,qsize*argo_get_taskavg()*1000000, (fifo->exp_len),qsize); */
			/* 	} */

			if(cntpref>0){
				unsigned int workers = starpu_worker_get_count();
				float time_until_cachemiss = 0.0;
				int i;
				float lowest_miss = 0;
				/* if(fifo->exp_cachemiss > pred_len){ */
				/* 	fifo->exp_cachemiss=pred_len; */
				/* } */

				if(fifo->exp_cachemiss > pred_start){
					fifo->exp_cachemiss=pred_start;
				}

				for(i = 0; i < workers; i++){
					//	i=best_workerid;
				 	struct _starpu_fifo_taskq *current_fifo = dt->queue_array[i];
					if(i==0 || lowest_miss > current_fifo->exp_cachemiss){
						lowest_miss = current_fifo->exp_cachemiss;
					}
				}

				if(fifo->exp_cachemiss > lowest_miss){
					fifo->exp_cachemiss=lowest_miss;
				}
			
					//				fifo->exp_cachemiss=lowest_miss;

				/* if(((ratio*fifo->exp_len) < fifo->exp_cachemiss) || (fifo->exp_cachemiss <0.01)){ */
				/* 		//						current_fifo->exp_cachemiss = pred_start; */
				/* 	fifo->exp_cachemiss = ratio*fifo->exp_len; */
				/* } */

				/* 	//				struct starpu_task_list task_list = current_fifo->taskq; */
				/* 	struct starpu_task *task_list = current_fifo->taskq.head; */
				/* 	int tasks_on_worker = current_fifo->ntasks; */
				/* 	int prefetched_task_buffers = 0; */
				/* 	int prefetched_tasks = 0; */
				/* 	int j; */

				/* 	for(j=0; j<tasks_on_worker; j++){ */
				/* 		if(task_list &&	_starpu_get_job_associated_to_task(task_list)->argo_prefetched==0){ */
				/* 			prefetched_task_buffers+=task_list->cl->nbuffers; */
				/* 			prefetched_tasks++; */
				/* 			task_list = task_list->next; */
				/* 		} */
				/* 		else{ */
				/* 			break; */
				/* 		} */
				/* 	} */
					/* float tmp_ratio = ((float)prefetched_tasks)/((float)tasks_on_worker); */
					/* float tmp_time_until_cachemiss = tmp_ratio*(current_fifo->exp_len); */
					/* //					if(i==0 || tmp_time_until_cachemiss<time_until_cachemiss){ */
					/* 	time_until_cachemiss=tmp_time_until_cachemiss; */
			
				//printf("trypref! starttime:%lf, nowtime:%lf qtime:%f, timeuntilcachemiss:%lf\n",fifo->exp_start,starpu_timing_now(),fifo->exp_start+qsize*argo_get_taskavg()*1000000,fifo->exp_cachemiss);
					//}
			//				if(qsize*argo_get_taskavg()*1000000 < time_until_cachemiss ){
				/* if( */
				/* 	 //					 1==1 || */
				/* 	 // (fifo->exp_start+qsize*argo_get_taskavg()*1000000) < pred_start){ */
				/* 	 (fifo->exp_start+qsize*argo_get_taskavg()*1000000) < fifo->exp_cachemiss){ */
				/* 	dopref=1; */
				/* 	task->argo_prefetched=1; */
				/* 	//argo_add_prefetch_queue_size(cntpref); */
				/* 	if(fifo->exp_cachemiss < pred_end){ */
				/* 		fifo->exp_cachemiss=pred_end; */
				/* 	} */

				/* 	//					printf("dopref! qtime:%f, timeuntilcachemiss:%lf\n",qsize*argo_get_taskavg()*1000000,fifo->exp_cachemiss); */
				/* 	//					printf("dopref! qtime:%f, timeuntilcachemiss:%lf\n",qsize*argo_get_taskavg()*1000000,time_until_cachemiss); */
				/* } */
			}			
			else{
				if(fifo->exp_cachemiss < pred_end){
					fifo->exp_cachemiss = pred_end;
				}
			}


			//			_starpu_get_job_associated_to_task(task)->argo_prefetched=dopref;
			//	printf("tsttasks%d exp qtime:%lf fifotime:%lf,qsize:%d\n",dt->queue_array[best_workerid]->ntasks,qsize*argo_get_taskavg()*1000000, (fifo->exp_len),qsize);
			dt->queue_array[best_workerid]->ntasks++;
			dt->queue_array[best_workerid]->nprocessed++;

#ifndef STARPU_NON_BLOCKING_DRIVERS
			STARPU_PTHREAD_COND_SIGNAL(sched_cond);
#endif
			//			_STARPU_DISP("work id:%d\n",starpu_worker_get_id());
			float penalty = 0;

			if(workerid != -1){
				/* float diff = starpu_timing_now()-old_start; */
				/* sched_fifo->exp_start+=diff; */
				/* sched_fifo->exp_end+=diff; */
			}

			/* if(dopref==1){ */
			/* 	cntpref=0; */
			
			/* for (index = 0; index < nbuffers; index++){ */
			/* 		starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(task, index); */
			/* 		enum starpu_data_access_mode mode = STARPU_CODELET_GET_MODE(task->cl, index); */

			/* 		if(mode & (STARPU_R|STARPU_W)){ */
			/* 			void *local_ptr; */
			/* 			size_t data_size; */
			/* 			local_ptr = starpu_data_get_local_ptr(handle); */
			/* 			data_size = starpu_data_get_size(handle); */
			/* 			//Check that we should prefetch, and the current buffer is not already cached */
			/* 			if(iscached[index]==0 */
			/* 				 //							 && workerid != -1 */
			/* 				 //&& argo_in_cache(local_ptr,data_size)==0 */
			/* 				 ){ */
			/* 				if(mode & (STARPU_W)){ */
			/* 					/\* if(argo_prefetch(local_ptr,data_size,1,pred_start,best_workerid)){ *\/ */
			/* 					/\* 	cntpref++; *\/ */
			/* 					/\* 	//									break; *\/ */
			/* 					/\* } *\/ */
			/* 					/\* else{ *\/ */
			/* 					/\* 	//dopref=0; *\/ */
			/* 					/\* } *\/ */
			/* 					/\* //argo_prefetch(local_ptr,0,0); *\/ */
			/* 				} */
			/* 				else if(mode & (STARPU_R)){ */
			/* 					if(argo_prefetch(local_ptr,data_size,1,pred_start,best_workerid)){ */
			/* 						cntpref++; */
			/* 						//break; */
			/* 					} */
			/* 					else{ */
			/* 						//									dopref=0; */
			/* 					} */
			/* 					//argo_prefetch(local_ptr,0,0); */
			/* 				} */
			/* 			} */
			/* 			else{ */
			/* 				//                  argo_prefetch(local_ptr,0,0,pred_start); */
			/* 			} */
			/* 		} */
			/* 	} */
			/* 	//				cntpref = nbuffers-cntpref; */
			/* } */

				//				if(dopref==0){
			//starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(task, 0);
			//float tmp_data_size = (float)starpu_data_get_size(handle);
			//			penalty = 1000000*(dopref)*argo_get_signalavg()*((tmp_data_size)/(128.0*4096.0));

					//	}
			//cntpref=0;
			//penalty -= 1000000*cntpref*argo_get_taskavg();
				//				penalty*=cntpref;
				//				_starpu_get_job_associated_to_task(task)->argo_prefetched=0;
			/* } */
			/* else{ */
			/* 	//	_starpu_get_job_associated_to_task(task)->argo_prefetched=1; */
			/* } */
			/* if(cntpref==0){ */
			/* 	//penalty = 1000000*(cntpref)*argo_get_taskavg(); */
			/* 	_starpu_get_job_associated_to_task(task)->argo_cached=1; */
			/* } */
			/* else{ */
			/* 	_starpu_get_job_associated_to_task(task)->argo_cached=0; */
			/* } */
			_starpu_get_job_associated_to_task(task)->argo_penalty=penalty;

			fifo->exp_end += penalty;
			fifo->exp_len += penalty;


			//printf("now:%lf fifo start:%lf end:%lf len:%lf readytask:%d totaltask:%d\n",starpu_timing_now(),fifo->exp_start,fifo->exp_end,fifo->exp_len,starpu_sched_ctx_get_nready_tasks(0),_starpu_get_nsubmitted_tasks_of_sched_ctx(0));				
			starpu_push_task_end(task);

			STARPU_PTHREAD_MUTEX_UNLOCK(sched_mutex);
			free(iscached);		
	

	//_STARPU_DISP("inb4 task\n");
	//_STARPU_DISP("inb4 ret\n");
	return ret;
}

/* TODO: factorize with dmda!! */
static int _dm_push_task(struct starpu_task *task, unsigned prio, unsigned sched_ctx_id)
{
	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);
	unsigned worker, worker_ctx = 0;
	int best = -1;

	double best_exp_end = 0.0;
	double model_best = 0.0;
	double transfer_model_best = 0.0;

	int ntasks_best = -1;
	double ntasks_best_end = 0.0;
	int calibrating = 0;

	/* A priori, we know all estimations */
	int unknown = 0;

	unsigned best_impl = 0;
	unsigned nimpl;
	struct starpu_worker_collection *workers = starpu_sched_ctx_get_worker_collection(sched_ctx_id);

	struct starpu_sched_ctx_iterator it;
	if(workers->init_iterator)
		workers->init_iterator(workers, &it);

	while(workers->has_next(workers, &it))
		{
			worker = workers->get_next(workers, &it);
			struct _starpu_fifo_taskq *fifo  = dt->queue_array[worker];
			unsigned memory_node = starpu_worker_get_memory_node(worker);
			enum starpu_perfmodel_archtype perf_arch = starpu_worker_get_perf_archtype(worker);

			/* Sometimes workers didn't take the tasks as early as we expected */
			double exp_start = STARPU_MAX(fifo->exp_start, starpu_timing_now());

			for (nimpl = 0; nimpl < STARPU_MAXIMPLEMENTATIONS; nimpl++)
				{
					if (!starpu_worker_can_execute_task(worker, task, nimpl))
						{
							/* no one on that queue may execute this task */
							//			worker_ctx++;
							continue;
						}

					double exp_end;
					double local_length = starpu_task_expected_length(task, perf_arch, nimpl);
					double local_penalty = starpu_task_expected_data_transfer_time(memory_node, task);
					double ntasks_end = fifo->ntasks / starpu_worker_get_relative_speedup(perf_arch);

					//_STARPU_DEBUG("Scheduler dm: task length (%lf) worker (%u) kernel (%u) \n", local_length,worker,nimpl);

					/*
					 * This implements a default greedy scheduler for the
					 * case of tasks which have no performance model, or
					 * whose performance model is not calibrated yet.
					 *
					 * It simply uses the number of tasks already pushed to
					 * the workers, divided by the relative performance of
					 * a CPU and of a GPU.
					 *
					 * This is always computed, but the ntasks_best
					 * selection is only really used if the task indeed has
					 * no performance model, or is not calibrated yet.
					 */
					if (ntasks_best == -1
			
							/* Always compute the greedy decision, at least for
							 * the tasks with no performance model. */
							|| (!calibrating && ntasks_end < ntasks_best_end)

							/* The performance model of this task is not
							 * calibrated on this worker, try to run it there
							 * to calibrate it there. */
							|| (!calibrating && isnan(local_length))

							/* the performance model of this task is not
							 * calibrated on this worker either, rather run it
							 * there if this one is low on scheduled tasks. */
							|| (calibrating && isnan(local_length) && ntasks_end < ntasks_best_end)
							)
						{
							ntasks_best_end = ntasks_end;
							ntasks_best = worker;
							best_impl = nimpl;
						}

					if (isnan(local_length))
						/* we are calibrating, we want to speed-up calibration time
						 * so we privilege non-calibrated tasks (but still
						 * greedily distribute them to avoid dumb schedules) */
						calibrating = 1;

					if (isnan(local_length) || _STARPU_IS_ZERO(local_length))
						/* there is no prediction available for that task
						 * with that arch yet, so switch to a greedy strategy */
						unknown = 1;

					if (unknown)
						continue;

					exp_end = exp_start + fifo->exp_len + local_length;

					if (best == -1 || exp_end < best_exp_end)
						{
							/* a better solution was found */
							best_exp_end = exp_end;
							best = worker;
							model_best = local_length;
							transfer_model_best = local_penalty;
							best_impl = nimpl;
						}
				}
			worker_ctx++;
		}

	if (unknown)
		{
			best = ntasks_best;
			model_best = 0.0;
			transfer_model_best = 0.0;
		}

	//_STARPU_DEBUG("Scheduler dm: kernel (%u)\n", best_impl);

	starpu_task_set_implementation(task, best_impl);

	/* we should now have the best worker in variable "best" */
	return push_task_on_best_worker(task, best,
																	model_best, transfer_model_best, prio, sched_ctx_id);
}

/* TODO: factorise CPU computations, expensive with a lot of cores */
static void compute_all_performance_predictions(struct starpu_task *task,
																								unsigned nworkers,
																								double local_task_length[nworkers][STARPU_MAXIMPLEMENTATIONS],
																								double exp_end[nworkers][STARPU_MAXIMPLEMENTATIONS],
																								double *max_exp_endp,
																								double *best_exp_endp,
																								double local_data_penalty[nworkers][STARPU_MAXIMPLEMENTATIONS],
																								double local_power[nworkers][STARPU_MAXIMPLEMENTATIONS],
																								int *forced_worker, int *forced_impl, unsigned sched_ctx_id){
	int calibrating = 0;
	double max_exp_end = DBL_MIN;
	double best_exp_end = DBL_MAX;
	int ntasks_best = -1;
	int nimpl_best = 0;
	double ntasks_best_end = 0.0;

	/* A priori, we know all estimations */
	int unknown = 0;
	unsigned worker, worker_ctx = 0;

	unsigned nimpl;

	starpu_task_bundle_t bundle = task->bundle;
	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);
	struct starpu_worker_collection *workers = starpu_sched_ctx_get_worker_collection(sched_ctx_id);

	struct starpu_sched_ctx_iterator it;
	if(workers->init_iterator)
		workers->init_iterator(workers, &it);

	while(workers->has_next(workers, &it))
		{
			worker = workers->get_next(workers, &it);

			if (worker >= nworkers)
				/* This is a just-added worker, discard it */
				continue;

			struct _starpu_fifo_taskq *fifo = dt->queue_array[worker];
			enum starpu_perfmodel_archtype perf_arch = starpu_worker_get_perf_archtype(worker);
			unsigned memory_node = starpu_worker_get_memory_node(worker);

			/* Sometimes workers didn't take the tasks as early as we expected */
			double exp_start = STARPU_MAX(fifo->exp_start, starpu_timing_now());

			for(nimpl  = 0; nimpl < STARPU_MAXIMPLEMENTATIONS; nimpl++)
				{
					if (!starpu_worker_can_execute_task(worker, task, nimpl))
						{
							/* no one on that queue may execute this task */
							continue;
						}

					STARPU_ASSERT_MSG(fifo != NULL, "worker %d ctx %d\n", worker, sched_ctx_id);
					exp_end[worker_ctx][nimpl] = exp_start + fifo->exp_len;
					if (exp_end[worker_ctx][nimpl] > max_exp_end)
						max_exp_end = exp_end[worker_ctx][nimpl];

					//_STARPU_DEBUG("Scheduler dmda: task length (%lf) worker (%u) kernel (%u) \n", local_task_length[worker][nimpl],worker,nimpl);

					if (bundle)
						{
							/* TODO : conversion time */
							local_task_length[worker_ctx][nimpl] = starpu_task_bundle_expected_length(bundle, perf_arch, nimpl);
							local_data_penalty[worker_ctx][nimpl] = starpu_task_bundle_expected_data_transfer_time(bundle, memory_node);
							local_power[worker_ctx][nimpl] = starpu_task_bundle_expected_power(bundle, perf_arch,nimpl);
						}
					else
						{
							local_task_length[worker_ctx][nimpl] = starpu_task_expected_length(task, perf_arch, nimpl);
							local_data_penalty[worker_ctx][nimpl] = starpu_task_expected_data_transfer_time(memory_node, task);
							local_power[worker_ctx][nimpl] = starpu_task_expected_power(task, perf_arch,nimpl);
							double conversion_time = starpu_task_expected_conversion_time(task, perf_arch, nimpl);
							if (conversion_time > 0.0)
								local_task_length[worker_ctx][nimpl] += conversion_time;
						}
					double ntasks_end = fifo->ntasks / starpu_worker_get_relative_speedup(perf_arch);

					/*
					 * This implements a default greedy scheduler for the
					 * case of tasks which have no performance model, or
					 * whose performance model is not calibrated yet.
					 *
					 * It simply uses the number of tasks already pushed to
					 * the workers, divided by the relative performance of
					 * a CPU and of a GPU.
					 *
					 * This is always computed, but the ntasks_best
					 * selection is only really used if the task indeed has
					 * no performance model, or is not calibrated yet.
					 */
					if (ntasks_best == -1

							/* Always compute the greedy decision, at least for
							* the tasks with no performance model. */
							|| (!calibrating && ntasks_end < ntasks_best_end)

							/* The performance model of this task is not
							 * calibrated on this worker, try to run it there
							 * to calibrate it there. */
							|| (!calibrating && isnan(local_task_length[worker_ctx][nimpl]))

							/* the performance model of this task is not
							 * calibrated on this worker either, rather run it
							 * there if this one is low on scheduled tasks. */
							|| (calibrating && isnan(local_task_length[worker_ctx][nimpl]) && ntasks_end < ntasks_best_end)
							)
						{
							ntasks_best_end = ntasks_end;
							ntasks_best = worker;
							nimpl_best = nimpl;
						}

					if (isnan(local_task_length[worker_ctx][nimpl]))
						/* we are calibrating, we want to speed-up calibration time
						 * so we privilege non-calibrated tasks (but still
						 * greedily distribute them to avoid dumb schedules) */
						calibrating = 1;

					if (isnan(local_task_length[worker_ctx][nimpl])
							|| _STARPU_IS_ZERO(local_task_length[worker_ctx][nimpl]))
						/* there is no prediction available for that task
						 * with that arch (yet or at all), so switch to a greedy strategy */
						unknown = 1;

					if (unknown)
						continue;

				/* 	unsigned nbuffers = task->cl->nbuffers; */
				/* 	unsigned index; */

				/* 	int qsize = argo_prefetch_queue_size(); */
				/* 	char cntpref = 0; */
				/* 	char dopref = 0; */
				/* 	index =0; */
				/* 	for (index = 0; index < nbuffers; index++){ */
				/* 		starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(task, index); */
				/* 		enum starpu_data_access_mode mode = STARPU_CODELET_GET_MODE(task->cl, index); */
				/* 		void *local_ptr; */
				/* 		size_t data_size; */
				/* 		local_ptr = starpu_data_get_local_ptr(handle); */
				/* 		data_size = starpu_data_get_size(handle); */

				/* 		char tmpcached  = argo_in_cache(local_ptr,data_size); */
				/* 		if(tmpcached == 0){ */
				/* 			cntpref++; */
				/* 			//							cntpref=nbuffers; */
				/* 		} */
				/* 	} */
										
				/* qsize+=cntpref; */

				/* if( */
				/* 	 //					 					 _starpu_get_nsubmitted_tasks_of_sched_ctx(0) > 15000 && */
				/* 	 qsize*argo_get_taskavg()*1000000 < (fifo->exp_len) */
				/* 	 ){ */
				/* 	dopref=1; */
				/* } */

				float penalty = 0;
				/* if(dopref==0){ */
				/* 	starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(task, 0); */
				/* 	float tmp_data_size = (float)starpu_data_get_size(handle); */
				/* 	penalty = 1000000*cntpref*argo_get_signalavg()*((tmp_data_size)/(128.0*4096.0)); */
				/* 	//penalty*=cntpref; */
				/* } */
					
					exp_end[worker_ctx][nimpl] = exp_start + fifo->exp_len + local_task_length[worker_ctx][nimpl]+penalty;

					if (exp_end[worker_ctx][nimpl] < best_exp_end)
						{
							/* a better solution was found */
							best_exp_end = exp_end[worker_ctx][nimpl];
							nimpl_best = nimpl;
						}

					if (isnan(local_power[worker_ctx][nimpl]))
						local_power[worker_ctx][nimpl] = 0.;

				}
			worker_ctx++;
			//			printf("worker:%d, exp_start:%lf,exp_end:%lf,exp_len:%lf,exp_cachemiss:%lf,tasks:%d\n",worker_ctx,fifo->exp_start,fifo->exp_end,fifo->exp_len, fifo->exp_cachemiss,fifo->ntasks);
		}

	*forced_worker = unknown?ntasks_best:-1;
	*forced_impl = unknown?nimpl_best:-1;

	*best_exp_endp = best_exp_end;
	*max_exp_endp = max_exp_end;
}

static int _dmda_push_task(struct starpu_task *task, unsigned prio, unsigned sched_ctx_id)
{
	prio = 0;
	/* find the queue */
	unsigned worker, worker_ctx = 0;
	int best = -1, best_in_ctx = -1;
	int selected_impl = 0;
	double model_best = 0.0;
	double transfer_model_best = 0.0;

	/* this flag is set if the corresponding worker is selected because
	   there is no performance prediction available yet */
	int forced_best = -1;
	int forced_impl = -1;

	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);
	struct starpu_worker_collection *workers = starpu_sched_ctx_get_worker_collection(sched_ctx_id);
	unsigned nworkers_ctx = workers->nworkers;
	double local_task_length[nworkers_ctx][STARPU_MAXIMPLEMENTATIONS];
	double local_data_penalty[nworkers_ctx][STARPU_MAXIMPLEMENTATIONS];
	double local_power[nworkers_ctx][STARPU_MAXIMPLEMENTATIONS];
	double exp_end[nworkers_ctx][STARPU_MAXIMPLEMENTATIONS];
	double max_exp_end = 0.0;
	double best_exp_end;

	double fitness[nworkers_ctx][STARPU_MAXIMPLEMENTATIONS];

	struct starpu_sched_ctx_iterator it;
	if(workers->init_iterator)
		workers->init_iterator(workers, &it);

	compute_all_performance_predictions(task,
																			nworkers_ctx,
																			local_task_length,
																			exp_end,
																			&max_exp_end,
																			&best_exp_end,
																			local_data_penalty,
																			local_power,
																			&forced_best,
																			&forced_impl, sched_ctx_id);
	
	double best_fitness = -1;

	unsigned nimpl;
	if (forced_best == -1)
		{
			while(workers->has_next(workers, &it))
				{
					worker = workers->get_next(workers, &it);
					if (worker >= nworkers_ctx)
						/* This is a just-added worker, discard it */
						continue;

					for (nimpl = 0; nimpl < STARPU_MAXIMPLEMENTATIONS; nimpl++)
						{
							if (!starpu_worker_can_execute_task(worker, task, nimpl))
								{
									/* no one on that queue may execute this task */
									continue;
								}


							fitness[worker_ctx][nimpl] = dt->alpha*(exp_end[worker_ctx][nimpl] - best_exp_end)
								+ dt->beta*(local_data_penalty[worker_ctx][nimpl])
								+ dt->_gamma*(local_power[worker_ctx][nimpl]);

							if (exp_end[worker_ctx][nimpl] > max_exp_end)
								{
									/* This placement will make the computation
									 * longer, take into account the idle
									 * consumption of other cpus */
									fitness[worker_ctx][nimpl] += dt->_gamma * dt->idle_power * (exp_end[worker_ctx][nimpl] - max_exp_end) / 1000000.0;
								}

							if (best == -1 || fitness[worker_ctx][nimpl] < best_fitness)
								{
									/* we found a better solution */
									best_fitness = fitness[worker_ctx][nimpl];
									best = worker;
									best_in_ctx = worker_ctx;
									selected_impl = nimpl;

									//_STARPU_DEBUG("best fitness (worker %d) %e = alpha*(%e) + beta(%e) +gamma(%e)\n", worker, best_fitness, exp_end[worker][nimpl] - best_exp_end, local_data_penalty[worker][nimpl], local_power[worker][nimpl]);
								}
						}
					worker_ctx++;
				}
		}

	STARPU_ASSERT(forced_best != -1 || best != -1);

	if (forced_best != -1)
		{
			/* there is no prediction available for that task
			 * with that arch we want to speed-up calibration time
			 * so we force this measurement */
			best = forced_best;
			selected_impl = forced_impl;
			model_best = 0.0;
			transfer_model_best = 0.0;
		}
	else if (task->bundle)
		{
			enum starpu_perfmodel_archtype perf_arch = starpu_worker_get_perf_archtype(best_in_ctx);
			unsigned memory_node = starpu_worker_get_memory_node(best);
			model_best = starpu_task_expected_length(task, perf_arch, selected_impl);
			transfer_model_best = starpu_task_expected_data_transfer_time(memory_node, task);
		}
	else
		{
			model_best = local_task_length[best_in_ctx][selected_impl];
			transfer_model_best = local_data_penalty[best_in_ctx][selected_impl];
		}

	//_STARPU_DEBUG("Scheduler dmda: kernel (%u)\n", best_impl);
	starpu_task_set_implementation(task, selected_impl);

	/* we should now have the best worker in variable "best" */
	return push_task_on_best_worker(task, best, model_best, transfer_model_best, prio, sched_ctx_id);
}

static int dmda_push_sorted_task(struct starpu_task *task)
{
#ifdef STARPU_DEVEL
#warning TODO: after defining a scheduling window, use that instead of empty_ctx_tasks
#endif
	return _dmda_push_task(task, 1, task->sched_ctx);
}

static int dm_push_task(struct starpu_task *task)
{
	return _dm_push_task(task, 0, task->sched_ctx);
}

static int dmda_push_task(struct starpu_task *task)
{
	STARPU_ASSERT(task);
	return _dmda_push_task(task, 0, task->sched_ctx);
}

static void dmda_add_workers(unsigned sched_ctx_id, int *workerids, unsigned nworkers)
{
	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	int workerid;
	unsigned i;
	for (i = 0; i < nworkers; i++)
		{
			workerid = workerids[i];
			/* if the worker has alreadry belonged to this context
				 the queue and the synchronization variables have been already initialized */
			if(dt->queue_array[workerid] == NULL)
				dt->queue_array[workerid] = _starpu_create_fifo();
		}
}

static void dmda_remove_workers(unsigned sched_ctx_id, int *workerids, unsigned nworkers)
{
	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	int workerid;
	unsigned i;
	for (i = 0; i < nworkers; i++)
		{
			workerid = workerids[i];
			if(dt->queue_array[workerid] != NULL)
				{
					_starpu_destroy_fifo(dt->queue_array[workerid]);
					dt->queue_array[workerid] = NULL;
				}
		}
}

static void initialize_dmda_policy(unsigned sched_ctx_id)
{
	starpu_sched_ctx_create_worker_collection(sched_ctx_id, STARPU_WORKER_LIST);

	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)malloc(sizeof(struct _starpu_dmda_data));
	dt->alpha = _STARPU_SCHED_ALPHA_DEFAULT;
	dt->beta = _STARPU_SCHED_BETA_DEFAULT;
	dt->_gamma = _STARPU_SCHED_GAMMA_DEFAULT;
	dt->idle_power = 0.0;

	starpu_sched_ctx_set_policy_data(sched_ctx_id, (void*)dt);

	dt->queue_array = (struct _starpu_fifo_taskq**)malloc(STARPU_NMAXWORKERS*sizeof(struct _starpu_fifo_taskq*));

	int i;
	for(i = 0; i < STARPU_NMAXWORKERS; i++)
		dt->queue_array[i] = NULL;

	const char *strval_alpha = starpu_getenv("STARPU_SCHED_ALPHA");
	if (strval_alpha)
		dt->alpha = atof(strval_alpha);

	const char *strval_beta = starpu_getenv("STARPU_SCHED_BETA");
	if (strval_beta)
		dt->beta = atof(strval_beta);

	const char *strval_gamma = starpu_getenv("STARPU_SCHED_GAMMA");
	if (strval_gamma)
		dt->_gamma = atof(strval_gamma);

	const char *strval_idle_power = starpu_getenv("STARPU_IDLE_POWER");
	if (strval_idle_power)
		dt->idle_power = atof(strval_idle_power);

#ifdef STARPU_USE_TOP
	starpu_top_register_parameter_float("DMDA_ALPHA", &alpha,
																			alpha_minimum, alpha_maximum, param_modified);
	starpu_top_register_parameter_float("DMDA_BETA", &beta,
																			beta_minimum, beta_maximum, param_modified);
	starpu_top_register_parameter_float("DMDA_GAMMA", &_gamma,
																			gamma_minimum, gamma_maximum, param_modified);
	starpu_top_register_parameter_float("DMDA_IDLE_POWER", &idle_power,
																			idle_power_minimum, idle_power_maximum, param_modified);
#endif /* !STARPU_USE_TOP */
}

static void initialize_dmda_sorted_policy(unsigned sched_ctx_id)
{
	initialize_dmda_policy(sched_ctx_id);

	/* The application may use any integer */
	if (starpu_sched_ctx_min_priority_is_set(sched_ctx_id) == 0)
		starpu_sched_ctx_set_min_priority(sched_ctx_id, INT_MIN);
	if (starpu_sched_ctx_max_priority_is_set(sched_ctx_id) == 0)
		starpu_sched_ctx_set_max_priority(sched_ctx_id, INT_MAX);
}

static void deinitialize_dmda_policy(unsigned sched_ctx_id)
{
	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	_STARPU_DEBUG("total_task_cnt %ld ready_task_cnt %ld -> %f\n", dt->total_task_cnt, dt->ready_task_cnt, (100.0f*dt->ready_task_cnt)/dt->total_task_cnt);

	free(dt->queue_array);
	free(dt);
	starpu_sched_ctx_delete_worker_collection(sched_ctx_id);
}

/* dmda_pre_exec_hook is called right after the data transfer is done and right
 * before the computation to begin, it is useful to update more precisely the
 * value of the expected start, end, length, etc... */
static void dmda_pre_exec_hook(struct starpu_task *task)
{

	//	char prefetched = _starpu_get_job_associated_to_task(task)->argo_prefetched;
	char prefetched=0;
	if(prefetched==1){

	if(task){
		unsigned nbuffers = task->cl->nbuffers;

		unsigned index;

		for (index = 0; index < nbuffers; index++)
			{
				starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(task, index);
				//				printf("task:%p, prefetch :%d/%dbuffers workerid:%d footprint:%ld getsize:%d\n",task,index,nbuffers,starpu_worker_get_id(),_starpu_data_get_footprint(handle), _starpu_data_get_size(handle));
				enum starpu_data_access_mode mode = STARPU_CODELET_GET_MODE(task->cl, index);

				//				if (mode & (STARPU_SCRATCH|STARPU_REDUX))
				//continue;
				if(mode & (STARPU_R|STARPU_W)){
					void *local_ptr;
					size_t data_size;
					local_ptr = starpu_data_get_local_ptr(handle);
					data_size = starpu_data_get_size(handle);
					//printf("---------Do PREFETCH ptr:%p, datasize:%d\n",local_ptr,data_size);fflush(stdout);
					//					argo_prefetch(local_ptr,data_size);
					//printf("---------Mid PREFETCH ptr:%p, datasize:%d\n",local_ptr,data_size);fflush(stdout);
					char mode = 0;
					if(mode & (STARPU_W)){mode = 1;}

					//argo_prefetch_wait(local_ptr,data_size,mode); //TODO: commented out by Vasilis&Zafer
					//printf("---------End PREFETCH ptr:%p, datasize:%d\n",local_ptr,data_size);fflush(stdout);
				}
				else{
					if(mode & STARPU_REDUX){
						printf("mode redux:%d\n",mode);
					}
					else if(mode & STARPU_SCRATCH){
						printf("mode scratch:%d\n",mode);
					}
					else{
						printf("mode other:%d\n",mode);
					}
				}
			}
	}
	}
	unsigned sched_ctx_id = task->sched_ctx;
	int workerid = starpu_worker_get_id();
	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);
	struct _starpu_fifo_taskq *fifo = dt->queue_array[workerid];
	double model = task->predicted+_starpu_get_job_associated_to_task(task)->argo_penalty;

	starpu_pthread_mutex_t *sched_mutex;
	starpu_pthread_cond_t *sched_cond;
	starpu_worker_get_sched_condition(workerid, &sched_mutex, &sched_cond);

	/* Once the task is executing, we can update the predicted amount
	 * of work. */
	STARPU_PTHREAD_MUTEX_LOCK(sched_mutex);
	if(!isnan(model))
		{
			/* We now start the computation, get rid of it in the completion
			 * prediction */
			fifo->exp_len-= model;
			fifo->exp_start = starpu_timing_now() + model;
			fifo->exp_end= fifo->exp_start + fifo->exp_len;
		}
	STARPU_PTHREAD_MUTEX_UNLOCK(sched_mutex);
}

static void dmda_push_task_notify(struct starpu_task *task, int workerid, int perf_workerid, unsigned sched_ctx_id)
{
	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);
	struct _starpu_fifo_taskq *fifo = dt->queue_array[workerid];
	/* Compute the expected penality */
	enum starpu_perfmodel_archtype perf_arch = starpu_worker_get_perf_archtype(perf_workerid);
	unsigned memory_node = starpu_worker_get_memory_node(workerid);

	double predicted = starpu_task_expected_length(task, perf_arch,
																								 starpu_task_get_implementation(task));

	double predicted_transfer = starpu_task_expected_data_transfer_time(memory_node, task);
	starpu_pthread_mutex_t *sched_mutex;
	starpu_pthread_cond_t *sched_cond;
	starpu_worker_get_sched_condition(workerid, &sched_mutex, &sched_cond);


	/* Update the predictions */
	STARPU_PTHREAD_MUTEX_LOCK(sched_mutex);
	/* Sometimes workers didn't take the tasks as early as we expected */
	fifo->exp_start = STARPU_MAX(fifo->exp_start, starpu_timing_now());
	fifo->exp_end = fifo->exp_start + fifo->exp_len;

	/* If there is no prediction available, we consider the task has a null length */
	if (!isnan(predicted_transfer))
		{
			if (starpu_timing_now() + predicted_transfer < fifo->exp_end)
				{
					/* We may hope that the transfer will be finished by
					 * the start of the task. */
					predicted_transfer = 0;
				}
			else
				{
					/* The transfer will not be finished by then, take the
					 * remainder into account */
					predicted_transfer = (starpu_timing_now() + predicted_transfer) - fifo->exp_end;
				}
			task->predicted_transfer = predicted_transfer;
			fifo->exp_end += predicted_transfer;
			fifo->exp_len += predicted_transfer;
		}

	/* If there is no prediction available, we consider the task has a null length */
	if (!isnan(predicted))
		{
			task->predicted = predicted;
			fifo->exp_end += predicted;
			fifo->exp_len += predicted;
		}

	fifo->ntasks++;

	STARPU_PTHREAD_MUTEX_UNLOCK(sched_mutex);
}


pthread_t starpu_prefetchthread;
char prefetch_running = 0;


/* void *starpu_prefetchloop(void *x){ */
/* 	return NULL; */
/* 	int core_id = 0; */
/* 	int num_cores = sysconf(_SC_NPROCESSORS_ONLN); */
/* 	int prefqremove_cnt=0; */
/* 	printf("numcores:%d core_id:%d\n",num_cores,core_id); */
/* 	if (core_id < 0 || core_id >= num_cores){ */
/*    printf("PINNING ERROR core id wrong\n"); */
/*     argo_finalize(); */
/* 		return NULL; */
/* 	} */

/* 	cpu_set_t cpuset; */
/* 	CPU_ZERO(&cpuset); */
/* 	CPU_SET(core_id, &cpuset); */

/* 	pthread_t current_thread = pthread_self();     */
/* 	int s = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset); */
/* 	printf("s:%d thgread:%d\n",s,current_thread); */
/*   if (s != 0){ */
/*     printf("PINNING ERROR\n"); */
/*   } */


/* 	//	printf("starpu prefloop1\n"); */
/* 	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(0); */
/* 	//int workerid = starpu_worker_get_id(); */
/* 	while(1){ */
/* 		int workerid; */
/* 		for(workerid = 0; workerid < starpu_worker_get_count(); workerid++){ */
/* 			//printf("starpu prefloop2\n"); */
/* 			struct _starpu_fifo_taskq *fifo = dt->queue_array[workerid]; */
/* 			starpu_pthread_mutex_t *sched_mutex; */
/* 			starpu_pthread_cond_t *sched_cond; */
/* 			starpu_worker_get_sched_condition(workerid, &sched_mutex, &sched_cond); */
/* 			//	printf("starpu prefloop3\n"); */
/* 			//			STARPU_PTHREAD_MUTEX_LOCK(sched_mutex); */
/* 			if(		STARPU_PTHREAD_MUTEX_TRYLOCK(sched_mutex) == 0){ */
/* 			if(fifo){ */
/* 				struct starpu_task_list *list = &fifo->taskq; */
/* 				if(list && list->head){ */
/* 					struct starpu_task *current = list->head; */
/* 					/\* if(current && _starpu_get_job_associated_to_task(current)->argo_cached == 0){ *\/ */
/* 					/\* 	current=current->next; *\/ */
/* 					/\* } *\/ */
/* 					/\* else{ *\/ */
/* 					/\* 	break; *\/ */
/* 					/\* } *\/ */
/* 					int i; */
/* 					for(i = 1; i < fifo->ntasks; i++){ */
/* 						//printf("starpu prefloop4\n"); */
/* 						struct starpu_task *task = current; */
/* 						if(task && task->argo_cached == 0){ */
/* 							if(task->argo_prefetched == 1){ */
/* 								unsigned nbuffers = task->cl->nbuffers; */
/* 								unsigned index; */
/* 								int cntpref=0; */
/* 								//printf("starpu prefloop5\n"); */
/* 								for (index = 0; index < nbuffers; index++){ */
/* 									starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(task, index); */
/* 									enum starpu_data_access_mode mode = STARPU_CODELET_GET_MODE(task->cl, index); */
/* 									if(mode & (STARPU_R|STARPU_W)){ */
/* 										void *local_ptr; */
/* 										size_t data_size; */
/* 										local_ptr = starpu_data_get_local_ptr(handle); */
/* 										data_size = starpu_data_get_size(handle); */
/* 										if(argo_prefetch(local_ptr,data_size,1,0,-1)){ */
/* 											cntpref++; */
/* 										} */
/* 									} */
/* 								} */
/* 								/\* _starpu_get_job_associated_to_task(task)->argo_prefetched=-1; *\/ */
/* 								/\* _starpu_get_job_associated_to_task(task)->argo_cached=1; *\/ */
/* 								task->argo_prefetched=-1; */
/* 								task->argo_cached=1; */
/* 							} */
/* 							current=current->next; */
/* 						} */
/* 						else{ */
/* 							break; */
/* 						} */
/* 					} */
/* 				} */
/* 				STARPU_PTHREAD_MUTEX_UNLOCK(sched_mutex); */
/* 			} */
/* 			} */
/* 						usleep(1000); */
/* 		} */
/* 				usleep(1000); */
/* 	} */
/* } */



static void dmda_post_exec_hook(struct starpu_task * task)
{

	struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(task->sched_ctx);
	//	printf("sched ctx:%d\n",task->sched_ctx);
	int workerid = starpu_worker_get_id();
	struct _starpu_fifo_taskq *fifo = dt->queue_array[workerid];
	starpu_pthread_mutex_t *sched_mutex;
	starpu_pthread_cond_t *sched_cond;
	starpu_worker_get_sched_condition(workerid, &sched_mutex, &sched_cond);
	STARPU_PTHREAD_MUTEX_LOCK(sched_mutex);
	if(prefetch_running == 0){
		prefetch_running=1;
		//		pthread_create(&starpu_prefetchthread,NULL,&starpu_prefetchloop,(void*)NULL);
	}

	/* struct _starpu_fifo_taskq *fifo = dt->queue_array[workerid]; */
		/* starpu_pthread_mutex_t *sched_mutex; */
		/* starpu_pthread_cond_t *sched_cond; */
		/* starpu_worker_get_sched_condition(workerid, &sched_mutex, &sched_cond); */

	//	STARPU_PTHREAD_MUTEX_LOCK(sched_mutex);
	/* if(fifo){ */

	/* 	struct starpu_task_list *list = &fifo->taskq; */
	/* 	if(list && list->head){ */
	/* 		struct starpu_task *current = list->head; */
	/* 		int i; */
	/* 		for(i = 1; i < fifo->ntasks; i++){ */
	/* 			if(current &&  _starpu_get_job_associated_to_task(task)->argo_prefetched == 1 &&  _starpu_get_job_associated_to_task(task)->argo_cached != 0){ */
	/* 				struct starpu_task *task = current->next; */
	/* 				unsigned nbuffers = task->cl->nbuffers; */
	/* 				unsigned index; */
	/* 				int cntpref=0; */
	/* 				for (index = 0; index < nbuffers; index++){ */
	/* 						starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(task, index); */
	/* 						enum starpu_data_access_mode mode = STARPU_CODELET_GET_MODE(task->cl, index); */
	/* 						if(mode & (STARPU_R|STARPU_W)){ */
	/* 							void *local_ptr; */
	/* 							size_t data_size; */
	/* 							local_ptr = starpu_data_get_local_ptr(handle); */
	/* 							data_size = starpu_data_get_size(handle); */
	/* 							if(argo_prefetch(local_ptr,data_size,1,0)){ */
	/* 								cntpref++; */
	/* 							} */
	/* 						} */
	/* 					} */
	/* 				_starpu_get_job_associated_to_task(task)->argo_prefetched=-1; */
	/* 				_starpu_get_job_associated_to_task(task)->argo_cached=1; */

	/* 				current=current->next; */
	/* 			} */
	/* 			else{ */
	/* 				break; */
	/* 			} */
	/* 		} */
	/* 	} */
	/* 	//		STARPU_PTHREAD_MUTEX_UNLOCK(sched_mutex); */
	/* } */




	/* char prefetched = _starpu_get_job_associated_to_task(task)->argo_prefetched; */
	/* //char prefetched = 0; */
	/* prefetched=0; */
	/* if(prefetched==1){ */
	/* 	if(task){ */
	/* 		unsigned nbuffers = task->cl->nbuffers; */

	/* 		unsigned index; */

	/* 		for (index = 0; index < nbuffers; index++) */
	/* 			{ */
	/* 				starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(task, index); */
	/* 				//				printf("task:%p, prefetch :%d/%dbuffers workerid:%d footprint:%ld getsize:%d\n",task,index,nbuffers,starpu_worker_get_id(),_starpu_data_get_footprint(handle), _starpu_data_get_size(handle)); */
	/* 				enum starpu_data_access_mode mode = STARPU_CODELET_GET_MODE(task->cl, index); */

	/* 				//				if (mode & (STARPU_SCRATCH|STARPU_REDUX)) */
	/* 				//continue; */
	/* 				if(mode & (STARPU_R|STARPU_W)){ */
	/* 					void *local_ptr; */
	/* 					size_t data_size; */
	/* 					local_ptr = starpu_data_get_local_ptr(handle); */
	/* 					data_size = starpu_data_get_size(handle); */
	/* 					//printf("---------Do PREFETCH ptr:%p, datasize:%d\n",local_ptr,data_size);fflush(stdout); */
	/* 					//					argo_prefetch(local_ptr,data_size); */
	/* 					//printf("---------Mid PREFETCH ptr:%p, datasize:%d\n",local_ptr,data_size);fflush(stdout); */
	/* 					char mode = 0; */
	/* 					if(mode & (STARPU_W)){mode = 1;} */

	/* 					//					argo_prefetch_wait(local_ptr,data_size,mode); */
	/* 					argo_prefetch(local_ptr,data_size,mode,0); */
	/* 					//argo_defetch(local_ptr,data_size,mode); */
	/* 					//printf("---------End PREFETCH ptr:%p, datasize:%d\n",local_ptr,data_size);fflush(stdout); */
	/* 				} */
	/* 				else{ */
	/* 					if(mode & STARPU_REDUX){ */
	/* 						printf("mode redux:%d\n",mode); */
	/* 					} */
	/* 					else if(mode & STARPU_SCRATCH){ */
	/* 						printf("mode scratch:%d\n",mode); */
	/* 					} */
	/* 					else{ */
	/* 						printf("mode other:%d\n",mode); */
	/* 					} */
	/* 				} */
	/* 			} */
	/* 	} */
	/* } */



	//char prefetched = _starpu_get_job_associated_to_task(task)->argo_prefetched;
		/* if(prefetched==1){ */
	/* 	int i; */
	/* 	unsigned nbuffers = task->cl->nbuffers; */
	/* 	for(i = 0; i < nbuffers; i++){ */
	/* 		argo_defetch(NULL,0,0); */
	/* 	} */
	/* } */
	/* if(task){ */
	/* 	unsigned nbuffers = task->cl->nbuffers; */
	/* 	unsigned index; */

	/* 	for (index = 0; index < nbuffers; index++) */
	/* 		{ */
	/* 			starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(task, index); */
	/* 			enum starpu_data_access_mode mode = STARPU_CODELET_GET_MODE(task->cl, index); */

	/* 			if (mode & (STARPU_SCRATCH|STARPU_REDUX)) */
	/* 				continue; */

	/* 			void *local_ptr; */
	/* 			size_t data_size; */
	/* 			local_ptr = starpu_data_get_local_ptr(handle); */
	/* 			data_size = starpu_data_get_size(handle); */
	/* 			argo_defetch(local_ptr,data_size); //unregs pages */
	/* 		} */
	/* } */


	/* if(task){ */
	/* 	unsigned nbuffers = task->cl->nbuffers; */

	/* 	unsigned index; */

	/* 	for (index = 0; index < nbuffers; index++) */
	/* 		{ */
	/* 			starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(task, index); */
	/* 			//				printf("task:%p, prefetch :%d/%dbuffers workerid:%d footprint:%ld getsize:%d\n",task,index,nbuffers,starpu_worker_get_id(),_starpu_data_get_footprint(handle), _starpu_data_get_size(handle)); */
	/* 			enum starpu_data_access_mode mode = STARPU_CODELET_GET_MODE(task->cl, index); */

	/* 			//				if (mode & (STARPU_SCRATCH|STARPU_REDUX)) */
	/* 			//continue; */
	/* 			if(mode & (STARPU_R|STARPU_W)){ */
	/* 				void *local_ptr; */
	/* 				size_t data_size; */
	/* 				local_ptr = starpu_data_get_local_ptr(handle); */
	/* 				data_size = starpu_data_get_size(handle); */
	/* 				//printf("---------Do PREFETCH ptr:%p, datasize:%d\n",local_ptr,data_size);fflush(stdout); */
	/* 				char mode = 0; */
	/* 				if(mode & (STARPU_W)){mode = 1;} */

	/* 				argo_defetch(local_ptr,data_size,mode); */
	/* 				//printf("---------Mid PREFETCH ptr:%p, datasize:%d\n",local_ptr,data_size);fflush(stdout); */
	/* 				//argo_prefetch_wait(local_ptr,data_size); */
	/* 				//					printf("---------End PREFETCH ptr:%p, datasize:%d\n",local_ptr,data_size);fflush(stdout); */
	/* 			} */
	/* 			else{ */
	/* 				if(mode & STARPU_REDUX){ */
	/* 					printf("mode redux:%d\n",mode); */
	/* 				} */
	/* 				else if(mode & STARPU_SCRATCH){ */
	/* 					printf("mode scratch:%d\n",mode); */
	/* 				} */
	/* 				else{ */
	/* 					printf("mode other:%d\n",mode); */
	/* 				} */
	/* 			} */
	/* 		} */
	/* } */



	if(task->execute_on_a_specific_worker)
		fifo->ntasks--;

	fifo->exp_start = starpu_timing_now();
	fifo->exp_end = fifo->exp_start + fifo->exp_len;

	STARPU_PTHREAD_MUTEX_UNLOCK(sched_mutex);
}

struct starpu_sched_policy _starpu_sched_dm_policy =
	{
		.init_sched = initialize_dmda_policy,
		.deinit_sched = deinitialize_dmda_policy,
		.add_workers = dmda_add_workers ,
		.remove_workers = dmda_remove_workers,
		.push_task = dm_push_task,
		.pop_task = dmda_pop_task,
		.pre_exec_hook = dmda_pre_exec_hook,
		.post_exec_hook = dmda_post_exec_hook,
		.pop_every_task = dmda_pop_every_task,
		.policy_name = "dm",
		.policy_description = "performance model"
	};

struct starpu_sched_policy _starpu_sched_dmda_policy =
	{
		.init_sched = initialize_dmda_policy,
		.deinit_sched = deinitialize_dmda_policy,
		.add_workers = dmda_add_workers ,
		.remove_workers = dmda_remove_workers,
		.push_task = dmda_push_task,
		.push_task_notify = dmda_push_task_notify,
		.pop_task = dmda_pop_task,
		.pre_exec_hook = dmda_pre_exec_hook,
		.post_exec_hook = dmda_post_exec_hook,
		.pop_every_task = dmda_pop_every_task,
		.policy_name = "dmda",
		.policy_description = "data-aware performance model"
	};

struct starpu_sched_policy _starpu_sched_dmda_sorted_policy =
	{
		.init_sched = initialize_dmda_sorted_policy,
		.deinit_sched = deinitialize_dmda_policy,
		.add_workers = dmda_add_workers ,
		.remove_workers = dmda_remove_workers,
		.push_task = dmda_push_sorted_task,
		.push_task_notify = dmda_push_task_notify,
		.pop_task = dmda_pop_ready_task,
		.pre_exec_hook = dmda_pre_exec_hook,
		.post_exec_hook = dmda_post_exec_hook,
		.pop_every_task = dmda_pop_every_task,
		.policy_name = "dmdas",
		.policy_description = "data-aware performance model (sorted)"
	};

struct starpu_sched_policy _starpu_sched_dmda_ready_policy =
	{
		.init_sched = initialize_dmda_policy,
		.deinit_sched = deinitialize_dmda_policy,
		.add_workers = dmda_add_workers ,
		.remove_workers = dmda_remove_workers,
		.push_task = dmda_push_task,
		.push_task_notify = dmda_push_task_notify,
		.pop_task = dmda_pop_ready_task,
		.pre_exec_hook = dmda_pre_exec_hook,
		.post_exec_hook = dmda_post_exec_hook,
		.pop_every_task = dmda_pop_every_task,
		.policy_name = "dmdar",
		.policy_description = "data-aware performance model (ready)"
	};
