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

#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/time.h>
#include <inttypes.h>
#ifdef _CHCORE_
#include <pthread.h>
#include <sched.h>
#include <chcore/syscall.h>
#endif

#include "map_reduce.h"
#include "stddefines.h"
#include "processor.h"

char *fname;
extern int thread_num;

void parse_args(int argc, char **argv) 
{
    int c;
    extern char *optarg;
    extern int optind;

    thread_num = 1;
    while ((c = getopt(argc, argv, "f:t:i:")) != EOF) 
    {
        switch (c) {
            case 'f':
                fname = malloc(strlen(optarg) + 1);
                strcpy(fname, optarg);
                break;
            case 't':   
                thread_num = atoi(optarg);
                break;
            #ifdef _CHCORE_
            case 'i':
                strcpy(thread_bind_cpu_filename, optarg);
                break;
            #endif
            case '?':
                printf("Usage: %s -f <filename> -t <thread_num> -i <thread bind cpu filename>\n", argv[0]);
                exit(1);
        }
    }
    
    if (thread_num <= 0 || fname == NULL) {
        printf("Illegal argument value. All values must be numeric and greater than 0\n");
        exit(1);
    }
    
    printf("Thread number = %d\n", thread_num);
    printf("File name = %s\n", fname);
    int fd = open(fname, O_RDONLY);
    if (fd < 0) {
        printf("Failed to open file %s\n", fname);
        exit(1);
    }
    close(fd);
    #ifdef _CHCORE_
    if (strlen(thread_bind_cpu_filename) == 0) {
        fprintf(stderr, "Thread bind cpu filename is not set default to linear_regression_bind_cpu.txt\n");
        strcpy(thread_bind_cpu_filename, "linear_regression_bind_cpu.txt");
    }
    fprintf(stderr, "Thread bind cpu filename=%s\n", thread_bind_cpu_filename);
    if (parse_cpu_bind_file(thread_bind_cpu_filename) < 0) {
        fprintf(stderr, "Failed to parse cpu bind file\n");
    } else {
        thread_bind_cpu_set = true;
    }
    #endif
}

typedef struct {
    char x;
    char y;
} POINT_T;

#ifdef _CHCORE_

#define LR_MAX_MACHINES 64
#define LR_MAX_WORKERS 1024
#define LR_TRANSFER_BYTES (4 * 1024 * 1024)
#define LR_STACK_BYTES (128 * 1024)
#define LR_POINTS_PER_TASK (MR_L1CACHESIZE / sizeof(POINT_T))

typedef struct {
    long long sx;
    long long sy;
    long long sxx;
    long long syy;
    long long sxy;
} lr_sums_t;

static void add_sums(lr_sums_t *dst, const lr_sums_t *src)
{
    dst->sx += src->sx;
    dst->sy += src->sy;
    dst->sxx += src->sxx;
    dst->syy += src->syy;
    dst->sxy += src->sxy;
}

static lr_sums_t sum_points(const POINT_T *data, uint64_t count)
{
    lr_sums_t sums = {0};

    for (uint64_t i = 0; i < count; i++) {
        long long x = data[i].x;
        long long y = data[i].y;

        sums.sx += x;
        sums.sy += y;
        sums.sxx += x * x;
        sums.syy += y * y;
        sums.sxy += x * y;
    }
    return sums;
}

struct lr_shared;
struct lr_worker_arg;

typedef struct {
    struct lr_shared *shared;
    int worker_start;
    int worker_count;
    uint64_t point_count;
    uint64_t input_bytes;
    POINT_T *input;
    struct lr_worker_arg *args;
    void *worker_stacks;
    volatile int start;
    volatile int ready_count;
    volatile int done;
    uint64_t points_processed;
    lr_sums_t sums;
} __attribute__((aligned(64))) lr_machine_slot_t;

typedef struct lr_shared {
    volatile int start;
    volatile int cleanup;
    volatile unsigned int transfer_seq;
    volatile unsigned int transfer_ack;
    int machine_count;
    uint64_t data_points;
    uint64_t transfer_va;
    uint64_t transfer_offset;
    uint64_t transfer_size;
    int worker_cpu[LR_MAX_WORKERS];
    pthread_t worker[LR_MAX_WORKERS];
    lr_machine_slot_t machines[LR_MAX_MACHINES];
} lr_shared_t;

