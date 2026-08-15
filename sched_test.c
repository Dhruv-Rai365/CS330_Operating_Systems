#include "types.h"
#include "stat.h"
#include "user.h"

/*
 * Number of iterations performed by every child.
 *
 * If all processes finish in almost the same number
 * of clock ticks on your machine, increase this.
 */
#define WORK 20000000


/*
 * CPU-intensive workload.
 *
 * volatile prevents the compiler from optimizing
 * the loop away.
 */
static void
cpu_work(void)
{
  volatile uint value;
  int i;

  value = 1;

  for(i = 0; i < WORK; i++)
    value = value * 1664525 + 1013904223;

  /*
   * Prevent an aggressive compiler from concluding that
   * the final value is completely irrelevant.
   */
  if(value == 0)
    printf(1, "");
}


int
main(int argc, char *argv[])
{
  int priorities[3];
  int i;
  int pid;
  int start;

  (void)argc;
  (void)argv;

  priorities[0] = 1;
  priorities[1] = 2;
  priorities[2] = 4;

  printf(1, "Weighted Round-Robin scheduler test\n");
  printf(1, "Priorities: 1, 2, 4\n\n");

  /*
   * All completion times are measured relative
   * to one common starting timestamp.
   */
  start = uptime();

  for(i = 0; i < 3; i++){

    pid = fork();

    if(pid < 0){
      printf(2, "sched_test: fork failed\n");
      exit();
    }

    if(pid == 0){

      /*
       * Each child receives a different scheduling weight.
       */
      if(setprio(priorities[i]) < 0){
        printf(2, "sched_test: setprio failed\n");
        exit();
      }

      /*
       * Verify that the priority syscall actually worked.
       */
      if(getprio() != priorities[i]){
        printf(2, "sched_test: priority verification failed\n");
        exit();
      }

      cpu_work();

      printf(1,
             "pid %d priority %d finished after %d ticks\n",
             getpid(),
             getprio(),
             uptime() - start);

      exit();
    }
  }

  /*
   * Parent waits for all three CPU-bound children.
   */
  for(i = 0; i < 3; i++)
    wait();

  printf(1, "\nScheduler test complete\n");

  exit();
}
