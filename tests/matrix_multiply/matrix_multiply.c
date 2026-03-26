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
#include <time.h>
#include <inttypes.h>
#include <sys/time.h>
#include <pthread.h>
#include <sched.h>

#ifdef _CHCORE_
#include <chcore/syscall.h>
#endif

#include "map_reduce.h"
#include "stddefines.h"
#include "processor.h"

typedef struct {
    int row_num;
    int *matrix_A;
    int *matrix_B;
    int matrix_len;
    int unit_size;
    int row_block_len;
    int* output;
} mm_data_t;

/* Structure to store the coordinates
and location for each value in the matrix */
typedef struct {
	int x_loc;
	int y_loc;
	int value;
} mm_key_t;

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

extern int thread_num;

#ifdef _CHCORE_
/* ---- Per-machine first-touch (like GeminiGraph's run_on_loader_threads) ---- */

/* Detect machine boundaries from thread_bind_cpu_list: a gap in consecutive
 * CPU IDs marks a new machine.  Returns number of machines found; fills
 * machine_first_cpu[] with the first CPU of each machine. */
static int  num_machines = 0;
static int  machine_first_cpu[64];

/* Per-machine input buffers, populated by per_machine_load() */
static int *g_machine_matrix_A[64];
static int *g_machine_matrix_B[64];

/* Return which machine index the calling thread is on */
static int get_current_machine(void) {
    int cpu = proc_get_cpuid();
    for (int m = num_machines - 1; m >= 0; m--) {
        if (cpu >= machine_first_cpu[m])
            return m;
    }
    return 0;
}

static void detect_machines(void) {
    if (!thread_bind_cpu_set || thread_num <= 0) {
        num_machines = 1;
        machine_first_cpu[0] = 0;
        return;
    }
    num_machines = 1;
    machine_first_cpu[0] = thread_bind_cpu_list[0];
    for (int i = 1; i < thread_num; i++) {
        if (thread_bind_cpu_list[i] != thread_bind_cpu_list[i-1] + 1) {
            machine_first_cpu[num_machines++] = thread_bind_cpu_list[i];
        }
    }
}

typedef struct {
    int   machine_id;
    char *buf_A;
    char *buf_B;
    int   mlen;        /* matrix side length */
    int   file_size;
} loader_arg_t;

/* Each machine's thread: bind to local CPU, malloc + fill matrix data.
 * Page faults on local CPU → physical pages land in local DRAM. */
static void *loader_thread_func(void *arg) {
    loader_arg_t *la = (loader_arg_t *)arg;
    proc_bind_thread(machine_first_cpu[la->machine_id]);

    la->buf_A = (char *)malloc(la->file_size);
    la->buf_B = (char *)malloc(la->file_size);
    assert(la->buf_A && la->buf_B);

    int *a = (int *)la->buf_A;
    int *b = (int *)la->buf_B;
    int n = la->mlen * la->mlen;
    srand(0);
    for (int i = 0; i < n; i++) a[i] = rand() % 11;
    srand(0);
    for (int i = 0; i < n; i++) b[i] = rand() % 11;
    return NULL;
}

/* Each machine launches one loader thread bound to its local CPU. */
static void per_machine_load(int mlen, int fsize,
                             char **out_A, char **out_B) {
    detect_machines();
    pthread_t tids[64];
    loader_arg_t args[64];
    for (int m = 0; m < num_machines; m++) {
        args[m].machine_id = m;
        args[m].buf_A = NULL;
        args[m].buf_B = NULL;
        args[m].mlen = mlen;
        args[m].file_size = fsize;
        pthread_create(&tids[m], NULL, loader_thread_func, &args[m]);
    }
    for (int m = 0; m < num_machines; m++) {
        pthread_join(tids[m], NULL);
        out_A[m] = args[m].buf_A;
        out_B[m] = args[m].buf_B;
    }
}
#endif /* _CHCORE_ */

int count = 0;
char * fname_A, *fname_B;
int create_files = 0;
int matrix_len = 0;
int row_block_len = 0;
int file_size = 0;
size_t output_size = 0;
extern int thread_num;
extern int thread_bind_cpu_list[1024];
#ifdef _CHCORE_
int memory_malloc_type = MALLOC_TYPE_PRIVATE;
#endif

