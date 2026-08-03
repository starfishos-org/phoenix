/* Copyright (c) 2007-2009, Stanford University
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above copyright
*       notice, this list of conditions and the following disclaimer in the
*       documentation and/or other materials provided with the distribution.
*     * Neither the name of Stanford University nor the names of its 
*       contributors may be used to endorse or promote products derived from 
*       this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY STANFORD UNIVERSITY ``AS IS'' AND ANY
* EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL STANFORD UNIVERSITY BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/ 

#include <pthread.h>
#include <assert.h>
#include <semaphore.h>

#include "atomic.h"
#include "memory.h"
#include "tpool.h"
#include "stddefines.h"
#include "processor.h"

#ifdef TQ_DIAG
#include <stdio.h>

/*
 * Barrier integrity probe.  tpool_wait() is the only thing that makes a phase's
 * workers finished as far as map_reduce.c is concerned: it returns, and the
 * caller then frees the phase's thread_arg blocks and moves the whole task
 * queue on to the next phase.  If it can ever return while a worker is still
 * inside its thread function, that next phase runs concurrently with the
 * previous one -- which is precisely the shape of the PCA corruption.
 */
volatile int tq_workers_running = 0;
#endif

typedef struct {
    sem_t           sem_run;
    unsigned int    *num_workers_done;
    sem_t           *sem_all_workers_done;
    thread_func     *thread_func;
    void            **thread_func_arg;
    void            **ret;
    int             *num_workers;
    int             *die;
    int             cpu_id;
} thread_arg_t;

struct tpool_t {
    int             num_threads;
    int             num_workers;
    int             die;
    thread_func     thread_func;
    sem_t           sem_all_workers_done;
    unsigned int    num_workers_done;
    void            **args;
    pthread_t       *threads;
    thread_arg_t    *thread_args;
    bool            shared_memory;
    mem_shared_arena_t *shared_arena;
};

static void* thread_loop (void *);

static void *tpool_malloc(tpool_t *tpool, size_t size)
{
    if (tpool->shared_memory)
        return mem_shared_arena_alloc(tpool->shared_arena, size);
    return mem_malloc(size);
}

static void *tpool_calloc(tpool_t *tpool, size_t num, size_t size)
{
    if (tpool->shared_memory)
        return mem_shared_arena_calloc(tpool->shared_arena, num, size);
    return mem_calloc(num, size);
}

static void tpool_mem_free(tpool_t *tpool, void *ptr)
{
    if (!tpool->shared_memory) {
        mem_free(ptr);
    } else if (ptr == tpool) {
        mem_shared_arena_destroy(tpool->shared_arena);
    }
}

tpool_t* tpool_create (int num_threads, bool shared_memory)
{
    int             i, ret;
    tpool_t         *tpool;
    pthread_attr_t  attr;

    mem_shared_arena_t *arena = NULL;

    if (shared_memory) {
        arena = mem_shared_arena_create(0);
        tpool = mem_shared_arena_calloc(arena, 1, sizeof(tpool_t));
    } else {
        tpool = mem_calloc(1, sizeof(tpool_t));
    }
    if (tpool == NULL) 
        return NULL;
    tpool->shared_memory = shared_memory;
    tpool->shared_arena = arena;
    if (shared_memory)
        fprintf(stderr, "[Phoenix placement] tpool=%p policy=shared arena=%p\n",
                (void *)tpool, (void *)arena);

    tpool->num_threads = num_threads;
    tpool->num_workers = num_threads;

    tpool->args = (void **)tpool_malloc (tpool, sizeof (void *) * num_threads);
    if (tpool->args == NULL) 
        goto fail_args;

    tpool->threads = (pthread_t *)tpool_malloc (
        tpool, sizeof (pthread_t) * num_threads);
    if (tpool->threads == NULL) 
        goto fail_threads;

    tpool->thread_args = (thread_arg_t *)tpool_calloc (
        tpool, num_threads, sizeof (thread_arg_t));
    if (tpool->thread_args == NULL) 
        goto fail_thread_args;

    ret = sem_init (&tpool->sem_all_workers_done, 0, 0);
    if (ret != 0) 
        goto fail_all_workers_done;

    CHECK_ERROR (pthread_attr_init (&attr));
    CHECK_ERROR (pthread_attr_setscope (&attr, PTHREAD_SCOPE_SYSTEM));
    CHECK_ERROR (pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_DETACHED));

    if (thread_bind_cpu_set) {
        proc_bind_thread(thread_bind_cpu_list[0]);
    } else {
        proc_bind_thread(0);
    }
    tpool->die = 0;
    for (i = 0; i < num_threads; ++i) {
        /* Initialize thread argument. */
        CHECK_ERROR (sem_init (&(tpool->thread_args[i].sem_run), 0, 0));
        tpool->thread_args[i].sem_all_workers_done = 
            &tpool->sem_all_workers_done;
        tpool->thread_args[i].num_workers_done = 
            &tpool->num_workers_done;
        tpool->thread_args[i].die = &tpool->die;
        tpool->thread_args[i].thread_func = &tpool->thread_func;
        tpool->thread_args[i].thread_func_arg = &tpool->args[i];
        tpool->thread_args[i].ret = (void **)tpool_malloc (
            tpool, sizeof (void *));
        CHECK_ERROR (tpool->thread_args[i].ret == NULL);
        tpool->thread_args[i].num_workers = &tpool->num_workers;
        tpool->thread_args[i].cpu_id = i;
        
        ret = pthread_create (
            &tpool->threads[i], &attr, thread_loop, &tpool->thread_args[i]);
        if (ret) 
            goto fail_thread_create;
    }

    return tpool;

