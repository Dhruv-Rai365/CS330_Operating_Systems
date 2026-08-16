#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"

int
sys_fork(void)
{
  return fork();
}

int
sys_exit(void)
{
  exit();
  return 0;  // not reached
}

int
sys_wait(void)
{
  return wait();
}

int
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int
sys_getpid(void)
{
  return myproc()->pid;
}

int
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

int
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

//this is the new api for the syscall_trace
int
sys_trace(void)
{
  int enabled;

  if(argint(0, &enabled) < 0)
    return -1;

  myproc()->trace_enabled = enabled ? 1 : 0;

  return 0;
}

/*
 * setprio(priority)
 *
 * Set the calling process's scheduler priority.
 */
int
sys_setprio(void)
{
  int priority;

  if(argint(0, &priority) < 0)
    return -1;

  return setprio(priority);
}


/*
 * getprio()
 *
 * Return the calling process's current priority.
 */
int
sys_getprio(void)
{
  return getprio();
}


/*
 * mmap(size)
 *
 * Reserve a page-aligned virtual-address range for this process.
 *
 * IMPORTANT:
 * mmap() does NOT allocate physical memory here.
 * Physical pages will be allocated later, on demand, when the
 * process accesses the mapping and generates a page fault.
 */
int
sys_mmap(void)
{
  int n;
  int i;
  uint length;
  uint start;
  struct proc *p;

  // Read mmap's size argument from user space.
  if(argint(0, &n) < 0 || n <= 0)
    return -1;

  p = myproc();

  // Every mapping occupies complete 4 KB pages.
  length = PGROUNDUP((uint)n);

  // Catch overflow / nonsensical mapping sizes.
  if(length == 0 || length >= KERNBASE)
    return -1;

  /*
   * Begin the mapping at the next page boundary after
   * the process's current address-space high-water mark.
   */
  start = PGROUNDUP(p->sz);

  // Mapping must remain completely below kernel virtual memory.
  if(start >= KERNBASE || length > KERNBASE - start)
    return -1;

  // Find an unused VMA descriptor.
  for(i = 0; i < MAX_VMAS; i++){
    if(!p->vmas[i].used)
      break;
  }

  // No VMA slots left.
  if(i == MAX_VMAS)
    return -1;

  /*
   * Record the virtual mapping.
   *
   * Notice that there is NO kalloc() here and NO mappages().
   * We are reserving virtual addresses only.
   */
  p->vmas[i].start = start;
  p->vmas[i].length = length;
  p->vmas[i].prot = PROT_READ | PROT_WRITE;
  p->vmas[i].used = 1;

  /*
   * Increase the process's virtual address-space high-water mark.
   * Again, this does not mean physical pages have been allocated.
   */
  p->sz = start + length;

  return (int)start;
}



/*
 * munmap(addr, size)
 *
 * For this simplified implementation, addr and size must
 * identify one complete mmap() region.
 */
int
sys_munmap(void)
{
  int addr_arg;
  int n;
  uint addr;
  uint length;
  struct proc *p;
  struct vma *v;

  if(argint(0, &addr_arg) < 0 ||
     argint(1, &n) < 0)
    return -1;

  if(n <= 0)
    return -1;

  addr = (uint)addr_arg;

  // mmap() always returns a page-aligned address.
  if(addr % PGSIZE)
    return -1;

  length = PGROUNDUP((uint)n);

  if(length == 0)
    return -1;

  p = myproc();

  v = vma_find_exact(p, addr, length);

  if(v == 0)
    return -1;

  /*
   * Free every physical page that has actually been faulted in.
   * Untouched lazy pages require no kfree().
   */
  if(vm_unmap_range(p->pgdir, v->start, v->length) < 0)
    return -1;

  // The virtual region itself is no longer valid.
  memset(v, 0, sizeof(*v));

  return 0;
}


/*
 * mprotect(addr, size, prot)
 *
 * Supported protections:
 *
 *     PROT_READ
 *     PROT_READ | PROT_WRITE
 */
int
sys_mprotect(void)
{
  int addr_arg;
  int n;
  int prot;
  int oldprot;
  uint addr;
  uint length;
  struct proc *p;
  struct vma *v;

  if(argint(0, &addr_arg) < 0 ||
     argint(1, &n) < 0 ||
     argint(2, &prot) < 0)
    return -1;

  if(n <= 0)
    return -1;

  if(prot != PROT_READ &&
     prot != (PROT_READ | PROT_WRITE))
    return -1;

  addr = (uint)addr_arg;

  if(addr % PGSIZE)
    return -1;

  length = PGROUNDUP((uint)n);

  if(length == 0)
    return -1;

  p = myproc();

  v = vma_find_exact(p, addr, length);

  if(v == 0)
    return -1;

  oldprot = v->prot;

  /*
   * Change the VMA first.  Lazy pages allocated after this syscall
   * will automatically receive the new protection.
   */
  v->prot = prot;

  /*
   * Also update PTE_W on pages that already exist physically.
   */
  if(vm_protect_range(p->pgdir,
                      v->start,
                      v->length,
                      prot) < 0){
    v->prot = oldprot;
    return -1;
  }

  return 0;
}

/*
 * numvp()
 *
 * Return the number of virtual pages currently owned
 * by this process, including lazy mmap pages.
 */
int
sys_numvp(void)
{
  return vm_numvp(myproc());
}


/*
 * numpp()
 *
 * Return the number of process pages that currently
 * have physical frames backing them.
 */
int
sys_numpp(void)
{
  return vm_numpp(myproc());
}


/*
 * getptsize()
 *
 * Return the number of 4 KB pages consumed by this
 * process's user page-table hierarchy.
 */
int
sys_getptsize(void)
{
  return vm_getptsize(myproc());
}

/*
 * pageref(va)
 *
 * Diagnostic syscall used to inspect the physical-frame
 * reference count for one user virtual address.
 */
int
sys_pageref(void)
{
  int addr;

  if(argint(0, &addr) < 0)
    return -1;

  if((uint)addr >= KERNBASE)
    return -1;

  return vm_pageref(myproc()->pgdir, (uint)addr);
}
