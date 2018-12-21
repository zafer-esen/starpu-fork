#!/bin/bash -l
#SBATCH -J prefetching_bench_dev
#SBATCH -t 0:15:00
#SBATCH -p devel
#SBATCH -o devtest_cholesky_tag.out
#SBATCH -N 2 -n 20
#SBATCH -A snic2017-7-43

#Starpu need to have a default scheduler, this is the one we modify
export STARPU_SCHED=dmda
#Eviction policy = Random
export ARGO_EVICTION_POLICY=0

module load intel/17.4 openmpi/3.0.0

#Make sure limits are reasonable, especially virtual memory
ulimit -a
#make sure maximum number of virtual memory mappings are reasonable
#THIS MAY CAUSE LOTS OF WEIRD ERRORS IF TOO LOW AS MMAP MAY FAIL
#NEED TO HAVE LARGER REGIONS/CACHELINES OR SMALLER CACHE IF THAT IS THE CASE
#Some MPI versions seem to get this problem anyway, so maybe those implementation have limitations on similar parameters - need further investigation
sysctl vm.max_map_count

#If we want to remove previous HW performance models done by StarPU, as we are adding argo-information to these it may be necessary
#rm -rf ~/.starpu/sampling

#FXT tracing support
#export STARPU_FXT_PREFIX="/home/mano9519/projfolder/fxttraces/"

for cacheline in 1
do
#Set cacheline size in Argo - page as unit
    export ARGO_CACHELINE_SIZE=${cacheline}
    echo "Cacheline = ${cacheline}"
    for size in 5000
    do
       #Number of CPUs used in StarPU (Per argonode)
       export STARPU_NCPUS=1
       export STARPU_WORKERS_CPUID="0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19"
       #Set cachesize in Argo - MB as unit
       export ARGO_CACHESIZE=${size}
       echo "Cachesize = ${size}"

       #0-normal dmda, 1-all cached front 2 some cached front 3-sorted
       #Working StarPU policies
       #0 'normal DMDA' with Argo knowledge
       #1 If all buffers has data cached, put it front of the queue
       #2 If some buffers has data cached, put it front of the queue
       #3 Try to sort the buffers based on how much is cached
       for schedpolicy in 0
       do
           export ARGO_STARPU_SCHED_POLICY=${schedpolicy}
           for prefetchpolicy in 4
           do
               #StarPU Prefetch policy - 0/1/2/3 available choices, see backend/mpi/swdsm.cpp for more info
               #export ARGO_STARPU_PREFETCH_POLICY=${prefetchpolicy}
               #StarPU Scheduler - we have modified the DMDA scheduler

               #export STARPU_SCHED=eager

               #Use prefetching
               export ARGO_USE_PREFETCHING=1
               #LU
               #time mpirun --map-by ppr:1:node -n 2 ./time_zgetrf_nopiv_tile --n_range=76800:76800:1 --nb=960
               #QR Decomposition
               #time mpirun --map-by ppr:1:node -n 2 ./time_zgeqrf_tile --n_range=76800:76800:1 --nb=960

               #Cholesky --nowarmup is a flag to not warmup performance models
               #time mpirun --map-by ppr:1:node -n 2 ./time_dpotrf_tile --n_range=67200:67200:1 --nb=960
               #2argo node per machine node, 4 argo nodes in total
#               time mpirun --map-by ppr:2:node -n 2 ./hello_world
               #4 nodes, 1 argonode per machine node
               #time mpirun --map-by ppr:1:node -n 4 ./time_dpotrf_tile --n_range=67200:67200:1 --nb=960
               export STARPU_SCHED=dmda
	       time mpirun --map-by ppr:1:node -n 2 ./examples/cholesky/cholesky_tag -size $((120*40)) -nblocks 40
	       #time mpirun --map-by ppr:2:node -n 2 ./examples/basic_examples/hello_world2
           done
       done
    done
done
