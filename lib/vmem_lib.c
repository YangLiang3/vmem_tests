#include "vmem_lib.h"
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include "vmem_ioctl.h"

#define VMEM_DEV_PATH "/dev/vmem"

uint32_t domain, bus, device, function;

static int vmem_info_log_enabled(void) {
    const char *env = getenv("VMEM_LOG_INFO");
    if (env == NULL || env[0] == '\0' || strcmp(env, "0") == 0) {
        return 0;
    }
    return 1;
}

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

int vmem_init(uint32_t addr_domain,
              uint32_t addr_bus, 
              uint32_t addr_device, 
              uint32_t addr_function) {
    domain = addr_domain;
    bus = addr_bus;
    device = addr_device;
    function = addr_function;
    return 0;
}

int vmem_open_handle(int fd, ze_ipc_mem_handle_t* handle, struct pfn_list *pfn_list) {
    if (vmem_info_log_enabled()) {
        printf("domain %04x, bus %02x, device %02x, function %x\n",
               domain, bus, device, function);
    }
    // pass handle to kernel
    struct open_handle_data data = {
        .ipc_handle = *handle,
        .domain = domain,
        .bus = bus,
        .device = device,
        .function = function
    };
    if (ioctl(fd, VMEM_IOCTL_GET_PFN_LIST, &data) < 0) {
        perror("VMEM_IOCTL_GET_PFN_LIST failed");
        return -1;
    }
    if (vmem_info_log_enabled()) {
        for (int i = 0; i < 8; i++) {
            printf("received phys addr %llx\n", data.pfn_list.addrs[i]);
        }
    }
    memcpy(pfn_list, &data.pfn_list, sizeof(data.pfn_list));
    return 0;
}
int vmem_get_handle(int fd, int *dma_fd, struct pfn_list *pfn_list) {
    // Kernel returns dma addresses directly; do not add fixed BAR offsets here.
    struct open_handle_data data = {0};
    memcpy(data.pfn_list.addrs, pfn_list->addrs, sizeof(data.pfn_list.addrs));
    memcpy(data.pfn_list.size, pfn_list->size, sizeof(data.pfn_list.size));
    data.pfn_list.nents = pfn_list->nents;
    if (ioctl(fd, VMEM_IOCTL_GET_IPC_HANDLE, &data) < 0) {
        perror("VMEM_IOCTL_GET_IPC_HANDLE failed");
        return -1;
    }

    *dma_fd = data.fd;
    if (vmem_info_log_enabled()) {
        printf("got dma_buf_fd %d from kernel\n", *dma_fd);
    }
    return fd;
}
