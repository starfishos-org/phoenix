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

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <string.h>

#include "memory.h"
#include "taskQ.h"
#include "queue.h"
#include "synch.h"
#include "locality.h"

static int num_strands_per_chip = 0;

typedef struct {
    task_t              task;
    queue_elem_t        queue_elem;
} tq_entry_t;

typedef struct {
    mr_lock_t  parent;
    uintptr_t  chksum;
    mr_lock_t  *per_thread;
#ifdef TQ_DIAG
    volatile int owner;         /* tid + 1 of the thread inside the section */
#endif
} tq_lock_t;

struct taskQ_t {
    int             num_queues;
    int             num_threads;
    queue_t         **queues;
    queue_t         **free_queues;
    tq_lock_t       *locks;
    /* putting all seeds together may lead to extra coherence traffic among cpus
     * if it's a problem we can pad it by l1 line size */
    /* per-thread random seed */
    unsigned int    *seeds;
    bool            shared_memory;
    mem_shared_arena_t *shared_arena;
#ifdef TQ_DIAG
    uint64_t        magic;      /* TQ_MAGIC while live, poisoned on finalize */
    int             generation; /* which tq_init() call produced this taskQ */
    /* Per-queue count of unlocked tq_enqueue_seq() pushes.  Map tasks all go
     * to queue lgrp (0 here, since loc_mem_to_lgrp() is a stub returning 0);
     * only gen_reduce_tasks() spreads over every queue.  A nonzero count on a
     * queue a *map* worker is popping therefore means the main thread has
     * already moved on to the reduce phase while this phase is still live. */
    unsigned int    enq[64];
#endif
 };

#ifdef TQ_DIAG
#include "processor.h"

/* Number of pool workers currently inside their thread function (tpool.c). */
extern volatile int tq_workers_running;

#define TQ_MAGIC 0x5451474f4f440001ULL

static int tq_generation_counter = 0;

/* Report a task queue whose state cannot be produced by correct execution.
 * Returns nonzero if the queue is unusable and the caller must not touch it. */
static int tq_diag_check (taskQ_t *tq, int idx, int tid, const char *where)
{
    queue_t         *q;
    queue_elem_t    *head;

    if (tq->magic != TQ_MAGIC) {
        printf("[TQDIAG] %s cpu=%d tid=%d idx=%d tq=%p STALE magic=%llx gen=%d\n",
               where, proc_get_cpuid(), tid, idx, (void *)tq,
               (unsigned long long)tq->magic, tq->generation);
        return 1;
    }

    if (idx < 0 || idx >= tq->num_queues) {
        printf("[TQDIAG] %s cpu=%d tid=%d idx=%d OUT-OF-RANGE nq=%d gen=%d\n",
               where, proc_get_cpuid(), tid, idx, tq->num_queues,
               tq->generation);
        return 1;
    }

    q = tq->queues[idx];
    if (q == NULL) {
        printf("[TQDIAG] %s cpu=%d tid=%d idx=%d NULL queue gen=%d\n",
               where, proc_get_cpuid(), tid, idx, tq->generation);
        return 1;
    }

    head = q->lst_list.li_next;
    if (head == &q->lst_list)
        return 0;                       /* empty, and consistently so */

    if (head == NULL || head->li_next == NULL || head->li_prev == NULL
        || head->li_prev != &q->lst_list) {
        printf("[TQDIAG] %s cpu=%d tid=%d idx=%d gen=%d CORRUPT q=%p "
               "q.next=%p q.prev=%p head=%p head.next=%p head.prev=%p "
               "owner=%d enq=%u enq0=%u running=%d\n",
               where, proc_get_cpuid(), tid, idx, tq->generation, (void *)q,
               (void *)q->lst_list.li_next, (void *)q->lst_list.li_prev,
               (void *)head,
               head ? (void *)head->li_next : NULL,
               head ? (void *)head->li_prev : NULL,
               tq->locks[idx].owner,
               idx < 64 ? tq->enq[idx] : 0u, tq->enq[0],
               __atomic_load_n(&tq_workers_running, __ATOMIC_SEQ_CST));
        return 1;
    }

    return 0;
}

static void tq_diag_enter (taskQ_t *tq, int idx, int tid)
{
    int prev = tq->locks[idx].owner;

    if (prev != 0) {
        printf("[TQDIAG] MUTEX-BROKEN cpu=%d tid=%d idx=%d gen=%d "
               "already owned by tid=%d\n",
               proc_get_cpuid(), tid, idx, tq->generation, prev - 1);
    }
    tq->locks[idx].owner = tid + 1;
}

