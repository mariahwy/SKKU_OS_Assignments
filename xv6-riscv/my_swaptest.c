#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define NUM_CHILD 30   // 많은 프로세스를 생성해 페이지를 소비

int
main(void)
{
  int pid;
  char *p;

  // 1. consume heap memory using sbrk()
  for(int i = 0; i < 100; i++){
    p = sbrk(4096);  // 1 page
    if(p == (char*)-1){
      printf("sbrk failed at iteration %d\n", i);
      exit(1);
    }
    *p = i; // write to the page so it is actually allocated
  }

  // 2. fork many processes to consume more pages
  for(int i = 0; i < NUM_CHILD; i++){
    pid = fork();
    if(pid < 0){
      printf("fork failed at %d\n", i);
      break;
    }
    if(pid == 0){ // child
      char *cp = sbrk(4096);
      *cp = i; // force page allocation
      sleep(5); // keep children alive for a while
      exit(0);
    }
  }

  // 3. wait for children
  while(wait(0) >= 0);

  // 4. check swap stats
  int r, w;
  swapstat(&r, &w);
  printf("Swap stats: read %d, write %d\n", r, w);

  exit(0);
}
