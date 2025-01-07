// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define PGSIZE 4096              // 4KB 페이지 크기
#define MAX_NODES 100            // 최대 노드 개수
#define RB_NODE_SIZE (sizeof(struct rb_node))

struct rb_tree rbtree;
struct counters counters = {0, 0, 0}; 

// counters 증가 함수들
void increment_bmap_count(void) {
   counters.bmap_count++;
}

void increment_cache_hit(void) {
   counters.cache_hit++;
}

void increment_disk_access(void) {
   counters.disk_access++;
}

// counters 초기화 함수
void reset_counters(void) {
    counters.bmap_count = 0;
    counters.cache_hit = 0;
    counters.disk_access = 0;
}

// counters 값 가져오는 함수
void get_counters(int *bmap, int *cache, int *disk) {
   *bmap = counters.bmap_count;
   *cache = counters.cache_hit;
   *disk = counters.disk_access;
}

#define min(a, b) ((a) < (b) ? (a) : (b))
static void itrunc(struct inode*);
// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb; 

// Read the super block.
void
readsb(int dev, struct superblock *sb)
{
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Zero a block.
static void
bzero(int dev, int bno)
{
  struct buf *bp;

  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

// Blocks.

// Allocate a zeroed disk block.
static uint
balloc(uint dev)
{
  int b, bi, m;
  struct buf *bp;

  bp = 0;
  for(b = 0; b < sb.size; b += BPB){
    bp = bread(dev, BBLOCK(b, sb));
    for(bi = 0; bi < BPB && b + bi < sb.size; bi++){
      m = 1 << (bi % 8);
      if((bp->data[bi/8] & m) == 0){  // Is block free?
        bp->data[bi/8] |= m;  // Mark block in use.
        log_write(bp);
        brelse(bp);
        bzero(dev, b + bi);
        return b + bi;
      }
    }
    brelse(bp);
  }
  panic("balloc: out of blocks");
}

// Free a disk block.
static void
bfree(int dev, uint b)
{
  struct buf *bp;
  int bi, m;

  bp = bread(dev, BBLOCK(b, sb));
  bi = b % BPB;
  m = 1 << (bi % 8);
  if((bp->data[bi/8] & m) == 0)
    panic("freeing free block");
  bp->data[bi/8] &= ~m;
  log_write(bp);
  brelse(bp);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at
// sb.startinode. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in cache: an entry in the inode cache
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a cache entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   cache entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays cached and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The icache.lock spin-lock protects the allocation of icache
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold icache.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} icache;

void
iinit(int dev)
{
  int i = 0;
  
  initlock(&icache.lock, "icache");
  for(i = 0; i < NINODE; i++) {
    initsleeplock(&icache.inode[i].lock, "inode");
  }

  readsb(dev, &sb);
  cprintf("sb: size %d nblocks %d ninodes %d nlog %d logstart %d\
 inodestart %d bmap start %d\n", sb.size, sb.nblocks,
          sb.ninodes, sb.nlog, sb.logstart, sb.inodestart,
          sb.bmapstart);
}

static struct inode* iget(uint dev, uint inum);

//PAGEBREAK!
// Allocate an inode on device dev.
// Mark it as allocated by  giving it type type.
// Returns an unlocked but allocated and referenced inode.
struct inode*
ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;

  for(inum = 1; inum < sb.ninodes; inum++){
    bp = bread(dev, IBLOCK(inum, sb));
    dip = (struct dinode*)bp->data + inum%IPB;
    if(dip->type == 0){  // a free inode
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      log_write(bp);   // mark it allocated on the disk
      brelse(bp);
      return iget(dev, inum);
    }
    brelse(bp);
  }
  panic("ialloc: no inodes");
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk, since i-node cache is write-through.
// Caller must hold ip->lock.
void
iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  dip = (struct dinode*)bp->data + ip->inum%IPB;
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  log_write(bp);
  brelse(bp);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
static struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip, *empty;

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for(ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if(empty == 0 && ip->ref == 0)    // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode cache entry.
  if(empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  release(&icache.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode*
idup(struct inode *ip)
{
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void
ilock(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock);

  if(ip->valid == 0){
    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode*)bp->data + ip->inum%IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);
    ip->valid = 1;
    if(ip->type == 0)
      panic("ilock: no type");
  }
}

// Unlock the given inode.
void
iunlock(struct inode *ip)
{
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");

  releasesleep(&ip->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
void
iput(struct inode *ip)
{
  acquiresleep(&ip->lock);
  if(ip->valid && ip->nlink == 0){
    acquire(&icache.lock);
    int r = ip->ref;
    release(&icache.lock);
    if(r == 1){
      // inode has no links and no other references: truncate and free.
      itrunc(ip);
      ip->type = 0;
      iupdate(ip);
      ip->valid = 0;
    }
  }
  releasesleep(&ip->lock);

  acquire(&icache.lock);
  ip->ref--;
  release(&icache.lock);
}

// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

//PAGEBREAK!
// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a, *b, *c;
  struct buf *bp, *bp2, *bp3;
  
  increment_bmap_count(); 

  struct rb_node *cached = rb_search(bn);
  if(cached) {
    increment_cache_hit();
    return cached->value;
  }

  increment_disk_access();
  
  // 직접 블록 처리
  if(bn < NDIRECT){  // 직접 블록 (Direct blocks)
    if((addr = ip->addrs[bn]) == 0)  // 해당 블록이 아직 할당되지 않았으면
      ip->addrs[bn] = addr = balloc(ip->dev);  // 블록을 할당  
    return addr;  // 블록 주소 반환
  }
  bn -= NDIRECT;

  // 단일 간접 블록 처리
  if(bn < NINDIRECT * 4){  // 단일 간접 블록 (Indirect blocks)
    uint indirect_index = NDIRECT + (bn / NINDIRECT);  // 간접 블록 테이블의 인덱스 계산
    bn = bn % NINDIRECT;  // 해당 블록의 오프셋 계산
    
    if((addr = ip->addrs[indirect_index]) == 0)  // 간접 블록 테이블이 할당되지 않았으면
      ip->addrs[indirect_index] = addr = balloc(ip->dev);  // 테이블 블록 할당
    
    bp = bread(ip->dev, addr);  // 블록을 읽어옴
    a = (uint*)bp->data;  // 블록 데이터를 배열로 취급
    if((addr = a[bn]) == 0){  // 오프셋 위치의 블록이 아직 할당되지 않았으면
      a[bn] = addr = balloc(ip->dev);  // 새로운 블록 할당
      log_write(bp);  // 로그에 기록
    }
    brelse(bp);  // 버퍼 해제
    return addr;  // 블록 주소 반환
  }
  bn -= NINDIRECT * 4;

  // 이중 간접 블록 처리
  if(bn < NDINDIRECT * 2){  // 이중 간접 블록 (Double indirect blocks)
    uint dindex = 10 + (bn / NDINDIRECT);  // 이중 간접 블록 테이블 인덱스 계산
    bn = bn % NDINDIRECT;  // 오프셋 계산
    
    if((addr = ip->addrs[dindex]) == 0)  // 이중 간접 블록 테이블이 할당되지 않았으면
      ip->addrs[dindex] = addr = balloc(ip->dev);  // 테이블 블록 할당
    
    bp = bread(ip->dev, addr);  // 테이블 블록 읽어옴
    a = (uint*)bp->data;  // 데이터를 배열로 취급
    uint index1 = bn / NINDIRECT;  // 첫 번째 간접 블록 인덱스 계산
    uint index2 = bn % NINDIRECT;  // 두 번째 간접 블록 인덱스 계산
    
    if((addr = a[index1]) == 0){  // 첫 번째 간접 블록이 아직 할당되지 않았으면
      a[index1] = addr = balloc(ip->dev);  // 할당
      log_write(bp);  // 로그에 기록
    }
    bp2 = bread(ip->dev, addr);  // 첫 번째 간접 블록 읽어옴
    b = (uint*)bp2->data;  // 데이터를 배열로 취급
    
    if((addr = b[index2]) == 0){  // 두 번째 간접 블록이 아직 할당되지 않았으면
      b[index2] = addr = balloc(ip->dev);  // 할당
      log_write(bp2);  // 로그에 기록
    }
    
    brelse(bp2);  // 버퍼 해제
    brelse(bp);  // 버퍼 해제
    return addr;  // 블록 주소 반환
  }
  bn -= NDINDIRECT * 2;

  // 삼중 간접 블록 처리
  if(bn < NTINDIRECT){  // 삼중 간접 블록 (Triple indirect block)
    if((addr = ip->addrs[12]) == 0)  // 삼중 간접 블록 테이블이 아직 할당되지 않았으면
      ip->addrs[12] = addr = balloc(ip->dev);  // 테이블 블록 할당
    
    bp = bread(ip->dev, addr);  // 삼중 간접 블록 테이블 읽어옴
    a = (uint*)bp->data;  // 데이터를 배열로 취급
    uint index1 = bn / (NINDIRECT * NINDIRECT);  // 첫 번째 인덱스 계산
    uint index2 = (bn % (NINDIRECT * NINDIRECT)) / NINDIRECT;  // 두 번째 인덱스 계산
    uint index3 = bn % NINDIRECT;  // 세 번째 인덱스 계산
    
    if((addr = a[index1]) == 0){  // 첫 번째 블록이 할당되지 않았으면
      a[index1] = addr = balloc(ip->dev);  // 할당
      log_write(bp);  // 로그에 기록
    }
    bp2 = bread(ip->dev, addr);  // 첫 번째 블록 읽어옴
    b = (uint*)bp2->data;  // 데이터를 배열로 취급
    
    if((addr = b[index2]) == 0){  // 두 번째 블록이 할당되지 않았으면
      b[index2] = addr = balloc(ip->dev);  // 할당
      log_write(bp2);  // 로그에 기록
    }
    bp3 = bread(ip->dev, addr);  // 두 번째 블록 읽어옴
    c = (uint*)bp3->data;  // 데이터를 배열로 취급
    
    if((addr = c[index3]) == 0){  // 세 번째 블록이 할당되지 않았으면
      c[index3] = addr = balloc(ip->dev);  // 할당
      log_write(bp3);  // 로그에 기록
    }
    
    brelse(bp3);  // 버퍼 해제
    brelse(bp2);  // 버퍼 해제
    brelse(bp);  // 버퍼 해제
    return addr;  // 블록 주소 반환
  }

  panic("bmap: out of range");  // 범위를 초과한 경우 오류 발생
}

// Truncate inode (discard contents).
// Only called when the inode has no links
// to it (no directory entries referring to it)
// and has no in-memory reference to it (is
// not an open file or current directory).
static void
itrunc(struct inode *ip)
{
  int i, j, k;
  struct buf *bp, *bp2, *bp3;
  uint *a, *b, *c;

  // Direct blocks
  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  // Indirect blocks (4)
  for(i = 0; i < 4; i++){
    if(ip->addrs[NDIRECT + i]){
      bp = bread(ip->dev, ip->addrs[NDIRECT + i]);
      a = (uint*)bp->data;
      for(j = 0; j < NINDIRECT; j++){
        if(a[j])
          bfree(ip->dev, a[j]);
      }
      brelse(bp);
      bfree(ip->dev, ip->addrs[NDIRECT + i]);
      ip->addrs[NDIRECT + i] = 0;
    }
  }

  // Double indirect blocks (2)
  for(i = 0; i < 2; i++){
    if(ip->addrs[10 + i]){
      bp = bread(ip->dev, ip->addrs[10 + i]);
      a = (uint*)bp->data;
      for(j = 0; j < NINDIRECT; j++){
        if(a[j]){
          bp2 = bread(ip->dev, a[j]);
          b = (uint*)bp2->data;
          for(k = 0; k < NINDIRECT; k++){
            if(b[k])
              bfree(ip->dev, b[k]);
          }
          brelse(bp2);
          bfree(ip->dev, a[j]);
        }
      }
      brelse(bp);
      bfree(ip->dev, ip->addrs[10 + i]);
      ip->addrs[10 + i] = 0;
    }
  }

  // Triple indirect block
  if(ip->addrs[12]){
    bp = bread(ip->dev, ip->addrs[12]);
    a = (uint*)bp->data;
    for(i = 0; i < NINDIRECT; i++){
      if(a[i]){
        bp2 = bread(ip->dev, a[i]);
        b = (uint*)bp2->data;
        for(j = 0; j < NINDIRECT; j++){
          if(b[j]){
            bp3 = bread(ip->dev, b[j]);
            c = (uint*)bp3->data;
            for(k = 0; k < NINDIRECT; k++){
              if(c[k])
                bfree(ip->dev, c[k]);
            }
            brelse(bp3);
            bfree(ip->dev, b[j]);
          }
        }
        brelse(bp2);
        bfree(ip->dev, a[i]);
      }
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[12]);
    ip->addrs[12] = 0;
  }

  ip->size = 0;
  iupdate(ip);
}

// Copy stat information from inode.
// Caller must hold ip->lock.
void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

//PAGEBREAK!
// Read data from inode.
// Caller must hold ip->lock.
int
readi(struct inode *ip, char *dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].read)
      return -1;
    return devsw[ip->major].read(ip, dst, n);
  }

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > ip->size)
    n = ip->size - off;

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    bp = bread(ip->dev, bmap(ip, off/BSIZE));
    m = min(n - tot, BSIZE - off%BSIZE);
    memmove(dst, bp->data + off%BSIZE, m);
    brelse(bp);
  }
  return n;
}