static void tq_diag_leave (taskQ_t *tq, int idx, int tid)
{
    int cur = tq->locks[idx].owner;

    if (cur != tid + 1) {
        printf("[TQDIAG] MUTEX-STOLEN cpu=%d tid=%d idx=%d gen=%d "
               "owner=%d on release\n",
               proc_get_cpuid(), tid, idx, tq->generation, cur - 1);
    }
    tq->locks[idx].owner = 0;
}
#endif /* TQ_DIAG */

typedef int (*dequeue_fn)(taskQ_t *, int, int, queue_elem_t**);

static inline taskQ_t* tq_init_normal(int numThreads, bool shared_memory);
static inline void tq_finalize_normal(taskQ_t* tq);
static inline int tq_dequeue_normal(
    taskQ_t* tq, task_t* task, int lgrp, int tid);
static inline int tq_dequeue_local_only(
    taskQ_t* tq, task_t* task, int lgrp, int tid);
static inline int tq_dequeue_normal_seq (
    taskQ_t* tq, task_t* task, int lgrp, int tid);
static inline int tq_dequeue_normal_internal (
    taskQ_t* tq, task_t* task, int lgrp, int tid, dequeue_fn dequeue_fn,
    int allow_steal);

static void *tq_alloc(taskQ_t *tq, size_t size);
static void *tq_calloc(taskQ_t *tq, size_t num, size_t size);
static void tq_free(taskQ_t *tq, void *ptr);
static queue_t* tq_alloc_queue(taskQ_t *tq);
static void tq_free_queue(taskQ_t *tq, queue_t* q);
static int tq_queue_init(taskQ_t* tq, unsigned int idx);
static void tq_queue_destroy(taskQ_t* tq, unsigned int idx);
static void tq_empty_queue(taskQ_t *tq, queue_t* q);

taskQ_t* tq_init (int num_threads, bool shared_memory)
{
    return tq_init_normal(num_threads, shared_memory);
}

void tq_reset (taskQ_t* tq, int num_threads)
{
}

/**
 * Initialize task queue for a normal machine
 */
static inline taskQ_t* tq_init_normal(int numThreads, bool shared_memory)
{
    int             i;
    taskQ_t         *tq = NULL;
    mem_shared_arena_t *arena = NULL;

    if (shared_memory) {
        arena = mem_shared_arena_create(0);
        tq = mem_shared_arena_calloc(arena, 1, sizeof(taskQ_t));
    } else {
        tq = mem_calloc(1, sizeof(taskQ_t));
    }
    if (tq == NULL) {
        return NULL;
    }
    tq->shared_memory = shared_memory;
    tq->shared_arena = arena;
    if (shared_memory)
        printf("[Phoenix placement] taskq=%p policy=shared arena=%p\n",
               (void *)tq, (void *)arena);

    /* XXX should this be local? */
    num_strands_per_chip = loc_get_lgrp_size ();
    tq->num_threads = numThreads;
    tq->num_queues = tq->num_threads / num_strands_per_chip;

    if (tq->num_queues == 0)
        tq->num_queues = 1;

    tq->queues = (queue_t **)tq_calloc (tq, tq->num_queues, sizeof (queue_t *));
    if (tq->queues == NULL) goto fail_queues;

    tq->free_queues = (queue_t **)tq_calloc (tq,
        tq->num_queues, sizeof (queue_t *));
    if (tq->free_queues == NULL) goto fail_free_queues;

    tq->locks = (tq_lock_t *)tq_calloc (tq, tq->num_queues, sizeof (tq_lock_t));
    if (tq->locks == NULL) goto fail_locks;

    tq->seeds = (unsigned int*)tq_calloc(tq,
        tq->num_threads, sizeof(unsigned int));
    if (tq->seeds == NULL) goto fail_seeds;
    mem_memset(tq->seeds, 0, sizeof(unsigned int) * tq->num_threads);

    for (i = 0; i < tq->num_queues; ++i)
        if (!tq_queue_init(tq, i))
            goto fail_tq_init;

#ifdef TQ_DIAG
    tq->generation = ++tq_generation_counter;
    tq->magic = TQ_MAGIC;
    printf("[TQDIAG] tq_init gen=%d tq=%p nq=%d nthreads=%d q0=%p q3=%p\n",
           tq->generation, (void *)tq, tq->num_queues, tq->num_threads,
           (void *)tq->queues[0],
           (void *)tq->queues[tq->num_queues > 3 ? 3 : 0]);
#endif

    return tq;

fail_tq_init:
    /* destroy all queues that have been allocated */
    i--;
    while (i >= 0) {
        tq_queue_destroy(tq, i);
        --i;
    }
    tq_free(tq, tq->seeds);
fail_seeds:
    tq_free(tq, tq->locks);
fail_locks:
    tq_free(tq, tq->free_queues);
fail_free_queues:
    tq_free(tq, tq->queues);
fail_queues:
    if (shared_memory)
        mem_shared_arena_destroy(arena);
    else
        mem_free(tq);
    return NULL;
}

