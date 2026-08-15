#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  if(tf->trapno == T_SYSCALL){
    if(myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if(myproc()->killed)
      exit();
    return;
  }

  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    if(cpuid() == 0){
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_PGFLT:
  {
    uint faultva;
    uint pageva;
    struct proc *p;
    struct vma *v;

    /*
     * CR2 contains the virtual address that caused
     * the x86 page fault.
     */
    faultva = rcr2();
    pageva = PGROUNDDOWN(faultva);
    p = myproc();

    if(p == 0){
      cprintf("page fault without process at 0x%x\n", faultva);
      panic("page fault");
    }

    /*
     * The fault is valid only if the address was previously
     * reserved by mmap().
     */
    v = vma_find(p, faultva);

    if(v == 0){
      /*
       * Preserve xv6's normal behaviour for genuine kernel
       * faults.  Invalid user accesses simply kill the process.
       */
      if((tf->cs & 3) == 0){
        cprintf("kernel page fault at 0x%x\n", faultva);
        panic("page fault");
      }

      cprintf("pid %d: invalid memory access at 0x%x\n",
              p->pid, faultva);

      p->killed = 1;
      break;
    }

    /*
     * Error-code bit 0 == 1 means the page was already present.
     * Therefore this is a protection fault rather than a
     * demand-allocation fault.
     *
     * Later, Copy-on-Write will make use of this distinction.
     */
    if(tf->err & 0x1){
      cprintf("pid %d: protection fault at 0x%x\n",
              p->pid, faultva);

      p->killed = 1;
      break;
    }

    /*
     * Error-code bit 1 == 1 means the faulting access was a write.
     * A write must not create a page inside a read-only VMA.
     */
    if((tf->err & 0x2) && !(v->prot & PROT_WRITE)){
      cprintf("pid %d: write to read-only mapping at 0x%x\n",
              p->pid, faultva);

      p->killed = 1;
      break;
    }

    /*
     * Valid non-present mmap page:
     * allocate one zero-filled physical page and install its PTE.
     */
    if(vm_alloc_mmap_page(p->pgdir, pageva, v->prot) < 0){
      cprintf("pid %d: mmap page allocation failed\n", p->pid);
      p->killed = 1;
      break;
    }

    break;
  }
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;

  //PAGEBREAK: 13
  default:
    if(myproc() == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(myproc() && myproc()->state == RUNNING &&
     tf->trapno == T_IRQ0+IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();
}
