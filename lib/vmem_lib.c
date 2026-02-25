#include "vmem_lib.h"
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/ioctl.h>

#define VMEM_DEV_PATH "/dev/vmem"
int vmem_open() {
    return open(VMEM_DEV_PATH, O_RDWR);
}
int vmem_close(int fd) {
    return close(fd);
}
int vmem_read(int fd, char *buf, int len) {
    return read(fd, buf, len);
}
int vmem_write(int fd, const char *buf, int len) {
    return write(fd, buf, len);
}
int vmem_open_handle(int fd, ze_ipc_mem_handle_t* handle) {
    // pass handle to kernel
    ioctl(fd, 0, handle);
    return 0;
}
int vmem_get_handle(int fd) {
    // demo: just return the fd as handle
    ioctl(fd, 1, 0);
    return fd;
}
