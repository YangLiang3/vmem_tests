#include "vmem_lib.h"
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/ioctl.h>

#define VMEM_DEV_PATH "/dev/vmem"

struct open_handle_data {
    ze_ipc_mem_handle_t ipc_handle;
    int rank;
    int device_id;
    struct pfn_list pfn_list;
};

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
int vmem_open_handle(int fd, ze_ipc_mem_handle_t* handle, int rank, int device_id, struct pfn_list *pfn_list) {
    // pass handle to kernel
    struct open_handle_data data = {
        .ipc_handle = *handle,
        .rank = rank,
        .device_id = device_id
    };
    ioctl(fd, 0, &data);
    for (int i = 0; i < 8; i++) {
        printf("rank %d device_id %d received phys addr %llx\n", rank, device_id, data.pfn_list.addrs[i]);
    }
    memcpy(pfn_list, &data.pfn_list, sizeof(data.pfn_list));
    return 0;
}
int vmem_get_handle(int fd, ze_ipc_mem_handle_t* handle, int rank, struct pfn_list *pfn_list) {
    // demo: just return the fd as handle
    struct open_handle_data data = {0};
    memcpy(data.pfn_list.addrs, pfn_list->addrs, sizeof(data.pfn_list.addrs));
    memcpy(data.pfn_list.size, pfn_list->size, sizeof(data.pfn_list.size));
    data.pfn_list.page_count = pfn_list->page_count;
    ioctl(fd, 1, &data);

    handle->data[0] = data.ipc_handle.data[0];
    return fd;
}
