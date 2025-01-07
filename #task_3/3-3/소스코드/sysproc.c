#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"  // sleeplock을 위해 필요
#include "sleeplock.h" // sleeplock을 위해 필요
#include "fs.h"
#include "file.h"
#include "fcntl.h"     // O_RDONLY 등의 플래그를 위해

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

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

int
sys_rb_count(void)
{
  struct file *f;
  if(argfd(0, 0, &f) < 0)
    return -1;
  
  if(f->off == 0) {  // 파일 위치가 시작점일 때는 초기화
      counters.bmap_count = 0;
      counters.cache_hit = 0;
      counters.disk_access = 0;
  }  
    
  int bmap_cnt, cache_cnt, disk_cnt;
  get_counters(&bmap_cnt, &cache_cnt, &disk_cnt);
  
  cprintf("bmap access count : %d, cache hit count : %d, disk access count : %d\n",
          bmap_cnt, cache_cnt, disk_cnt);
  
  return 0;
}

int
sys_rb_print(void)
{
    struct file *f;
    if(argfd(0, 0, &f) < 0)
        return -1;

    if(rbtree.root)
        print_rb_tree_inorder(rbtree.root, 1, -1);
        
    return 0;
}

int
sys_lseek(void)
{
  struct file *f;
  int n, whence;
  uint off;
  
  if(argfd(0, 0, &f) < 0 || argint(1, &n) || argint(2, &whence))
    return -1;

  switch(whence) {
    case SEEK_SET:
      off = 0;
      break;

    case SEEK_CUR:
      off = f->off;
      break;

    case SEEK_END:
      off = f->ip->size;
      break;

    default:
      return -1;
      break;
  }
  
  off += n;

  // 범위 체크
  if(off < 0 || off > f->ip->size)
    return -1;

  f->off = off;
  return off;
}
