#define _GNU_SOURCE

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
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

#include "stddefines.h"


typedef volatile _Atomic(int32_t) atomic32_t;
typedef volatile _Atomic(int64_t) atomic64_t;
typedef volatile _Atomic(void*) atomicptr_t;

#ifndef FORCEINLINE
    #define FORCEINLINE inline __attribute__((__always_inline__))
#endif

static FORCEINLINE int32_t atomic_load32(atomic32_t* src) { return atomic_load_explicit(src, memory_order_relaxed); }
static FORCEINLINE void    atomic_store32(atomic32_t* dst, int32_t val) { atomic_store_explicit(dst, val, memory_order_relaxed); }
static FORCEINLINE int32_t atomic_incr32(atomic32_t* val) { return atomic_fetch_add_explicit(val, 1, memory_order_relaxed) + 1; }
static FORCEINLINE int32_t atomic_decr32(atomic32_t* val) { return atomic_fetch_add_explicit(val, -1, memory_order_relaxed) - 1; }
static FORCEINLINE int32_t atomic_add32(atomic32_t* val, int32_t add) { return atomic_fetch_add_explicit(val, add, memory_order_relaxed) + add; }
static FORCEINLINE int     atomic_cas32_acquire(atomic32_t* dst, int32_t val, int32_t ref) { return atomic_compare_exchange_weak_explicit(dst, &ref, val, memory_order_acquire, memory_order_relaxed); }
static FORCEINLINE void    atomic_store32_release(atomic32_t* dst, int32_t val) { atomic_store_explicit(dst, val, memory_order_release); }
static FORCEINLINE int64_t atomic_load64(atomic64_t* val) { return atomic_load_explicit(val, memory_order_relaxed); }
static FORCEINLINE int64_t atomic_add64(atomic64_t* val, int64_t add) { return atomic_fetch_add_explicit(val, add, memory_order_relaxed) + add; }
static FORCEINLINE void*   atomic_load_ptr(atomicptr_t* src) { return atomic_load_explicit(src, memory_order_relaxed); }
static FORCEINLINE void    atomic_store_ptr(atomicptr_t* dst, void* val) { atomic_store_explicit(dst, val, memory_order_relaxed); }
static FORCEINLINE void    atomic_store_ptr_release(atomicptr_t* dst, void* val) { atomic_store_explicit(dst, val, memory_order_release); }
static FORCEINLINE void*   atomic_exchange_ptr_acquire(atomicptr_t* dst, void* val) { return atomic_exchange_explicit(dst, val, memory_order_acquire); }
static FORCEINLINE int     atomic_cas_ptr(atomicptr_t* dst, void* val, void* ref) { return atomic_compare_exchange_weak_explicit(dst, &ref, val, memory_order_relaxed, memory_order_relaxed); }

#define EXPECTED(x) __builtin_expect((x), 1)
#define UNEXPECTED(x) __builtin_expect((x), 0)

// #include "stddefines.h"

#define ARRAY_SIZE 100000 // 数组大小

#define CPU_PAUSE() asm volatile("pause\n":::"memory")
int array[ARRAY_SIZE]; // 全局数组

void set_aff(int cpu)
{
    cpu_set_t   cpu_set;

    CPU_ZERO (&cpu_set);
    CPU_SET (cpu, &cpu_set);

    sched_setaffinity (0, sizeof (cpu_set), &cpu_set);
}

atomic32_t cpu = 0;
int size = 10000000;
void *func(void *arg) {
    set_aff(cpu++);
    CPU_PAUSE();
    printf("set aff:%d\n", cpu-1);

    atomic32_t* counter = arg;
    int old;
    for (int i = 0; i < size; i++)
    {
        old = *counter;
        CPU_PAUSE();
        *counter = old + 1;
    }
    printf("res:%d\n", *counter);
    return NULL;
}

int main() {
    atomic32_t counter;
    pthread_t tid[10];
    for (int i = 0; i < 10; i++)
    {
        pthread_create(&tid[i], NULL, func, (void *)&counter);    
    }
    for (int i = 0; i < 10; i++)
    {
        pthread_detach(tid[i]);    
    }
    
    return 0;
}
