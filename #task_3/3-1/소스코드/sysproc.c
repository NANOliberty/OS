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


struct dealloc_info pending_dealloc = {0, 0, 0, 0, 0};

int
sys_ssusbrk(void)
{
  int n;
  int ticks_delay;
  struct rtcdate t;
  struct proc *curproc = myproc();
  
  if(argint(0, &n) < 0 || argint(1, &ticks_delay) < 0)
    return -1;
    
  if(n == 0 || (n > 0 && n % PGSIZE != 0))
    return -1;
  
  if(n > 0) {  // 메모리 할당
    uint old_sz = curproc->sz;
    uint new_sz = old_sz + n;
    
    if(new_sz >= KERNBASE)
      return -1;
      
    curproc->sz = new_sz;
    return old_sz;
    
  } else {  // 메모리 해제
    if(ticks_delay <= 0)
      return -1;
      
    n = -n;
    if(n % PGSIZE != 0 || n > curproc->sz)
      return -1;
      
    cmostime(&t);
    cprintf("Memory deallocation request(%d): %d-%d-%d %d:%d:%d\n",
            ticks_delay, t.year, t.month, t.day, t.hour, t.minute, t.second);
    
    acquire(&tickslock);
    if(pending_dealloc.active) {
      // 이전 요청과 새로운 요청 병합
      pending_dealloc.size += n;
      pending_dealloc.ticks_delay = ticks_delay;  // 새로운 딜레이로 갱신
      pending_dealloc.request_ticks = ticks;      // 현재 시점으로 갱신
    } else {
      pending_dealloc.addr = curproc->sz - n;
      pending_dealloc.size = n;
      pending_dealloc.ticks_delay = ticks_delay;
      pending_dealloc.request_ticks = ticks;
      pending_dealloc.active = 1;
    }
    release(&tickslock);
    
    return curproc->sz - n;
  }
}

int
sys_memstat(void)
{
  struct proc *curproc = myproc();
  pde_t *pgdir = curproc->pgdir;
  uint vp = 0, pp = 0;
  pte_t *pgtab;
  uint va;

  vp = PGROUNDUP(curproc->sz) / PGSIZE;
  cprintf("vp: %d, ", vp);

  for(va = 0; va < curproc->sz; va += PGSIZE) {
    pde_t *pde = &pgdir[PDX(va)];
    if(*pde & PTE_P) {
      pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
      pte_t *pte = &pgtab[PTX(va)];
      if(*pte & PTE_P) {
        pp++;
      }
    }
  }
  cprintf("pp: %d\n", pp);
  
  pde_t *pde = &pgdir[0];
  cprintf("PDE - 0x%x\n", *pde);
  
  cprintf("PTE - ");
  int first = 1;
  for(va = 0; va < curproc->sz; va += PGSIZE) {
    pde = &pgdir[PDX(va)];
    if(*pde & PTE_P) {
      pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
      pte_t *pte = &pgtab[PTX(va)];
      // Present이고 User 접근 가능한 페이지만 출력
      if((*pte & PTE_P) && (*pte & PTE_U)) {
        if(!first) 
          cprintf(" - ");
        cprintf("0x%x", *pte);
        first = 0;
      }
    }
  }
  cprintf("\n");
  
  return 0;
}