/**
 * Destroys an initialized queue (i.e. free queue and alloc queue) in task queue
 * @param tq    tq to index
 * @param idx   index of queue to destroy in tq
 */
static void tq_queue_destroy(taskQ_t* tq, unsigned int idx)
{
    int             j;
    uintptr_t       chksum;

    assert (idx < tq->num_queues);

#ifdef TQ_DIAG
    /*
     * tq_empty_queue() pops without the lock, on the assumption that no worker
     * can still be in the queue by the time the taskQ is finalized.
     */
    if (tq->locks[idx].owner != 0) {
        printf("[TQDIAG] UNLOCKED-FINALIZE cpu=%d idx=%d gen=%d races tid=%d "
               "holding the lock\n",
               proc_get_cpuid(), (int)idx, tq->generation,
               tq->locks[idx].owner - 1);
    }
#endif

    tq_empty_queue(tq, tq->queues[idx]);
    tq_free_queue(tq, tq->queues[idx]);

    tq_empty_queue(tq, tq->free_queues[idx]);
    tq_free_queue(tq, tq->free_queues[idx]);

    /* free all lock data associated with queue */
    chksum = 0;
    for (j = 0; j < tq->num_threads; j++) {
        chksum += (uintptr_t)tq->locks[idx].per_thread[j];
        if (tq->shared_memory)
            lock_free_per_thread_shared(tq->locks[idx].per_thread[j]);
        else
            lock_free_per_thread(tq->locks[idx].per_thread[j]);
    }

    if (tq->shared_memory)
        lock_free_shared(tq->locks[idx].parent);
    else
        lock_free(tq->locks[idx].parent);

    tq_free(tq, tq->locks[idx].per_thread);
    tq->locks[idx].per_thread = NULL;
}

/**
 * Initialize a queue for a given index in the task queue
 * @return zero on failure, nonzero on success
 */
static int tq_queue_init(taskQ_t* tq, unsigned int idx)
{
    int     j;

    assert (idx < tq->num_queues);

    tq->queues[idx] = tq_alloc_queue(tq);
    if (tq->queues[idx] == NULL) return 0;

    tq->free_queues[idx] = tq_alloc_queue(tq);
    if (tq->free_queues[idx] == NULL) goto fail_free_queue;

    if (tq->shared_memory)
        tq->locks[idx].parent = lock_alloc_shared(tq->shared_arena);
    else
        tq->locks[idx].parent = lock_alloc();

    tq->locks[idx].per_thread = (mr_lock_t *)tq_calloc(tq,
        tq->num_threads, sizeof(mr_lock_t));
    if (tq->locks[idx].per_thread == NULL) goto fail_priv_alloc;

    tq->locks[idx].chksum = 0;
    for (j = 0; j < tq->num_threads; ++j) {
        mr_lock_t   per_thread;
        if (tq->shared_memory) {
            per_thread = lock_alloc_per_thread_shared(
                tq->locks[idx].parent, tq->shared_arena);
        } else {
            per_thread = lock_alloc_per_thread(tq->locks[idx].parent);
        }
        tq->locks[idx].per_thread[j] = per_thread;
        tq->locks[idx].chksum += (uintptr_t)per_thread;
    }

    return 1;

fail_priv_alloc:
    if (tq->shared_memory)
        lock_free_shared(tq->locks[idx].parent);
    else
        lock_free(tq->locks[idx].parent);
    tq_free_queue(tq, tq->free_queues[idx]);
    tq->free_queues[idx] = NULL;
fail_free_queue:
    tq_free_queue(tq, tq->queues[idx]);
    tq->queues[idx] = NULL;

    return 0;
}

/**
 * Allocates an initialized queue
 * @return NULL on failure, initialized queue pointer on success
 */
static void *tq_alloc(taskQ_t *tq, size_t size)
{
    if (tq->shared_memory)
        return mem_shared_arena_alloc(tq->shared_arena, size);
    return mem_malloc(size);
}

static void *tq_calloc(taskQ_t *tq, size_t num, size_t size)
{
    if (tq->shared_memory)
        return mem_shared_arena_calloc(tq->shared_arena, num, size);
    return mem_calloc(num, size);
}

static void tq_free(taskQ_t *tq, void *ptr)
{
    if (!tq->shared_memory)
        mem_free(ptr);
}

