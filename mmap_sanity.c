#include "types.h"
#include "stat.h"
#include "user.h"
#include "mmap.h"

#define PAGE_SIZE 4096

int
main(void)
{
  char *a;
  char *b;
  int base_vp;
  int base_pp;
  int pid;
  int fd[2];
  char child_value;

  printf(1, "\n=== mmap sanity test ===\n");

  base_vp = numvp();
  base_pp = numpp();

  /*
   * Create two independent mappings.
   */
  a = mmap(PAGE_SIZE);
  b = mmap(2 * PAGE_SIZE);

  if(a == (char*)-1 || b == (char*)-1){
    printf(1, "FAIL: mmap\n");
    exit();
  }

  if(a == b){
    printf(1, "FAIL: mappings overlap\n");
    exit();
  }

  if(((uint)a % PAGE_SIZE) != 0 ||
     ((uint)b % PAGE_SIZE) != 0){
    printf(1, "FAIL: mmap address not page aligned\n");
    exit();
  }

  if(numvp() != base_vp + 3){
    printf(1, "FAIL: virtual page accounting\n");
    exit();
  }

  if(numpp() != base_pp){
    printf(1, "FAIL: mmap eagerly allocated pages\n");
    exit();
  }

  printf(1, "PASS: independent lazy VMAs created\n");

  /*
   * Allocate one page from each mapping.
   */
  a[0] = 'A';
  b[PAGE_SIZE] = 'B';

  if(numpp() != base_pp + 2){
    printf(1, "FAIL: resident page accounting\n");
    exit();
  }

  printf(1, "PASS: pages allocated independently\n");

  /*
   * Partial operations are deliberately unsupported.
   * They should fail rather than silently doing something wrong.
   */
  if(mprotect(b + PAGE_SIZE,
              PAGE_SIZE,
              PROT_READ) != -1){
    printf(1, "FAIL: partial mprotect unexpectedly succeeded\n");
    exit();
  }

  if(munmap(b + PAGE_SIZE,
             PAGE_SIZE) != -1){
    printf(1, "FAIL: partial munmap unexpectedly succeeded\n");
    exit();
  }

  printf(1, "PASS: unsupported partial operations rejected\n");

  /*
   * Check CURRENT fork semantics:
   *
   * Resident mmap pages are copied normally by xv6.
   * Parent and child start with identical contents but do
   * NOT share the same writable physical page.
   */
  if(pipe(fd) < 0){
    printf(1, "FAIL: pipe\n");
    exit();
  }

  a[0] = 'P';

  pid = fork();

  if(pid < 0){
    printf(1, "FAIL: fork\n");
    exit();
  }

  if(pid == 0){
    close(fd[0]);

    if(a[0] != 'P'){
      printf(1, "CHILD FAIL: inherited contents incorrect\n");
      exit();
    }

    a[0] = 'C';

    write(fd[1], &a[0], 1);

    close(fd[1]);
    exit();
  }

  close(fd[1]);

  if(read(fd[0], &child_value, 1) != 1){
    printf(1, "FAIL: child result\n");
    exit();
  }

  close(fd[0]);
  wait();

  if(child_value != 'C'){
    printf(1, "FAIL: child write failed\n");
    exit();
  }

  if(a[0] != 'P'){
    printf(1, "FAIL: child modified parent mapping\n");
    exit();
  }

  printf(1, "PASS: fork produced private mmap page copies\n");

  /*
   * Remove A only. B must remain perfectly usable.
   */
  if(munmap(a, PAGE_SIZE) < 0){
    printf(1, "FAIL: munmap A\n");
    exit();
  }

  if(numvp() != base_vp + 2){
    printf(1, "FAIL: unmapping A damaged VMA accounting\n");
    exit();
  }

  if(b[PAGE_SIZE] != 'B'){
    printf(1, "FAIL: unmapping A damaged mapping B\n");
    exit();
  }

  printf(1, "PASS: one VMA can be removed independently\n");

  /*
   * Remove B.
   */
  if(munmap(b, 2 * PAGE_SIZE) < 0){
    printf(1, "FAIL: munmap B\n");
    exit();
  }

  if(numvp() != base_vp){
    printf(1, "FAIL: virtual pages leaked\n");
    exit();
  }

  if(numpp() != base_pp){
    printf(1, "FAIL: physical pages leaked\n");
    exit();
  }

  printf(1, "PASS: all mappings reclaimed\n");

  printf(1, "\n=== MMAP SANITY TEST PASSED ===\n");
  exit();
}
