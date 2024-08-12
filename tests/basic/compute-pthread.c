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

#include "stddefines.h"
#define SIZE 0x1c00*(4*sizeof(size_t))


void get_aff(int tid)
{
  int i, ret;
  cpu_set_t cpu_set;

  CPU_ZERO(&cpu_set);
  
  ret = sched_getaffinity (0, sizeof (cpu_set), &cpu_set);
  if (ret < 0) fprintf(stderr, "tid:%d error\n", tid);

  for (i = 0; i < CPU_SETSIZE; ++i)
  {
      if (CPU_ISSET (i, &cpu_set)) {fprintf(stderr, "tid:%d -> cpu:%d\n", tid, i);break;};
  }
}

void set_aff(int cpu)
{
    cpu_set_t   cpu_set;

    CPU_ZERO (&cpu_set);
    CPU_SET (cpu, &cpu_set);

    sched_setaffinity (0, sizeof (cpu_set), &cpu_set);
}
/** parse_args()
 *  Parse the user arguments to determine the number of rows and colums
 */  
#define DEF_THREAD_NUM 4
#define DEF_ELE_NUM 1000
extern int thread_num;
int element_number;
int do_page_fault = 0;
void parse_args(int argc, char **argv) 
{
    int c;
    extern char *optarg;
    extern int optind;

    thread_num = 4;
    element_number = DEF_ELE_NUM;

    while ((c = getopt(argc, argv, "e:t:f:q")) != EOF) 
    {
        switch (c) {
            case 'e':
                element_number = atoi(optarg);
                break;
            case 't':
                thread_num = atoi(optarg);   
                break;
            case 'f':
                do_page_fault = atoi(optarg);   
                break;
            case '?':
                printf("Usage: %s -e <element number per thread> -t <thread num> \n", argv[0]);
                exit(1);
        }
    }
    
    if (element_number <= 0 || thread_num <= 0 || thread_num > 128) {
        printf("Illegal argument value. All values must be numeric and greater than 0. And thread num must be less than 128\n");
        exit(1);
    }
    
    printf("Element number = %d\n", element_number);    
    printf("Number of threads = %d\n", thread_num);  
}
void* compute_fun(void* args)
{
  int tid = *((int*)args);
  // usys_set_affinity(0, tid + 2);
  set_aff(tid + 1);
  struct timeval begin, end;
  int *ptr_data[element_number];
  get_time(&begin);
  for (int j = 0; j < element_number; j++)
  {
    int* data = malloc(SIZE);
    ptr_data[j] = data;
    if (do_page_fault)
    {
      for (int i = 0; i < SIZE / 4; i += 1024 * 4 / 4)
      {
        data[i] = i;
      }
    }
    if (j % 100 == 0)
    {
      fprintf(stderr ,"tid:%d element:%d\n", tid, j);
    }
  }

  get_time(&end);
  fprintf(stderr, "tid:%d assign cost:%u\n", tid, time_diff(&end, &begin));
  for (int j = 0; j < element_number; j++){
    free(ptr_data[j]);
  }
  return NULL;
}

int main(int argc, char **argv)
{
  parse_args(argc, argv); 
  int count = 10;
  for (size_t i = 0; i < count; i++)
  {  
    pthread_t pid[thread_num];
    int tid[thread_num];
    for (int i = 0; i < thread_num; i++)
    {
      tid[i] = i;
      pthread_create(pid + i, NULL, compute_fun, tid + i);
    }
    for (int i = 0; i < thread_num; i++)
    {
      pthread_join(pid[i], NULL);
    }
  }

}
