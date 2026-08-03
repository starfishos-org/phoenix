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
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/mman.h>

#include "stddefines.h"
#include "map_reduce.h"
#include "../../src/memory.h"
#include "locality.h"
#include "processor.h"

#define MAX_PCA_LGRPS 64

typedef enum {
    PCA_PHASE_MEAN,
    PCA_PHASE_COVARIANCE,
} pca_phase_t;

typedef struct {
    int unit_size;
    uint64_t next_work;
    uint64_t total_work;
    pca_phase_t phase;
    int num_lgrps;
    int rows;
    int cols;
    int grid_size;
    size_t matrix_bytes;
    uint64_t covariance_elems;
    int *machine_matrix[MAX_PCA_LGRPS];
    intptr_t *mean;
    intptr_t *covariance;
} pca_data_t;

#define DEF_GRID_SIZE 100  // all values in the matrix are from 0 to this value 
#define DEF_NUM_ROWS 10
#define DEF_NUM_COLS 10

static pca_data_t *pca_data;
int num_rows;
int num_cols;
int grid_size;
extern int thread_num;
int memory_malloc_type;
static bool shared_coordination;

static void *pca_coord_calloc(size_t num, size_t size)
{
    if (shared_coordination)
        return mem_shared_calloc(num, size);
    return mem_calloc(num, size);
}

static void pca_coord_free(void *ptr)
{
    if (shared_coordination)
        mem_shared_free(ptr);
    else
        mem_free(ptr);
}

typedef struct {
    int lgrp;
    int *matrix;
    pca_data_t *data;
} pca_loader_arg_t;

static void generate_points(int *points, int rows, int cols, int max_value)
{
    uint64_t state = 0;
    size_t elements = (size_t)rows * (size_t)cols;

    for (size_t i = 0; i < elements; ++i) {
        state = UINT64_C(6364136223846793005) * state + 1;
        points[i] = (int)(state >> 33) % max_value;
    }
}

static void require_success(int ret, int lgrp, const char *operation)
{
    if (ret != 0) {
        fprintf(stderr, "PCA locality: lgrp %d %s failed (ret=%d)\n",
                lgrp, operation, ret);
        abort();
    }
}

static void *pca_loader(void *opaque)
{
    pca_loader_arg_t *arg = opaque;
    pca_data_t *data = arg->data;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;

    require_success(proc_bind_thread(loc_get_lgrp_first_cpu(arg->lgrp)),
                    arg->lgrp, "bind");
    arg->matrix = mmap(NULL, data->matrix_bytes, PROT_READ | PROT_WRITE,
                       flags, -1, 0);
    if (arg->matrix == MAP_FAILED) {
        fprintf(stderr, "PCA locality: lgrp %d input mmap failed\n", arg->lgrp);
        abort();
    }
    /* This local generator reproduces musl rand() from its default seed, so
     * replicas match the original benchmark without sharing PRNG state. */
    generate_points(arg->matrix, data->rows, data->cols, data->grid_size);
    fprintf(stderr,
            "[PCA placement] lgrp=%d input=%p bytes=%zu policy=default initialized=local\n",
            arg->lgrp, (void *)arg->matrix, data->matrix_bytes);
    return NULL;
}

static void load_machine_inputs(void)
{
    assert(pca_data->num_lgrps > 0 &&
           pca_data->num_lgrps <= MAX_PCA_LGRPS);

    for (int lgrp = 0; lgrp < pca_data->num_lgrps; ++lgrp) {
        pthread_t thread;
        pca_loader_arg_t *arg = pca_coord_calloc(1, sizeof(*arg));

        arg->lgrp = lgrp;
        arg->data = pca_data;
        require_success(pthread_create(&thread, NULL, pca_loader, arg),
                        lgrp, "loader create");
        require_success(pthread_join(thread, NULL), lgrp, "loader join");
        pca_data->machine_matrix[lgrp] = arg->matrix;
        pca_coord_free(arg);
    }
    /* pthread_join may resume the caller on the joined worker's machine.
     * Return before emitting the aggregate evidence so the runner's archived
     * machine-0 log contains every replica placement. */
    require_success(proc_bind_thread(loc_get_lgrp_first_cpu(0)), 0,
                    "main bind");
    for (int lgrp = 0; lgrp < pca_data->num_lgrps; ++lgrp) {
        fprintf(stderr,
                "[PCA placement summary] lgrp=%d input=%p bytes=%zu "
                "policy=default initializer_cpu=%d\n",
                lgrp, (void *)pca_data->machine_matrix[lgrp],
                pca_data->matrix_bytes, loc_get_lgrp_first_cpu(lgrp));
    }
}

