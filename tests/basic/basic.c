#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

int main(int argc, char **argv) {
  int flags = atoi(argv[1]);
  int size = atoi(argv[2]);
  printf("flags %d size %d\n", flags, size);
  int *m = (int *)mixed_malloc(0x100000, flags);
  printf("pos:%p\n", m);
  m[0] = 1;
  printf("%d\n", m[0]);
  free(m);
  return 0;
}
