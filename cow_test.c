#include "types.h"
#include "stat.h"
#include "user.h"
#include "mmap.h"

#define PGSIZE_U 4096

/*
 * Used by the nested-fork test.
 */
#define WHO_CHILD       1
#define WHO_GRANDCHILD  2

#define STAGE_BEFORE    1
#define STAGE_AFTER     2

struct nested_msg {
  int who;
  int stage;
  int ref;
  char value;
};


/*
 * Write exactly n bytes.
 *
 * Keeping this helper makes our pipe-based tests less dependent
 * on whether one read/write happens to transfer the entire
 * structure in one operation.
 */
static int
write_full(int fd, void *buf, int n)
{
  int total;
  int r;
  char *p;

  total = 0;
  p = (char*)buf;

  while(total < n){
    r = write(fd, p + total, n - total);

    if(r <= 0)
      return -1;

    total += r;
  }

  return 0;
}


/*
 * Read exactly n bytes.
 */
static int
read_full(int fd, void *buf, int n)
{
  int total;
  int r;
  char *p;

  total = 0;
  p = (char*)buf;

  while(total < n){
    r = read(fd, p + total, n - total);

    if(r <= 0)
      return -1;

    total += r;
  }

  return 0;
}


/*
 * Verify that writing to a genuinely read-only mapping
 * kills the child rather than being treated as a COW write.
 *
 * Return:
 *
 *   0  -> write failed as expected
 *  -1  -> child unexpectedly completed the write
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
     * This process should be killed here.
     */
    *addr = 'X';

    /*
     * If this write executes, protection failed.
     */
    marker = 'F';
    write(fd[1], &marker, 1);

    close(fd[1]);
    exit();
  }

  close(fd[1]);

  /*
   * If the child dies before writing a marker, read()
   * eventually returns 0.
   */
  n = read(fd[0], &marker, 1);

  close(fd[0]);
  wait();

  if(n == 0)
    return 0;

  return -1;
}


