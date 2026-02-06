#ifndef VMEM_LIB_H
#define VMEM_LIB_H
int vmem_open();
int vmem_close(int fd);
int vmem_read(int fd, char *buf, int len);
int vmem_write(int fd, const char *buf, int len);
int vmem_export_handle(int fd);
int vmem_import_handle(int fd);
#endif // VMEM_LIB_H