static queue_t* tq_alloc_queue(taskQ_t *tq)
{
    queue_t *q;

    q = (queue_t*)tq_alloc(tq, sizeof(queue_t));
    if (q == NULL) {
        return NULL;
    }

    queue_init(q);

    return q;
}

/**
 * Frees an initialized queue that was allocated on the heap.
 */
static void tq_free_queue(taskQ_t *tq, queue_t* q)
{
    tq_free(tq, q);
}

/**
 * Empties out a queue in the task queue by dequeuing and freeing
 * every task.
 */
static void tq_empty_queue(taskQ_t *tq, queue_t* q)
{
    do {
        tq_entry_t      *entry;
        queue_elem_t    *queue_elem;

        if (queue_pop_front (q, &queue_elem) == 0)
            break;

        entry = queue_entry (queue_elem, tq_entry_t, queue_elem);
        assert (entry != NULL);
        tq_free(tq, entry);
    } while (1);
}

static inline void tq_finalize_normal(taskQ_t* tq)
{
    int i;
    bool shared_memory;
    mem_shared_arena_t *arena;

    assert (tq->queues != NULL);
    assert (tq->free_queues != NULL);
    assert (tq->locks != NULL);

#ifdef TQ_DIAG
    printf("[TQDIAG] tq_finalize gen=%d tq=%p\n", tq->generation, (void *)tq);
    tq->magic = 0xdeaddeaddeaddeadULL;
#endif

    /* destroy all queues */
    for (i = 0; i < tq->num_queues; ++i) {
        tq_queue_destroy(tq, i);
    }

    /* destroy all first level pointers in tq */
    tq_free(tq, tq->queues);
    tq_free(tq, tq->free_queues);
    tq_free(tq, tq->locks);
    tq_free(tq, tq->seeds);

    /* finally kill tq */
    shared_memory = tq->shared_memory;
    arena = tq->shared_arena;
    if (shared_memory)
        mem_shared_arena_destroy(arena);
    else
        mem_free(tq);
}

void tq_finalize (taskQ_t* tq)
{
    tq_finalize_normal(tq);
}

/* Queue TASK at LGRP task queue with locking.
   LGRP is a locality hint denoting to which locality group this task 
   should be queued at. If LGRP is less than 0, the locality group is 
   randomly selected. TID is required for MCS locking. */
int tq_enqueue (taskQ_t* tq, task_t *task, int lgrp, int tid)
{
    tq_entry_t      *entry;
    int             index;

    assert (tq != NULL);
    assert (task != NULL);

    entry = (tq_entry_t *)tq_alloc(tq, sizeof (tq_entry_t));
    if (entry == NULL) {
        return -1;
    }

    mem_memcpy (&entry->task, task, sizeof (task_t));

    index = (lgrp < 0) ? rand_r(&tq->seeds[tid]) : lgrp;
    index %= tq->num_queues;

    lock_acquire (tq->locks[index].per_thread[tid]);
    queue_push_back (tq->queues[index], &entry->queue_elem);
    lock_release (tq->locks[index].per_thread[tid]);

    return 0;
}

/* Queue TASK at LGRP task queue without locking.
   LGRP is a locality hint denoting to which locality group this task
   should be queued at. If LGRP is less than 0, the locality group is
   randomly selected. */
int tq_enqueue_seq (taskQ_t* tq, task_t *task, int lgrp)
{
    tq_entry_t      *entry;
    int             index;

    assert (task != NULL);

    entry = (tq_entry_t *)tq_alloc(tq, sizeof (tq_entry_t));
    if (entry == NULL) {
        return -1;
    }

    mem_memcpy (&entry->task, task, sizeof (task_t));

    index = (lgrp < 0) ? rand() % tq->num_queues : lgrp % tq->num_queues;
#ifdef TQ_DIAG
    if (index < 64)
        tq->enq[index]++;
    /*
     * This push takes no lock.  It is only safe because every caller
     * (gen_map_tasks / gen_reduce_tasks) runs on the main thread between
     * phases, with no worker touching the queue.  If a worker is inside the
     * locked section right now, that assumption is broken -- and an unlocked
     * push racing a locked pop is exactly the shape of the observed damage.
     */
    if (tq->locks[index].owner != 0) {
        printf("[TQDIAG] UNLOCKED-ENQUEUE cpu=%d idx=%d gen=%d races tid=%d "
               "holding the lock\n",
               proc_get_cpuid(), index, tq->generation,
               tq->locks[index].owner - 1);
    }
#endif
    queue_push_back (tq->queues[index], &entry->queue_elem);

    return 0;
}

