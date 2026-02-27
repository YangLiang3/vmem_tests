#ifndef VMEM_LIB_H
#define VMEM_LIB_H
#include <ze_api.h>
#ifdef __cplusplus
extern "C" {
#endif

struct pfn_list {
    int page_count;
    unsigned long long addrs[8];
    size_t size[8];
};

int vmem_open();
int vmem_close(int fd);
int vmem_read(int fd, char *buf, int len);
int vmem_write(int fd, const char *buf, int len);
int vmem_open_handle(int fd, ze_ipc_mem_handle_t* handle, int rank, int device_id, struct pfn_list *pfn_list);
int vmem_get_handle(int fd, int *dma_fd, int rank, struct pfn_list *pfn_list);
#ifdef __cplusplus
}
#endif
#endif // VMEM_LIB_H
