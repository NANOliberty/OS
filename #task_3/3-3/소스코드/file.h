struct file {
  enum { FD_NONE, FD_PIPE, FD_INODE } type;
  int ref; // reference count
  char readable;
  char writable;
  struct pipe *pipe;
  struct inode *ip;
  uint off;
};

#define RB_RED   0
#define RB_BLACK 1

struct rb_node {
    uint key;           // 블록 번호
    uint value;         // 블록 주소
    uint color;         // 노드 색상 (RED 또는 BLACK)
    struct rb_node *parent;
    struct rb_node *left;
    struct rb_node *right;
};

struct rb_tree {
    struct rb_node *root;
    int node_count;     // 현재 노드 개수
    void *mem_pool;     // 4KB 메모리 풀의 시작 주소
};

extern struct rb_tree rbtree;

// in-memory copy of an inode
struct inode {
  uint dev;           
  uint inum;          
  int ref;            
  struct sleeplock lock;
  int valid;          

  short type;         
  short major;
  short minor;
  short nlink;
  uint size;
  uint addrs[13];    // NDIRECT+1에서 13으로 변경
  struct rb_tree *rbtree;
};

// table mapping major device number to
// device functions
struct devsw {
  int (*read)(struct inode*, char*, int);
  int (*write)(struct inode*, char*, int);
};

extern struct devsw devsw[];

#define CONSOLE 1
