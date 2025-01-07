#include "types.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

extern int sys_memstat(void);

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

extern struct dealloc_info pending_dealloc;
//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  if(tf->trapno == T_PGFLT) {
    uint va = rcr2();
    struct proc *curproc = myproc();
    
    if(va < curproc->sz && va >= PGSIZE) {
      pde_t *pde = &curproc->pgdir[PDX(va)];
      
      if(!(*pde & PTE_P)) {
        char *new_pt = kalloc();
        if(new_pt == 0) {
          cprintf("out of memory in page table allocation\n");
          curproc->killed = 1;
          return;
        }
        memset(new_pt, 0, PGSIZE);
        *pde = V2P(new_pt) | PTE_P | PTE_W | PTE_U;
      }
      
      pte_t *pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
      pte_t *pte = &pgtab[PTX(va)];
      
      if(!(*pte & PTE_P)) {
        char *mem = kalloc();
        if(mem == 0) {
          cprintf("out of memory\n");
          curproc->killed = 1;
          return;
        }
        memset(mem, 0, PGSIZE);
        *pte = V2P(mem) | PTE_P | PTE_W | PTE_U;
      }
      return;
    }
  }

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
    struct proc *curproc = myproc();
    if(pending_dealloc.active) {
      if(ticks - pending_dealloc.request_ticks >= pending_dealloc.ticks_delay) {
        // 메모리 해제 시작
        uint new_sz = curproc->sz - pending_dealloc.size;
        
        release(&tickslock);
        if(deallocuvm(curproc->pgdir, curproc->sz, new_sz) == 0) {
          cprintf("deallocation failed\n");
          acquire(&tickslock);
          return;
        }
        curproc->sz = new_sz;
        
        // 해제 완료 시간 출력
        struct rtcdate t;
        cmostime(&t);
        cprintf("Memory deallocation excute: %d-%d-%d %d:%d:%d\n",
                t.year, t.month, t.day, t.hour, t.minute, t.second);
        
        // 상태 확인
        sys_memstat();
        
        acquire(&tickslock);
        pending_dealloc.active = 0;
        pending_dealloc.size = 0;
        curproc->killed = 1;
      }
    }
    
    wakeup(&ticks);
    release(&tickslock);
  }
  lapiceoi();
  break;
    
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
     tf->trapno == T_IRQ0+IRQ_TIMER) {
    if(!pending_dealloc.active) {
      yield();
    }
  }

  // Check if the process has been killed since we yielded
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();
}
