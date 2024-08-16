#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <rpmalloc.h>

int main(int argc, char **argv) {
  // int flags = atoi(argv[1]);
  // int size = atoi(argv[2]);
  // printf("flags %d size %d\n", flags, size);
  // int *m = (int *)mixed_malloc(size, flags);
  // printf("pos:%p\n", m);
  // m[0] = 1;
  // printf("m[0]=%d\n", m[0]);
  // free(m);

  // printf("mmap file\n");
  // char *f;
  // int fd, file_size;
  // fd = open("matrix_file_A.txt", O_RDONLY);
  // f = mmap(0,
  //          file_size + 1,
  //          PROT_READ | PROT_WRITE,
  //          MAP_PRIVATE | 0x200000,
  //          fd,
  //          0);
  // printf("mmap fata A success to va=%llx\n", f);
  // printf("f[0]=%d\n", f[0]);
  // f[0] = 1;
  // printf("f[0]=%d\n", f[0]);
  int *m = rpmalloc(100);
  m[0] = 1;
  printf("%d\n", m[0]);
  rpfree(m);
  return 0;
}
