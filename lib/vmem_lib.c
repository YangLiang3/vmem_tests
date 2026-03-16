#include "vmem_lib.h"
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include "vmem_ioctl.h"

#define VGPU_BAR_START 0x100000000ULL
#define VMEM_DEV_PATH "/dev/vmem"

uint32_t bus, device, function;

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

int vmem_init(uint32_t addr_bus, 
              uint32_t addr_device, 
              uint32_t addr_function) {
    bus = addr_bus;
    device = addr_device;
    function = addr_function;
    return 0;
}

int vmem_open_handle(int fd, ze_ipc_mem_handle_t* handle, int rank, int device_id, struct pfn_list *pfn_list) {
    printf("bus %d, device %d, function %d\n", bus, device, function);
    // pass handle to kernel
    struct open_handle_data data = {
        .ipc_handle = *handle,
        .rank = rank,
        .device_id = device_id,
        .bus = bus,
        .device = device,
        .function = function
    };
    ioctl(fd, VMEM_IOCTL_GET_PFN_LIST, &data);
    for (int i = 0; i < 8; i++) {
        printf("rank %d device_id %d received phys addr %llx\n", rank, device_id, data.pfn_list.addrs[i]);
    }
    memcpy(pfn_list, &data.pfn_list, sizeof(data.pfn_list));
    return 0;
}
int vmem_get_handle(int fd, int *dma_fd, int rank, struct pfn_list *pfn_list) {
    for(int i = 0; i < pfn_list->nents; i++) {
        pfn_list->addrs[i] += VGPU_BAR_START;
    }

    // demo: just return the fd as handle
    struct open_handle_data data = {0};
    memcpy(data.pfn_list.addrs, pfn_list->addrs, sizeof(data.pfn_list.addrs));
    memcpy(data.pfn_list.size, pfn_list->size, sizeof(data.pfn_list.size));
    data.pfn_list.nents = pfn_list->nents;
    ioctl(fd, VMEM_IOCTL_GET_IPC_HANDLE, &data);

    *dma_fd = data.fd;
    printf("rank %d got dma_buf_fd %d from kernel\n", rank, *dma_fd);
    return fd;
}
