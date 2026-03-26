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
#include <unistd.h>

#include "locality.h"
#include "stddefines.h"
#include "processor.h"

#if defined _LINUX_ || defined _CHCORE_

static int lgrp_inited = 0;
static int lgrp_num = 1;
static int lgrp_first_cpu[64];
static int lgrp_size[64];

static void init_lgrps(void)
{
    if (lgrp_inited)
        return;

    if (!thread_bind_cpu_set || thread_num <= 0) {
        lgrp_num = 1;
        lgrp_first_cpu[0] = 0;
        lgrp_size[0] = proc_get_num_cpus();
        lgrp_inited = 1;
        return;
    }

    lgrp_num = 1;
    lgrp_first_cpu[0] = thread_bind_cpu_list[0];
    lgrp_size[0] = 1;

    for (int i = 1; i < thread_num; i++) {
        if (thread_bind_cpu_list[i] != thread_bind_cpu_list[i - 1] + 1) {
            lgrp_first_cpu[lgrp_num] = thread_bind_cpu_list[i];
            lgrp_size[lgrp_num] = 1;
            lgrp_num++;
        } else {
            lgrp_size[lgrp_num - 1]++;
        }
    }

    lgrp_inited = 1;
}

#elif defined (_SOLARIS_)
#include <sys/lgrp_user.h>
#include <sys/mman.h>

#else
#error OS not supported
#endif

/* Retrieve the number of processors that belong to the locality
   group of the calling LWP. */
inline int loc_get_lgrp_size ()
{
#if defined _LINUX_ || defined _CHCORE_
    init_lgrps();
    return lgrp_size[loc_get_lgrp()];
#elif defined (_SOLARIS_)
    int ret, num_cpus;
    lgrp_id_t lgrp;
    lgrp_cookie_t cookie;

    cookie = lgrp_init (LGRP_VIEW_CALLER);

    lgrp = lgrp_home (P_LWPID, P_MYID);
    num_cpus = lgrp_cpus (cookie, lgrp, NULL, 0, LGRP_CONTENT_DIRECT);
    assert (num_cpus > 0);

    ret = lgrp_fini (cookie);
    assert (! ret);

    return num_cpus;
#endif
}

/* Retrieve the number of total locality groups on system. */
inline int loc_get_num_lgrps ()
{
#if defined _LINUX_ || defined _CHCORE_
    init_lgrps();
    return lgrp_num;
#elif defined (_SOLARIS_)
    int ret;
    lgrp_cookie_t cookie;
    int nlgrps;

    cookie = lgrp_init (LGRP_VIEW_CALLER);
    nlgrps = lgrp_nlgrps (cookie);
    ret = lgrp_fini (cookie);
    assert (!ret);

    if (nlgrps > 1)
    {
        /* Do not count the locality group that encompasses all the 
           locality groups. */
        nlgrps -= 1;
    }

    return nlgrps;
#endif
}

/* Retrieve the locality group of the calling LWP. */
inline int loc_get_lgrp ()
{
#if defined _LINUX_ || defined _CHCORE_
    int cpu;

    init_lgrps();
    cpu = proc_get_cpuid();
    for (int i = lgrp_num - 1; i >= 0; i--) {
        if (cpu >= lgrp_first_cpu[i]) {
            return i;
        }
    }
    return 0;
#elif defined (_SOLARIS_)
    int lgrp = lgrp_home (P_LWPID, P_MYID);

    if (lgrp > 0) {
        /* On a system with multiple locality groups, there exists a
           mother locality group (lgroup 0) that encompasses all the 
           locality groups. Collapse down the hierarchy. */
        lgrp -= 1;
    }
    
    return lgrp;
#endif
}

/* Retrieve the locality group of the physical memory that backs
   the virtual address ADDR. */
inline int loc_mem_to_lgrp (void *addr)
{
#if defined _LINUX_ || defined _CHCORE_
    /* XXX just one for now */
    return 0;
#elif defined (_SOLARIS_)
    uint_t info = MEMINFO_VLGRP;
    uint64_t inaddr;
    uint64_t lgrp;
    uint_t validity;

    if (sizeof (void *) == 4) {
        /* 32 bit. */
        inaddr = 0xffffffff & (intptr_t)addr;
    } else {
        /* 64 bit. */
        assert (sizeof (void *) == 8);
        inaddr = addr;
    }

    CHECK_ERROR(meminfo (&inaddr, 1, &info, 1, &lgrp, &validity));
    if (validity != 3)
    {
        /* VALIDITY better be 3 here. 
           If it is 1, it means the memory has been assigned, but
           not allocated yet. */
        lgrp = 1;
    }

    if (lgrp > 0) {
        /* On a system with multiple locality groups, there exists a
           mother locality group (lgroup 0) that encompasses all the 
           locality groups. Collapse down the hierarchy. */
        lgrp -= 1;
    }
    
    return lgrp;
#endif
}
