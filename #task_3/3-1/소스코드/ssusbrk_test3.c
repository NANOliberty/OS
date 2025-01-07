#include "types.h"
#include "stat.h"
#include "user.h"

void _error(const char *msg) {
    printf(1, "%s\ndealloc_test failed...\n");
    exit();
}

int main() {
    int ret;
    char *addr;

    printf(1, "### Deallocation test2 start\n");

    if ((ret = ssusbrk(8192, 0)) < 0)
        _error("Allocation error");

    addr = (char *)ret;
    addr[0] = 'S';
    addr[4096] = 'S';
    memstat();

    if ((ret = ssusbrk(-4096, 1000)) < 0)
        _error("Deallocation error");

    if ((ret = ssusbrk(-4096, 500)) < 0)
        _error("Deallocation error");
    
    while (1) ;

    exit();
}