static void *pca_cleanup_input(void *opaque)
{
    pca_loader_arg_t *arg = opaque;
    int lgrp = arg->lgrp;
    pca_data_t *data = arg->data;

    require_success(proc_bind_thread(loc_get_lgrp_first_cpu(lgrp)), lgrp,
                    "cleanup bind");
    require_success(munmap(data->machine_matrix[lgrp], data->matrix_bytes),
                    lgrp,
                    "input unmap");
    data->machine_matrix[lgrp] = NULL;
    return NULL;
}

static void cleanup_machine_inputs(void)
{
    for (int lgrp = 0; lgrp < pca_data->num_lgrps; ++lgrp) {
        pthread_t thread;
        pca_loader_arg_t *arg = pca_coord_calloc(1, sizeof(*arg));

        arg->lgrp = lgrp;
        arg->data = pca_data;
        require_success(pthread_create(
                            &thread, NULL, pca_cleanup_input, arg),
                        lgrp, "cleanup create");
        require_success(pthread_join(thread, NULL), lgrp, "cleanup join");
        pca_coord_free(arg);
    }
}

/** parse_args()
 *  Parse the user arguments to determine the number of rows and colums
 */  
void parse_args(int argc, char **argv) 
{
    int c;
    extern char *optarg;
    
    num_rows = DEF_NUM_ROWS;
    num_cols = DEF_NUM_COLS;
    grid_size = DEF_GRID_SIZE;
    thread_num = 1;

    while ((c = getopt(argc, argv, "r:c:t:s:i:")) != EOF) 
    {
        switch (c) {
            case 'r':
                num_rows = atoi(optarg);
                break;
            case 'c':
                num_cols = atoi(optarg);
                break;
            case 's':
                grid_size = atoi(optarg);
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
                fprintf(stderr, "Usage: %s -r <num_rows> -c <num_cols> -s <max value> -t <thread_num> -i <thread bind cpu filename>\n", argv[0]);
                exit(1);
        }
    }
    
    if (num_rows <= 0 || num_cols <= 1 || grid_size <= 0 || thread_num <= 0) {
        fprintf(stderr, "Rows, grid size, and thread count must be positive; columns must exceed one\n");
        exit(1);
    }

    fprintf(stderr, "Number of rows = %d\n", num_rows);
    fprintf(stderr, "Number of cols = %d\n", num_cols);
    fprintf(stderr, "Max value for each element = %d\n", grid_size);   
    fprintf(stderr, "Number of threads=%d\n", thread_num);  
    #ifdef _CHCORE_
    if (strlen(thread_bind_cpu_filename) == 0) {
        fprintf(stderr, "Thread bind cpu filename is not set default to pca_bind_cpu.txt\n");
        strcpy(thread_bind_cpu_filename, "pca_bind_cpu.txt");
    }
    fprintf(stderr, "Thread bind cpu filename=%s\n", thread_bind_cpu_filename);
    if (parse_cpu_bind_file(thread_bind_cpu_filename) < 0) {
        fprintf(stderr, "Failed to parse cpu bind file\n");
    } else {
        thread_bind_cpu_set = true;
    }
    #endif
}

/** dump_points()
 *  Print the values in the matrix to the screen
 */
void dump_points(int **vals, int rows, int cols)
{
    int i, j;
    
    for (i = 0; i < rows; i++) 
    {
        for (j = 0; j < cols; j++)
        {
            dprintf("%5d ",vals[i][j]);
        }
        dprintf("\n");
    }
}

/** mymeancmp()
 *  Comparison Function for computing the mean
 */
int mymeancmp(const void *v1, const void *v2)
{
    intptr_t k1 = (intptr_t)v1;
    intptr_t k2 = (intptr_t)v2;
    
    if (k1 < k2) return -1;
    else if (k1 > k2) return 1;
    else return 0;
}