fail_thread_create:
    --i;
    while (i >= 0)
    {
        pthread_cancel (tpool->threads[i]);
        --i;
    }
fail_all_workers_done:
    tpool_mem_free (tpool, tpool->thread_args);
fail_thread_args:
    tpool_mem_free (tpool, tpool->threads);
fail_threads:
    tpool_mem_free (tpool, tpool->args);
fail_args:
    tpool_mem_free (tpool, tpool);

    return NULL;
}

int tpool_set (
    tpool_t *tpool, thread_func thread_func, void **args, int num_workers)
{
    int             i;
    
    assert (tpool != NULL);

    tpool->thread_func = thread_func;

    assert (num_workers <= tpool->num_threads);
    tpool->num_workers = num_workers;

    for (i = 0; i < num_workers; ++i)
    {
        tpool->args[i] = args[i];
    }
    

    return 0;
}

int tpool_begin (tpool_t *tpool)
{
    int             i, ret;

    assert (tpool != NULL);

    if (tpool->num_workers == 0)
        return 0;

    tpool->num_workers_done = 0;

    for (i = 0; i < tpool->num_workers; ++i) {
        ret = sem_post (&(tpool->thread_args[i].sem_run));
        if (ret != 0) 
            return -1;
    }

    return 0;
}

int tpool_wait (tpool_t *tpool)
{
    int             ret;

    assert (tpool != NULL);

    if (tpool->num_workers == 0)
        return 0;

    ret = sem_wait (&tpool->sem_all_workers_done);
    if (ret != 0)
        return -1;

#ifdef TQ_DIAG
    {
        int still = __atomic_load_n(&tq_workers_running, __ATOMIC_SEQ_CST);
        if (still != 0) {
            printf("[TQDIAG] BARRIER-BROKEN cpu=%d: tpool_wait returned with "
                   "%d worker(s) still running (num_workers=%d)\n",
                   proc_get_cpuid(), still, tpool->num_workers);
        }
    }
#endif

    return 0;
}

void** tpool_get_results (tpool_t *tpool)
{
    int             i;
    void            **rets;

    assert (tpool != NULL);

    rets = (void **)mem_malloc (sizeof (void *) * tpool->num_threads);
    CHECK_ERROR (rets == NULL);

    for (i = 0; i < tpool->num_threads; ++i) {
        rets[i] = *(tpool->thread_args[i].ret);   
    }

    return rets;
}

int tpool_destroy (tpool_t *tpool)
{
    int             i;
    int             result;
    
    assert (tpool != NULL);
    assert (tpool->die == 0);

    result = 0;
    tpool->num_workers = tpool->num_threads;
    tpool->num_workers_done = 0;
    
    for (i = 0; i < tpool->num_threads; ++i) {
        tpool_mem_free (tpool, tpool->thread_args[i].ret);

        tpool->die = 1;
        sem_post(&tpool->thread_args[i].sem_run);
    }

    sem_wait(&tpool->sem_all_workers_done);

    sem_destroy(&tpool->sem_all_workers_done);
    tpool_mem_free (tpool, tpool->args);
    tpool_mem_free (tpool, tpool->threads);
    tpool_mem_free (tpool, tpool->thread_args);

    tpool_mem_free (tpool, tpool);

    return result;
}

static void* thread_loop (void *arg)
{
    thread_arg_t    *thread_arg = arg;
    thread_func     thread_func;
    void            *thread_func_arg;
    void            **ret;
    int             num_workers_done;

    assert (thread_arg);
    if (thread_bind_cpu_set) {
        proc_bind_thread(thread_bind_cpu_list[thread_arg->cpu_id]);
    } else {
        proc_bind_thread(thread_arg->cpu_id);
    }

    while (1)
    {
        CHECK_ERROR (sem_wait (&thread_arg->sem_run));
        if (*thread_arg->die)
            break;

        thread_func = *(thread_arg->thread_func);
        thread_func_arg = *(thread_arg->thread_func_arg);
        ret = thread_arg->ret;

        /* Run thread function. */
#ifdef TQ_DIAG
        __atomic_fetch_add(&tq_workers_running, 1, __ATOMIC_SEQ_CST);
#endif
        *ret = (*thread_func)(thread_func_arg);
#ifdef TQ_DIAG
        __atomic_fetch_sub(&tq_workers_running, 1, __ATOMIC_SEQ_CST);
#endif

        num_workers_done = fetch_and_inc(thread_arg->num_workers_done) + 1;
        if (num_workers_done == *thread_arg->num_workers)
        {
            /* Everybody's done. */
            CHECK_ERROR (sem_post (thread_arg->sem_all_workers_done));
        }
    }

    sem_destroy (&thread_arg->sem_run);
    num_workers_done = fetch_and_inc(thread_arg->num_workers_done) + 1;
    if (num_workers_done == *thread_arg->num_workers)
    {
        /* Everybody's done. */
        CHECK_ERROR (sem_post (thread_arg->sem_all_workers_done));
    }

    return NULL;
}