void parse_args(int argc, char **argv) 
{
    int c;
    extern char *optarg;
    extern int optind;
    thread_num = 1;

    fname_A = "matrix_file_A.txt";
    fname_B = "matrix_file_B.txt";

    while ((c = getopt(argc, argv, "l:r:t:c:i:")) != EOF)
    {
        switch (c) {
            case 'l':
                matrix_len = atoi(optarg);
                break;
            case 'r':
                row_block_len = atoi(optarg);
                break;
            case 't':
                thread_num = atoi(optarg);
                break;
            case 'c':
                create_files = atoi(optarg);
                break;
            #ifdef _CHCORE_
            case 'i':
                strcpy(thread_bind_cpu_filename, optarg);
                break;
            #endif
            case '?':
                fprintf(stderr, "Usage: %s -l <matrix side> -r <row block> -t <threads> -c <create> -i <cpu file>\n", argv[0]);
                exit(1);
        }
    }
    
    if (matrix_len <= 0 || row_block_len <= 0 || thread_num <= 0) {
        fprintf(stderr, "Illegal argument value. All values must be numeric and greater than 0\n");
        exit(1);
    }

    file_size = ((matrix_len*matrix_len))*sizeof(int);

    fprintf(stderr, "***** file size is %d\n", file_size);
    fprintf(stderr, "MatrixMult: Side of the matrix is %d\n", matrix_len);
    fprintf(stderr, "MatrixMult: Row Block Len is %d\n", row_block_len);
    fprintf(stderr, "Number of threads=%d\n", thread_num);
    #ifdef _CHCORE_
    if (strlen(thread_bind_cpu_filename) == 0) {
        fprintf(stderr, "Thread bind cpu filename is not set default to matrix_multiply_bind_cpu.txt\n");
        strcpy(thread_bind_cpu_filename, "matrix_multiply_bind_cpu.txt");
    }
    fprintf(stderr, "Thread bind cpu filename=%s\n", thread_bind_cpu_filename);
    if (parse_cpu_bind_file(thread_bind_cpu_filename) < 0) {
        fprintf(stderr, "Failed to parse cpu bind file\n");
    } else {
        thread_bind_cpu_set = true;
    }
    #endif
}

/** myintcmp()
 *  Comparison Function to compare 2 locations in the matrix
 */
int myintcmp(const void *v1, const void *v2)
{
    mm_key_t* key1 = (mm_key_t*)v1;
    mm_key_t* key2 = (mm_key_t*)v2;

    if(key1->x_loc < key2->x_loc) return -1;
    else if(key1->x_loc > key2->x_loc) return 1;
    else
    {
        if(key1->y_loc < key2->y_loc) return -1;
        else if(key1->y_loc > key2->y_loc) return 1;
        else return 0;
    }
}

/** matrixmul_splitter()
 *  Assign a set of rows of the output matrix to each map task
 */
int matrixmult_splitter(void *data_in, int req_units, map_args_t *out)
{
    /* Make a copy of the mm_data structure */
    mm_data_t * data = (mm_data_t *)data_in;  
    mm_data_t * data_out = (mm_data_t *)mem_malloc(sizeof(mm_data_t));
    memcpy((char*)data_out,(char*)data,sizeof(mm_data_t));

    /* Check whether the various terms exist */
    assert(data_in);
    assert(out);
    assert(req_units >= 0);
    
    assert(data->matrix_len >= 0);
    assert(data->unit_size >= 0);
    assert(data->row_block_len >= 0);

    assert(data->matrix_A);
    assert(data->matrix_B);

    assert(data->row_num <= data->matrix_len);
    
    /* dprintf("Required units is %d\n",req_units); */

    /* Reached the end of the matrix */
    if(data->row_num >= data->matrix_len)
    {
        fflush(stdout);
        mem_free(data_out);
        return 0;
    }

    /* Compute available rows */
    int available_rows = data->matrix_len - data->row_num;
    out->length = (req_units < available_rows)? req_units:available_rows;
    out->data = data_out;

    data->row_num += out->length;
    /* dprintf("Allocated rows is %d\n",out->length); */

    return 1;
}

/** matrixmult_locator()
 *  Returns the memory address where this map task would be heavily accessing.
 */
void *matrixmult_locator(map_args_t *task)
{
    assert (task);

    mm_data_t *data = (mm_data_t *)task->data;

    return data->matrix_A + data->row_num * data->matrix_len;
}

/** matrixmul_map()
 * Multiplies the allocated regions of matrix to compute partial sums 
 */