/** pca_mean_splitter()
 *  
 * Assigns one or more points to each map task
 */
static uint64_t task_start(const map_args_t *task)
{
    uintptr_t encoded = (uintptr_t)task->data;

    assert(encoded > 0);
    return encoded - 1;
}

int pca_splitter(void *data_in, int req_units, map_args_t *out)
{
    pca_data_t *data = data_in;
    uint64_t remaining;

    assert(data != NULL && out != NULL && req_units > 0);
    if (data->next_work >= data->total_work)
        return 0;

    remaining = data->total_work - data->next_work;
    out->length = remaining < (uint64_t)req_units ? remaining : req_units;
    out->data = (void *)(uintptr_t)(data->next_work + 1);
    data->next_work += out->length;
    return 1;
}

int pca_task_lgrp(map_args_t *task)
{
    pca_data_t *data = task->map_data;
    uint64_t start;
    uint64_t group;

    assert(task != NULL && data != NULL && data->total_work > 0);
    start = task_start(task);
    group = start * (uint64_t)data->num_lgrps / data->total_work;
    if (group >= (uint64_t)data->num_lgrps)
        group = data->num_lgrps - 1;
    return (int)group;
}

/** pca_mean_map()
 *  Map task to compute the mean
 */
void pca_mean_map(map_args_t *args)
{
    pca_data_t *data = args->map_data;
    uint64_t start = task_start(args);
    int lgrp = args->lgrp;
    int *matrix;

    assert(data != NULL && lgrp >= 0 && lgrp < data->num_lgrps);
    matrix = data->machine_matrix[lgrp];
    assert(matrix != NULL);

    for (uint64_t offset = 0; offset < (uint64_t)args->length; ++offset) {
        uint64_t row = start + offset;
        int64_t sum = 0;

        assert(row < (uint64_t)data->rows);
        for (int col = 0; col < data->cols; ++col)
            sum += matrix[row * data->cols + col];
        data->mean[row] = sum / data->cols;
    }
}

static uint64_t covariance_row_start(const pca_data_t *data, uint64_t row)
{
    return row * (uint64_t)data->rows - row * (row - 1) / 2;
}

static void covariance_pair(
    const pca_data_t *data, uint64_t index, int *row, int *cov_row)
{
    int low = 0;
    int high = data->rows;

    assert(index < data->covariance_elems);
    while (low + 1 < high) {
        int middle = low + (high - low) / 2;

        if (covariance_row_start(data, middle) <= index)
            low = middle;
        else
            high = middle;
    }
    *row = low;
    *cov_row = low + (int)(index - covariance_row_start(data, low));
    assert(*cov_row >= *row && *cov_row < data->rows);
}

/** pca_cov_map()
 *  Map task for computing the covariance matrix
 * 
 */
void pca_cov_map(map_args_t *args)
{
    pca_data_t *data = args->map_data;
    uint64_t index = task_start(args);
    int lgrp = args->lgrp;
    int *matrix;
    int row;
    int cov_row;

    assert(data != NULL && lgrp >= 0 && lgrp < data->num_lgrps);
    matrix = data->machine_matrix[lgrp];
    assert(matrix != NULL);
    covariance_pair(data, index, &row, &cov_row);

    for (uint64_t offset = 0; offset < (uint64_t)args->length; ++offset) {
        int64_t sum = 0;
        int *first = &matrix[row * data->cols];
        int *second = &matrix[cov_row * data->cols];

        for (int col = 0; col < data->cols; ++col) {
            sum += (first[col] - data->mean[row]) *
                   (second[col] - data->mean[cov_row]);
        }
        data->covariance[index + offset] = sum / (data->cols - 1);

        cov_row++;
        if (cov_row == data->rows) {
            row++;
            cov_row = row;
        }
    }
}

