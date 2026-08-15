#include "types.h"
#include "stat.h"
#include "user.h"

#define NPROC_TEST      3
#define TEST_TICKS    100
#define START_DELAY    10
#define CHUNK_ITERS  5000

/*
 * Result sent from each child to the parent.
 */
struct result {
  int priority;
  int work_units;
};

/*
 * Perform a fixed amount of CPU-only work.
 *
 * We count one invocation of this function as one "work unit".
 */
static void
do_cpu_chunk(void)
{
  volatile uint x;
  int i;

  x = 1;

  for(i = 0; i < CHUNK_ITERS; i++)
    x = x * 1664525 + 1013904223;

  /*
   * Keep the compiler from removing the calculation.
   */
  if(x == 0)
    printf(1, "");
}


int
main(int argc, char *argv[])
{
  int priorities[NPROC_TEST];
  int p[2];
  int start_tick;
  int end_tick;
  int i;
  int pid;
  struct result r;
  struct result results[NPROC_TEST];

  /*
   * Initialize result slots so the compiler and our code both
   * have a defined value before measurements arrive.
   */
  results[0].priority = 1;
  results[0].work_units = 0;

  results[1].priority = 2;
  results[1].work_units = 0;

  results[2].priority = 4;
  results[2].work_units = 0;

  (void)argc;
  (void)argv;

  priorities[0] = 1;
  priorities[1] = 2;
  priorities[2] = 4;

  if(pipe(p) < 0){
    printf(2, "sched_bench: pipe failed\n");
    exit();
  }

  /*
   * All children use exactly the same measurement window.
   *
   * The small delay gives the parent enough time to create
   * all three children before measurement begins.
   */
  start_tick = uptime() + START_DELAY;
  end_tick = start_tick + TEST_TICKS;

  printf(1, "Weighted Round-Robin quantitative benchmark\n");
  printf(1, "Priorities: 1, 2, 4\n");
  printf(1, "Measurement interval: %d ticks\n\n", TEST_TICKS);

  for(i = 0; i < NPROC_TEST; i++){

    pid = fork();

    if(pid < 0){
      printf(2, "sched_bench: fork failed\n");
      exit();
    }

    if(pid == 0){
      int work_units;

      close(p[0]);

      if(setprio(priorities[i]) < 0){
        printf(2, "sched_bench: setprio failed\n");
        exit();
      }

      /*
       * Wait for the common start time.
       *
       * sleep(1) prevents the waiting phase itself from
       * consuming significant CPU time.
       */
      while(uptime() < start_tick)
        sleep(1);

      work_units = 0;

      /*
       * Every child runs until the same wall-clock deadline.
       *
       * A process receiving a larger CPU share should complete
       * more work chunks during this interval.
       */
      while(uptime() < end_tick){
        do_cpu_chunk();
        work_units++;
      }

      r.priority = priorities[i];
      r.work_units = work_units;

      /*
       * Send the measurement back to the parent.
       */
      write(p[1], &r, sizeof(r));

      close(p[1]);
      exit();
    }
  }

  /*
   * Parent only reads results.
   */
  close(p[1]);

  for(i = 0; i < NPROC_TEST; i++){
    if(read(p[0], &r, sizeof(r)) != sizeof(r)){
      printf(2, "sched_bench: failed to read result\n");
      exit();
    }

    /*
     * Store by priority position rather than arrival order.
     */
    if(r.priority == 1)
      results[0] = r;
    else if(r.priority == 2)
      results[1] = r;
    else if(r.priority == 4)
      results[2] = r;
  }

  close(p[0]);

  for(i = 0; i < NPROC_TEST; i++)
    wait();

  printf(1, "Results:\n");
  printf(1, "priority 1 : %d work units\n", results[0].work_units);
  printf(1, "priority 2 : %d work units\n", results[1].work_units);
  printf(1, "priority 4 : %d work units\n", results[2].work_units);

  /*
   * Avoid floating point: report ratios multiplied by 100.
   *
   * Example:
   *   195 means 1.95x
   */
  if(results[0].work_units > 0){
    printf(1, "\nRelative to priority 1:\n");
    printf(1, "priority 1 : 100/100\n");
    printf(1, "priority 2 : %d/100\n",
           (results[1].work_units * 100) /
           results[0].work_units);
    printf(1, "priority 4 : %d/100\n",
           (results[2].work_units * 100) /
           results[0].work_units);
  }

  exit();
}
