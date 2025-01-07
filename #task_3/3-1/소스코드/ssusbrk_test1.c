#include "types.h"
#include "stat.h"
#include "user.h"

void _error(const char *msg) {
    printf(1, "%s\nalloc_test failed...\n");
    exit();
}

int main() {
    int ret;
    char *addr;

    printf(1, "### Allocation test start\n");
    memstat();

    if (ssusbrk(0, 10) >= 0)
        _error("Parameter error");
    else
        printf(1, "ok\n");

    if (ssusbrk(2048, 10) >= 0)
        _error("Parameter error");
    else
        printf(1, "ok\n");

    if ((ret = ssusbrk(4096, -1)) < 0)
        _error("Allocation error");
    
    memstat();
    addr = (char *)ret;
    *addr = '*';
    memstat();
    printf(1, "ok\n");

    memstat();
    if ((ret = ssusbrk(8192, 0)) < 0)
        _error("Allocation error");

    memstat();
    addr = (char *)ret;
    addr[0] = 'S';
    memstat();
    addr[4096] = 'U';
    memstat();
    printf(1, "ok\n");

    printf(1, "### Allocation test passed...\n");

    exit();
}
