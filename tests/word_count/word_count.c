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
#include <sys/time.h>
#include <fcntl.h>
#include <ctype.h>
#include <inttypes.h>

#include "map_reduce.h"
#include "stddefines.h"
#include "sort.h"

#define DEFAULT_DISP_NUM 10

char *fname;
int disp_num;
extern int thread_num;

void parse_args(int argc, char *argv[]) {
    int c;
    extern char *optarg;
    extern int optind;

    thread_num = 1;
    disp_num = DEFAULT_DISP_NUM;
    while ((c = getopt(argc, argv, "f:t:n:")) != EOF) {
        switch (c) {
            case 'f':
                fname = malloc(strlen(optarg) + 1);
                strcpy(fname, optarg);
                break;
            case 't':
                thread_num = atoi(optarg);
                break;
            case 'n':
                disp_num = atoi(optarg);
                break;
            case '?':
                printf("Usage: %s -f <filename> -t <thread_num> -n <disp_num>\n", argv[0]);
                exit(1);
        }
    }
    if (fname == NULL) {
        printf("filename is required\n");
        exit(1);
    }
    if (disp_num <= 0) {
        printf("disp_num is required\n");
        exit(1);
    }
    if (thread_num <= 0) {
        printf("thread_num is required\n");
        exit(1);
    }
    int fd = open(fname, O_RDONLY);
    if (fd < 0) {
        printf("failed to open file\n");
        exit(1);
    }
    close(fd);
}

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

typedef struct {
    int fpos;
    off_t flen;
    char *fdata;
    int unit_size;
} wc_data_t;

enum {
    IN_WORD,
    NOT_IN_WORD
};

    struct timeval begin, end;
#ifdef TIMING
    unsigned int library_time = 0;
#endif

/** mystrcmp()
 *  Comparison function to compare 2 words
 */
int mystrcmp(const void *s1, const void *s2)
{
    return strcmp((const char *)s1, (const char *) s2);
}

/** mykeyvalcmp()
 *  Comparison function to compare 2 ints
 */
int mykeyvalcmp(const void *v1, const void *v2)
{
    keyval_t* kv1 = (keyval_t*)v1;
    keyval_t* kv2 = (keyval_t*)v2;

    intptr_t *i1 = kv1->val;
    intptr_t *i2 = kv2->val;

    if (i1 < i2) return 1;
    else if (i1 > i2) return -1;
    else {
    /*** don't just return 0 immediately coz the mapreduce scheduler provides 
    1 key with multiple values to the reduce task. since the different words that 
    we require are each part of a different keyval pair, returning 0 makes
    the mapreduce scheduler think that it can just keep one key and disregard
    the rest. That's not desirable in this case. Returning 0 when the values are
    equal produces results where the same word is repeated for all the instances
    which share the same frequency. Instead, we check the word as well, and only 
    return 0 if both the value and the word match ****/
        return strcmp((char *)kv1->key, (char *)kv2->key);
        //return 0;
    }
}

/** wordcount_splitter()
 *  Memory map the file and divide file on a word border i.e. a space.
 */
int wordcount_splitter(void *data_in, int req_units, map_args_t *out)
{
    wc_data_t * data = (wc_data_t *)data_in; 
    
    assert(data_in);
    assert(out);
    
    assert(data->flen >= 0);
    assert(data->fdata);
    assert(req_units);
    assert(data->fpos >= 0);

    // End of file reached, return FALSE for no more data
    if (data->fpos >= data->flen) return 0;

    // Set the start of the next data
    out->data = (void *)&data->fdata[data->fpos];
    
    // Determine the nominal length
    out->length = req_units * data->unit_size;
    
    if (data->fpos + out->length > data->flen)
        out->length = data->flen - data->fpos;
    
    // Set the length to end at a space
    for (data->fpos += (long)out->length;
          data->fpos < data->flen && 
          data->fdata[data->fpos] != ' ' && data->fdata[data->fpos] != '\t' &&
          data->fdata[data->fpos] != '\r' && data->fdata[data->fpos] != '\n';
          data->fpos++, out->length++);
  
    return 1;
}

/** wordcount_locator()
 *  Return the memory address where this map task would heavily access.
 */
void *wordcount_locator (map_args_t *task)
{
    assert (task);

    return task->data;
}

/** wordcount_map()
 * Go through the allocated portion of the file and count the words
 */
void wordcount_map(map_args_t *args) 
{
    char *curr_start, curr_ltr;
    int state = NOT_IN_WORD;
    int i;
  
    assert(args);

    char *data = (char *)args->data;

    assert(data);
    curr_start = data;
    
    for (i = 0; i < args->length; i++)
    {
        curr_ltr = toupper(data[i]);
        switch (state)
        {
        case IN_WORD:
            data[i] = curr_ltr;
            if ((curr_ltr < 'A' || curr_ltr > 'Z') && curr_ltr != '\'')
            {
                data[i] = 0;
                emit_intermediate(curr_start, (void *)1, &data[i] - curr_start + 1);
                state = NOT_IN_WORD;
            }
            break;

        default:
        case NOT_IN_WORD:
            if (curr_ltr >= 'A' && curr_ltr <= 'Z')
            {
                curr_start = &data[i];
                data[i] = curr_ltr;
                state = IN_WORD;
            }
            break;
        }
    }

    // Add the last word
    if (state == IN_WORD)
    {
        data[args->length] = 0;
        emit_intermediate(curr_start, (void *)1, &data[i] - curr_start + 1);
    }
}