typedef struct lr_worker_arg {
    lr_machine_slot_t *machine;
    uint64_t worker_index;
    uint64_t points_processed;
    volatile int complete;
    int cpu;
    lr_sums_t sums;
} __attribute__((aligned(64))) lr_worker_arg_t;

typedef struct {
    lr_shared_t *shared;
    pthread_t coordinator[LR_MAX_MACHINES];
    void *coordinator_stacks;
} lr_run_t;

static void lr_check(int error)
{
    if (error)
        abort();
}

static void *lr_map(size_t size, int shared)
{
    void *addr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS |
                          (shared ? MAP_FLAG_SHARED : 0),
                      -1, 0);

    lr_check(addr == MAP_FAILED);
    return addr;
}

static void *lr_worker(void *opaque)
{
    lr_worker_arg_t *arg = (lr_worker_arg_t *)opaque;
    lr_machine_slot_t *machine = arg->machine;
    lr_sums_t sums = {0};
    uint64_t total_tasks =
        (machine->point_count + LR_POINTS_PER_TASK - 1) /
        LR_POINTS_PER_TASK;

    lr_check(proc_bind_thread(arg->cpu) != 0);
    __atomic_fetch_add(&machine->ready_count, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&machine->start, __ATOMIC_ACQUIRE))
        sched_yield();
    for (uint64_t task = arg->worker_index; task < total_tasks;
         task += machine->worker_count) {
        uint64_t first = task * LR_POINTS_PER_TASK;
        uint64_t count = LR_POINTS_PER_TASK;
        lr_sums_t task_sums;

        if (first + count > machine->point_count)
            count = machine->point_count - first;
        task_sums = sum_points(machine->input + first, count);
        add_sums(&sums, &task_sums);
        arg->points_processed += count;
    }
    arg->sums = sums;
    __atomic_store_n(&arg->complete, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void *lr_machine_coordinator(void *opaque)
{
    lr_machine_slot_t *slot = (lr_machine_slot_t *)opaque;
    lr_shared_t *shared = slot->shared;
    size_t args_size = (size_t)slot->worker_count * sizeof(lr_worker_arg_t);
    size_t worker_stacks_size =
        (size_t)slot->worker_count * LR_STACK_BYTES;
    pthread_attr_t attr;
    lr_sums_t machine_sums = {0};
    uint64_t machine_points = 0;

    lr_check(proc_bind_thread(shared->worker_cpu[slot->worker_start]) != 0);
    slot->input = (POINT_T *)lr_map(slot->input_bytes, 0);
    uint64_t loaded = 0;
    unsigned int observed_seq =
        __atomic_load_n(&shared->transfer_ack, __ATOMIC_ACQUIRE);
    while (loaded < slot->input_bytes) {
        unsigned int seq;

        do {
            seq = __atomic_load_n(&shared->transfer_seq, __ATOMIC_ACQUIRE);
            if (seq == observed_seq)
                sched_yield();
        } while (seq == observed_seq);

        uint64_t offset = shared->transfer_offset;
        uint64_t size = shared->transfer_size;
        lr_check(offset != loaded || loaded + size > slot->input_bytes);
        memcpy((char *)slot->input + offset,
               (void *)(uintptr_t)shared->transfer_va, size);
        loaded += size;
        observed_seq = seq;
        __atomic_store_n(&shared->transfer_ack, seq, __ATOMIC_RELEASE);
    }

    slot->args = (lr_worker_arg_t *)lr_map(args_size, 0);
    slot->worker_stacks = lr_map(worker_stacks_size, 0);
    memset(slot->args, 0, args_size);
    lr_check(pthread_attr_init(&attr) != 0);

    for (int i = 0; i < slot->worker_count; i++) {
        int worker = slot->worker_start + i;

        slot->args[i].machine = slot;
        slot->args[i].worker_index = (uint64_t)i;
        slot->args[i].cpu = shared->worker_cpu[worker];
        lr_check(pthread_attr_setstack(
                     &attr,
                     (char *)slot->worker_stacks +
                         (size_t)i * LR_STACK_BYTES,
                     LR_STACK_BYTES) != 0);
        lr_check(pthread_create(&shared->worker[worker], &attr, lr_worker,
                                &slot->args[i]) != 0);
    }
    lr_check(pthread_attr_destroy(&attr) != 0);
    while (__atomic_load_n(&slot->ready_count, __ATOMIC_ACQUIRE) !=
           slot->worker_count)
        sched_yield();
    while (!__atomic_load_n(&shared->start, __ATOMIC_ACQUIRE))
        sched_yield();
    __atomic_store_n(&slot->start, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < slot->worker_count; i++) {
        while (!__atomic_load_n(&slot->args[i].complete, __ATOMIC_ACQUIRE))
            sched_yield();
        add_sums(&machine_sums, &slot->args[i].sums);
        machine_points += slot->args[i].points_processed;
    }
    slot->sums = machine_sums;
    slot->points_processed = machine_points;
    __atomic_store_n(&slot->done, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&shared->cleanup, __ATOMIC_ACQUIRE))
        sched_yield();
    for (int i = 0; i < slot->worker_count; i++)
        lr_check(pthread_join(shared->worker[slot->worker_start + i], NULL) !=
                 0);
    return NULL;
}

