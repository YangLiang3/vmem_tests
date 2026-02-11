#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/dma-buf.h>


#define VMEM_DEV_NAME "vmem"
#define VMEM_DEV_CLASS "vmem_class"
#define VMEM_BUF_SIZE 256

#define ZE_MAX_IPC_HANDLE_SIZE  64
typedef struct _ze_ipc_mem_handle_t
{
    char data[ZE_MAX_IPC_HANDLE_SIZE];                                      ///< [out] Opaque data representing an IPC handle

} ze_ipc_mem_handle_t;



static dev_t vmem_dev;
static struct cdev vmem_cdev;
static struct class *vmem_class;
static char vmem_buf[VMEM_BUF_SIZE];


static int vmem_open(struct inode *inode, struct file *file) {
    printk(KERN_INFO "vmem device opened\n");
    return 0;
}
static int vmem_release(struct inode *inode, struct file *file) {
    printk(KERN_INFO "vmem device closed\n");
    return 0;
}
static ssize_t vmem_read(struct file *file, char __user *buf, size_t count, loff_t *ppos) {
    size_t to_copy = min(count, (size_t)VMEM_BUF_SIZE);
    if (copy_to_user(buf, vmem_buf, to_copy))
        return -EFAULT;
    return to_copy;
}
static ssize_t vmem_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
    size_t to_copy = min(count, (size_t)VMEM_BUF_SIZE);
    if (copy_from_user(vmem_buf, buf, to_copy))
        return -EFAULT;
    return to_copy;
}

static int vmem_match_any(struct device *dev, const void *data)
{
    return 1;
}

static long vmem_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    // demo: no real ioctl
    switch (cmd)
    {
    case 0:
        printk(KERN_INFO "vmem ioctl cmd 0\n");
        // get the level zero fd from user and print it in kernel log for demo
        {
            ze_ipc_mem_handle_t local_ipc_handle;
            if (copy_from_user(&local_ipc_handle, (ze_ipc_mem_handle_t __user *)arg, sizeof(ze_ipc_mem_handle_t)))
                return -EFAULT;
            printk(KERN_INFO "vmem ioctl received fd: %d\n", local_ipc_handle.data[0]);

            int fd = local_ipc_handle.data[0]; // or however the fd is passed
            struct dma_buf *dmabuf = dma_buf_get(fd);
            printk(KERN_INFO "dma_buf_get returned %p\n", dmabuf);
            struct device *dev = class_find_device(vmem_class, NULL, NULL, vmem_match_any);
            if (!dev) {
                printk(KERN_ERR "Failed to find device for dma_buf_attach\n");
                return -ENODEV;
            }
            struct dma_buf_attachment *attach = dma_buf_attach(dmabuf, dev);
            struct sg_table *sgt = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);

            struct scatterlist *sg;
            int i = 0;
            for_each_sg(sgt->sgl, sg, sgt->nents, i) {
                phys_addr_t phys = sg_phys(sg);
                size_t len = sg->length;
                // 记录物理地址和长度
                printk(KERN_INFO "sg %d: phys %pa, len %zu\n", i, &phys, len);
            }

            dma_buf_unmap_attachment(attach, sgt, DMA_BIDIRECTIONAL);
            dma_buf_detach(dmabuf, attach);
            dma_buf_put(dmabuf);

        }
        break;
    case 1: // example command
        printk(KERN_INFO "vmem ioctl cmd 1\n");
        break;
    default:
        break;
    }
    return 0;
}


static struct file_operations vmem_fops = {
    .owner = THIS_MODULE,
    .open = vmem_open,
    .release = vmem_release,
    .read = vmem_read,
    .write = vmem_write,
    .unlocked_ioctl = vmem_ioctl,
};
static int __init vmem_init(void) {
    alloc_chrdev_region(&vmem_dev, 0, 1, VMEM_DEV_NAME);
    cdev_init(&vmem_cdev, &vmem_fops);
    cdev_add(&vmem_cdev, vmem_dev, 1);
    vmem_class = class_create(VMEM_DEV_CLASS);
    device_create(vmem_class, NULL, vmem_dev, NULL, VMEM_DEV_NAME);
    printk(KERN_INFO "vmem driver loaded\n");
    return 0;
}
static void __exit vmem_exit(void) {
    device_destroy(vmem_class, vmem_dev);
    class_destroy(vmem_class);
    cdev_del(&vmem_cdev);
    unregister_chrdev_region(vmem_dev, 1);
    printk(KERN_INFO "vmem driver unloaded\n");
}

module_init(vmem_init);
module_exit(vmem_exit);
MODULE_IMPORT_NS("DMA_BUF");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Demo");
MODULE_DESCRIPTION("Simple vmem driver");

