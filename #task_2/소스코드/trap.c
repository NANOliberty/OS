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

#define DEBUG 1  // 디버그 모드 활성화

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

// 모든 러너블 프로세스의 cpu_wait를 증가시키는 함수
void increase_cpu_wait() {
    struct proc *runnable_procs[NPROC];
    int num_runnable = 0;

    // 모든 러너블 프로세스를 가져오기
    get_runnable_procs(runnable_procs, &num_runnable);

    // 각 러너블 프로세스의 cpu_wait 증가
    for (int i = 0; i < num_runnable; i++) {
        runnable_procs[i]->cpu_wait++;

        // SLEEPING 상태인 경우 io_wait 증가
        if (runnable_procs[i]->state == SLEEPING) {
            runnable_procs[i]->io_wait_time++;
        }
        // cpu_wait가 250 이상
        if (runnable_procs[i]->cpu_wait >= 250) {
            if (runnable_procs[i]->q_level > 0) {
                runnable_procs[i]->q_level--;
                #ifdef DEBUG
                cprintf("PID: %d Aging\n",
                        runnable_procs[i]->pid);
                #endif
            }
            runnable_procs[i]->cpu_wait = 0;  // 초기화
            for_aging = 1;
        }
    }
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

int tq[4] = {10, 20, 40, 80};

  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    if(cpuid() == 0){
      acquire(&tickslock);
      ticks++;
      increase_cpu_wait();
      
      struct proc* cur = myproc();
      if(cur) {
        cur->cpu_wait = 0;
        cur->cpu_burst++;
        cur->cpu_usage++;
        cur->is_q_timeout = 0;
        if(cur->pid >= 4 && cur->end_time != -1 && cur->cpu_burst / tq[cur->q_level] >= 1) {
          #ifdef DEBUG
          cprintf("PID: %d uses %d ticks in mlfq[%d], total(%d/%d)\n", cur->pid, cur->cpu_burst, cur->q_level, cur->cpu_usage, cur->end_time);
          #endif
          cur->cpu_burst = 0;
          cur->is_q_timeout = 1;
          if(cur->q_level < 3)
            cur->q_level++;
        }

        if(cur->pid >= 4 && cur->end_time != -1 && cur->cpu_usage >= cur->end_time) {
          #ifdef DEBUG
          cprintf("PID: %d uses %d ticks in mlfq[%d], total(%d/%d)\n", cur->pid, cur->cpu_burst, cur->q_level, cur->cpu_usage, cur->end_time);
          #endif
          cur->aging_time = 1;
          kill(cur->pid);
          #ifdef DEBUG
          cprintf("PID: %d, used %d ticks. terminated\n", cur->pid, cur->end_time);
          #endif
        }
      }

      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_PGFLT:  // 페이지 폴트 예외 처리
      cprintf("Page fault at address 0x%x, eip %x\n", rcr2(), tf->eip);
      panic("Page fault");
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
  // 큐에 할당된 시간을 다 썼으면 yield
  if(myproc() && myproc()->state == RUNNING &&
     tf->trapno == T_IRQ0+IRQ_TIMER && myproc()->is_q_timeout == 1) 
    yield();

  // Check if the process has been killed since we yielded
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();
}