int
main(void)
{
  char *p;
  char *q;
  char *r;
  char *s;

  int pid;
  int gpid;

  int p2c[2];
  int c2p[2];

  int report[2];
  int gate[2];

  int data[2];
  int result[2];

  int i;
  int seen_child_before;
  int seen_grand_before;
  int seen_child_after;
  int seen_grand_after;

  char token;
  char child_value;

  struct nested_msg msg;


  printf(1, "\n");
  printf(1, "============================================\n");
  printf(1, " xv6 Copy-on-Write acceptance test\n");
  printf(1, "============================================\n");


  /*
   * ====================================================
   * TEST 1
   *
   * Basic fork:
   *
   *   parent
   *      |
   *      +---- child
   *
   * Immediately after fork, both should point to the SAME
   * physical frame, so pageref() should become 2.
   * ====================================================
   */

  printf(1, "\nTEST 1 - Basic COW sharing and write split\n");

  p = mmap(2 * PGSIZE_U);

  if(p == (char*)-1){
    printf(1, "FAIL: mmap in TEST 1\n");
    exit();
  }

  /*
   * Touch only page 0.
   *
   * Page 0 becomes resident.
   * Page 1 stays lazy.
   */
  p[0] = 'A';

  if(pageref(p) != 1){
    printf(1, "FAIL: initial page refcount should be 1\n");
    exit();
  }

  if(pageref(p + PGSIZE_U) != -1){
    printf(1, "FAIL: untouched mmap page is not lazy\n");
    exit();
  }


  if(pipe(p2c) < 0 || pipe(c2p) < 0){
    printf(1, "FAIL: pipe in TEST 1\n");
    exit();
  }

  pid = fork();

  if(pid < 0){
    printf(1, "FAIL: fork in TEST 1\n");
    exit();
  }


  if(pid == 0){
    int before;
    int after;
    int lazy_before;
    int lazy_after;

    close(p2c[1]);
    close(c2p[0]);

    /*
     * Parent and child should currently share page 0.
     */
    before = pageref(p);

    /*
     * Lazy page should remain absent even after fork.
     */
    lazy_before = pageref(p + PGSIZE_U);

    write_full(c2p[1], &before, sizeof(before));
    write_full(c2p[1], &lazy_before, sizeof(lazy_before));

    /*
     * Wait until parent has inspected the shared state.
     */
    if(read_full(p2c[0], &token, 1) < 0)
      exit();


    /*
     * THIS should trigger the COW write fault.
     */
    p[0] = 'C';

    after = pageref(p);

    /*
     * Touch the lazy page only in the child.
     */
    p[PGSIZE_U] = 'L';

    lazy_after = pageref(p + PGSIZE_U);

    child_value = p[0];

    write_full(c2p[1], &after, sizeof(after));
    write_full(c2p[1], &lazy_after, sizeof(lazy_after));
    write_full(c2p[1], &child_value, 1);

    close(p2c[0]);
    close(c2p[1]);

    exit();
  }


  /*
   * ---------------- Parent ----------------
   */

  close(p2c[0]);
  close(c2p[1]);

  {
    int child_before;
    int child_after;
    int lazy_before;
    int lazy_after;

    if(read_full(c2p[0],
                 &child_before,
                 sizeof(child_before)) < 0){
      printf(1, "FAIL: reading child refcount\n");
      exit();
    }

    if(read_full(c2p[0],
                 &lazy_before,
                 sizeof(lazy_before)) < 0){
      printf(1, "FAIL: reading child lazy state\n");
      exit();
    }


    if(child_before != 2){
      printf(1,
             "FAIL: child expected refcount 2 after fork, got %d\n",
             child_before);
      exit();
    }

    if(pageref(p) != 2){
      printf(1,
             "FAIL: parent expected refcount 2 after fork\n");
      exit();
    }

    if(lazy_before != -1 ||
       pageref(p + PGSIZE_U) != -1){
      printf(1,
             "FAIL: fork allocated an untouched lazy page\n");
      exit();
    }

    printf(1,
           "PASS: parent and child share one physical frame\n");

    printf(1,
           "PASS: untouched mmap page stayed lazy across fork\n");


    /*
     * Now let child perform its write.
     */
    token = 'G';

    write_full(p2c[1], &token, 1);


    if(read_full(c2p[0],
                 &child_after,
                 sizeof(child_after)) < 0){
      printf(1, "FAIL: reading post-COW refcount\n");
      exit();
    }

    if(read_full(c2p[0],
                 &lazy_after,
                 sizeof(lazy_after)) < 0){
      printf(1, "FAIL: reading lazy allocation result\n");
      exit();
    }

    if(read_full(c2p[0],
                 &child_value,
                 1) < 0){
      printf(1, "FAIL: reading child value\n");
      exit();
    }

    close(p2c[1]);
    close(c2p[0]);

    wait();


    /*
     * Child's COW frame should now be private.
     */
    if(child_after != 1){
      printf(1,
             "FAIL: child's private COW frame refcount != 1\n");
      exit();
    }

    if(child_value != 'C'){
      printf(1,
             "FAIL: child did not observe its own write\n");
      exit();
    }

    /*
     * Parent must still see original data.
     */
    if(p[0] != 'A'){
      printf(1,
             "FAIL: child write modified parent memory\n");
      exit();
    }

    /*
     * Child has exited, leaving parent as sole owner.
     */
    if(pageref(p) != 1){
      printf(1,
             "FAIL: parent refcount did not return to 1\n");
      exit();
    }

    if(lazy_after != 1){
      printf(1,
             "FAIL: child lazy page expected refcount 1\n");
      exit();
    }

    /*
     * Child's demand allocation must not materialize the
     * parent's corresponding lazy page.
     */
    if(pageref(p + PGSIZE_U) != -1){
      printf(1,
             "FAIL: child's lazy allocation affected parent\n");
      exit();
    }
  }

  printf(1,
         "PASS: child write cloned page and preserved parent\n");

  printf(1,
         "PASS: child-only lazy allocation stayed private\n");


  /*
   * ====================================================
   * TEST 2
   *
   * Sole-owner optimization.
   *
   * Parent's PTE may still say PTE_COW after child exits.
   * But refcount is now only 1.
   *
   * cow_fault() should simply make the existing page writable
   * instead of allocating/copying another physical frame.
   * ====================================================
   */

  printf(1, "\nTEST 2 - Sole-owner COW optimization\n");

  if(pageref(p) != 1){
    printf(1, "FAIL: expected sole owner before write\n");
    exit();
  }

  p[0] = 'P';

  if(p[0] != 'P'){
    printf(1, "FAIL: sole-owner write failed\n");
    exit();
  }

  if(pageref(p) != 1){
    printf(1,
           "FAIL: sole-owner write changed frame refcount\n");
    exit();
  }

  printf(1,
         "PASS: refcount==1 COW page became writable without duplication\n");


  /*
   * ====================================================
   * TEST 3
   *
   * NESTED FORK:
   *
   *                original parent
   *                       |
   *                    child
   *                       |
   *                 grandchild
   *
   * All three processes should initially share ONE physical
   * frame.
   *
   * Therefore:
   *
   *        refcount == 3
   *
   * This directly tests the case where an already-COW child
   * calls fork() again.
   * ====================================================
   */

  printf(1, "\nTEST 3 - Nested fork and three-way sharing\n");

  q = mmap(PGSIZE_U);

  if(q == (char*)-1){
    printf(1, "FAIL: mmap in nested-fork test\n");
    exit();
  }

  q[0] = 'N';

  if(pageref(q) != 1){
    printf(1,
           "FAIL: nested-test initial refcount should be 1\n");
    exit();
  }


  if(pipe(report) < 0 || pipe(gate) < 0){
    printf(1, "FAIL: nested-test pipes\n");
    exit();
  }


  pid = fork();

  if(pid < 0){
    printf(1, "FAIL: first nested fork\n");
    exit();
  }


  if(pid == 0){

    close(report[0]);
    close(gate[1]);

    /*
     * The child itself now forks.
     */
    gpid = fork();

    if(gpid < 0)
      exit();


    if(gpid == 0){

      /*
       * ---------------- Grandchild ----------------
       *
       * At this point:
       *
       * parent + child + grandchild
       *
       * should all map the same physical frame.
       */

      msg.who = WHO_GRANDCHILD;
      msg.stage = STAGE_BEFORE;
      msg.ref = pageref(q);
      msg.value = q[0];

      write_full(report[1], &msg, sizeof(msg));

      /*
       * Wait until original parent confirms refcount==3.
       */
      if(read_full(gate[0], &token, 1) < 0)
        exit();


      /*
       * Grandchild writes first.
       *
       * Since refcount is 3, this must allocate a private
       * physical frame.
       */
      q[0] = 'G';

      msg.who = WHO_GRANDCHILD;
      msg.stage = STAGE_AFTER;
      msg.ref = pageref(q);
      msg.value = q[0];

      write_full(report[1], &msg, sizeof(msg));

      close(report[1]);
      close(gate[0]);

      exit();
    }


    /*
     * ---------------- Child ----------------
     *
     * Grandchild already exists when we take this measurement.
     */
    msg.who = WHO_CHILD;
    msg.stage = STAGE_BEFORE;
    msg.ref = pageref(q);
    msg.value = q[0];

    write_full(report[1], &msg, sizeof(msg));


    /*
     * Wait for parent to verify three-way sharing.
     */
    if(read_full(gate[0], &token, 1) < 0)
      exit();


    /*
     * Wait until grandchild has completed its private COW
     * write and exited.
     */
    wait();


    /*
     * Now the original frame is shared only between:
     *
     *     original parent + this child
     *
     * so this write should perform another COW split.
     */
    q[0] = 'C';

    msg.who = WHO_CHILD;
    msg.stage = STAGE_AFTER;
    msg.ref = pageref(q);
    msg.value = q[0];

    write_full(report[1], &msg, sizeof(msg));

    close(report[1]);
    close(gate[0]);

    exit();
  }


  /*
   * ---------------- Original parent ----------------
   */

  close(report[1]);
  close(gate[0]);

  seen_child_before = 0;
  seen_grand_before = 0;


  /*
   * Both descendants report before either is allowed to write.
   */
  for(i = 0; i < 2; i++){

    if(read_full(report[0], &msg, sizeof(msg)) < 0){
      printf(1,
             "FAIL: reading nested pre-write report\n");
      exit();
    }

    if(msg.stage != STAGE_BEFORE){
      printf(1,
             "FAIL: unexpected nested-test stage\n");
      exit();
    }

    if(msg.ref != 3){
      printf(1,
             "FAIL: nested process expected refcount 3, got %d\n",
             msg.ref);
      exit();
    }

    if(msg.value != 'N'){
      printf(1,
             "FAIL: nested process saw incorrect initial value\n");
      exit();
    }

    if(msg.who == WHO_CHILD)
      seen_child_before = 1;

    if(msg.who == WHO_GRANDCHILD)
      seen_grand_before = 1;
  }


  if(!seen_child_before || !seen_grand_before){
    printf(1,
           "FAIL: missing nested pre-write report\n");
    exit();
  }


  /*
   * The original parent must independently see the same
   * three-way reference count.
   */
  if(pageref(q) != 3){
    printf(1,
           "FAIL: original parent expected refcount 3\n");
    exit();
  }

  printf(1,
         "PASS: parent, child, and grandchild share one frame (ref=3)\n");


  /*
   * Release both descendants.
   *
   * There are two readers on gate[0], so send two bytes.
   */
  token = '1';
  write_full(gate[1], &token, 1);

  token = '2';
  write_full(gate[1], &token, 1);

  close(gate[1]);


  seen_child_after = 0;
  seen_grand_after = 0;


  /*
   * Read the two post-write reports.
   */
  for(i = 0; i < 2; i++){

    if(read_full(report[0], &msg, sizeof(msg)) < 0){
      printf(1,
             "FAIL: reading nested post-write report\n");
      exit();
    }

    if(msg.stage != STAGE_AFTER){
      printf(1,
             "FAIL: unexpected nested post-write stage\n");
      exit();
    }


    if(msg.who == WHO_GRANDCHILD){

      seen_grand_after = 1;

      if(msg.value != 'G' || msg.ref != 1){
        printf(1,
               "FAIL: grandchild did not receive private frame\n");
        exit();
      }
    }


    if(msg.who == WHO_CHILD){

      seen_child_after = 1;

      if(msg.value != 'C' || msg.ref != 1){
        printf(1,
               "FAIL: child did not receive private frame\n");
        exit();
      }
    }
  }


  close(report[0]);

  wait();


  if(!seen_child_after || !seen_grand_after){
    printf(1,
           "FAIL: missing nested post-write report\n");
    exit();
  }


  /*
   * Both descendants modified private copies.
   *
   * Original parent's value must remain unchanged.
   */
  if(q[0] != 'N'){
    printf(1,
           "FAIL: nested writes modified original parent\n");
    exit();
  }

  if(pageref(q) != 1){
    printf(1,
           "FAIL: original frame not reclaimed to refcount 1\n");
    exit();
  }

  printf(1,
         "PASS: nested COW writes produced independent pages\n");


  /*
   * ====================================================
   * TEST 4
   *
   * mprotect(PROT_READ) followed by:
   *
   *      fork()
   *
   * and then only the PARENT calls:
   *
   *      mprotect(PROT_READ | PROT_WRITE)
   *
   * Important:
   *
   * mprotect() should NOT immediately duplicate the frame.
   *
   * Instead:
   *
   *   VMA says writing is permitted
   *   PTE remains read-only + COW while shared
   *   actual write triggers private-page creation
   *
   * This is the exact scenario discussed during implementation.
   * ====================================================
   */

  printf(1,
         "\nTEST 4 - mprotect READ->RW on a shared page\n");

  r = mmap(PGSIZE_U);

  if(r == (char*)-1){
    printf(1, "FAIL: mmap in mprotect test\n");
    exit();
  }

  r[0] = 'A';


  /*
   * Start with a genuinely read-only mapping.
   */
  if(mprotect(r,
              PGSIZE_U,
              PROT_READ) < 0){
    printf(1,
           "FAIL: initial mprotect(PROT_READ)\n");
    exit();
  }


  if(pipe(p2c) < 0 || pipe(c2p) < 0){
    printf(1,
           "FAIL: pipes in mprotect test\n");
    exit();
  }


  pid = fork();

  if(pid < 0){
    printf(1,
           "FAIL: fork in mprotect test\n");
    exit();
  }


  if(pid == 0){
    int child_ref;

    close(p2c[1]);
    close(c2p[0]);


    /*
     * Wait until parent has:
     *
     *   1. changed only its VMA to READ|WRITE
     *   2. performed its COW write
     */
    if(read_full(p2c[0], &token, 1) < 0)
      exit();


    /*
     * Child must still see the old physical frame/data.
     */
    child_ref = pageref(r);
    child_value = r[0];

    write_full(c2p[1], &child_ref, sizeof(child_ref));
    write_full(c2p[1], &child_value, 1);

    close(p2c[0]);
    close(c2p[1]);

    exit();
  }


  close(p2c[0]);
  close(c2p[1]);


  /*
   * Parent and child currently share the original frame.
   */
  if(pageref(r) != 2){
    printf(1,
           "FAIL: mprotect test expected shared refcount 2\n");
    exit();
  }


  /*
   * Only the parent now requests write permission.
   */
  if(mprotect(r,
              PGSIZE_U,
              PROT_READ | PROT_WRITE) < 0){
    printf(1,
           "FAIL: parent mprotect(PROT_READ|PROT_WRITE)\n");
    exit();
  }


  /*
   * THIS CHECK IS IMPORTANT.
   *
   * mprotect() changes permission policy, but should NOT
   * allocate a private page yet.
   *
   * The frame should still have two references.
   */
  if(pageref(r) != 2){
    printf(1,
           "FAIL: mprotect RW eagerly copied shared page\n");
    exit();
  }

  printf(1,
         "PASS: mprotect RW preserved sharing until actual write\n");


  /*
   * Now the parent actually writes.
   *
   * This is the point at which COW should split the frame.
   */
  r[0] = 'P';


  if(r[0] != 'P'){
    printf(1,
           "FAIL: parent write after mprotect RW\n");
    exit();
  }


  if(pageref(r) != 1){
    printf(1,
           "FAIL: parent's post-write frame not private\n");
    exit();
  }


  /*
   * Tell child to inspect its still-read-only mapping.
   */
  token = 'G';
  write_full(p2c[1], &token, 1);


  {
    int child_ref;

    if(read_full(c2p[0],
                 &child_ref,
                 sizeof(child_ref)) < 0){
      printf(1,
             "FAIL: reading child mprotect result\n");
      exit();
    }

    if(read_full(c2p[0],
                 &child_value,
                 1) < 0){
      printf(1,
             "FAIL: reading child mprotect value\n");
      exit();
    }

    close(p2c[1]);
    close(c2p[0]);

    wait();


    if(child_value != 'A'){
      printf(1,
             "FAIL: parent's RW transition modified child data\n");
      exit();
    }

    if(child_ref != 1){
      printf(1,
             "FAIL: child old frame expected refcount 1\n");
      exit();
    }
  }

  printf(1,
         "PASS: actual parent write created private COW page\n");

  printf(1,
         "PASS: child's read-only mapping remained independent\n");


  /*
   * ====================================================
   * TEST 5
   *
   * Genuine read-only protection must still reject writes.
   *
   * This proves that:
   *
   *     read-only != automatically COW
   * ====================================================
   */

  printf(1,
         "\nTEST 5 - Genuine read-only protection\n");


  if(mprotect(r,
              PGSIZE_U,
              PROT_READ) < 0){
    printf(1,
           "FAIL: mprotect READ in protection test\n");
    exit();
  }


  if(expect_write_failure(r) < 0){
    printf(1,
           "FAIL: read-only page was incorrectly writable/COW\n");
    exit();
  }

  if(r[0] != 'P'){
    printf(1,
           "FAIL: failed write changed protected data\n");
    exit();
  }

  printf(1,
         "PASS: genuine PROT_READ write was rejected\n");


  /*
   * ====================================================
   * TEST 6
   *
   * Kernel copyout() must also respect COW.
   *
   * read(fd, user_buffer, ...) eventually causes the kernel
   * to copy data into user memory.
   *
   * If copyout() bypassed COW, the kernel would modify the
   * parent's shared physical frame too.
   * ====================================================
   */

  printf(1,
         "\nTEST 6 - Kernel copyout into COW page\n");

  s = mmap(PGSIZE_U);

  if(s == (char*)-1){
    printf(1,
           "FAIL: mmap in copyout test\n");
    exit();
  }

  s[0] = 'O';


  if(pipe(data) < 0 || pipe(result) < 0){
    printf(1,
           "FAIL: pipes in copyout test\n");
    exit();
  }


  pid = fork();

  if(pid < 0){
    printf(1,
           "FAIL: fork in copyout test\n");
    exit();
  }


  if(pid == 0){
    int ref;

    close(data[1]);
    close(result[0]);


    /*
     * read() asks the kernel to write into s.
     *
     * Since s is currently COW-shared, copyout() must first
     * give this child a private frame.
     */
    if(read(data[0], s, 1) != 1)
      exit();

    ref = pageref(s);
    child_value = s[0];

    write_full(result[1], &ref, sizeof(ref));
    write_full(result[1], &child_value, 1);

    close(data[0]);
    close(result[1]);

    exit();
  }


  close(data[0]);
  close(result[1]);


  /*
   * Make the child's read() receive 'K'.
   */
  token = 'K';

  write_full(data[1], &token, 1);
  close(data[1]);


  {
    int child_ref;

    if(read_full(result[0],
                 &child_ref,
                 sizeof(child_ref)) < 0){
      printf(1,
             "FAIL: copyout child refcount\n");
      exit();
    }

    if(read_full(result[0],
                 &child_value,
                 1) < 0){
      printf(1,
             "FAIL: copyout child value\n");
      exit();
    }

    close(result[0]);

    wait();


    if(child_value != 'K'){
      printf(1,
             "FAIL: kernel copyout did not update child\n");
      exit();
    }

    if(child_ref != 1){
      printf(1,
             "FAIL: copyout child frame not private\n");
      exit();
    }

    if(s[0] != 'O'){
      printf(1,
             "FAIL: copyout modified parent's shared frame\n");
      exit();
    }

    if(pageref(s) != 1){
      printf(1,
             "FAIL: parent copyout frame refcount != 1\n");
      exit();
    }
  }

  printf(1,
         "PASS: copyout resolved COW without modifying parent\n");


  /*
   * ====================================================
   * CLEANUP
   * ====================================================
   */

  printf(1, "\nTEST 7 - Cleanup\n");


  /*
   * r is currently PROT_READ; munmap does not require
   * write permission.
   */
  if(munmap(p, 2 * PGSIZE_U) < 0){
    printf(1, "FAIL: munmap p\n");
    exit();
  }

  if(munmap(q, PGSIZE_U) < 0){
    printf(1, "FAIL: munmap q\n");
    exit();
  }

  if(munmap(r, PGSIZE_U) < 0){
    printf(1, "FAIL: munmap r\n");
    exit();
  }

  if(munmap(s, PGSIZE_U) < 0){
    printf(1, "FAIL: munmap s\n");
    exit();
  }


  if(pageref(p) != -1 ||
     pageref(q) != -1 ||
     pageref(r) != -1 ||
     pageref(s) != -1){
    printf(1,
           "FAIL: mapping remained after munmap\n");
    exit();
  }

  printf(1,
         "PASS: final mappings released\n");


  printf(1, "\n");
  printf(1, "============================================\n");
  printf(1, " ALL COPY-ON-WRITE TESTS PASSED\n");
  printf(1, "============================================\n");

  exit();
}