static int lr_detect_machines(lr_shared_t *shared)
{
    int cpus_per_machine = (int)usys_get_machine_cpu_count();
    int machine = 0;

    lr_check(cpus_per_machine <= 0);
    lr_check(thread_num <= 0 || thread_num > LR_MAX_WORKERS);

    shared->machine_count = 1;
    shared->machines[0].worker_start = 0;
    for (int i = 0; i < thread_num; i++) {
        int cpu = thread_bind_cpu_set ? thread_bind_cpu_list[i] : i;

        shared->worker_cpu[i] = cpu;
        if (i > 0 && cpu / cpus_per_machine !=
                         shared->worker_cpu[i - 1] / cpus_per_machine) {
            machine++;
            lr_check(machine >= LR_MAX_MACHINES);
            shared->machines[machine].worker_start = i;
            shared->machine_count++;
        }
        shared->machines[machine].worker_count++;
    }
    return shared->machine_count;
}

static void lr_read_exact(int fd, void *buffer, size_t size)
{
    size_t done = 0;

    while (done < size) {
        ssize_t got = read(fd, (char *)buffer + done, size - done);

        lr_check(got <= 0);
        done += (size_t)got;
    }
}

static void lr_prepare(lr_run_t *run, int fd, uint64_t data_size)
{
    lr_shared_t *shared = (lr_shared_t *)lr_map(sizeof(*shared), 1);
    memset(shared, 0, sizeof(*shared));
    run->shared = shared;
    shared->data_points = data_size / sizeof(POINT_T);
    lr_check(LR_POINTS_PER_TASK == 0);

    int machines = lr_detect_machines(shared);
    lr_check(shared->data_points < (uint64_t)machines);
    uint64_t base = shared->data_points / (uint64_t)machines;
    uint64_t leftover = shared->data_points % (uint64_t)machines;
    run->coordinator_stacks =
        lr_map((size_t)machines * LR_STACK_BYTES, 1);

    for (int m = 0; m < machines; m++) {
        lr_machine_slot_t *slot = &shared->machines[m];

        slot->shared = shared;
        slot->point_count = base + ((uint64_t)m < leftover ? 1 : 0);
        slot->input_bytes = slot->point_count * sizeof(POINT_T);
        lr_check(slot->worker_count <= 0);
    }

    size_t transfer_size = LR_TRANSFER_BYTES;
    void *transfer = lr_map(transfer_size, 1);
    shared->transfer_va = (uint64_t)(uintptr_t)transfer;
    lr_check(lseek(fd, 0, SEEK_SET) != 0);

    unsigned int seq = 0;
    for (int m = 0; m < machines; m++) {
        lr_machine_slot_t *slot = &shared->machines[m];
        pthread_attr_t attr;

        lr_check(pthread_attr_init(&attr) != 0);
        lr_check(pthread_attr_setstack(
                     &attr,
                     (char *)run->coordinator_stacks +
                         (size_t)m * LR_STACK_BYTES,
                     LR_STACK_BYTES) != 0);
        lr_check(pthread_create(&run->coordinator[m], &attr,
                                lr_machine_coordinator, slot) != 0);
        lr_check(pthread_attr_destroy(&attr) != 0);

        for (uint64_t offset = 0; offset < slot->input_bytes;) {
            size_t bytes = transfer_size;

            if (offset + bytes > slot->input_bytes)
                bytes = (size_t)(slot->input_bytes - offset);
            lr_read_exact(fd, transfer, bytes);
            shared->transfer_offset = offset;
            shared->transfer_size = bytes;
            __atomic_store_n(&shared->transfer_seq, ++seq, __ATOMIC_RELEASE);
            while (__atomic_load_n(&shared->transfer_ack, __ATOMIC_ACQUIRE) !=
                   seq)
                sched_yield();
            offset += bytes;
        }
    }

    for (int m = 0; m < machines; m++) {
        while (__atomic_load_n(&shared->machines[m].ready_count,
                               __ATOMIC_ACQUIRE) !=
               shared->machines[m].worker_count)
            sched_yield();
    }
    lr_check(munmap(transfer, transfer_size) != 0);
}

