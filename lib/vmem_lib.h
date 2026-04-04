#ifndef VMEM_LIB_H
#define VMEM_LIB_H
#if defined(__has_include)
#if __has_include(<ze_api.h>)
#include <ze_api.h>
#elif __has_include(<level_zero/ze_api.h>)
#include <level_zero/ze_api.h>
#else
#error "Level Zero header not found: expected ze_api.h or level_zero/ze_api.h"
#endif
#else
#include <level_zero/ze_api.h>
#endif
#include "vmem_ioctl.h"

#ifdef __cplusplus
extern "C" {
#endif


int vmem_open();
int vmem_close(int fd);
int vmem_read(int fd, char *buf, int len);
int vmem_write(int fd, const char *buf, int len);
int vmem_init(uint32_t addr_domain,
              uint32_t addr_bus, 
              uint32_t addr_device, 
              uint32_t addr_function);
int vmem_open_handle(int fd, ze_ipc_mem_handle_t* handle, struct pfn_list *pfn_list);
int vmem_get_handle(int fd, int *dma_fd, struct pfn_list *pfn_list);
#ifdef __cplusplus
}
#endif
#endif // VMEM_LIB_H
