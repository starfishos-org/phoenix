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
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef _CHCORE_
#include <chcore/syscall.h>
#endif

#include "map_reduce.h"
#include "stddefines.h"
#include "sort.h"
#include "processor.h"

#define DEFAULT_DISP_NUM 10
#define WC_MAX_MACHINES          64
#define WC_TASK_BYTES            (512UL * 1024)
#define WC_WORKER_HASH_CAPACITY  (128UL * 1024)
#define WC_MACHINE_HASH_CAPACITY (256UL * 1024)
#define WC_REGION_HEADER_SIZE    64UL
#define WC_THREAD_PENDING        1
#define WC_EXCHANGE_PENDING      2

char *fname;
int disp_num;
extern int thread_num;

void parse_args(int argc, char *argv[]) {
    int c;
    extern char *optarg;
    extern int optind;

    thread_num = 1;
    disp_num = DEFAULT_DISP_NUM;
    while ((c = getopt(argc, argv, "f:t:n:i:")) != EOF) {
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
            #ifdef _CHCORE_
            case 'i':
                strcpy(thread_bind_cpu_filename, optarg);
                break;
            #endif
            case '?':
                printf("Usage: %s -f <filename> -t <thread_num> -n <disp_num> -i <thread bind cpu filename>\n", argv[0]);
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
    #ifdef _CHCORE_
    if (strlen(thread_bind_cpu_filename) == 0) {
        fprintf(stderr, "Thread bind cpu filename is not set default to word_count_bind_cpu.txt\n");
        strcpy(thread_bind_cpu_filename, "word_count_bind_cpu.txt");
    }
    fprintf(stderr, "Thread bind cpu filename=%s\n", thread_bind_cpu_filename);
    if (parse_cpu_bind_file(thread_bind_cpu_filename) < 0) {
        fprintf(stderr, "Failed to parse cpu bind file\n");
    } else {
        thread_bind_cpu_set = true;
    }
    #endif
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
#pragma GCC optimize("O3")

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

typedef struct {
    size_t map_length;
} wc_region_header_t;

typedef struct {
    char *key;
    uint64_t hash;
    uint64_t count;
} wc_hash_entry_t;

typedef struct {
    wc_hash_entry_t *entries;
    size_t capacity;
    size_t size;
} wc_hash_t;

typedef struct {
    size_t offset;
    size_t length;
} wc_task_t;

typedef struct {
    char *input;
    wc_hash_t *worker_tables;
    wc_hash_t combined;
    size_t input_length;
    size_t task_count;
    _Atomic size_t next_task;
    _Atomic size_t executed_tasks;
    wc_task_t tasks[];
} wc_machine_local_t;

typedef struct {
    uint64_t hash;
    uint64_t count;
    uint32_t key_length;
    uint32_t key_offset;
} wc_exchange_entry_t;

typedef struct {
    size_t entry_count;
    size_t allocation_size;
    size_t strings_offset;
    wc_exchange_entry_t entries[];
} wc_exchange_t;

typedef struct {
    int locality_group;
    int machine_id;
    int first_cpu;
    int worker_begin;
    int worker_count;
    off_t file_start;
    off_t file_end;
    char *staging;
    wc_machine_local_t *local;
    char *input_region;
    wc_hash_t *worker_table_region;
    wc_hash_entry_t **worker_entry_regions;
    wc_hash_entry_t *combined_entry_region;
    size_t local_region_size;
    size_t input_length;
    size_t task_count;
    size_t executed_tasks;
    _Atomic int stage_state;
    _Atomic int load_state;
} wc_machine_desc_t;

typedef struct {
    int worker_id;
    int local_worker_id;
    int cpu_id;
    bool loader;
    wc_machine_desc_t *machine;
    _Atomic int *start_workers;
    wc_hash_t output;
    _Atomic int status;
    bool persistent;
} wc_worker_arg_t;

typedef struct {
    wc_machine_desc_t *machine;
    wc_worker_arg_t *workers;
    wc_exchange_t **exchange_slot;
    bool persistent;
    _Atomic int status;
    _Atomic int cleanup_request;
    _Atomic int cleanup_done;
    _Atomic int exchange_ready;
    size_t exchange_size;
} wc_combiner_arg_t;

static bool is_word_boundary(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

static size_t round_up_size(size_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

/*
 * Allocate one independently releasable mapping. Default mappings preserve the
 * Figure 13 user-placement choice: U-share maps them in CXL, while U-mix and
 * Private map them in DRAM. Only cross-machine coordination and compressed
 * exchange mappings pass shared=true and explicitly request CXL on ChCore.
 */
static void *wc_region_alloc(size_t size, bool shared)
{
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    size_t map_length;
    void *base;
    wc_region_header_t *header;

    if (size == 0)
        size = 1;
    if (size > SIZE_MAX - WC_REGION_HEADER_SIZE)
        return NULL;

    map_length = round_up_size(size + WC_REGION_HEADER_SIZE, 4096);
#ifdef _CHCORE_
    if (shared)
        flags |= MAP_FLAG_SHARED;
#else
    (void)shared;
#endif
    base = mmap(NULL, map_length, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (base == MAP_FAILED)
        return NULL;

    header = (wc_region_header_t *)base;
    header->map_length = map_length;
    return (unsigned char *)base + WC_REGION_HEADER_SIZE;
}

static void *wc_region_calloc(size_t count, size_t size, bool shared)
{
    size_t total;
    void *region;

    if (size != 0 && count > SIZE_MAX / size)
        return NULL;
    total = count * size;
    region = wc_region_alloc(total, shared);
    if (region != NULL)
        memset(region, 0, total);
    return region;
}

static int wc_region_free(void *region)
{
    wc_region_header_t *header;

    if (region == NULL)
        return 0;
    header = (wc_region_header_t *)((unsigned char *)region
                                    - WC_REGION_HEADER_SIZE);
    return munmap(header, header->map_length);
}

static int wc_region_free_sized(void *region, size_t size)
{
    size_t map_length;

    if (region == NULL)
        return 0;
    map_length = round_up_size(size + WC_REGION_HEADER_SIZE, 4096);
    return munmap((unsigned char *)region - WC_REGION_HEADER_SIZE, map_length);
}

static uint64_t wc_hash_bytes(const char *key, size_t length)
{
    uint64_t hash = UINT64_C(1469598103934665603);

    for (size_t i = 0; i < length; ++i) {
        hash ^= (unsigned char)key[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int wc_hash_add_existing(wc_hash_t *table, char *key, uint64_t hash,
                                uint64_t count)
{
    size_t index;

    index = hash & (table->capacity - 1);
    for (size_t probe = 0; probe < table->capacity; ++probe) {
        wc_hash_entry_t *entry = &table->entries[index];
        if (entry->key == NULL) {
            entry->key = key;
            entry->hash = hash;
            entry->count = count;
            table->size++;
            return 0;
        }
        if (entry->hash == hash && strcmp(entry->key, key) == 0) {
            entry->count += count;
            return 0;
        }
        index = (index + 1) & (table->capacity - 1);
    }
    return -1;
}

static size_t wc_find_file_boundary(int fd, off_t file_length, off_t nominal)
{
    unsigned char buffer[4096];
    off_t position = nominal;

    while (position < file_length) {
        size_t wanted = (size_t)(file_length - position);
        ssize_t bytes;

        if (wanted > sizeof(buffer))
            wanted = sizeof(buffer);
        bytes = pread(fd, buffer, wanted, position);
        if (bytes <= 0)
            return (size_t)file_length;
        for (ssize_t i = 0; i < bytes; ++i) {
            if (is_word_boundary((char)buffer[i]))
                return (size_t)(position + i);
        }
        position += bytes;
    }
    return (size_t)file_length;
}

static int wc_detect_machines(wc_machine_desc_t *machines, int max_machines)
{
    int machine_count = 1;

    memset(machines, 0, sizeof(*machines) * max_machines);
    machines[0].locality_group = 0;
    machines[0].machine_id = 0;
    machines[0].first_cpu = thread_bind_cpu_set ? thread_bind_cpu_list[0] : 0;
    machines[0].worker_begin = 0;
    machines[0].worker_count = 1;

    if (!thread_bind_cpu_set || thread_num <= 1) {
        machines[0].worker_count = thread_num;
        return 1;
    }

#ifdef _CHCORE_
    int cpus_per_machine = (int)usys_get_machine_cpu_count();
    if (cpus_per_machine <= 0)
        return -1;
#endif

    for (int worker = 1; worker < thread_num; ++worker) {
        bool new_machine;
#ifdef _CHCORE_
        new_machine = thread_bind_cpu_list[worker] / cpus_per_machine
                      != thread_bind_cpu_list[worker - 1] / cpus_per_machine;
#else
        new_machine = false;
#endif
        if (new_machine) {
            if (machine_count >= max_machines)
                return -1;
            machines[machine_count].locality_group = machine_count;
#ifdef _CHCORE_
            machines[machine_count].machine_id =
                    thread_bind_cpu_list[worker] / cpus_per_machine;
#else
            machines[machine_count].machine_id = 0;
#endif
            machines[machine_count].first_cpu = thread_bind_cpu_list[worker];
            machines[machine_count].worker_begin = worker;
            machines[machine_count].worker_count = 1;
            machine_count++;
        } else {
            machines[machine_count - 1].worker_count++;
        }
    }
    return machine_count;
}

static int wc_read_full_at(int fd, char *buffer, size_t length, off_t offset)
{
    size_t completed = 0;

    while (completed < length) {
        ssize_t bytes = pread(fd,
                              buffer + completed,
                              length - completed,
                              offset + (off_t)completed);
        if (bytes < 0 && errno == EINTR)
            continue;
        if (bytes <= 0)
            return -1;
        completed += (size_t)bytes;
    }
    return 0;
}

/*
 * Create every PMO on machine 0. The remote loader performs the first data
 * touch, so U-mix pages settle in that machine's DRAM, but kernel PMO metadata
 * remains owned by machine 0 and can be torn down there safely.
 */
static int wc_prepare_machine(wc_machine_desc_t *machine, bool shared_coord)
{
    size_t input_length = (size_t)(machine->file_end - machine->file_start);
    size_t max_tasks = input_length / WC_TASK_BYTES + 2;
    size_t local_size = sizeof(wc_machine_local_t)
                        + max_tasks * sizeof(wc_task_t);

    machine->local = (wc_machine_local_t *)wc_region_alloc(local_size, false);
    machine->input_region =
            (char *)wc_region_alloc(input_length + 1, false);
    machine->worker_table_region = (wc_hash_t *)wc_region_alloc(
            (size_t)machine->worker_count * sizeof(wc_hash_t), false);
    machine->worker_entry_regions = (wc_hash_entry_t **)wc_region_calloc(
            machine->worker_count,
            sizeof(*machine->worker_entry_regions),
            shared_coord);
    machine->combined_entry_region = (wc_hash_entry_t *)wc_region_alloc(
            WC_MACHINE_HASH_CAPACITY * sizeof(wc_hash_entry_t), false);
    machine->local_region_size = local_size;
    machine->input_length = input_length;
    if (machine->local == NULL || machine->input_region == NULL
        || machine->worker_table_region == NULL
        || machine->worker_entry_regions == NULL
        || machine->combined_entry_region == NULL)
        return -1;

    for (int worker = 0; worker < machine->worker_count; ++worker) {
        machine->worker_entry_regions[worker] =
                (wc_hash_entry_t *)wc_region_alloc(
                        WC_WORKER_HASH_CAPACITY * sizeof(wc_hash_entry_t),
                        false);
        if (machine->worker_entry_regions[worker] == NULL)
            return -1;
    }
    return 0;
}

static int wc_load_machine(wc_machine_desc_t *machine)
{
    size_t input_length = (size_t)(machine->file_end - machine->file_start);
    size_t max_tasks = input_length / WC_TASK_BYTES + 2;
    wc_machine_local_t *local = machine->local;
    size_t position = 0;

    if (local == NULL || machine->input_region == NULL
        || machine->worker_table_region == NULL)
        return -1;
    memset(local, 0, machine->local_region_size);
    local->input = machine->input_region;
    local->worker_tables = machine->worker_table_region;
    memset(local->worker_tables,
           0,
           (size_t)machine->worker_count * sizeof(*local->worker_tables));
    for (int worker = 0; worker < machine->worker_count; ++worker) {
        wc_hash_t *table = &local->worker_tables[worker];

        table->entries = machine->worker_entry_regions[worker];
        table->capacity = WC_WORKER_HASH_CAPACITY;
        memset(table->entries,
               0,
               WC_WORKER_HASH_CAPACITY * sizeof(*table->entries));
    }
    local->combined.entries = machine->combined_entry_region;
    local->combined.capacity = WC_MACHINE_HASH_CAPACITY;
    memset(local->combined.entries,
           0,
           WC_MACHINE_HASH_CAPACITY * sizeof(*local->combined.entries));
    local->input_length = input_length;

    memcpy(local->input, machine->staging, input_length);
    local->input[input_length] = '\0';

    while (position < input_length) {
        size_t task_start = position;
        size_t task_end = task_start + WC_TASK_BYTES;

        if (task_end > input_length)
            task_end = input_length;
        while (task_end < input_length
               && !is_word_boundary(local->input[task_end]))
            task_end++;
        if (task_end < input_length)
            task_end++;
        if (task_end == task_start) {
            return -1;
        }

        assert(local->task_count < max_tasks);
        local->tasks[local->task_count].offset = task_start;
        local->tasks[local->task_count].length = task_end - task_start;
        local->task_count++;
        position = task_end;
    }

    machine->input_length = local->input_length;
    machine->task_count = local->task_count;
    return 0;
}

static int wc_map_task(wc_hash_t *table, char *data, size_t length)
{
    char *word_start = NULL;
    bool in_word = false;

    for (size_t i = 0; i < length; ++i) {
        unsigned char byte = (unsigned char)data[i];
        char upper = (char)toupper(byte);

        if (in_word) {
            data[i] = upper;
            if ((upper < 'A' || upper > 'Z') && upper != '\'') {
                size_t word_length;
                uint64_t hash;

                data[i] = '\0';
                word_length = (size_t)(&data[i] - word_start);
                hash = wc_hash_bytes(word_start, word_length);
                if (wc_hash_add_existing(table, word_start, hash, 1) != 0)
                    return -1;
                in_word = false;
            }
        } else if (upper >= 'A' && upper <= 'Z') {
            word_start = &data[i];
            data[i] = upper;
            in_word = true;
        }
    }

    if (in_word) {
        size_t word_length = (size_t)(&data[length] - word_start);
        uint64_t hash;

        data[length] = '\0';
        hash = wc_hash_bytes(word_start, word_length);
        if (wc_hash_add_existing(table, word_start, hash, 1) != 0)
            return -1;
    }
    return 0;
}

static int wc_exchange_size(const wc_hash_t *table, size_t *size_out)
{
    size_t strings_size = 0;
    size_t strings_offset;

    for (size_t i = 0; i < table->capacity; ++i) {
        if (table->entries[i].key != NULL)
            strings_size += strlen(table->entries[i].key) + 1;
    }

    strings_offset =
            sizeof(wc_exchange_t)
            + table->size * sizeof(wc_exchange_entry_t);
    if (strings_offset > SIZE_MAX - strings_size)
        return -1;
    *size_out = strings_offset + strings_size;
    return 0;
}

static int wc_publish_hash_into(const wc_hash_t *table,
                                wc_exchange_t *exchange,
                                size_t allocation_size)
{
    size_t strings_offset =
            sizeof(*exchange) + table->size * sizeof(exchange->entries[0]);
    char *strings;
    size_t output_index = 0;

    exchange->entry_count = table->size;
    exchange->allocation_size = allocation_size;
    exchange->strings_offset = strings_offset;
    strings = (char *)exchange + strings_offset;

    for (size_t i = 0; i < table->capacity; ++i) {
        const wc_hash_entry_t *source = &table->entries[i];
        wc_exchange_entry_t *target;
        size_t key_length;
        size_t key_offset;

        if (source->key == NULL)
            continue;
        key_length = strlen(source->key) + 1;
        key_offset = (size_t)(strings - ((char *)exchange + strings_offset));
        if (key_length > UINT32_MAX || key_offset > UINT32_MAX) {
            return -1;
        }

        target = &exchange->entries[output_index++];
        target->hash = source->hash;
        target->count = source->count;
        target->key_length = (uint32_t)key_length;
        target->key_offset = (uint32_t)key_offset;
        memcpy(strings, source->key, key_length);
        strings += key_length;
    }
    assert(output_index == table->size);
    return (size_t)(strings - (char *)exchange) == allocation_size ? 0 : -1;
}

/*
 * ChCore's cross-machine pthread teardown may touch the creator's private
 * thread metadata after the benchmark has finished. Phoenix's normal worker
 * pool avoids that path by keeping remote workers alive until process exit.
 * Do the same for Word Count after all shared state has been released.
 */
static void wc_park_remote_thread(void)
{
    for (;;)
        sched_yield();
}

static void *wc_worker(void *opaque)
{
    wc_worker_arg_t *arg = (wc_worker_arg_t *)opaque;
    wc_machine_local_t *local;
    wc_hash_t *table;
    int load_state;

    int status = -1;

    if (thread_bind_cpu_set && proc_bind_thread(arg->cpu_id) != 0)
        goto out;
    if (arg->loader) {
        int stage_state;

        do {
            stage_state = atomic_load_explicit(&arg->machine->stage_state,
                                               memory_order_acquire);
            if (stage_state == 0)
                sched_yield();
        } while (stage_state == 0);
        load_state =
                stage_state < 0 || wc_load_machine(arg->machine) != 0 ? -1 : 1;
        atomic_store_explicit(
                &arg->machine->load_state, load_state, memory_order_release);
    } else {
        do {
            load_state = atomic_load_explicit(&arg->machine->load_state,
                                              memory_order_acquire);
            if (load_state == 0)
                sched_yield();
        } while (load_state == 0);
    }
    if (load_state < 0)
        goto out;

    while (atomic_load_explicit(arg->start_workers, memory_order_acquire) == 0)
        sched_yield();
    if (atomic_load_explicit(arg->start_workers, memory_order_relaxed) < 0)
        goto out;

    local = arg->machine->local;
    table = &local->worker_tables[arg->local_worker_id];

    for (;;) {
        size_t task_index = atomic_fetch_add_explicit(
                &local->next_task, 1, memory_order_relaxed);
        wc_task_t *task;

        if (task_index >= local->task_count)
            break;
        task = &local->tasks[task_index];
        if (wc_map_task(table, local->input + task->offset, task->length)
            != 0)
            goto out;
        atomic_fetch_add_explicit(
                &local->executed_tasks, 1, memory_order_relaxed);
    }
    arg->output = *table;
    memset(table, 0, sizeof(*table));

    status = 0;
out:
    atomic_store_explicit(&arg->status, status, memory_order_release);
    if (arg->persistent)
        wc_park_remote_thread();
    return NULL;
}

static void *wc_combiner(void *opaque)
{
    wc_combiner_arg_t *arg = (wc_combiner_arg_t *)opaque;
    wc_machine_desc_t *machine = arg->machine;
    wc_hash_t *combined = &machine->local->combined;

    int status = -1;

    if (proc_bind_thread(machine->first_cpu) != 0)
        goto publish_done;
    if (combined->entries == NULL
        || combined->capacity != WC_MACHINE_HASH_CAPACITY)
        goto publish_done;

    machine->executed_tasks = atomic_load_explicit(
            &machine->local->executed_tasks, memory_order_relaxed);

    for (int local_worker = 0; local_worker < machine->worker_count;
         ++local_worker) {
        wc_worker_arg_t *worker =
                &arg->workers[machine->worker_begin + local_worker];

        for (size_t i = 0; i < worker->output.capacity; ++i) {
            wc_hash_entry_t *entry = &worker->output.entries[i];

            if (entry->key != NULL
                && wc_hash_add_existing(
                           combined, entry->key, entry->hash, entry->count)
                           != 0)
                goto out;
        }
    }

    if (wc_exchange_size(combined, &arg->exchange_size) != 0)
        goto out;
    atomic_store_explicit(
            &arg->status, WC_EXCHANGE_PENDING, memory_order_release);
    while (atomic_load_explicit(&arg->exchange_ready, memory_order_acquire)
           == 0)
        sched_yield();
    if (atomic_load_explicit(&arg->exchange_ready, memory_order_relaxed) < 0
        || *arg->exchange_slot == NULL)
        goto out;
    if (wc_publish_hash_into(
                combined, *arg->exchange_slot, arg->exchange_size)
        != 0)
        goto out;
    status = 0;

out:
publish_done:
    atomic_store_explicit(&arg->status, status, memory_order_release);
    if (arg->persistent) {
        while (atomic_load_explicit(&arg->cleanup_request,
                                    memory_order_acquire)
               == 0)
            sched_yield();
        atomic_store_explicit(
                &arg->cleanup_done, status == 0 ? 1 : -1, memory_order_release);
        wc_park_remote_thread();
    }
    return NULL;
}

static int wc_merge_exchanges(wc_exchange_t **exchanges, int exchange_count,
                              wc_hash_t *global, uint64_t *total_words)
{
    *total_words = 0;
    for (int exchange_index = 0; exchange_index < exchange_count;
         ++exchange_index) {
        wc_exchange_t *exchange = exchanges[exchange_index];
        const char *string_base;

        if (exchange == NULL)
            return -1;
        string_base = (const char *)exchange + exchange->strings_offset;
        for (size_t i = 0; i < exchange->entry_count; ++i) {
            wc_exchange_entry_t *source = &exchange->entries[i];
            const char *source_key = string_base + source->key_offset;
            *total_words += source->count;
            if (wc_hash_add_existing(global,
                                     (char *)source_key,
                                     source->hash,
                                     source->count)
                != 0)
                return -1;
        }
    }
    return 0;
}

static int wc_make_results(const wc_hash_t *global, keyval_t **result_out)
{
    keyval_t *results =
            (keyval_t *)wc_region_alloc(global->size * sizeof(*results), false);
    size_t output_index = 0;

    if (results == NULL)
        return -1;
    for (size_t i = 0; i < global->capacity; ++i) {
        if (global->entries[i].key == NULL)
            continue;
        results[output_index].key = global->entries[i].key;
        results[output_index].val = (void *)(intptr_t)global->entries[i].count;
        output_index++;
    }
    assert(output_index == global->size);
    qsort(results, global->size, sizeof(*results), mykeyvalcmp);
    *result_out = results;
    return 0;
}

static int wc_release_machine(wc_machine_desc_t *machine)
{
    int status = 0;

    if (machine->worker_entry_regions != NULL) {
        for (int worker = 0; worker < machine->worker_count; ++worker) {
            if (wc_region_free_sized(
                        machine->worker_entry_regions[worker],
                        WC_WORKER_HASH_CAPACITY * sizeof(wc_hash_entry_t))
                != 0)
                status = -1;
        }
    }
    if (wc_region_free_sized(machine->combined_entry_region,
                             WC_MACHINE_HASH_CAPACITY
                                     * sizeof(wc_hash_entry_t))
        != 0)
        status = -1;
    if (wc_region_free_sized(machine->worker_table_region,
                             (size_t)machine->worker_count
                                     * sizeof(wc_hash_t))
        != 0)
        status = -1;
    if (wc_region_free_sized(
                machine->input_region, machine->input_length + 1)
        != 0)
        status = -1;
    if (wc_region_free_sized(machine->local, machine->local_region_size) != 0)
        status = -1;
    if (wc_region_free(machine->worker_entry_regions) != 0)
        status = -1;
    machine->local = NULL;
    machine->input_region = NULL;
    machine->worker_table_region = NULL;
    machine->worker_entry_regions = NULL;
    machine->combined_entry_region = NULL;
    return status;
}

int main(int argc, char *argv[])
{
    struct stat file_info;
    wc_machine_desc_t machine_template[WC_MAX_MACHINES];
    wc_machine_desc_t *machines = NULL;
    wc_worker_arg_t *worker_args = NULL;
    wc_combiner_arg_t *combiner_args = NULL;
    wc_exchange_t **exchanges = NULL;
    pthread_t *worker_threads = NULL;
    pthread_t *combiner_threads = NULL;
    _Atomic int *start_workers = NULL;
    wc_hash_t *global = NULL;
    keyval_t *results = NULL;
    uint64_t total_words = 0;
    int fd = -1;
    int machine_count = 0;
    int exchange_count = 0;
    size_t coordination_bytes = 0;
    size_t max_staging_bytes = 0;
    bool shared_coord;
    int status = 1;

    (void)access_pages;
    get_time(&begin);
    parse_args(argc, argv);
    fprintf(stdout, "Wordcount: Running...\n");

    fd = open(fname, O_RDONLY);
    if (fd < 0 || fstat(fd, &file_info) != 0 || file_info.st_size <= 0) {
        fprintf(stderr, "Wordcount: failed to open a non-empty input file\n");
        goto cleanup;
    }

    machine_count = wc_detect_machines(machine_template, WC_MAX_MACHINES);
    if (machine_count <= 0 || machine_count > thread_num) {
        fprintf(stderr, "Wordcount: invalid machine topology\n");
        goto cleanup;
    }
    shared_coord = machine_count > 1;
    exchange_count = machine_count;
    coordination_bytes = machine_count * sizeof(*machines)
                         + thread_num * sizeof(*worker_args)
                         + machine_count * sizeof(*combiner_args)
                         + exchange_count * sizeof(*exchanges)
                         + thread_num * sizeof(wc_hash_entry_t *)
                         + sizeof(*start_workers);

    machines = (wc_machine_desc_t *)wc_region_calloc(
            machine_count, sizeof(*machines), shared_coord);
    worker_args = (wc_worker_arg_t *)wc_region_calloc(
            thread_num, sizeof(*worker_args), shared_coord);
    exchanges = (wc_exchange_t **)wc_region_calloc(
            exchange_count, sizeof(*exchanges), shared_coord);
    combiner_args = (wc_combiner_arg_t *)wc_region_calloc(
            machine_count, sizeof(*combiner_args), shared_coord);
    start_workers = (_Atomic int *)wc_region_calloc(
            1, sizeof(*start_workers), shared_coord);
    worker_threads = (pthread_t *)calloc(thread_num, sizeof(*worker_threads));
    combiner_threads =
            (pthread_t *)calloc(machine_count, sizeof(*combiner_threads));
    if (machines == NULL || worker_args == NULL || exchanges == NULL
        || combiner_args == NULL || start_workers == NULL
        || worker_threads == NULL || combiner_threads == NULL) {
        fprintf(stderr, "Wordcount: allocation failure during setup\n");
        goto cleanup;
    }
    memcpy(machines, machine_template, machine_count * sizeof(*machines));

    machines[0].file_start = 0;
    for (int machine = 1; machine < machine_count; ++machine) {
        off_t nominal = (file_info.st_size * machine) / machine_count;
        machines[machine].file_start =
                (off_t)wc_find_file_boundary(fd, file_info.st_size, nominal);
        machines[machine - 1].file_end = machines[machine].file_start;
    }
    machines[machine_count - 1].file_end = file_info.st_size;

    for (int machine = 0; machine < machine_count; ++machine) {
        if (wc_prepare_machine(&machines[machine], shared_coord) != 0) {
            fprintf(stderr,
                    "Wordcount: failed to prepare locality group %d\n",
                    machine);
            goto cleanup;
        }
    }

    for (int machine = 0; machine < machine_count; ++machine) {
        for (int local_worker = 0;
             local_worker < machines[machine].worker_count;
             ++local_worker) {
            int worker = machines[machine].worker_begin + local_worker;

            worker_args[worker].worker_id = worker;
            worker_args[worker].local_worker_id = local_worker;
            worker_args[worker].cpu_id =
                    thread_bind_cpu_set ? thread_bind_cpu_list[worker] : worker;
            worker_args[worker].loader = local_worker == 0;
            worker_args[worker].persistent =
                    shared_coord && machine != 0;
            worker_args[worker].machine = &machines[machine];
            worker_args[worker].start_workers = start_workers;
            atomic_init(&worker_args[worker].status, WC_THREAD_PENDING);
            if (pthread_create(&worker_threads[worker],
                               NULL,
                               wc_worker,
                               &worker_args[worker])
                != 0) {
                worker_args[worker].persistent = false;
                atomic_store_explicit(start_workers, -1, memory_order_release);
                fprintf(stderr,
                        "Wordcount: failed to create worker %d\n",
                        worker);
                goto cleanup;
            }
        }
    }

    /*
     * ChCore file descriptors are machine-local. Machine 0 therefore reads
     * one shard at a time into a temporary transfer mapping; a remote loader
     * copies that shard into its own default mapping, then the transfer is
     * released before the next shard. The full input is never fixed in CXL.
     */
    for (int machine = 0; machine < machine_count; ++machine) {
        size_t input_length = (size_t)(machines[machine].file_end
                                       - machines[machine].file_start);
        bool shared_staging = shared_coord && machine != 0;
        char *staging = (char *)wc_region_alloc(input_length, shared_staging);
        int load_state;

        if (input_length > max_staging_bytes)
            max_staging_bytes = input_length;
        if (staging == NULL
            || wc_read_full_at(
                       fd, staging, input_length, machines[machine].file_start)
                       != 0) {
            wc_region_free(staging);
            for (int pending = machine; pending < machine_count; ++pending)
                atomic_store_explicit(&machines[pending].stage_state,
                                      -1,
                                      memory_order_release);
            atomic_store_explicit(start_workers, -1, memory_order_release);
            fprintf(stderr, "Wordcount: failed to stage shard %d\n", machine);
            goto cleanup;
        }
        machines[machine].staging = staging;
        atomic_store_explicit(
                &machines[machine].stage_state, 1, memory_order_release);
        do {
            load_state = atomic_load_explicit(&machines[machine].load_state,
                                              memory_order_acquire);
            if (load_state == 0)
                sched_yield();
        } while (load_state == 0);
        machines[machine].staging = NULL;
        wc_region_free(staging);
        if (load_state < 0) {
            atomic_store_explicit(start_workers, -1, memory_order_release);
            fprintf(stderr, "Wordcount: loader %d failed\n", machine);
            goto cleanup;
        }
        fprintf(stderr,
                "[WordCount shard] locality_group=%d machine=%d bytes=%zu "
                "tasks=%zu\n",
                machines[machine].locality_group,
                machines[machine].machine_id,
                machines[machine].input_length,
                machines[machine].task_count);
    }
    if (close(fd) != 0) {
        fprintf(stderr, "Wordcount: failed to close the staging input\n");
        goto cleanup;
    }
    fd = -1;

    fprintf(stderr,
            "[WordCount placement] machines=%d workers=%d input=per-machine-"
            "default-first-touch intermediate=per-worker-and-machine-default-"
            "first-touch pmo_creator=machine0 pmo_releaser=machine0 "
            "coordination=%s coordination_payload_bytes=%zu "
            "remote_threads=%s "
            "staging=%s max_staging_bytes=%zu exchange=%s\n",
            machine_count,
            thread_num,
            shared_coord ? "explicit-cxl" : "local",
            coordination_bytes,
            shared_coord ? "persistent-until-process-exit" : "joined",
            shared_coord ? "sequential-remote-explicit-cxl" : "local",
            max_staging_bytes,
            shared_coord ? "machine0-default-remote-compressed-explicit-cxl" :
                           "local");

    get_time(&end);
#ifdef TIMING
    fprintf(stderr, "initialize: %u\n", time_diff(&end, &begin));
#endif

    get_time(&begin);
    fprintf(stdout, "Wordcount: Calling locality-aware Word Count scheduler\n");
    atomic_store_explicit(start_workers, 1, memory_order_release);
    for (int worker = 0; worker < thread_num; ++worker) {
        int worker_status;

        if (worker_args[worker].persistent) {
            do {
                worker_status = atomic_load_explicit(
                        &worker_args[worker].status, memory_order_acquire);
                if (worker_status == WC_THREAD_PENDING)
                    sched_yield();
            } while (worker_status == WC_THREAD_PENDING);
        } else if (pthread_join(worker_threads[worker], NULL) != 0) {
            fprintf(stderr, "Wordcount: failed to join worker %d\n", worker);
            goto cleanup;
        } else {
            worker_status = atomic_load_explicit(
                    &worker_args[worker].status, memory_order_acquire);
        }
        if (worker_status != 0) {
            fprintf(stderr, "Wordcount: worker %d failed\n", worker);
            goto cleanup;
        }
        worker_threads[worker] = 0;
    }

    for (int machine = 0; machine < machine_count; ++machine) {
        combiner_args[machine].machine = &machines[machine];
        combiner_args[machine].workers = worker_args;
        combiner_args[machine].exchange_slot = &exchanges[machine];
        combiner_args[machine].persistent = machine != 0;
        atomic_init(&combiner_args[machine].status, WC_THREAD_PENDING);
        if (pthread_create(&combiner_threads[machine],
                           NULL,
                           wc_combiner,
                           &combiner_args[machine])
            != 0) {
            combiner_args[machine].persistent = false;
            fprintf(stderr,
                    "Wordcount: failed to create combiner %d\n",
                    machine);
            goto cleanup;
        }
    }
    for (int machine = 0; machine < machine_count; ++machine) {
        int combiner_status;

        do {
            combiner_status = atomic_load_explicit(
                    &combiner_args[machine].status, memory_order_acquire);
            if (combiner_status == WC_THREAD_PENDING)
                sched_yield();
        } while (combiner_status == WC_THREAD_PENDING);
        if (combiner_status == WC_EXCHANGE_PENDING) {
            exchanges[machine] = (wc_exchange_t *)wc_region_alloc(
                    combiner_args[machine].exchange_size, machine != 0);
            atomic_store_explicit(
                    &combiner_args[machine].exchange_ready,
                    exchanges[machine] == NULL ? -1 : 1,
                    memory_order_release);
            do {
                combiner_status = atomic_load_explicit(
                        &combiner_args[machine].status,
                        memory_order_acquire);
                if (combiner_status == WC_EXCHANGE_PENDING)
                    sched_yield();
            } while (combiner_status == WC_EXCHANGE_PENDING);
        }
        if (!combiner_args[machine].persistent
            && pthread_join(combiner_threads[machine], NULL) != 0) {
            fprintf(stderr,
                    "Wordcount: failed to join combiner %d\n",
                    machine);
            goto cleanup;
        }
        if (combiner_status != 0) {
            fprintf(stderr, "Wordcount: combiner %d failed\n", machine);
            goto cleanup;
        }
        combiner_threads[machine] = 0;
    }

    {
        size_t exchange_entries = 0;
        size_t exchange_bytes = 0;
        size_t explicit_shared_bytes = 0;

        for (int exchange_index = 0; exchange_index < exchange_count;
             ++exchange_index) {
            exchange_entries += exchanges[exchange_index]->entry_count;
            exchange_bytes += exchanges[exchange_index]->allocation_size;
            if (shared_coord && exchange_index != 0)
                explicit_shared_bytes +=
                        exchanges[exchange_index]->allocation_size;
        }
        fprintf(stderr,
                "[WordCount exchange] entries=%zu bytes=%zu "
                "explicit_shared_bytes=%zu input_bytes=%jd placement=%s\n",
                exchange_entries,
                exchange_bytes,
                explicit_shared_bytes,
                (intmax_t)file_info.st_size,
                shared_coord ? "machine0-default-remote-explicit-cxl" :
                               "local");
    }

    for (int machine = 0; machine < machine_count; ++machine) {
        size_t queued = machines[machine].task_count;
        size_t executed;

        executed = machines[machine].executed_tasks;

        fprintf(stderr,
                "[WordCount tasks] locality_group=%d machine=%d queued=%zu "
                "executed=%zu workers=%d\n",
                machines[machine].locality_group,
                machines[machine].machine_id,
                queued,
                executed,
                machines[machine].worker_count);
        if (queued == 0 || executed != queued) {
            fprintf(stderr,
                    "Wordcount: locality group %d did not execute its assigned "
                    "tasks\n",
                    machines[machine].locality_group);
            goto cleanup;
        }
    }

    if (proc_bind_thread(machines[0].first_cpu) != 0) {
        fprintf(stderr, "Wordcount: failed to bind the reducer to machine 0\n");
        goto cleanup;
    }
    global = (wc_hash_t *)wc_region_calloc(1, sizeof(*global), false);
    if (global != NULL) {
        global->entries = (wc_hash_entry_t *)wc_region_calloc(
                WC_MACHINE_HASH_CAPACITY, sizeof(*global->entries), false);
        global->capacity = WC_MACHINE_HASH_CAPACITY;
    }
    if (global == NULL || global->entries == NULL) {
        fprintf(stderr, "Wordcount: global reducer allocation failed\n");
        goto cleanup;
    }
    if (wc_merge_exchanges(exchanges, exchange_count, global, &total_words) != 0
        || wc_make_results(global, &results) != 0) {
        fprintf(stderr, "Wordcount: reduce or sort failed\n");
        goto cleanup;
    }

    get_time(&end);
#ifdef TIMING
    library_time = time_diff(&end, &begin);
    fprintf(stderr, "library: %u\n", library_time);
#endif

#ifdef _CHCORE_
    if (getenv("WC_PRINT_VMSPACE_STATS") != NULL)
        usys_print_vmspace_stats();
#endif

    fprintf(stdout, "Wordcount: Scheduler Completed\n");
    fprintf(stderr,
            "[WordCount result] unique_words=%zu total_words=%" PRIu64 "\n",
            global->size,
            total_words);
    fprintf(stdout, "\nWordcount: Results (TOP %d):\n", disp_num);
    for (int i = 0; i < disp_num && (size_t)i < global->size; ++i) {
        fprintf(stdout,
                "%15s - %" PRIdPTR "\n",
                (char *)results[i].key,
                (intptr_t)results[i].val);
    }

    status = 0;

cleanup:
    get_time(&begin);

    if (start_workers != NULL && status != 0)
        atomic_store_explicit(start_workers, -1, memory_order_release);
    if (machines != NULL && status != 0) {
        for (int machine = 0; machine < machine_count; ++machine) {
            if (atomic_load_explicit(&machines[machine].stage_state,
                                     memory_order_relaxed)
                == 0)
                atomic_store_explicit(
                        &machines[machine].stage_state, -1, memory_order_release);
        }
    }
    if (combiner_args != NULL && status != 0) {
        for (int machine = 0; machine < machine_count; ++machine) {
            if (atomic_load_explicit(&combiner_args[machine].status,
                                     memory_order_relaxed)
                == WC_EXCHANGE_PENDING)
                atomic_store_explicit(
                        &combiner_args[machine].exchange_ready,
                        -1,
                        memory_order_release);
        }
    }

    if (combiner_threads != NULL) {
        for (int machine = 0; machine < machine_count; ++machine) {
            if (combiner_threads[machine] != 0
                && !combiner_args[machine].persistent)
                pthread_join(combiner_threads[machine], NULL);
        }
    }
    if (worker_threads != NULL) {
        for (int worker = 0; worker < thread_num; ++worker) {
            if (worker_threads[worker] != 0
                && !worker_args[worker].persistent)
                pthread_join(worker_threads[worker], NULL);
        }
    }
    if (exchanges != NULL) {
        for (int exchange_index = 0; exchange_index < exchange_count;
             ++exchange_index)
            wc_region_free(exchanges[exchange_index]);
    }
    wc_region_free(results);
    if (global != NULL)
        wc_region_free(global->entries);
    wc_region_free(global);

    if (combiner_args != NULL) {
        for (int machine = 1; machine < machine_count; ++machine) {
            if (!combiner_args[machine].persistent)
                continue;
            atomic_store_explicit(&combiner_args[machine].cleanup_request,
                                  1,
                                  memory_order_release);
            while (atomic_load_explicit(&combiner_args[machine].cleanup_done,
                                        memory_order_acquire)
                   == 0)
                sched_yield();
        }
    }
    if (machines != NULL) {
        for (int machine = 0; machine < machine_count; ++machine) {
            int release_status = wc_release_machine(&machines[machine]);

            fprintf(stderr,
                    "[WordCount release] locality_group=%d machine=%d "
                    "pmo_creator=0 pmo_releaser=0 status=%s\n",
                    machines[machine].locality_group,
                    machines[machine].machine_id,
                    release_status == 0 ? "released" : "failed");
            if (release_status != 0)
                status = 1;
        }
    }

    wc_region_free(exchanges);
    wc_region_free(combiner_args);
    wc_region_free(worker_args);
    wc_region_free(start_workers);
    wc_region_free(machines);
    free(combiner_threads);
    free(worker_threads);
    if (fd >= 0)
        close(fd);
    free(fname);

    get_time(&end);
#ifdef TIMING
    fprintf(stderr, "finalize: %u\n", time_diff(&end, &begin));
#endif
    fprintf(stderr, status == 0 ? "done\n" : "failed\n");
    return status;
}
