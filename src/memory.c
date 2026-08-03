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
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#ifdef _SOLARIS_
#define PAGE_SIZE (8 * 1024)
#include <mtmalloc.h>
#include <sys/mman.h>
#else
#include <stdlib.h>
#define PAGE_SIZE (4 * 1024)
#endif

#define ALIGN_PAGE(ptr) (void *)((uintptr_t)(ptr) & (~(PAGE_SIZE - 1)))

#include "memory.h"
#include "stddefines.h"

#define SHARED_ALLOC_MAGIC UINT64_C(0x5348415245444d52)
#define DEFAULT_SHARED_ARENA_CHUNK (2 * 1024 * 1024)

typedef union shared_alloc_header {
    struct {
        size_t mapping_size;
        uint64_t magic;
    } fields;
    max_align_t alignment;
} shared_alloc_header_t;

typedef struct mem_shared_chunk {
    struct mem_shared_chunk *next;
    size_t capacity;
    size_t used;
    max_align_t alignment;
    unsigned char data[];
} mem_shared_chunk_t;

struct mem_shared_arena {
    size_t chunk_size;
    mem_shared_chunk_t *chunks;
};

static size_t align_up(size_t value, size_t alignment)
{
    assert(alignment != 0 && (alignment & (alignment - 1)) == 0);
    return (value + alignment - 1) & ~(alignment - 1);
}

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

void *mem_shared_malloc (size_t size)
{
    shared_alloc_header_t *header;
    size_t total;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;

    assert(size > 0);
    assert(size <= SIZE_MAX - sizeof(*header));
    total = sizeof(*header) + size;
    assert(total <= SIZE_MAX - (PAGE_SIZE - 1));
    total = align_up(total, PAGE_SIZE);
#ifdef _CHCORE_
    flags |= MAP_FLAG_SHARED;
#endif
    header = mmap(NULL, total, PROT_READ | PROT_WRITE, flags, -1, 0);
    assert(header != MAP_FAILED);
    header->fields.mapping_size = total;
    header->fields.magic = SHARED_ALLOC_MAGIC;
    return header + 1;
}

void *mem_shared_calloc (size_t num, size_t size)
{
    void *ptr;

    assert(num > 0 && size > 0);
    assert(num <= SIZE_MAX / size);
    ptr = mem_shared_malloc(num * size);
    memset(ptr, 0, num * size);
    return ptr;
}

void mem_shared_free (void *ptr)
{
    shared_alloc_header_t *header;
    size_t mapping_size;
    int ret;

    if (ptr == NULL)
        return;
    header = (shared_alloc_header_t *)ptr - 1;
    assert(header->fields.magic == SHARED_ALLOC_MAGIC);
    mapping_size = header->fields.mapping_size;
    header->fields.magic = 0;
    ret = munmap(header, mapping_size);
    assert(ret == 0);
}

mem_shared_arena_t *mem_shared_arena_create (size_t chunk_size)
{
    mem_shared_arena_t *arena = mem_calloc(1, sizeof(*arena));

    arena->chunk_size = chunk_size ? chunk_size : DEFAULT_SHARED_ARENA_CHUNK;
    return arena;
}

void *mem_shared_arena_alloc (mem_shared_arena_t *arena, size_t size)
{
    const size_t alignment = _Alignof(max_align_t);
    mem_shared_chunk_t *chunk;
    size_t offset;

    assert(arena != NULL && size > 0);
    chunk = arena->chunks;
    offset = chunk ? align_up(chunk->used, alignment) : 0;
    if (chunk == NULL || size > chunk->capacity - offset) {
        size_t minimum = sizeof(*chunk) + alignment - 1 + size;
        size_t mapping = arena->chunk_size;

        if (mapping < minimum)
            mapping = align_up(minimum, PAGE_SIZE);
        chunk = mem_shared_malloc(mapping);
        chunk->next = arena->chunks;
        chunk->capacity = mapping - offsetof(mem_shared_chunk_t, data);
        chunk->used = 0;
        arena->chunks = chunk;
        offset = 0;
    }
    assert(size <= chunk->capacity - offset);
    chunk->used = offset + size;
    return chunk->data + offset;
}

void *mem_shared_arena_calloc (
    mem_shared_arena_t *arena, size_t num, size_t size)
{
    void *ptr;

    assert(num > 0 && size > 0);
    assert(num <= SIZE_MAX / size);
    ptr = mem_shared_arena_alloc(arena, num * size);
    memset(ptr, 0, num * size);
    return ptr;
}

void mem_shared_arena_destroy (mem_shared_arena_t *arena)
{
    mem_shared_chunk_t *chunk;

    if (arena == NULL)
        return;
    chunk = arena->chunks;
    while (chunk != NULL) {
        mem_shared_chunk_t *next = chunk->next;
        mem_shared_free(chunk);
        chunk = next;
    }
    mem_free(arena);
}
