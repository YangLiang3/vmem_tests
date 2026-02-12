// gcc -o l0_dmabuf l0_dmabuf.c -lmpi -lze_loader -I/usr/include/level_zero/ -g
// icpx -x c -o l0_dmabuf l0_dmabuf.c -lmpi -lze_loader -I/usr/include/level_zero/ -g
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include <unistd.h>      // For syscall()
#include <sys/syscall.h> // For SYS_pidfd_open
#include <errno.h>       // For errno
#include <string.h>      // For strerror()
#include <stdio.h>       // For printf()
#include <stdlib.h>      // For exit()
#include <sys/pidfd.h>
#include "ze_api.h"
#include "../lib/vmem_lib.h"
 
ze_driver_handle_t driver = NULL;
ze_context_handle_t context = NULL;
ze_event_handle_t event = NULL;
 
struct exchange_data {
    int dma_buf_fd;
    int pid;
};
 
#define ERR_PRINT(error_msg_, ...)                                                                 \
    do {                                                                                           \
        fprintf(stderr, "Error: ");                                                                \
        fprintf(stderr, error_msg_, __VA_ARGS__);                                                  \
        fprintf(stderr, "\n");                                                                     \
        fflush(stderr);                                                                            \
    } while (0);
 
#define ERR_CHECK_AND_PRINT(check_, error_msg_, ...)                                               \
    do {                                                                                           \
        if ((check_)) {                                                                            \
            ERR_PRINT(error_msg_, __VA_ARGS__);                                                    \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0);
 
static void l0_init(uint32_t driver_idx, uint32_t * device_count, ze_device_handle_t ** devices)
{
    zeInit(0);
 
    uint32_t driverCount = 0;
    zeDriverGet(&driverCount, NULL);
    ze_driver_handle_t* allDrivers = (ze_driver_handle_t*)malloc(driverCount * sizeof(ze_driver_handle_t));
    zeDriverGet(&driverCount, allDrivers);
 
    driver = allDrivers[driver_idx];
 
    ze_context_desc_t ctxtDesc = { ZE_STRUCTURE_TYPE_CONTEXT_DESC, NULL, 0 };
    zeContextCreate(driver, &ctxtDesc, &context);
 
    zeDeviceGet(driver, device_count, NULL);
    (*devices) = (ze_device_handle_t *)malloc((*device_count) * sizeof(ze_device_handle_t));
    zeDeviceGet(driver, device_count, (*devices));
 
    free(allDrivers);
}
 
static void l0_command_list_create_immediate(uint32_t index, uint32_t ordinal,
                                             ze_device_handle_t device,
                                             ze_command_list_handle_t *cl)
{
    ze_command_queue_desc_t commandQueueDesc = {.stype = ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC,
                                                .pNext = NULL,
                                                .ordinal = ordinal,
                                                .index = index,
                                                .flags = 0,
                                                .mode = ZE_COMMAND_QUEUE_MODE_DEFAULT,
                                                .priority = ZE_COMMAND_QUEUE_PRIORITY_NORMAL};
 
    zeCommandListCreateImmediate(context, device, &commandQueueDesc, cl);
}
 
static void l0_event_pool_create(uint32_t num_events, ze_event_pool_handle_t *l0_event_pool)
{
    const ze_event_pool_desc_t l0_event_pool_desc = {.stype = ZE_STRUCTURE_TYPE_EVENT_POOL_DESC,
                                                     .pNext = NULL,
                                                     .flags = 0,
                                                     .count = num_events};
 
    zeEventPoolCreate(context, &l0_event_pool_desc, 0, NULL, l0_event_pool);
}
 
static void l0_event_create(ze_event_pool_handle_t l0_event_pool, uint32_t index,
                            ze_event_handle_t *l0_event)
{
    const ze_event_desc_t l0_event_desc = {.stype = ZE_STRUCTURE_TYPE_EVENT_DESC,
                                           .pNext = NULL,
                                           .index = index,
                                           .signal = ZE_EVENT_SCOPE_FLAG_HOST,
                                           .wait = 0};
 
    zeEventCreate(l0_event_pool, &l0_event_desc, l0_event);
}
 
static void l0_allocate_device_buffer(void **buf, size_t size, ze_device_handle_t device)
{
    ze_device_mem_alloc_desc_t l0_device_mem_desc = {
        .stype = ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC,
        .pNext = NULL,
        .flags = 0,
        .ordinal = 0 /* this must be less than count of zeDeviceGetMemoryProperties */
    };
 
    zeMemAllocDevice(context, &l0_device_mem_desc, size, sizeof(unsigned), device, (void **) buf);
}
 
static void l0_memcpy(void *dst, const void *src, size_t sz, ze_command_list_handle_t cl)
{
    zeCommandListAppendMemoryCopy(cl, dst, src, sz, event, 0, NULL);
    zeEventHostSynchronize(event, UINT64_MAX);
    zeEventHostReset(event);
}
 
static void init_device_buffer(uint8_t *device_buf, size_t size, ze_command_list_handle_t cl)
{
    uint8_t *host_buf = ( uint8_t *)malloc(size);
    for (uint8_t i = 0; i < size; i++) {
        host_buf[i] = i;
    }
 
    l0_memcpy(device_buf, host_buf, size, cl);
    free(host_buf);
}
 
static void zero_device_buffer(uint8_t *device_buf, size_t size, ze_command_list_handle_t cl)
{
    uint8_t *host_buf = (uint8_t *)calloc(size, 1);
    l0_memcpy(device_buf, host_buf, size, cl);
    free(host_buf);
}
 
static int verify_device_buffer(uint8_t *device_buf, size_t size, ze_command_list_handle_t cl)
{
    uint8_t *host_buf = (uint8_t *)calloc(size, 1);
    int ret = 0;
 
    l0_memcpy(host_buf, device_buf, size, cl);
 
    for (uint8_t i = 0; i < size; i++) {
        if (host_buf[i] != i) {
            ERR_PRINT("Verification failed: host_buf[%d] = %u != %u", i, host_buf[i], i);
            ret = 1;
            break;
        }
    }
 
    free(host_buf);
    return ret;
}
 
static int print_device_buffer(uint8_t *device_buf, size_t size, int rank, ze_command_list_handle_t cl)
{
    int *host_buf = (int *)calloc(size, 1);
    int ret = 0;
 
    l0_memcpy(host_buf, device_buf, size, cl);
    printf("rank %d:", rank);
    for (uint8_t i = 0; i < size / sizeof(int); i++) {
            printf("%d  ", host_buf[i]);
 
       
    }
    printf("\n");
    free(host_buf);
    return ret;
}
 
static int print_host_buffer(int *host_buf, size_t nr_elements, int rank)
{
 
    int ret = 0;
    printf("rank %d:", rank);
    for (uint8_t i = 0; i < nr_elements; i++) {
            printf("%d  ", host_buf[i]);        
    }
    printf("\n");
    return ret;
}
 
static int get_remote_buf_ptr(ze_device_handle_t *device,
                                void *local_ptr,
                                void *remote_ptr,
                                int rank)
{
    struct exchange_data send_data, recv_data;
    int local_dma_fd;
    int remote_pid;
    ze_ipc_mem_handle_t local_ipc_handle = {};
    ze_ipc_mem_handle_t remote_ipc_handle = {};    
    ze_result_t ret = zeMemGetIpcHandle(context, local_ptr, &local_ipc_handle);
 
    if (ret != ZE_RESULT_SUCCESS) {
        printf("rank %d failed to get ipc handle with ret:%d\n", rank ,ret);
    }

    vmem_export_handle(vmem_open(), &local_ipc_handle);
 
    memcpy(&local_dma_fd, &local_ipc_handle, sizeof(local_dma_fd));
    send_data.pid = getpid();
    send_data.dma_buf_fd = local_dma_fd;
    MPI_Sendrecv(&send_data, sizeof(send_data), MPI_BYTE,
                /*dest*/ 1 - rank, 0,
                &recv_data, sizeof(recv_data), MPI_BYTE,
                /*src*/ 1 - rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
 
    remote_pid = recv_data.pid;
 
    printf("Rank %d local ipc local_dma_fd=%d local process pid=%d\n", rank, local_dma_fd, send_data.pid);
    printf("Rank %d received remote process pid=%d, ipc dma_buf_fd=%d\n",
           rank, remote_pid, recv_data.dma_buf_fd);
 
    int pidfd = syscall(SYS_pidfd_open, remote_pid, 0);
    if (pidfd == -1) {
        printf("syscall pidfd_open failed: %d, %s", errno,
                     strerror(errno));
        exit(1);
    }
 
    int fd_for_this_process = syscall(SYS_pidfd_getfd, pidfd, (unsigned int)recv_data.dma_buf_fd, 0);
    if (fd_for_this_process == -1) {
        printf("Rank %d syscall pidfd_getfd failed: %d, %s", rank, errno,
                     strerror(errno));
        close(pidfd);
        exit(1);
    }
 
    printf("Rank %d obtained fd_for_this_process=%d\n",
           rank, fd_for_this_process);
 
    memcpy(&remote_ipc_handle, &fd_for_this_process, sizeof(fd_for_this_process));
 
    ret = zeMemOpenIpcHandle(context,
                            //  devices[(rank +1) % rank_size ],
                             *device,
                             remote_ipc_handle,
                             0,
                             &remote_ptr);  
 
    if (ret != ZE_RESULT_SUCCESS) {
        printf("rank %d failed to open ipc handle with ret:%d\n", rank ,ret);
        exit(1);
    }
    // printf("rank %d success to open ipc handle with peer_dst_ptr:%p\n", rank, (void *)(*remote_ptr));
 
    return 0;
}
 
 
 
int main(int argc, char *argv[])
{
    uint32_t device_count;
    ze_device_handle_t *devices;
    ze_event_pool_handle_t event_pool = NULL;
    ze_command_list_handle_t cl1 = NULL;
    ze_command_list_handle_t cl2 = NULL;
 
    uint32_t nr_elements = 4;
    uint32_t nr_rank = 2;
    uint32_t N = nr_elements * nr_rank;
    uint32_t bytes =  N * sizeof(int);
 
    uint8_t *send_buf = NULL;
    uint8_t *recv_buf = NULL;
    uint64_t peer_recv_ptr = 0;
 
    MPI_Init(&argc, &argv);
    int rank, rank_size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &rank_size);
    if (rank_size != 2)
    {
        if (rank == 0)
            printf("Please run with -n 2\n");
 
        MPI_Finalize();
        return 0;
    }
 
    printf("MPI rank %d, size :%d\n", rank, rank_size);
    l0_init(0, &device_count, &devices);
    ERR_CHECK_AND_PRINT(device_count < 2, "Requires at least 2 devices, found %u", device_count);
 
    l0_event_pool_create(1, &event_pool);
    l0_event_create(event_pool, 0, &event);
 
    l0_command_list_create_immediate(0, 0, devices[rank], &cl1);
    // l0_command_list_create_immediate(0, 0, devices[1], &cl2);
 
    l0_allocate_device_buffer((void **) &send_buf, bytes, devices[rank]);
    l0_allocate_device_buffer((void **) &recv_buf, bytes, devices[rank]);
    // l0_allocate_device_buffer((void **) &device_buf2, size, devices[1]);
 
    int *send_cpu_buf = (int *)malloc(bytes);
    int *recv_cpu_buf = (int *)malloc(bytes);
 
    for (int i = 0; i < N; i++) {
        send_cpu_buf[i] = rank * 10 + 1;
        recv_cpu_buf[i] = rank * 10 + 2;
 
    }
 
    print_host_buffer(send_cpu_buf, N, rank);
    print_host_buffer(recv_cpu_buf, N, rank);
 
    // Copy from device_buf1 to device_buf2 (WRITE)
    // zero_device_buffer(send_buf, size, cl1);
    // zero_device_buffer(device_buf2, size, cl2);
    l0_memcpy(send_buf, send_cpu_buf, bytes, cl1);
    l0_memcpy(recv_buf, recv_cpu_buf, bytes, cl1);

    if (rank == 0) {
        printf("wait gdb attach to process %d\n", getpid());
        getchar();
    }
 
    MPI_Barrier(MPI_COMM_WORLD);
 
    get_remote_buf_ptr(&devices[rank], recv_buf, &peer_recv_ptr, rank);
    printf("Rank %d peer_recv_ptr %p\n", rank, (void *)peer_recv_ptr);
 
    int copied_data = send_cpu_buf[0];
 
    l0_memcpy(recv_buf, send_buf, nr_elements * sizeof(int), cl1);
    
    printf("Rank %d copy local src data to local dst with data %d\n", rank, copied_data);
 
    zeMemCloseIpcHandle(context, (void *)peer_recv_ptr);
    if (rank == 0) {
        print_device_buffer(recv_buf, bytes, rank, cl1);
    }
   
 
 
    // // Verify the WRITE copy
    // int ret1 = verify_device_buffer(device_buf2, size, cl2);
    // if (ret1 == 0) {
    //     printf("WRITE: Verification successful\n");
    // } else {
    //     printf("WRITE: Verification failed\n");
    // }
 
    // // Copy from device_buf1 to device_buf2 (READ)
    // init_device_buffer(device_buf1, size, cl1);
    // zero_device_buffer(device_buf2, size, cl2);
    // l0_memcpy(device_buf2, device_buf1, size, cl2);
 
    // // Verify the READ copy
    // int ret2 = verify_device_buffer(device_buf2, size, cl2);
    // if (ret2 == 0) {
    //     printf("READ: Verification successful\n");
    // } else {
    //     printf("READ: Verification failed\n");
    // }
    free(send_cpu_buf);
    free(recv_cpu_buf);
    return 0;
}
 