// PAGEBREAK!
// Write data to inode.
// Caller must hold ip->lock.
int
writei(struct inode *ip, char *src, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].write)
      return -1;
    return devsw[ip->major].write(ip, src, n);
  }

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > MAXFILE*BSIZE)
    return -1;

  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
    bp = bread(ip->dev, bmap(ip, off/BSIZE));
    m = min(n - tot, BSIZE - off%BSIZE);
    memmove(bp->data + off%BSIZE, src, m);
    log_write(bp);
    brelse(bp);
  }

  if(n > 0 && off > ip->size){
    ip->size = off;
    iupdate(ip);
  }
  return n;
}

//PAGEBREAK!
// Directories

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    if(de.inum == 0)
      continue;
    if(namecmp(name, de.name) == 0){
      // entry matches path element
      if(poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
int
dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("dirlink");

  return 0;
}

//PAGEBREAK!
// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(myproc()->cwd);

  while((path = skipelem(path, name)) != 0){
    ilock(ip);
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    if(nameiparent && *path == '\0'){
      // Stop one level early.
      iunlock(ip);
      return ip;
    }
    if((next = dirlookup(ip, name, 0)) == 0){
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
  }
  if(nameiparent){
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode*
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}

// 레드블랙트리 초기화 함수
void rbtree_init(void)
{
   // 4KB 메모리 할당
   rbtree.mem_pool = kalloc();
   if(rbtree.mem_pool == 0)
       panic("rbtree_init: kalloc failed");
   
   // 초기화
   rbtree.root = 0;
   rbtree.node_count = 0;
   
   // 메모리 풀 초기화
   memset(rbtree.mem_pool, 0, PGSIZE);
}

// 새 노드 할당 함수
struct rb_node* rb_node_alloc(void)
{
   if(rbtree.node_count >= MAX_NODES)
       return 0;
   
   // 메모리 풀에서 새 노드 공간 계산
   struct rb_node* new_node = (struct rb_node*)(rbtree.mem_pool + 
                             (rbtree.node_count * RB_NODE_SIZE));
   rbtree.node_count++;
   
   // 노드 초기화
   new_node->left = 0;
   new_node->right = 0;
   new_node->parent = 0;
   new_node->color = RB_RED;    // 새 노드는 항상 RED로 시작
   
   return new_node;
}

// 좌회전
void rb_left_rotate(struct rb_node *x)
{
    struct rb_node *y = x->right;
    
    // y의 왼쪽 서브트리를 x의 오른쪽 서브트리로
    x->right = y->left;
    if(y->left)
        y->left->parent = x;
    
    // x의 부모를 y의 부모로
    y->parent = x->parent;
    
    if(!x->parent)
        rbtree.root = y;
    else if(x == x->parent->left)
        x->parent->left = y;
    else
        x->parent->right = y;
    
    // x를 y의 왼쪽 자식으로
    y->left = x;
    x->parent = y;
}

// 우회전 (좌회전의 대칭)
void rb_right_rotate(struct rb_node *y)
{
    struct rb_node *x = y->left;
    
    y->left = x->right;
    if(x->right)
        x->right->parent = y;
    
    x->parent = y->parent;
    
    if(!y->parent)
        rbtree.root = x;
    else if(y == y->parent->right)
        y->parent->right = x;
    else
        y->parent->left = x;
    
    x->right = y;
    y->parent = x;
}

// 삽입 후 레드블랙트리 속성 유지를 위한 조정
void rb_insert_fixup(struct rb_node *z)
{
    while(z->parent && z->parent->color == RB_RED) {
        if(z->parent == z->parent->parent->left) {
            struct rb_node *y = z->parent->parent->right;
            
            // Case 1: 삼촌이 RED
            if(y && y->color == RB_RED) {
                z->parent->color = RB_BLACK;
                y->color = RB_BLACK;
                z->parent->parent->color = RB_RED;
                z = z->parent->parent;
            } else {
                // Case 2: 삼촌이 BLACK & z가 오른쪽 자식
                if(z == z->parent->right) {
                    z = z->parent;
                    rb_left_rotate(z);
                }
                // Case 3: 삼촌이 BLACK & z가 왼쪽 자식
                z->parent->color = RB_BLACK;
                z->parent->parent->color = RB_RED;
                rb_right_rotate(z->parent->parent);
            }
        } else {  // 대칭적인 경우
            struct rb_node *y = z->parent->parent->left;
            
            if(y && y->color == RB_RED) {
                z->parent->color = RB_BLACK;
                y->color = RB_BLACK;
                z->parent->parent->color = RB_RED;
                z = z->parent->parent;
            } else {
                if(z == z->parent->left) {
                    z = z->parent;
                    rb_right_rotate(z);
                }
                z->parent->color = RB_BLACK;
                z->parent->parent->color = RB_RED;
                rb_left_rotate(z->parent->parent);
            }
        }
    }
    rbtree.root->color = RB_BLACK;
}

// 새 노드 삽입
struct rb_node* rb_insert(uint key, uint value)
{
    struct rb_node *y = 0;
    struct rb_node *x = rbtree.root;
    struct rb_node *z = rb_node_alloc();
    
    if(!z) return 0;  // 노드 할당 실패
    
    z->key = key;
    z->value = value;
    
    // 이진 탐색 트리처럼 삽입 위치 찾기
    while(x) {
        y = x;
        if(z->key < x->key)
            x = x->left;
        else if(z->key > x->key)
            x = x->right;
        else {  // 키가 이미 존재하면 값만 업데이트
            x->value = value;
            return x;
        }
    }
    
    z->parent = y;
    if(!y)
        rbtree.root = z;
    else if(z->key < y->key)
        y->left = z;
    else
        y->right = z;
    
    rb_insert_fixup(z);
    return z;
}

// 노드 검색 함수
struct rb_node* rb_search(uint key)
{
   struct rb_node *x = rbtree.root;
   while(x && x->key != key) {
       if(key < x->key)
           x = x->left;
       else
           x = x->right;
   }
   return x;
}

// 트리의 최소값(가장 왼쪽) 노드 찾기
struct rb_node* rb_minimum(struct rb_node *x)
{
   while(x->left)
       x = x->left;
   return x;
}

// 트리의 최대값(가장 오른쪽) 노드 찾기
struct rb_node* rb_maximum(struct rb_node *x)
{
   while(x->right)
       x = x->right;
   return x;
}

// 주어진 노드의 successor 찾기 (중위 순회에서 다음 노드)
struct rb_node* rb_successor(struct rb_node *x)
{
   if(x->right)
       return rb_minimum(x->right);
   
   struct rb_node *y = x->parent;
   while(y && x == y->right) {
       x = y;
       y = y->parent;
   }
   return y;
}

// 가장 오래된 노드 삭제를 위한 노드 삭제 함수
void rb_delete(struct rb_node *z)
{
   struct rb_node *y = z;
   struct rb_node *x;
   int y_original_color = y->color;

   if(!z->left) {
       x = z->right;
       rb_transplant(z, z->right);
   }
   else if(!z->right) {
       x = z->left;
       rb_transplant(z, z->left);
   }
   else {
       y = rb_minimum(z->right);
       y_original_color = y->color;
       x = y->right;
       
       if(y->parent == z && x)
           x->parent = y;
       else {
           rb_transplant(y, y->right);
           y->right = z->right;
           if(y->right)
               y->right->parent = y;
       }
       
       rb_transplant(z, y);
       y->left = z->left;
       y->left->parent = y;
       y->color = z->color;
   }

   if(y_original_color == RB_BLACK && x)
       rb_delete_fixup(x);
       
   rbtree.node_count--;
}

// 노드 교체 helper 함수
void rb_transplant(struct rb_node *u, struct rb_node *v)
{
   if(!u->parent)
       rbtree.root = v;
   else if(u == u->parent->left)
       u->parent->left = v;
   else
       u->parent->right = v;
   
   if(v)
       v->parent = u->parent;
}

// 삭제 후 레드블랙트리 속성 유지를 위한 조정
void rb_delete_fixup(struct rb_node *x)
{
   while(x != rbtree.root && x->color == RB_BLACK) {
       if(x == x->parent->left) {
           struct rb_node *w = x->parent->right;
           
           if(w->color == RB_RED) {
               w->color = RB_BLACK;
               x->parent->color = RB_RED;
               rb_left_rotate(x->parent);
               w = x->parent->right;
           }
           
           if((!w->left || w->left->color == RB_BLACK) &&
              (!w->right || w->right->color == RB_BLACK)) {
               w->color = RB_RED;
               x = x->parent;
           }
           else {
               if(!w->right || w->right->color == RB_BLACK) {
                   if(w->left)
                       w->left->color = RB_BLACK;
                   w->color = RB_RED;
                   rb_right_rotate(w);
                   w = x->parent->right;
               }
               
               w->color = x->parent->color;
               x->parent->color = RB_BLACK;
               if(w->right)
                   w->right->color = RB_BLACK;
               rb_left_rotate(x->parent);
               x = rbtree.root;
           }
       }
       else {  // 대칭적인 경우
           // ... 위와 동일한 로직을 왼쪽/오른쪽만 바꿔서 구현
       }
   }
   x->color = RB_BLACK;
}

void print_rb_tree_inorder(struct rb_node *node, int depth, int parent_key)
{
    if(node == 0)
        return;

    // 현재 노드 출력
    cprintf("key : %d, value : %d, depth : %d, color : %c, parent key : %d\n",
            node->key,
            node->value,
            depth,
            node->color == RB_RED ? 'R' : 'B',
            parent_key);

    // 왼쪽 서브트리 출력
    print_rb_tree_inorder(node->left, depth + 1, node->key);

    // 오른쪽 서브트리 출력
    print_rb_tree_inorder(node->right, depth + 1, node->key);
}