#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

int main(int argc, char **argv) {
  int flags = atoi(argv[1]);
  printf("flags %d\n", flags);
  void *vm = mixed_malloc(0x100000, flags);
  int *m = (int *)vm;
  printf("pos:%p vm:%p\n", m, vm);
  m[0] = 1;
  printf("%d\n", m[0]);
  return 0;
}