void matrixmult_map(map_args_t *args)
{
    int row_count = 0;
    int i,j, x_loc, value;
    // int y_loc;
    int * a_ptr,* b_ptr;

    assert(args);

    mm_data_t* data = (mm_data_t*)(args->data);
    assert(data);

#ifdef _CHCORE_
    /* Use this machine's local DRAM buffer instead of machine 0's */
    int cur_m = get_current_machine();
    int *local_matrix_A = g_machine_matrix_A[cur_m] ? g_machine_matrix_A[cur_m] : data->matrix_A;
    int *local_matrix_B = g_machine_matrix_B[cur_m] ? g_machine_matrix_B[cur_m] : data->matrix_B;
#else
    int *local_matrix_A = data->matrix_A;
    int *local_matrix_B = data->matrix_B;
#endif

    while(row_count < args->length)
    {
        a_ptr = local_matrix_A + (data->row_num + row_count)*data->matrix_len;

        for(i=0; i < data->matrix_len ; i++)
        {
            b_ptr = local_matrix_B + i;
            value = 0;

            for(j=0;j<data->matrix_len ; j++)
            {
                    value += ( a_ptr[j] * (*b_ptr));
                    b_ptr+= data->matrix_len;
            }
            x_loc = (data->row_num + row_count);
            // y_loc = i;
            data->output[x_loc*data->matrix_len + i] = value;
            /* fflush(stdout); */
        }
        /* dprintf("%d Loop\n",data->row_num); */
	    
        row_count++;	
    }

    /* dprintf("Finished Map task %d\n",data->row_num); */

    /* fflush(stdout); */
    mem_free(args->data);
}

