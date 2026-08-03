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
#include <string.h>
#ifdef _SOLARIS_
#define PAGE_SIZE (8 * 1024)
#include <mtmalloc.h>
#include <sys/mman.h>
#else
#include <stdlib.h>
#define PAGE_SIZE (4 * 1024)
#endif

#ifdef _CHCORE_
#include <sys/mman.h>
#include <stdio.h>
#endif

#define ALIGN_PAGE(ptr) (void *)((uintptr_t)(ptr) & (~(PAGE_SIZE - 1)))

#include "memory.h"
#include "stddefines.h"

inline void *mem_malloc (size_t size)
{
    void *temp = malloc (size);
    assert(temp);

    return temp;
}

inline void *mem_malloc_here (size_t size)
{
    void *temp = malloc (size);
    assert(temp);

    return temp;
}

/* Allocate a region that every worker of the job reads or writes, wherever
 * those workers end up running.  Plain malloc() follows DSM_USER_MALLOC_MODE,
 * so under the K-mix/U-mix placement it lands in the allocating machine's
 * local DRAM and every remote worker has to fault the region in one page at a
 * time.  MAP_FLAG_SHARED pins it to CXL in both mixed modes (the kernel
 * honours __MT_SHARED__ regardless of DSM_USER_MALLOC_MODE), which is what
 * "shared state goes to CXL" is supposed to mean.  Matrix Multiply already
 * does this by hand for its output matrix. */
inline void *mem_malloc_shared (size_t size)
{
#ifdef _CHCORE_
    void *temp = mmap (NULL, size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FLAG_SHARED, -1, 0);
    if (temp == MAP_FAILED) {
        fprintf (stderr, "mem_malloc_shared: mmap of %zu bytes failed\n", size);
        return NULL;
    }
    return temp;
#else
    void *temp = malloc (size);
    assert(temp);

    return temp;
#endif
}

inline void mem_free_shared (void *ptr, size_t size)
{
#ifdef _CHCORE_
    if (ptr != NULL) {
        munmap (ptr, size);
    }
#else
    (void)size;
    free (ptr);
#endif
}

inline void *mem_calloc (size_t num, size_t size)
{
    void *temp = calloc (num, size);
    assert(temp);

    return temp;
}

inline void *mem_realloc (void *ptr, size_t size)
{
    void *temp = realloc (ptr, size);
    assert(temp);

    return temp;
}

inline void *mem_memcpy (void *dest, const void *src, size_t size)
{
    return memcpy (dest, src, size);
}

inline void *mem_memset (void *s, int c, size_t n)
{
    return memset (s, c, n);
}

inline void mem_free (void *ptr)
{
    free (ptr);
}
