#include "../lib/vmem_lib.h"
#include <stdio.h>
#include <string.h>
int main() {
    int fd = vmem_open();
    if (fd < 0) {
        printf("Failed to open device\n");
        return 1;
    }
    char wbuf[] = "Hello Kernel!";
    char rbuf[64] = {0};
    vmem_write(fd, wbuf, strlen(wbuf));
    vmem_read(fd, rbuf, sizeof(rbuf));
    printf("Read from kernel: %s\n", rbuf);
    vmem_export_handle(fd);
    vmem_import_handle(fd);
    vmem_close(fd);
    return 0;
}