int main(int argc, char *argv[]) {

    final_data_t mm_vals;
    int i,j;
    int fd_A, fd_B;
    char * fdata_A, *fdata_B;
    struct stat finfo_A, finfo_B;
    int *matrix_A_ptr, *matrix_B_ptr;

    struct timeval begin, end;

    get_time (&begin);

    parse_args(argc, argv);

    /* If the matrix files do not exist, create them */
    if(create_files)
    {
        dprintf("Creating files\n");

        int value = 0;
        CHECK_ERROR((fd_A = open(fname_A,O_CREAT | O_RDWR,S_IRWXU)) < 0);
        CHECK_ERROR((fd_B = open(fname_B,O_CREAT | O_RDWR,S_IRWXU)) < 0);
        
        for(i=0;i<matrix_len;i++)
        {
            for(j=0;j<matrix_len;j++)
            {
                value = (rand())%11;
                assert(write(fd_A,&value,sizeof(int)) != -1);
                //dprintf("%d  ",value);
            }
            //dprintf("\n");
        }
        //dprintf("\n");

        for(i=0;i<matrix_len;i++)
        {
            for(j=0;j<matrix_len;j++)
            {
                value = (rand())%11;
                assert(write(fd_B,&value,sizeof(int)) != -1);
                //dprintf("%d  ",value);
            }
            //dprintf("\n");
        }

        CHECK_ERROR(close(fd_A) < 0);
        CHECK_ERROR(close(fd_B) < 0);
    }

#ifdef _CHCORE_
    {
        detect_machines();
        proc_bind_thread(machine_first_cpu[0]);
        char *per_machine_A[64], *per_machine_B[64];
        per_machine_load(matrix_len, file_size, per_machine_A, per_machine_B);
        for (int m = 0; m < num_machines; m++) {
            g_machine_matrix_A[m] = (int *)per_machine_A[m];
            g_machine_matrix_B[m] = (int *)per_machine_B[m];
        }
        fdata_A = per_machine_A[0];
        fdata_B = per_machine_B[0];
    }
#else
    // Read in the file
    CHECK_ERROR((fd_A = open(fname_A,O_RDONLY)) < 0);
    CHECK_ERROR(fstat(fd_A, &finfo_A) < 0);
  #ifndef NO_MMAP
    CHECK_ERROR((fdata_A= mmap(0, file_size + 1,
        PROT_READ | PROT_WRITE, MAP_PRIVATE, fd_A, 0)) == NULL);
  #else
    int ret;
    fdata_A = (char *)mem_malloc(file_size);
    CHECK_ERROR (fdata_A == NULL);
    ret = read (fd_A, fdata_A, file_size);
    CHECK_ERROR (ret != file_size);
  #endif

    // Read in the file
    CHECK_ERROR((fd_B = open(fname_B,O_RDONLY)) < 0);
    CHECK_ERROR(fstat(fd_B, &finfo_B) < 0);
  #ifndef NO_MMAP
    CHECK_ERROR((fdata_B= mmap(0, file_size + 1,
        PROT_READ, MAP_PRIVATE, fd_B, 0)) == NULL);
  #else
    fdata_B = (char *)mem_malloc(file_size);
    CHECK_ERROR (fdata_B == NULL);
    ret = read (fd_B, fdata_B, file_size);
    CHECK_ERROR (ret != file_size);
  #endif
#endif

    // Setup splitter args
    mm_data_t mm_data;
    mm_data.unit_size = row_block_len*matrix_len*sizeof(int); 
    mm_data.matrix_len = matrix_len;
    mm_data.row_block_len = row_block_len;
    mm_data.matrix_A = NULL;
    mm_data.matrix_B = NULL;
    mm_data.row_num = 0;

    output_size = (size_t)matrix_len * matrix_len * sizeof(int);
#ifdef _CHCORE_
    // Allocate output in CXL (shared) memory, like GeminiGraph's alloc_vertex_array_cxl
    mm_data.output = (int*)mmap(NULL, output_size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FLAG_SHARED,
        -1, 0);
    assert(mm_data.output != MAP_FAILED);
#else
    mm_data.output = (int*)mem_malloc(output_size);
#endif

    mm_data.matrix_A = matrix_A_ptr = ((int *)fdata_A);
    mm_data.matrix_B = matrix_B_ptr = ((int *)fdata_B);

/* Debug VA / vmspace prints disabled. */

    CHECK_ERROR (map_reduce_init ());

    // Setup map reduce args
    map_reduce_args_t map_reduce_args;
    /* RMY: Was this memset intentionally absent? */
    memset(&map_reduce_args, 0, sizeof(map_reduce_args_t));
    map_reduce_args.task_data = &mm_data;
    map_reduce_args.map = matrixmult_map;
    map_reduce_args.reduce = NULL;
    map_reduce_args.splitter = matrixmult_splitter;
#ifdef _CHCORE_
    map_reduce_args.locator = NULL;
#else
    map_reduce_args.locator = matrixmult_locator;
#endif
    map_reduce_args.key_cmp = myintcmp;
    map_reduce_args.unit_size = mm_data.unit_size;
    map_reduce_args.partition = NULL; // use default
    map_reduce_args.result = &mm_vals;
    map_reduce_args.data_size = file_size;
    map_reduce_args.L1_cache_size = atoi(GETENV("MR_L1CACHESIZE"));//1024 * 8;
    map_reduce_args.num_map_threads = atoi(GETENV("MR_NUMTHREADS"));//8;
    map_reduce_args.num_reduce_threads = atoi(GETENV("MR_NUMTHREADS"));//16;
    map_reduce_args.num_merge_threads = atoi(GETENV("MR_NUMTHREADS"));//8;
    map_reduce_args.num_procs = atoi(GETENV("MR_NUMPROCS"));//16;
    map_reduce_args.key_match_factor = (float)atof(GETENV("MR_KEYMATCHFACTOR"));//2;

    get_time (&end);

#ifdef TIMING
    fprintf (stderr, "initialize: %u\n", time_diff (&end, &begin));
#endif

    get_time (&begin);
    CHECK_ERROR (map_reduce (&map_reduce_args) < 0);
    get_time (&end);

#ifdef TIMING
    fprintf (stderr, "library: %u\n", time_diff (&end, &begin));
#endif

    get_time (&begin);

    CHECK_ERROR (map_reduce_finalize ());

    //dprintf("\n");
    //dprintf("The length of the final output is %d\n",mm_vals.length );
    int sum = 0;
    for(i=0;i<matrix_len*matrix_len;i++)
    {
          sum += mm_data.output[i];
    }
    dprintf ("MatrixMult: total sum is %d\n", sum);
    //dprintf("\n");

    dprintf("MatrixMult: MapReduce Completed\n");

/* Debug vmspace print disabled before cleanup. */

    mem_free(mm_vals.data);
#ifdef _CHCORE_
    munmap(mm_data.output, output_size);
    for (int m = 0; m < num_machines; m++) {
        free((void *)g_machine_matrix_A[m]);
        free((void *)g_machine_matrix_B[m]);
    }
#else
    mem_free(mm_data.output);
  #ifndef NO_MMAP
    CHECK_ERROR(munmap(fdata_A, file_size + 1) < 0);
  #else
    mem_free(fdata_A);
  #endif
    CHECK_ERROR(close(fd_A) < 0);
  #ifndef NO_MMAP
    CHECK_ERROR(munmap(fdata_B, file_size + 1) < 0);
  #else
    mem_free(fdata_B);
  #endif
    CHECK_ERROR(close(fd_B) < 0);
#endif

    get_time (&end);

#ifdef TIMING
    fprintf (stderr, "finalize: %u\n", time_diff (&end, &begin));
#endif

    fprintf(stdout, "matrix multiply finished\n");

    fprintf(stderr, "done\n");

    return 0;
}
