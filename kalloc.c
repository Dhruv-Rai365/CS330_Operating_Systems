// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file
                   // defined by the kernel linker script in kernel.ld

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  int use_lock;
  struct run *freelist;
} kmem;


/*
 * Reference count for every physical 4 KB frame below PHYSTOP.
 *
 * Most pages normally have:
 *
 *   0 -> free
 *   1 -> owned by exactly one kernel/user mapping
 *
 * COW pages may temporarily have counts greater than one.
 *
 * The same kmem.lock protects both the freelist and these counts.
 */
static int refcnt[PHYSTOP / PGSIZE];


/*
 * Convert a page-aligned physical address into its refcount index.
 */
static int
refindex(uint pa)
{
  if(pa >= PHYSTOP || pa % PGSIZE)
    panic("refindex");

  return pa / PGSIZE;
}


// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.
void
kinit1(void *vstart, void *vend)
{
  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 0;
  freerange(vstart, vend);
}

void
kinit2(void *vstart, void *vend)
{
  freerange(vstart, vend);
  kmem.use_lock = 1;
}


void
freerange(void *vstart, void *vend)
{
  char *p;

  p = (char*)PGROUNDUP((uint)vstart);

  for(; p + PGSIZE <= (char*)vend; p += PGSIZE){
    /*
     * These pages are being inserted into the allocator for
     * the first time during boot.
     *
     * Give them one temporary reference so that our normal
     * kfree() path can reduce the count to zero and place the
     * page on the freelist.
     */
    refcnt[V2P(p) / PGSIZE] = 1;
    kfree(p);
  }
}

/*
void
freerange(void *vstart, void *vend)
{
  char *p;
  p = (char*)PGROUNDUP((uint)vstart);
  for(; p + PGSIZE <= (char*)vend; p += PGSIZE)
    kfree(p);
}
*/

//PAGEBREAK: 21
// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)


/*
 * Release one reference to a physical page.
 *
 * With COW, kfree() no longer necessarily means:
 *
 *      "free this page immediately"
 *
 * Instead it means:
 *
 *      "this owner is done with the page"
 *
 * The page returns to the freelist only when the final
 * reference disappears.
 */
void
kfree(char *v)
{
  struct run *r;
  uint pa;
  int idx;

  if((uint)v % PGSIZE || v < end || V2P(v) >= PHYSTOP)
    panic("kfree");

  pa = V2P(v);
  idx = refindex(pa);

  if(kmem.use_lock)
    acquire(&kmem.lock);

  /*
   * A page being released must currently have at least
   * one owner.
   */
  if(refcnt[idx] <= 0){
    if(kmem.use_lock)
      release(&kmem.lock);

    panic("kfree ref");
  }

  refcnt[idx]--;

  /*
   * Somebody else still owns this physical frame.
   *
   * This is the common case when one process exits or
   * replaces a COW mapping while another process still
   * points at the original frame.
   */
  if(refcnt[idx] > 0){
    if(kmem.use_lock)
      release(&kmem.lock);

    return;
  }

  /*
   * refcount is now zero: nobody can legitimately access
   * this physical page anymore, so it can finally return
   * to xv6's freelist.
   */
  memset(v, 1, PGSIZE);

  r = (struct run*)v;
  r->next = kmem.freelist;
  kmem.freelist = r;

  if(kmem.use_lock)
    release(&kmem.lock);
}


/*
void
kfree(char *v)
{
  struct run *r;

  if((uint)v % PGSIZE || v < end || V2P(v) >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(v, 1, PGSIZE);

  if(kmem.use_lock)
    acquire(&kmem.lock);
  r = (struct run*)v;
  r->next = kmem.freelist;
  kmem.freelist = r;
  if(kmem.use_lock)
    release(&kmem.lock);
}
*/

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
/*
 * Allocate one physical 4 KB page.
 *
 * A newly allocated page begins with exactly one owner.
 */
char*
kalloc(void)
{
  struct run *r;
  int idx;

  if(kmem.use_lock)
    acquire(&kmem.lock);

  r = kmem.freelist;

  if(r){
    kmem.freelist = r->next;

    /*
     * The page was free, so its reference count should
     * have been zero.  It now belongs to one caller.
     */
    idx = refindex(V2P(r));
    refcnt[idx] = 1;
  }

  if(kmem.use_lock)
    release(&kmem.lock);

  return (char*)r;
}


/*
 * Add another owner of an already allocated physical frame.
 *
 * COW fork calls this after mapping the same physical frame
 * into the child's page table.
 */
void
kref_inc(uint pa)
{
  int idx;

  idx = refindex(pa);

  if(kmem.use_lock)
    acquire(&kmem.lock);

  if(refcnt[idx] <= 0){
    if(kmem.use_lock)
      release(&kmem.lock);

    panic("kref_inc");
  }

  refcnt[idx]++;

  if(kmem.use_lock)
    release(&kmem.lock);
}


/*
 * Return the current number of owners of a physical frame.
 */
int
kref_get(uint pa)
{
  int idx;
  int count;

  idx = refindex(pa);

  if(kmem.use_lock)
    acquire(&kmem.lock);

  count = refcnt[idx];

  if(kmem.use_lock)
    release(&kmem.lock);

  return count;
}