/** wordcount_reduce()
 * Add up the partial sums for each word
 */
void wordcount_reduce(void *key_in, iterator_t *itr)
{
    char *key = (char *)key_in;
    void *val;
    intptr_t sum = 0;

    assert(key);
    assert(itr);

    while (iter_next (itr, &val))
    {
        sum += (intptr_t)val;
    }

    emit(key, (void *)sum);
}

void *wordcount_combiner (iterator_t *itr)
{
    void *val;
    intptr_t sum = 0;

    assert(itr);

    while (iter_next (itr, &val))
    {
        sum += (intptr_t)val;
    }

    return (void *)sum;
}

int main(int argc, char *argv[]) 
{
    final_data_t wc_vals;
    int i;
    int fd;
    char * fdata;
    struct stat finfo;

    get_time (&begin);

    parse_args(argc, argv);

    printf("Wordcount: Running...\n");

    // Read in the file
    CHECK_ERROR((fd = open(fname, O_RDONLY)) < 0);
    // Get the file info (for file length)
    CHECK_ERROR(fstat(fd, &finfo) < 0);
#ifndef NO_MMAP
    // Memory map the file
    CHECK_ERROR((fdata = mmap(0, finfo.st_size + 1, 
      PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0)) == NULL);
#else
    int ret;

    fdata = (char *)malloc (finfo.st_size);
    CHECK_ERROR (fdata == NULL);

    ret = read (fd, fdata, finfo.st_size);
    CHECK_ERROR (ret != finfo.st_size);
#endif

    access_pages(fdata, finfo.st_size);

    // Setup splitter args
    wc_data_t wc_data;
    wc_data.unit_size = 5; // approx 5 bytes per word
    wc_data.fpos = 0;
    wc_data.flen = finfo.st_size;
    wc_data.fdata = fdata;

    CHECK_ERROR (map_reduce_init ());

    // Setup map reduce args
    map_reduce_args_t map_reduce_args;
    memset(&map_reduce_args, 0, sizeof(map_reduce_args_t));
    map_reduce_args.task_data = &wc_data;
    map_reduce_args.map = wordcount_map;
    map_reduce_args.reduce = wordcount_reduce;
    map_reduce_args.combiner = wordcount_combiner;
    map_reduce_args.splitter = wordcount_splitter;
    map_reduce_args.locator = wordcount_locator;
    map_reduce_args.key_cmp = mystrcmp;
    map_reduce_args.unit_size = wc_data.unit_size;
    map_reduce_args.partition = NULL; // use default
    map_reduce_args.result = &wc_vals;
    map_reduce_args.data_size = finfo.st_size;
    map_reduce_args.L1_cache_size = atoi(GETENV("MR_L1CACHESIZE"));//1024 * 1024 * 2;
    map_reduce_args.num_map_threads = atoi(GETENV("MR_NUMTHREADS"));//8;
    map_reduce_args.num_reduce_threads = atoi(GETENV("MR_NUMTHREADS"));//16;
    map_reduce_args.num_merge_threads = atoi(GETENV("MR_NUMTHREADS"));//8;
    map_reduce_args.num_procs = atoi(GETENV("MR_NUMPROCS"));//16;
    map_reduce_args.key_match_factor = (float)atof(GETENV("MR_KEYMATCHFACTOR"));//2;

    printf("Wordcount: Calling MapReduce Scheduler Wordcount\n");

    get_time (&end);

#ifdef TIMING
    fprintf (stderr, "initialize: %u\n", time_diff (&end, &begin));
#endif

    get_time (&begin);
    CHECK_ERROR(map_reduce (&map_reduce_args) < 0);
    get_time (&end);

#ifdef TIMING
    library_time += time_diff (&end, &begin);
#endif

    get_time (&begin);

    printf("Wordcount: MapReduce Completed\n");

    printf("Wordcount: Calling MapReduce Scheduler Sort\n");

    mapreduce_sort(wc_vals.data, wc_vals.length, sizeof(keyval_t), mykeyvalcmp);

    CHECK_ERROR (map_reduce_finalize ());

    printf("Wordcount: MapReduce Completed\n");

    dprintf("\nWordcount: Results (TOP %d):\n", disp_num);
    for (i = 0; i < disp_num && i < wc_vals.length; i++)
    {
      keyval_t * curr = &((keyval_t *)wc_vals.data)[i];
      dprintf("%15s - %" PRIdPTR "\n", (char *)curr->key, (intptr_t)curr->val);
    }

    free(wc_vals.data);

#ifndef NO_MMAP
    CHECK_ERROR(munmap(fdata, finfo.st_size + 1) < 0);
#else
    free (fdata);
#endif
    CHECK_ERROR(close(fd) < 0);

    get_time (&end);

#ifdef TIMING
    fprintf (stderr, "finalize: %u\n", time_diff (&end, &begin));
#endif

    fprintf(stderr, "done\n");

    return 0;
}