/**
 * Safely dequeues an element from the normal queue and queues onto free queue
 * @param tq        taskQ to operate on
 * @param idx       index of queue to use
 * @param tid       task id
 * @param qe        queue element of element we operated on
 * @return nonzero if normal queue was empty
 */
static inline int tq_elem_into_free_seq (
    taskQ_t* tq, int idx, int tid, queue_elem_t** qe)
{
    queue_elem_t    *queue_elem = NULL;
    int             ret;

#ifdef TQ_DIAG
    /*
     * Report but do NOT suppress.  Suppressing turns the queue into "empty",
     * so every map worker exits immediately and PCA never finishes -- which
     * also means the run cannot answer whether the crash is gone.  Let the
     * original code run into the fault it would have hit anyway.
     */
    (void)tq_diag_check (tq, idx, tid, "pop");
#endif

    ret = queue_pop_front (tq->queues[idx], &queue_elem);
    if (ret != 0)
        queue_push_back (tq->free_queues[idx], queue_elem);

    *qe = queue_elem;

    return ret;
}

/**
 * Safely dequeues an element from the normal queue and queues onto free queue
 * @param tq        taskQ to operate on
 * @param idx       index of queue to use
 * @param tid       task id
 * @param qe        queue element of element we operated on
 * @return nonzero if normal queue was empty
 */
static inline int tq_elem_into_free (
    taskQ_t* tq, int idx, int tid, queue_elem_t** qe)
{
    int             ret;

    lock_acquire (tq->locks[idx].per_thread[tid]);
#ifdef TQ_DIAG
    tq_diag_enter (tq, idx, tid);
#endif
    ret = tq_elem_into_free_seq (tq, idx, tid, qe);
#ifdef TQ_DIAG
    tq_diag_leave (tq, idx, tid);
#endif
    lock_release (tq->locks[idx].per_thread[tid]);

    return ret;
}

static inline int tq_dequeue_normal_seq (
    taskQ_t* tq, task_t* task, int lgrp, int tid)
{
    return tq_dequeue_normal_internal (
        tq, task, lgrp, tid, tq_elem_into_free_seq, 1);
}

static inline int tq_dequeue_normal(
    taskQ_t* tq, task_t* task, int lgrp, int tid)
{
    return tq_dequeue_normal_internal (
        tq, task, lgrp, tid, tq_elem_into_free, 1);
}

static inline int tq_dequeue_local_only(
    taskQ_t* tq, task_t* task, int lgrp, int tid)
{
    return tq_dequeue_normal_internal (
        tq, task, lgrp, tid, tq_elem_into_free, 0);
}

static inline int tq_dequeue_normal_internal (
    taskQ_t* tq, task_t* task, int lgrp, int tid, dequeue_fn dequeue_fn,
    int allow_steal)
{
    int             i, ret, index;
    queue_elem_t    *queue_elem;
    tq_entry_t      *entry;

    assert (task != NULL);

    mem_memset (task, 0, sizeof (task_t));

#ifdef TQ_DIAG
    if (tq->magic != TQ_MAGIC) {
        printf("[TQDIAG] dequeue cpu=%d tid=%d lgrp=%d tq=%p STALE magic=%llx "
               "gen=%d steal=%d\n",
               proc_get_cpuid(), tid, lgrp, (void *)tq,
               (unsigned long long)tq->magic, tq->generation, allow_steal);
        return 0;
    }
#endif

    index = (lgrp < 0) ? rand_r(&tq->seeds[tid]) : lgrp;
    index %= tq->num_queues;
    ret = (*dequeue_fn)(tq, index, tid, &queue_elem);

   /* Do task stealing if nothing on our queue.
      Cycle through all indexes until success or exhaustion */
    if (allow_steal) {
        for (i = (index + 1) % tq->num_queues;
            (ret == 0) && (i != index);
            i = (i + 1) % tq->num_queues)
        {
            ret = (*dequeue_fn)(tq, i, tid, &queue_elem);
        }
    }

    if (ret == 0) {
        /* There really is no more work. */
        return 0;
    }

    entry = queue_entry (queue_elem, tq_entry_t, queue_elem);
    assert (entry != NULL);

    mem_memcpy (task, &entry->task, sizeof (task_t));

    return 1;
}

int tq_dequeue (taskQ_t* tq, task_t *task, int lgrp, int tid)
{
    return tq_dequeue_normal(tq, task, lgrp, tid);
}

int tq_dequeue_local (taskQ_t* tq, task_t *task, int lgrp, int tid)
{
    return tq_dequeue_local_only(tq, task, lgrp, tid);
}
