#ifndef _VMEM_IOCTL_H_
#define _VMEM_IOCTL_H_

#include <linux/ioctl.h>
#include <linux/types.h>

#define VMEM_DEV_NAME "vmem"
#define VMEM_DEV_CLASS "vmem_class"
#define VMEM_BUF_SIZE 256

#ifndef ZE_MAX_IPC_HANDLE_SIZE
#define ZE_MAX_IPC_HANDLE_SIZE  64
#endif

#ifndef _ZE_API_H
typedef struct _ze_ipc_mem_handle_t
{
    char data[ZE_MAX_IPC_HANDLE_SIZE];                                      ///< [out] Opaque data representing an IPC handle
} ze_ipc_mem_handle_t;
#endif

struct pfn_list {
    int nents;
    unsigned long long addrs[8];
    size_t size[8];
};

struct open_handle_data {
    ze_ipc_mem_handle_t ipc_handle;
    int rank;
    int device_id;
    uint32_t bus;
    uint32_t device;
    uint32_t function;
    struct pfn_list pfn_list;
    int fd;
};

#define VMEM_MAGIC 'v'

/* Case 0: Get PFN list from an IPC handle/FD */
#define VMEM_IOCTL_GET_PFN_LIST _IOWR(VMEM_MAGIC, 0, struct open_handle_data)

/* Case 1: Create DMA_BUF/IPC handle from PFN list */
#define VMEM_IOCTL_GET_IPC_HANDLE _IOWR(VMEM_MAGIC, 1, struct open_handle_data)

#endif // _VMEM_IOCTL_H_