static lr_sums_t lr_execute(lr_run_t *run)
{
    lr_shared_t *shared = run->shared;
    lr_sums_t total = {0};
    uint64_t total_points = 0;

    __atomic_store_n(&shared->start, 1, __ATOMIC_RELEASE);

    for (int m = 0; m < shared->machine_count; m++) {
        lr_machine_slot_t *slot = &shared->machines[m];

        while (!__atomic_load_n(&slot->done, __ATOMIC_ACQUIRE))
            sched_yield();
        lr_check(slot->points_processed != slot->point_count);
        total_points += slot->points_processed;
        add_sums(&total, &slot->sums);
    }
    lr_check(total_points != shared->data_points);
    return total;
}

static void lr_rehome(void *addr, size_t size)
{
    volatile unsigned char *bytes = (volatile unsigned char *)addr;

    for (size_t offset = 0; offset < size; offset += 4096) {
        unsigned char value = bytes[offset];

        bytes[offset] = value;
    }
}

static void lr_finish(lr_run_t *run)
{
    lr_shared_t *shared = run->shared;

    __atomic_store_n(&shared->cleanup, 1, __ATOMIC_RELEASE);
    for (int m = 0; m < shared->machine_count; m++)
        lr_check(pthread_join(run->coordinator[m], NULL) != 0);
    lr_check(proc_bind_thread(shared->worker_cpu[0]) != 0);
    for (int m = 1; m < shared->machine_count; m++) {
        lr_machine_slot_t *slot = &shared->machines[m];

        lr_rehome(slot->input, slot->input_bytes);
        lr_rehome(slot->args,
                  (size_t)slot->worker_count * sizeof(lr_worker_arg_t));
        lr_rehome(slot->worker_stacks,
                  (size_t)slot->worker_count * LR_STACK_BYTES);
    }
    for (int m = 0; m < shared->machine_count; m++) {
        lr_machine_slot_t *slot = &shared->machines[m];

        lr_check(munmap(slot->args,
                        (size_t)slot->worker_count *
                            sizeof(lr_worker_arg_t)) != 0);
        lr_check(munmap(slot->worker_stacks,
                        (size_t)slot->worker_count * LR_STACK_BYTES) != 0);
        lr_check(munmap(slot->input, slot->input_bytes) != 0);
    }
    lr_check(munmap(run->coordinator_stacks,
                    (size_t)shared->machine_count * LR_STACK_BYTES) != 0);
    lr_check(munmap(shared, sizeof(*shared)) != 0);
    run->shared = NULL;
}

#endif /* _CHCORE_ */

enum {
    KEY_SX = 0,
    KEY_SY,
    KEY_SXX,
    KEY_SYY,
    KEY_SXY,
};

static int intkeycmp(const void *v1, const void *v2)
{
    intptr_t i1 = (intptr_t)v1;
    intptr_t i2 = (intptr_t)v2;

    if (i1 < i2) 
         return 1;
    else if (i1 > i2) 
         return -1;
    else 
         return 0;
}

/** sort_map()
 *  Sorts based on the val output of wordcount
 */
static void linear_regression_map(map_args_t *args) 
{
    assert(args);
    
    POINT_T *data = (POINT_T *)args->data;
    int i;

    assert(data);

    long long * SX  = CALLOC(sizeof(long long), 1);
    long long * SXX = CALLOC(sizeof(long long), 1);
    long long * SY  = CALLOC(sizeof(long long), 1);
    long long * SYY = CALLOC(sizeof(long long), 1);
    long long * SXY = CALLOC(sizeof(long long), 1);

    register long long x, y;
    register long long sx = 0, sxx = 0, sy = 0, syy = 0, sxy = 0;

    for (i = 0; i < args->length; i++)
    {
        //Compute SX, SY, SYY, SXX, SXY
        x = data[i].x;
        y = data[i].y;

        sx  += x;
        sxx += x * x;
        sy  += y;
        syy += y * y;
        sxy += x * y;
    }

    *SX = sx;
    *SXX = sxx;
    *SY = sy;
    *SYY = syy;
    *SXY = sxy;

    emit_intermediate((void*)KEY_SX,  (void*)SX,  sizeof(void*)); 
    emit_intermediate((void*)KEY_SXX, (void*)SXX, sizeof(void*)); 
    emit_intermediate((void*)KEY_SY,  (void*)SY,  sizeof(void*)); 
    emit_intermediate((void*)KEY_SYY, (void*)SYY, sizeof(void*)); 
    emit_intermediate((void*)KEY_SXY, (void*)SXY, sizeof(void*)); 
}

