#include "types.h"
#include "stat.h"
#include "user.h"
#include "mmap.h"

#define PAGE_SIZE 4096

/*
 * Print our three memory diagnostic values.
 */
static void
print_stats(char *label)
{
  printf(1, "\n%s\n", label);
  printf(1, "  virtual pages    : %d\n", numvp());
  printf(1, "  physical pages   : %d\n", numpp());
  printf(1, "  page-table pages : %d\n", getptsize());
}


/*
 * Run a write in a child process.
 *
 * If the write succeeds, the child writes one byte into the
 * pipe. If protection works, the child is killed by xv6 before
 * it can write that byte.
 *
 * Returns:
 *   0  -> write was correctly rejected
 *  -1  -> write unexpectedly succeeded / setup failed
 */
static int
expect_write_failure(char *addr)
{
  int fd[2];
  int pid;
  int n;
  char marker;

  if(pipe(fd) < 0)
    return -1;

  pid = fork();

  if(pid < 0){
    close(fd[0]);
    close(fd[1]);
    return -1;
  }

  if(pid == 0){
    close(fd[0]);

    /*
     * This is expected to kill the child.
     */
    *addr = 'X';

    /*
     * Reaching this point means the protection failed.
     */
    marker = 'F';
    write(fd[1], &marker, 1);

    close(fd[1]);
    exit();
  }

  close(fd[1]);

  /*
   * If the child dies before writing anything, read()
   * returns 0 after the descriptor is closed.
   */
  n = read(fd[0], &marker, 1);

  close(fd[0]);
  wait();

  if(n == 0)
    return 0;

  return -1;
}


/*
 * Same idea, but for testing an address that should not
 * even be readable, such as memory after munmap().
 */
static int
expect_read_failure(char *addr)
{
  int fd[2];
  int pid;
  int n;
  char marker;

  if(pipe(fd) < 0)
    return -1;

  pid = fork();

  if(pid < 0){
    close(fd[0]);
    close(fd[1]);
    return -1;
  }

  if(pid == 0){
    close(fd[0]);

    /*
     * If this address is valid, the read succeeds and
     * the child reports failure to the parent.
     */
    marker = *addr;

    write(fd[1], &marker, 1);

    close(fd[1]);
    exit();
  }

  close(fd[1]);

  n = read(fd[0], &marker, 1);

  close(fd[0]);
  wait();

  if(n == 0)
    return 0;

  return -1;
}


/*
 * Verify that a child can inherit an untouched lazy VMA
 * and fault in its own physical page successfully.
 */
static int
test_lazy_fork(char *addr)
{
  int fd[2];
  int pid;
  int n;
  char marker;

  if(pipe(fd) < 0)
    return -1;

  pid = fork();

  if(pid < 0){
    close(fd[0]);
    close(fd[1]);
    return -1;
  }

  if(pid == 0){
    close(fd[0]);

    /*
     * The page is still absent when fork() occurs.
     * The child should inherit the VMA metadata and
     * allocate its own physical page on this write.
     */
    *addr = 'C';

    if(*addr != 'C')
      exit();

    marker = 'S';
    write(fd[1], &marker, 1);

    close(fd[1]);
    exit();
  }

  close(fd[1]);

  n = read(fd[0], &marker, 1);

  close(fd[0]);
  wait();

  if(n == 1 && marker == 'S')
    return 0;

  return -1;
}


