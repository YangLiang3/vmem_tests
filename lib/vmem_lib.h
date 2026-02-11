#ifndef VMEM_LIB_H
#define VMEM_LIB_H
#include <ze_api.h>
#ifdef __cplusplus
extern "C" {
#endif
int vmem_open();
int vmem_close(int fd);
int vmem_read(int fd, char *buf, int len);
int vmem_write(int fd, const char *buf, int len);
int vmem_export_handle(int fd, ze_ipc_mem_handle_t* handle);
int vmem_import_handle(int fd);
#ifdef __cplusplus
}
#endif
#endif // VMEM_LIB_H