static int linear_regression_partition(int reduce_tasks, void* key, int key_size)
{
    return default_partition(reduce_tasks, (void *)&key, key_size);
}

/** linear_regression_reduce()
 *
 */
static void linear_regression_reduce(void *key_in, iterator_t *itr)
{
    long long *sumptr = CALLOC(sizeof(long long), 1);
    register long long sum = 0;
    long long *val;

    assert (itr);

    while (iter_next (itr, (void **)&val))
    {
        sum += *val;
        free (val);
    }

    *sumptr = sum;
    emit(key_in, (void *)sumptr);
}

#ifndef _CHCORE_
#pragma GCC diagnostic push
#pragma GCC optimize("O0")
static void access_pages(char *fdata, int size) {
    volatile char p;
    for (int i = 0; i < size; i += 4096) {
        p = (volatile char )fdata[i];
    }
    (void)p;
}
#pragma GCC diagnostic pop
#endif

static void *linear_regression_combiner (iterator_t *itr)
{
    long long *sumptr = CALLOC(sizeof(long long), 1);
    register long long sum = 0;
    long long *val;

    assert(itr);

    while (iter_next (itr, (void **)&val))
    {
        sum += *val;
        free (val);
    }

    *sumptr = sum;
    return (void *)sumptr;
}

int main(int argc, char *argv[]) {

    final_data_t final_vals;
    int fd;
#ifndef _CHCORE_
    char * fdata;
#else
    lr_run_t lr_run;
    lr_sums_t lr_sums;
#endif
    struct stat finfo;
    int i;

    struct timeval begin, end;

    get_time (&begin);

    parse_args(argc, argv);

    printf("Linear Regression: Running...\n");
    
    // Read in the file
    fd = open(fname, O_RDONLY);
    if (fd < 0) {
        printf("Failed to open file %s\n", fname);
        exit(1);
    }
    // Get the file info (for file length)
    CHECK_ERROR(fstat(fd, &finfo) < 0);
#ifdef _CHCORE_
    lr_prepare(&lr_run, fd,
               finfo.st_size - (finfo.st_size % sizeof(POINT_T)));
    CHECK_ERROR(close(fd) < 0);
    fd = -1;
#elif !defined(NO_MMAP)
    // Memory map the file
    CHECK_ERROR((fdata = mmap(0, finfo.st_size + 1, 
        PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0)) == NULL);
#else
    uint64_t ret;

    /* The input is read by every map worker on every machine, so it is shared
     * state: allocate it in CXL rather than letting it follow
     * DSM_USER_MALLOC_MODE into the loader machine's local DRAM. */
    fdata = (char *)mem_malloc_shared (finfo.st_size);
    CHECK_ERROR (fdata == NULL);

    ret = read (fd, fdata, finfo.st_size);
    CHECK_ERROR (ret != finfo.st_size);
#endif

#ifndef _CHCORE_
    access_pages(fdata, finfo.st_size);
#endif

#ifndef _CHCORE_
    CHECK_ERROR (map_reduce_init ());

    // Setup scheduler args
    map_reduce_args_t map_reduce_args;
    memset(&map_reduce_args, 0, sizeof(map_reduce_args_t));
    map_reduce_args.task_data = fdata; // Array to regress
    map_reduce_args.map = linear_regression_map;
    map_reduce_args.reduce = linear_regression_reduce; // Identity Reduce
    map_reduce_args.combiner = linear_regression_combiner;
    map_reduce_args.splitter = NULL; // Array splitter;
    map_reduce_args.key_cmp = intkeycmp;
    map_reduce_args.unit_size = sizeof(POINT_T);
    map_reduce_args.partition = linear_regression_partition; 
    map_reduce_args.result = &final_vals;
    map_reduce_args.data_size = finfo.st_size - (finfo.st_size % map_reduce_args.unit_size);
    map_reduce_args.L1_cache_size = atoi(GETENV("MR_L1CACHESIZE"));//1024 * 512;
    map_reduce_args.num_map_threads = atoi(GETENV("MR_NUMTHREADS"));//8;
    map_reduce_args.num_reduce_threads = atoi(GETENV("MR_NUMTHREADS"));//16;
    map_reduce_args.num_merge_threads = atoi(GETENV("MR_NUMTHREADS"));//8;
    map_reduce_args.num_procs = atoi(GETENV("MR_NUMPROCS"));//16;
    map_reduce_args.key_match_factor = (float)atof(GETENV("MR_KEYMATCHFACTOR"));//2;
#endif

#ifdef _CHCORE_
    printf("Linear Regression: Calling locality-aware scheduler\n");
#else
    printf("Linear Regression: Calling MapReduce Scheduler\n");
#endif

    get_time (&end);

#ifdef TIMING
    fprintf (stderr, "initialize: %u\n", time_diff (&end, &begin));
#endif

    get_time (&begin);
#ifdef _CHCORE_
    lr_sums = lr_execute(&lr_run);
#else
    CHECK_ERROR (map_reduce (&map_reduce_args) < 0);
#endif
    get_time (&end);

#ifdef TIMING
    fprintf (stderr, "library: %u\n", time_diff (&end, &begin));
#endif

    get_time (&begin);

    long long n;
    double a, b, xbar, ybar, r2;
    long long SX_ll = 0, SY_ll = 0, SXX_ll = 0, SYY_ll = 0, SXY_ll = 0;
#ifdef _CHCORE_
    SX_ll = lr_sums.sx;
    SY_ll = lr_sums.sy;
    SXX_ll = lr_sums.sxx;
    SYY_ll = lr_sums.syy;
    SXY_ll = lr_sums.sxy;
    lr_finish(&lr_run);
#else
    CHECK_ERROR (map_reduce_finalize ());
    // ADD UP RESULTS
    for (i = 0; i < final_vals.length; i++)
    {
        keyval_t * curr = &final_vals.data[i];
        switch ((intptr_t)curr->key)
        {
        case KEY_SX:
             SX_ll = (*(long long*)curr->val);
             break;
        case KEY_SY:
             SY_ll = (*(long long*)curr->val);
             break;
        case KEY_SXX:
             SXX_ll = (*(long long*)curr->val);
             break;
        case KEY_SYY:
             SYY_ll = (*(long long*)curr->val);
             break;
        case KEY_SXY:
             SXY_ll = (*(long long*)curr->val);
             break;
        default:
             // INVALID KEY
             CHECK_ERROR(1);
        }
        free(curr->val);
    }
#endif

    double SX = (double)SX_ll;
    double SY = (double)SY_ll;
    double SXX= (double)SXX_ll;
    double SYY= (double)SYY_ll;
    double SXY= (double)SXY_ll;

    n = (long long) finfo.st_size / sizeof(POINT_T); 
    b = (double)(n*SXY - SX*SY) / (n*SXX - SX*SX);
    a = (SY_ll - b*SX_ll) / n;
    xbar = (double)SX_ll / n;
    ybar = (double)SY_ll / n;
    r2 = (double)(n*SXY - SX*SY) * (n*SXY - SX*SY) / ((n*SXX - SX*SX)*(n*SYY - SY*SY));

    printf("Linear Regression Results:\n");
    printf("\ta     = %lf\n", a);
    printf("\tb     = %lf\n", b);
    printf("\txbar = %lf\n", xbar);
    printf("\tybar = %lf\n", ybar);
    printf("\tr2    = %lf\n", r2);
    printf("\tSX    = %lld\n", SX_ll);
    printf("\tSY    = %lld\n", SY_ll);
    printf("\tSXX  = %lld\n", SXX_ll);
    printf("\tSYY  = %lld\n", SYY_ll);
    printf("\tSXY  = %lld\n", SXY_ll);

#ifndef _CHCORE_
    free(final_vals.data);

#ifndef NO_MMAP
    CHECK_ERROR(munmap(fdata, finfo.st_size + 1) < 0);
#else
    mem_free_shared (fdata, finfo.st_size);
#endif
    CHECK_ERROR(close(fd) < 0);
#endif

    get_time (&end);

#ifdef TIMING
    fprintf (stderr, "finalize: %u\n", time_diff (&end, &begin));
#endif

    fprintf (stderr, "done\n");

    return 0;
}
