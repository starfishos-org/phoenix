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

#include "stddefines.h"
#include "map_reduce.h"
#include "processor.h"

typedef struct {
    int *matrix;
    keyval_t *mean;
    int unit_size;    // size of one row
    int next_start_row;
    int next_cov_row;
} pca_data_t;

typedef struct {
    int *matrix;
} pca_map_data_t;

typedef struct {
    int start_row;
    int cov_row;    
} pca_cov_loc_t;

typedef struct {
    int *matrix;
    keyval_t *mean;
    int size;    // number of cov_locs
    pca_cov_loc_t *cov_locs;
} pca_cov_data_t;

#define DEF_GRID_SIZE 100  // all values in the matrix are from 0 to this value 
#define DEF_NUM_ROWS 10
#define DEF_NUM_COLS 10

pca_data_t pca_data;
int num_rows;
int num_cols;
int grid_size;
extern int thread_num;
int memory_malloc_type;

/** parse_args()
 *  Parse the user arguments to determine the number of rows and colums
 */  
void parse_args(int argc, char **argv) 
{
    int c;
    extern char *optarg;
    extern int optind;
    
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
    
    if (num_rows <= 0 || num_cols <= 0 || grid_size <= 0) {
        fprintf(stderr, "Illegal argument value. All values must be numeric and greater than 0\n");
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

/** generate_points()
 *  Create the values in the matrix
 */
void generate_points(int *pts, int rows, int cols) 
{    
    int i, j;
    
    for (i=0; i<rows; i++) 
    {
        for (j=0; j<cols; j++) 
        {
            pts[i * cols + j] = rand() % grid_size;
        }
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
int pca_mean_splitter(void *data_in, int req_units, map_args_t *out)
{
    assert(data_in);
    assert(out);
    
    pca_data_t *pca_data = (pca_data_t *)data_in;    
    assert(pca_data->matrix);
    assert(req_units);
    
    /* Assign a fixed number of rows to each map task */
    if (pca_data->next_start_row >= num_rows) return 0;
    pca_map_data_t *map_data = (pca_map_data_t *)mem_malloc(sizeof(pca_map_data_t));
    /* Allocate last few rows if less than required number of rows */
    if ( (pca_data->next_start_row + req_units) <= num_rows)
    {
        out->length = req_units;
        out->data = (void *)map_data;
        map_data->matrix =&(pca_data->matrix[pca_data->next_start_row * num_cols]);
    }
    else 
    {
        out->length = num_rows - pca_data->next_start_row;
        out->data = (void *)map_data;
        map_data->matrix = &(pca_data->matrix[pca_data->next_start_row * num_cols]);
    }
    //dprintf("Returning %d rows starting at %d\n", out->length, map_data->start_row);
    pca_data->next_start_row += req_units;
    return 1;
}

void *pca_mean_locator(map_args_t *task)
{
    assert (task);

    pca_map_data_t *map_data = (pca_map_data_t *)task->data;

    return map_data->matrix;
}

/** pca_mean_map()
 *  Map task to compute the mean
 */
void pca_mean_map(map_args_t *args)
{
    int sum;
    intptr_t mean;
    int i, j;
    pca_map_data_t *data = (pca_map_data_t *)args->data;
    int *matrix = data->matrix;
    
    /* Compute the mean for the allocated rows to the map task */
    for (i=0; i<args->length; i++) 
    {
        sum = 0;
        for (j=0; j<num_cols; j++) 
        {
            sum += matrix[i * num_cols + j]; 
        }
        mean = sum / num_cols;
        emit_intermediate((void *)&matrix[i * num_cols], (void *)mean, sizeof(int *));
    }
    mem_free(data);
}

/** mycovcmp()
 *  Comparison function for computing the covariance
 */
int mycovcmp(const void *v1, const void *v2)
{
    pca_cov_loc_t *k1 = (pca_cov_loc_t *)v1;
    pca_cov_loc_t *k2 = (pca_cov_loc_t *)v2;
    
    if(k1->start_row < k2->start_row) return -1;
    else if(k1->start_row > k2->start_row) return 1;
    else
    {
        if(k1->cov_row < k2->cov_row) return -1;
        else if(k1->cov_row > k2->cov_row) return 1;
        else return 0;
    }
}

/** pca_cov_splitter()
 *  Splitter function for computing the covariance
 */
int pca_cov_splitter(void *data_in, int req_units, map_args_t *out) 
{
    assert(data_in);
    assert(out);
    
    pca_data_t *pca_data = (pca_data_t *)data_in;    
    assert(pca_data->matrix);
    assert(pca_data->mean);
    assert(req_units);
    
    if ((pca_data->next_start_row >= num_rows) && (pca_data->next_cov_row >= num_rows)) 
        return 0;
    
    pca_cov_loc_t *cov_locs;
    pca_cov_data_t *cov_data;
    
    /* Allocate memory for the structures */

    CHECK_ERROR((cov_locs = (pca_cov_loc_t *)
                                  mem_malloc(sizeof(pca_cov_loc_t) * req_units)) == NULL);
    CHECK_ERROR((cov_data = (pca_cov_data_t *)
                                  mem_malloc(sizeof(pca_cov_data_t))) == NULL);  

    out->length = 1;
    out->data = (void *)cov_data;
    cov_data->size = 0;
    
    /* Compute the boundaries of the region that is to be allocated to the map task*/
    while (pca_data->next_start_row < num_rows && cov_data->size < req_units)
    {
        cov_locs[cov_data->size].start_row = pca_data->next_start_row;
        cov_locs[cov_data->size].cov_row = pca_data->next_cov_row;
        
        if (pca_data->next_cov_row + 1 >= num_rows) 
        {
            pca_data->next_start_row ++;    
            pca_data->next_cov_row = pca_data->next_start_row;
        }
        else
        {
            pca_data->next_cov_row += 1;
        }        
        cov_data->size++;
    }
    
    /* Assign pointers to the matrix with the data */
    cov_data->matrix = pca_data->matrix;
    cov_data->mean = pca_data->mean;
    cov_data->cov_locs = cov_locs;
    
#if 0
    dprintf("Returning %d elems starting <%d,%d> till <%d,%d>\n", 
                cov_data->size, cov_locs[0].start_row, cov_locs[0].cov_row,
                cov_locs[cov_data->size-1].start_row, 
                cov_locs[cov_data->size-1].cov_row);
#endif
    
    return 1;
}

void *pca_cov_locator (map_args_t *task)
{
    assert (task);

    pca_cov_data_t *cov_data = (pca_cov_data_t *)task->data;

    int cov_idx = cov_data->cov_locs[0].cov_row;

    return &cov_data->matrix[cov_idx * num_cols];
}

/** pca_cov_map()
 *  Map task for computing the covariance matrix
 * 
 */
void pca_cov_map(map_args_t *args)
{
    assert(args);
    assert(args->length == 1);
    int i, j;
    int *start_row, *cov_row;
    int start_idx, cov_idx;
    keyval_t *mean;
    int sum;
    intptr_t covariance;
    intptr_t m1, m2;
    
    pca_cov_data_t *cov_data = (pca_cov_data_t *)args->data;
    mean = cov_data->mean;
    pca_cov_loc_t *cov_loc;
    
    /* compute the covariance for the allocated region */
    for (i=0; i<cov_data->size; i++) 
    {
        start_idx = cov_data->cov_locs[i].start_row;
        cov_idx = cov_data->cov_locs[i].cov_row;
        assert(cov_idx >= start_idx);
        start_row = &cov_data->matrix[start_idx * num_cols];
        cov_row = &cov_data->matrix[cov_idx * num_cols];
        sum = 0;
        //dprintf("Mean for row %d is %d\n", start_idx, *((int *)(mean[start_idx].val)));
        //dprintf("Mean for row %d is %d\n", cov_idx, *((int *)(mean[cov_idx].val)));
        m1 = (intptr_t)mean[start_idx].val;
        m2 = (intptr_t)mean[cov_idx].val;
        /* XXX: Shouldn't this be num_cols? */
        for (j=0; j<num_rows; j++)
        {
            sum += (start_row[j] - m1) * (cov_row[j] - m2);
        }
        
        covariance = sum / (num_rows-1);
        
        //dprintf("Covariance for <%d, %d> is %d\n", start_idx, cov_idx, *covariance);

        CHECK_ERROR((cov_loc = (pca_cov_loc_t *)mem_malloc(sizeof(pca_cov_loc_t))) == NULL);
        cov_loc->start_row = cov_data->cov_locs[i].start_row;
        cov_loc->cov_row = cov_data->cov_locs[i].cov_row;
        emit_intermediate((void *)cov_loc, (void *)covariance, sizeof(pca_cov_loc_t));
    }

    mem_free(cov_data->cov_locs);
    mem_free(cov_data);
}


int main(int argc, char **argv)
{
    final_data_t pca_mean_vals;
    final_data_t pca_cov_vals;
    map_reduce_args_t map_reduce_args;
    int i;
    struct timeval begin, end;
#ifdef TIMING
    unsigned int library_time = 0;
#endif
    // gettimeofday(&begin, NULL);
    get_time (&begin);
    
    parse_args(argc, argv);   
    
    // Allocate space for the matrix.  Both MapReduce phases read it from
    // every worker on every machine, so it is shared state and belongs in
    // CXL; plain mem_malloc would follow DSM_USER_MALLOC_MODE and strand it
    // in machine 0's local DRAM under the K-mix/U-mix placement.
    // The display loop below decrements num_rows, so remember the size here.
    size_t matrix_size = sizeof(int) * (size_t)num_rows * (size_t)num_cols;
    pca_data.matrix = (int *)mem_malloc_shared(matrix_size);
    CHECK_ERROR (pca_data.matrix == NULL);
    
    //Generate random values for all the points in the matrix 
    generate_points(pca_data.matrix, num_rows, num_cols);
    
    // Print the points
    //dump_points(pca_data.matrix, num_rows, num_cols);
    
    /* Create the structure to store the mean value */
    pca_data.unit_size = sizeof(int) * num_cols;    // size of one row
    pca_data.next_start_row = pca_data.next_cov_row = 0;
    pca_data.mean = NULL;

    CHECK_ERROR (map_reduce_init ());
    
    // Setup scheduler args for computing the mean
    memset(&map_reduce_args, 0, sizeof(map_reduce_args_t));
    map_reduce_args.task_data = &pca_data;
    map_reduce_args.map = pca_mean_map;
    map_reduce_args.reduce = NULL; // use identity reduce
    map_reduce_args.splitter = pca_mean_splitter;
    map_reduce_args.locator = pca_mean_locator;
    map_reduce_args.key_cmp = mymeancmp;
    map_reduce_args.unit_size = pca_data.unit_size;
    map_reduce_args.partition = NULL; // use default
    map_reduce_args.result = &pca_mean_vals;
    map_reduce_args.data_size = num_rows * num_cols * sizeof(int);  
    map_reduce_args.L1_cache_size = atoi(GETENV("MR_L1CACHESIZE"));//1024 * 1024 * 16;
    map_reduce_args.num_map_threads = atoi(GETENV("MR_NUMTHREADS"));//8;
    map_reduce_args.num_reduce_threads = atoi(GETENV("MR_NUMTHREADS"));//16;
    map_reduce_args.num_merge_threads = atoi(GETENV("MR_NUMTHREADS"));//8;
    map_reduce_args.num_procs = atoi(GETENV("MR_NUMPROCS"));//16;
    map_reduce_args.key_match_factor = (float)atof(GETENV("MR_KEYMATCHFACTOR"));//2;
    
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
    
    assert (pca_mean_vals.length == num_rows);
    //dprintf("Mean vector:\n");

    pca_data.unit_size = sizeof(int) * num_cols * 2;    // size of two rows
    pca_data.next_start_row = pca_data.next_cov_row = 0;
    pca_data.mean = pca_mean_vals.data; // array of keys and values - the keys have been freed tho
    
    // Setup Scheduler args for computing the covariance
    memset(&map_reduce_args, 0, sizeof(map_reduce_args_t));
    map_reduce_args.task_data = &pca_data;
    map_reduce_args.map = pca_cov_map;
    map_reduce_args.reduce = NULL; // use identity reduce
    map_reduce_args.splitter = pca_cov_splitter;
    map_reduce_args.locator = pca_cov_locator;
    map_reduce_args.key_cmp = mycovcmp;
    map_reduce_args.unit_size = pca_data.unit_size;
    map_reduce_args.partition = NULL; // use default
    map_reduce_args.result = &pca_cov_vals;
    // data size is number of elements that need to be calculated in a cov matrix
    // multiplied by the size of two rows for each element
    map_reduce_args.data_size = ((((num_rows * num_rows) - num_rows)/2) + num_rows) * pca_data.unit_size;  
    map_reduce_args.L1_cache_size = atoi(GETENV("MR_L1CACHESIZE"));//1024 * 1024 * 16;
    map_reduce_args.num_map_threads = atoi(GETENV("MR_NUMTHREADS"));//8;
    map_reduce_args.num_reduce_threads = atoi(GETENV("MR_NUMTHREADS"));//16;
    map_reduce_args.num_merge_threads = atoi(GETENV("MR_NUMTHREADS"));//8;
    map_reduce_args.num_procs = atoi(GETENV("MR_NUMPROCS"));//16;
    map_reduce_args.key_match_factor = atoi(GETENV("MR_KEYMATCHFACTOR"));//2;
    map_reduce_args.use_one_queue_per_task = true;
    
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

    // assert(pca_cov_vals.length == ((((num_rows * num_rows) - num_rows)/2) + num_rows));
    
    // Free the allocated structures
    int cnt = 0;
    intptr_t sum = 0;
    dprintf("\n\nCovariance sum: ");
    for (i = 0; i <pca_cov_vals.length; i++) 
    {
        sum += (intptr_t)(pca_cov_vals.data[i].val);
        //dprintf("%5d ", );
        cnt++;
        if (cnt == num_rows)
        {
            //dprintf("\n"); 
            num_rows--;
            cnt = 0;
        }
        mem_free(pca_cov_vals.data[i].key);
    }
    dprintf ("%" PRIdPTR "\n", sum);
    
    mem_free (pca_cov_vals.data);
    mem_free (pca_mean_vals.data);
    mem_free_shared (pca_data.matrix, matrix_size);

    get_time (&end);
    // gettimeofday(&end, NULL);
    // fprintf(stderr, "sum %lu\n", (end.tv_sec - begin.tv_sec) * 1000000 + (end.tv_usec - begin.tv_usec));

#ifdef TIMING
    fprintf (stderr, "finalize: %u\n", time_diff (&end, &begin));
#endif

    fprintf(stderr, "done\n");

    return 0;
}