int
main(void)
{
  char *p;

  int base_vp;
  int base_pp;

  int after_map_vp;
  int after_map_pp;

  int pp;

  printf(1, "\n");
  printf(1, "========================================\n");
  printf(1, " xv6 demand-paged mmap acceptance test\n");
  printf(1, "========================================\n");


  /*
   * ----------------------------------------------------
   * TEST 1: baseline
   * ----------------------------------------------------
   */
  base_vp = numvp();
  base_pp = numpp();

  print_stats("TEST 1 - Initial process memory");


  /*
   * ----------------------------------------------------
   * TEST 2: mmap reserves virtual memory only
   * ----------------------------------------------------
   */
  printf(1, "\nTEST 2 - mmap virtual reservation\n");

  p = mmap(3 * PAGE_SIZE);

  if(p == (char*)-1){
    printf(1, "FAIL: mmap returned -1\n");
    exit();
  }

  printf(1, "mmap returned address 0x%x\n", (uint)p);

  after_map_vp = numvp();
  after_map_pp = numpp();

  print_stats("After mmap(3 pages)");

  if(after_map_vp != base_vp + 3){
    printf(1, "FAIL: virtual page count did not increase by 3\n");
    exit();
  }

  if(after_map_pp != base_pp){
    printf(1, "FAIL: mmap allocated physical pages eagerly\n");
    exit();
  }

  printf(1, "PASS: 3 virtual pages reserved, 0 physical pages allocated\n");


  /*
   * ----------------------------------------------------
   * TEST 3: first access performs demand allocation
   * ----------------------------------------------------
   */
  printf(1, "\nTEST 3 - Demand allocation of page 0\n");

  /*
   * Anonymous mmap memory should initially contain zero.
   *
   * This read itself faults the page into memory.
   */
  if(p[0] != 0){
    printf(1, "FAIL: new mmap page was not zero-filled\n");
    exit();
  }

  pp = numpp();

  if(pp != after_map_pp + 1){
    printf(1, "FAIL: first access did not allocate exactly one page\n");
    exit();
  }

  p[0] = 'A';

  if(p[0] != 'A'){
    printf(1, "FAIL: page 0 read/write failed\n");
    exit();
  }

  print_stats("After first access to page 0");

  printf(1, "PASS: page 0 allocated lazily and zero-filled\n");


  /*
   * ----------------------------------------------------
   * TEST 4: untouched lazy page survives fork
   * ----------------------------------------------------
   *
   * Page 2 is still not resident in the parent.
   */
  printf(1, "\nTEST 4 - Lazy mmap region across fork\n");

  if(test_lazy_fork(p + 2 * PAGE_SIZE) < 0){
    printf(1, "FAIL: child could not use inherited lazy mapping\n");
    exit();
  }

  /*
   * The child allocated its own page and then exited.
   * Parent page 2 should STILL be lazy.
   */
  if(numpp() != after_map_pp + 1){
    printf(1, "FAIL: child allocation changed parent's page count\n");
    exit();
  }

  printf(1, "PASS: child inherited VMA and faulted its own page\n");


  /*
   * ----------------------------------------------------
   * TEST 5: make complete mapping read-only
   * ----------------------------------------------------
   */
  printf(1, "\nTEST 5 - mprotect(PROT_READ)\n");

  if(mprotect(p,
              3 * PAGE_SIZE,
              PROT_READ) < 0){
    printf(1, "FAIL: mprotect(PROT_READ) failed\n");
    exit();
  }

  /*
   * Existing resident page must remain readable.
   */
  if(p[0] != 'A'){
    printf(1, "FAIL: read from protected page failed\n");
    exit();
  }

  printf(1, "PASS: resident read-only page remains readable\n");


  /*
   * ----------------------------------------------------
   * TEST 6: writing resident read-only page must fail
   * ----------------------------------------------------
   */
  printf(1, "\nTEST 6 - Resident-page write protection\n");

  if(expect_write_failure(p) < 0){
    printf(1, "FAIL: write to resident read-only page succeeded\n");
    exit();
  }

  printf(1, "PASS: write to resident read-only page rejected\n");


  /*
   * ----------------------------------------------------
   * TEST 7: write to LAZY read-only page must also fail
   * ----------------------------------------------------
   *
   * Page 1 has never been touched.
   *
   * Therefore there is no physical page/PTE yet.
   * Protection must come from VMA metadata.
   */
  printf(1, "\nTEST 7 - Lazy-page write protection\n");

  pp = numpp();

  if(expect_write_failure(p + PAGE_SIZE) < 0){
    printf(1, "FAIL: write to lazy read-only page succeeded\n");
    exit();
  }

  /*
   * Illegal write should NOT have allocated the page.
   */
  if(numpp() != pp){
    printf(1, "FAIL: rejected write allocated physical memory\n");
    exit();
  }

  printf(1, "PASS: lazy read-only write rejected without allocation\n");


  /*
   * ----------------------------------------------------
   * TEST 8: legal read of lazy read-only page
   * ----------------------------------------------------
   */
  printf(1, "\nTEST 8 - Read fault on lazy read-only page\n");

  pp = numpp();

  if(p[PAGE_SIZE] != 0){
    printf(1, "FAIL: newly allocated page was not zero-filled\n");
    exit();
  }

  if(numpp() != pp + 1){
    printf(1, "FAIL: lazy read did not allocate exactly one page\n");
    exit();
  }

  printf(1, "PASS: read allocated a zero-filled read-only page\n");


  /*
   * ----------------------------------------------------
   * TEST 9: restore write permission
   * ----------------------------------------------------
   */
  printf(1, "\nTEST 9 - Restore read/write permission\n");

  if(mprotect(p,
              3 * PAGE_SIZE,
              PROT_READ | PROT_WRITE) < 0){
    printf(1, "FAIL: mprotect(read-write) failed\n");
    exit();
  }

  p[0] = 'D';
  p[PAGE_SIZE] = 'E';

  if(p[0] != 'D' || p[PAGE_SIZE] != 'E'){
    printf(1, "FAIL: restored write permission does not work\n");
    exit();
  }

  printf(1, "PASS: writable permission restored\n");


  /*
   * ----------------------------------------------------
   * TEST 10: demand allocate final page in parent
   * ----------------------------------------------------
   */
  printf(1, "\nTEST 10 - Allocate final lazy page\n");

  pp = numpp();

  if(p[2 * PAGE_SIZE] != 0){
    printf(1, "FAIL: final lazy page was not zero-filled\n");
    exit();
  }

  if(numpp() != pp + 1){
    printf(1, "FAIL: final page allocation count incorrect\n");
    exit();
  }

  p[2 * PAGE_SIZE] = 'F';

  print_stats("All three mmap pages resident");

  printf(1, "PASS: final page allocated on demand\n");


  /*
   * ----------------------------------------------------
   * TEST 11: munmap reclaims all resident pages
   * ----------------------------------------------------
   */
  printf(1, "\nTEST 11 - munmap and page reclamation\n");

  if(munmap(p, 3 * PAGE_SIZE) < 0){
    printf(1, "FAIL: munmap failed\n");
    exit();
  }

  print_stats("After munmap");

  if(numvp() != base_vp){
    printf(1, "FAIL: virtual pages were not released\n");
    exit();
  }

  if(numpp() != base_pp){
    printf(1, "FAIL: physical pages were not reclaimed\n");
    exit();
  }

  printf(1, "PASS: virtual mapping removed and physical pages reclaimed\n");


  /*
   * ----------------------------------------------------
   * TEST 12: old address must now be invalid
   * ----------------------------------------------------
   */
  printf(1, "\nTEST 12 - Access after munmap\n");

  if(expect_read_failure(p) < 0){
    printf(1, "FAIL: unmapped memory remained accessible\n");
    exit();
  }

  printf(1, "PASS: unmapped address correctly rejected\n");


  /*
   * ----------------------------------------------------
   * TEST 13: invalid mmap arguments
   * ----------------------------------------------------
   */
  printf(1, "\nTEST 13 - Basic argument validation\n");

  if(mmap(0) != (char*)-1){
    printf(1, "FAIL: mmap(0) unexpectedly succeeded\n");
    exit();
  }

  printf(1, "PASS: invalid zero-length mapping rejected\n");


  printf(1, "\n");
  printf(1, "========================================\n");
  printf(1, " ALL MMAP / MPROTECT TESTS PASSED\n");
  printf(1, "========================================\n");

  exit();
}
