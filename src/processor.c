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

/* OS specific headers and defines. */
#if defined _LINUX_ || defined _CHCORE_
#define _GNU_SOURCE
#include <sched.h>

#elif defined (_SOLARIS_)
#include <sys/procset.h>
#include <sys/processor.h>
#include <sys/lgrp_user.h>

#else
#error OS not supported
#endif

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <assert.h>
#include <stdio.h>
#include <stdatomic.h>
#include <ctype.h>

#include "processor.h"
#include "memory.h"

int thread_num = 0;
bool thread_bind_cpu_set = false;
int thread_bind_cpu_list[1024];
char thread_bind_cpu_filename[1024];

#define info_once(fmt, ...) do {  \
	static int __warned = 0;  \
	if (__warned) break;      \
	__warned = 1;             \
	printf(fmt, ##__VA_ARGS__);    \
} while (0)

int parse_cpu_bind_file(char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "Failed to open CPU bind file: %s\n", filename);
        return -1;
    }
    
    char buffer[1024];
    int index = 0;
    if (fgets(buffer, sizeof(buffer), file)) {
        char *p = buffer;
        char *end;
        
        while (*p) {
            // 跳过空白字符
            while (*p && isspace(*p)) p++;
            if (!*p) break;
            
            // 读取第一个数字
            long start = strtol(p, &end, 10);
            if (end == p) {
                fprintf(stderr, "invalid number: %s\n", p);
                break;
            }
            p = end;
            
            // 检查是否有范围符号 '-'
            if (*p == '-') {
                p++;
                // 读取第二个数字
                long end_num = strtol(p, &end, 10);
                if (end == p) {
                    fprintf(stderr, "invalid range end: %s\n", p);
                    break;
                }
                p = end;
                
                // 处理范围
                for (long i = start; i <= end_num; i++) {
                    thread_bind_cpu_list[index++] = i;
                }
            } else {
                // 单个数字
                thread_bind_cpu_list[index++] = start;
            }
            
            // 跳过逗号或空格
            while (*p && (*p == ',' || isspace(*p))) p++;
        }
    }

    if (index == 0) {
        fprintf(stderr, "No CPU bind file found\n");
        return -1;
    }
    if (index < thread_num) {
        fprintf(stderr, "CPU bind file size is less than thread number\n");
        return -1;
    }

    fprintf(stderr, "bind %d cpu: ", index);
    for (int i = 0; i < index; i++) {
        fprintf(stderr, "%d ", thread_bind_cpu_list[i]);
    }
    fprintf(stderr, "\n");
    
    fclose(file);
    return 0;
}

/* Query the number of CPUs online. */
inline int proc_get_num_cpus (void)
{
    int num_cpus;
    char *num_proc_str;

    num_cpus = sysconf(_SC_NPROCESSORS_ONLN);

    if (thread_num > 0) {
        /* FIXME(FN): need to check whelther it exceed max cpu number*/
        /* but in chcore, sysconf is not implemented */
        num_cpus = thread_num;
        // printf("Thread number is set as %d\n", thread_num);
        goto out;
    }

    /* Check if the user specified a different number of processors. */
    if ((num_proc_str = getenv("MAPRED_NPROCESSORS")))
    {
        int temp = atoi(num_proc_str);
        if (temp < 1 || temp > num_cpus)
            num_cpus = 0;
        else
            num_cpus = temp;
    }

out:
    // info_once("phoenix cpu num=%d\n", num_cpus);
    return num_cpus;
}

#if defined _LINUX_ || defined _CHCORE_
static cpu_set_t    full_cs;
static cpu_set_t* proc_get_full_set(void)
{
    static int          inited = 0;

    if (inited == 0) {
        int i;
        int n_cpus;

        CPU_ZERO (&full_cs);
        n_cpus = sysconf(_SC_NPROCESSORS_ONLN);
        for (i = 0; i < n_cpus; i++) {
            CPU_SET(i, &full_cs);
        }

        inited = 1;
    }

    return &full_cs;
}
#endif

/* Bind the calling thread to run on CPU_ID. 
   Returns 0 if successful, -1 if failed. */
inline int proc_bind_thread (int cpu_id)
{
#if defined _LINUX_ || defined _CHCORE_
    cpu_set_t   cpu_set;

    CPU_ZERO (&cpu_set);
    CPU_SET (cpu_id, &cpu_set);
#if defined DSM_ENABLED
    sched_setaffinity(-2, sizeof(cpu_set), &cpu_set);
#else
    sched_setaffinity (0, sizeof (cpu_set), &cpu_set);
#endif
    return sched_yield();
#elif defined (_SOLARIS_)
    return processor_bind (P_LWPID, P_MYID, cpu_id, NULL);
#endif
}

inline int proc_unbind_thread ()
{
#if defined _LINUX_ || defined _CHCORE_
#if defined DSM_ENABLED
    /* avoid rescheding */
    return 0;
#endif
    return sched_setaffinity (0, sizeof (cpu_set_t), proc_get_full_set());
#elif defined (_SOLARIS_)
    return processor_bind (P_LWPID, P_MYID, PBIND_NONE, NULL);
#endif
}

/* Test whether processor CPU_ID is available. */
inline bool proc_is_available (int cpu_id)
{
#if defined _LINUX_ || defined _CHCORE_
    int ret;
    cpu_set_t cpu_set;
    
    ret = sched_getaffinity (0, sizeof (cpu_set), &cpu_set);
    if (ret < 0) return false;

    return CPU_ISSET (cpu_id, &cpu_set) ? true : false;
#elif defined (_SOLARIS_)
    return (p_online (cpu_id, P_STATUS) == P_ONLINE);
#endif
}

inline int proc_get_cpuid (void)
{
#if defined _LINUX_ || defined _CHCORE_
    int i, ret;
    cpu_set_t cpu_set;
    
    ret = sched_getaffinity (0, sizeof (cpu_set), &cpu_set);
    if (ret < 0) return -1;

    for (i = 0; i < CPU_SETSIZE; ++i)
    {
        if (CPU_ISSET (i, &cpu_set)) break;
    }
    return i;
#elif defined (_SOLARIS_)
    return getcpuid ();
#endif
}