static void verify_small_result(void)
{
    int *matrix = pca_data->machine_matrix[0];

    if (num_rows > 64 || num_cols > 64)
        return;

    for (int row = 0; row < num_rows; ++row) {
        int64_t sum = 0;

        for (int col = 0; col < num_cols; ++col)
            sum += matrix[row * num_cols + col];
        assert(pca_data->mean[row] == sum / num_cols);
    }

    for (uint64_t index = 0; index < pca_data->covariance_elems; ++index) {
        int row;
        int cov_row;
        int64_t sum = 0;

        covariance_pair(pca_data, index, &row, &cov_row);
        for (int col = 0; col < num_cols; ++col) {
            sum += (matrix[row * num_cols + col] - pca_data->mean[row]) *
                   (matrix[cov_row * num_cols + col] - pca_data->mean[cov_row]);
        }
        assert(pca_data->covariance[index] == sum / (num_cols - 1));
    }
    fprintf(stderr, "PCA correctness: sequential small-matrix check passed\n");
}


int main(int argc, char **argv)
{
    final_data_t unused_result;
    map_reduce_args_t map_reduce_args;
    int64_t covariance_sum = 0;
    struct timeval begin, end;
#ifdef TIMING
    unsigned int library_time = 0;
#endif
    // gettimeofday(&begin, NULL);
    get_time (&begin);
    
    parse_args(argc, argv);   

    assert((size_t)num_rows <= SIZE_MAX / (size_t)num_cols / sizeof(int));
    int detected_lgrps = loc_get_num_lgrps();

    assert(detected_lgrps > 0 && detected_lgrps <= MAX_PCA_LGRPS);
    shared_coordination = detected_lgrps > 1;
    pca_data = pca_coord_calloc(1, sizeof(*pca_data));
    pca_data->num_lgrps = detected_lgrps;
    pca_data->rows = num_rows;
    pca_data->cols = num_cols;
    pca_data->grid_size = grid_size;
    pca_data->matrix_bytes = (size_t)num_rows * num_cols * sizeof(int);
    pca_data->covariance_elems = (uint64_t)num_rows * (num_rows + 1) / 2;

    load_machine_inputs();
    pca_data->mean = pca_coord_calloc(num_rows, sizeof(*pca_data->mean));
    pca_data->covariance = pca_coord_calloc(
        pca_data->covariance_elems, sizeof(*pca_data->covariance));
    fprintf(stderr,
            "[PCA placement] context=%p bytes=%zu mean=%p bytes=%zu "
            "covariance=%p bytes=%zu policy=%s\n",
            (void *)pca_data, sizeof(*pca_data),
            (void *)pca_data->mean,
            (size_t)num_rows * sizeof(*pca_data->mean),
            (void *)pca_data->covariance,
            (size_t)pca_data->covariance_elems *
                sizeof(*pca_data->covariance),
            shared_coordination ? "shared" : "private");

    pca_data->unit_size = sizeof(int) * num_cols;
    pca_data->next_work = 0;
    pca_data->total_work = num_rows;
    pca_data->phase = PCA_PHASE_MEAN;

    CHECK_ERROR (map_reduce_init ());
    
    // Setup scheduler args for computing the mean
    memset(&map_reduce_args, 0, sizeof(map_reduce_args_t));
    map_reduce_args.task_data = pca_data;
    map_reduce_args.map_data = pca_data;
    map_reduce_args.map = pca_mean_map;
    map_reduce_args.splitter = pca_splitter;
    map_reduce_args.task_lgrp = pca_task_lgrp;
    map_reduce_args.key_cmp = mymeancmp;
    map_reduce_args.unit_size = pca_data->unit_size;
    map_reduce_args.result = &unused_result;
    map_reduce_args.data_size = num_rows * num_cols * sizeof(int);  
    map_reduce_args.L1_cache_size = atoi(GETENV("MR_L1CACHESIZE"));//1024 * 1024 * 16;
    map_reduce_args.num_map_threads = atoi(GETENV("MR_NUMTHREADS"));//8;
    map_reduce_args.num_reduce_threads = atoi(GETENV("MR_NUMTHREADS"));//16;
    map_reduce_args.num_merge_threads = atoi(GETENV("MR_NUMTHREADS"));//8;
    map_reduce_args.num_procs = atoi(GETENV("MR_NUMPROCS"));//16;
    map_reduce_args.key_match_factor = (float)atof(GETENV("MR_KEYMATCHFACTOR"));//2;
    map_reduce_args.map_phase_name = "pca-mean";
    map_reduce_args.require_map_lgrp_coverage = true;
    map_reduce_args.shared_runtime = shared_coordination;
    map_reduce_args.map_only = true;
    
    fprintf(stderr, "PCA Mean: Calling MapReduce Scheduler\n");

    get_time (&end);

#ifdef TIMING
    fprintf (stderr, "initialize: %u\n", time_diff (&end, &begin));
#endif

    get_time (&begin);    
    CHECK_ERROR(map_reduce(&map_reduce_args) < 0);
    get_time (&end);

#ifdef TIMING
    library_time += time_diff (&end, &begin);
#endif

    get_time (&begin);

    fprintf(stderr, "PCA Mean: MapReduce Completed\n"); 
    
    pca_data->unit_size = sizeof(int) * num_cols * 2;
    pca_data->next_work = 0;
    pca_data->total_work = pca_data->covariance_elems;
    pca_data->phase = PCA_PHASE_COVARIANCE;
    
    // Setup Scheduler args for computing the covariance
    memset(&map_reduce_args, 0, sizeof(map_reduce_args_t));
    map_reduce_args.task_data = pca_data;
    map_reduce_args.map_data = pca_data;
    map_reduce_args.map = pca_cov_map;
    map_reduce_args.splitter = pca_splitter;
    map_reduce_args.task_lgrp = pca_task_lgrp;
    map_reduce_args.key_cmp = mymeancmp;
    map_reduce_args.unit_size = pca_data->unit_size;
    map_reduce_args.result = &unused_result;
    // data size is number of elements that need to be calculated in a cov matrix
    // multiplied by the size of two rows for each element
    map_reduce_args.data_size =
        pca_data->covariance_elems * pca_data->unit_size;
    map_reduce_args.L1_cache_size = atoi(GETENV("MR_L1CACHESIZE"));//1024 * 1024 * 16;
    map_reduce_args.num_map_threads = atoi(GETENV("MR_NUMTHREADS"));//8;
    map_reduce_args.num_reduce_threads = atoi(GETENV("MR_NUMTHREADS"));//16;
    map_reduce_args.num_merge_threads = atoi(GETENV("MR_NUMTHREADS"));//8;
    map_reduce_args.num_procs = atoi(GETENV("MR_NUMPROCS"));//16;
    map_reduce_args.key_match_factor = atoi(GETENV("MR_KEYMATCHFACTOR"));//2;
    map_reduce_args.map_phase_name = "pca-covariance";
    map_reduce_args.require_map_lgrp_coverage = true;
    map_reduce_args.shared_runtime = shared_coordination;
    map_reduce_args.map_only = true;
    
    fprintf(stderr, "PCA Cov: Calling MapReduce Scheduler\n");

    get_time (&end);

#ifdef TIMING
    fprintf (stderr, "inter library: %u\n", time_diff (&end, &begin));
#endif

    get_time (&begin);
    CHECK_ERROR(map_reduce(&map_reduce_args) < 0);
    get_time (&end);

#ifdef TIMING
    library_time += time_diff (&end, &begin);
    fprintf (stderr, "library: %u\n", library_time);
#endif

    get_time (&begin);

    CHECK_ERROR (map_reduce_finalize ());
    
    fprintf(stderr, "PCA Cov: MapReduce Completed\n"); 

    verify_small_result();
    dprintf("\n\nCovariance sum: ");
    for (uint64_t index = 0; index < pca_data->covariance_elems; ++index)
        covariance_sum += pca_data->covariance[index];
    dprintf ("%" PRId64 "\n", covariance_sum);

    pca_coord_free(pca_data->covariance);
    pca_coord_free(pca_data->mean);
    cleanup_machine_inputs();
    pca_coord_free(pca_data);

    get_time (&end);
    // gettimeofday(&end, NULL);
    // fprintf(stderr, "sum %lu\n", (end.tv_sec - begin.tv_sec) * 1000000 + (end.tv_usec - begin.tv_usec));

#ifdef TIMING
    fprintf (stderr, "finalize: %u\n", time_diff (&end, &begin));
#endif

    fprintf(stderr, "done\n");

    return 0;
}
