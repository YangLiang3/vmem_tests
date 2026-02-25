#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/err.h>
#include <linux/dma-mapping.h>
#include <linux/pci.h>
#include <linux/dma-resv.h>
#include <linux/slab.h>


#define VMEM_DEV_NAME "vmem"
#define VMEM_DEV_CLASS "vmem_class"
#define VMEM_BUF_SIZE 256

#define ZE_MAX_IPC_HANDLE_SIZE  64
typedef struct _ze_ipc_mem_handle_t
{
    char data[ZE_MAX_IPC_HANDLE_SIZE];                                      ///< [out] Opaque data representing an IPC handle

} ze_ipc_mem_handle_t;

struct pfn_list {
    int page_count;
    unsigned long long addrs[8];
    size_t size[8];
};

struct open_handle_data {
    ze_ipc_mem_handle_t ipc_handle;
    int rank;
    int device_id;
    struct pfn_list pfn_list;
};

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

static void vmem_move_notify(struct dma_buf_attachment *attach)
{
    printk(KERN_INFO "vmem: dma_buf move notify\n");
}

static const struct dma_buf_attach_ops vmem_attach_ops = {
    .allow_peer2peer = true,
    .move_notify = vmem_move_notify,
};

struct vmem_dmabuf_priv {
    struct sg_table *sgt;
};

static struct sg_table *vmem_map_dma_buf(struct dma_buf_attachment *attachment,
                                        enum dma_data_direction dir)
{
    struct vmem_dmabuf_priv *priv = attachment->dmabuf->priv;
    printk(KERN_INFO "vmem: vmem_map_dma_buf\n");
    return priv->sgt;
}

static void vmem_unmap_dma_buf(struct dma_buf_attachment *attachment,
                                 struct sg_table *sgt,
                                 enum dma_data_direction dir)
{
    printk(KERN_INFO "vmem: vmem_unmap_dma_buf\n");
    // Nothing to do here since we are not the ones who allocated the pages
}

static void vmem_dmabuf_release(struct dma_buf *dmabuf)
{
    struct vmem_dmabuf_priv *priv = dmabuf->priv;
    printk(KERN_INFO "vmem: vmem_dmabuf_release\n");
    sg_free_table(priv->sgt);
    kfree(priv->sgt);
    kfree(priv);
}

static const struct dma_buf_ops vmem_dmabuf_ops = {
    .map_dma_buf = vmem_map_dma_buf,
    .unmap_dma_buf = vmem_unmap_dma_buf,
    .release = vmem_dmabuf_release,
};


static long vmem_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    // demo: no real ioctl
    switch (cmd)
    {
    case 0:
        printk(KERN_INFO "vmem ioctl cmd 0\n");
        // get the level zero fd from user and print it in kernel log for demo
        {
            struct open_handle_data local_data;
            if (copy_from_user(&local_data, (struct open_handle_data __user *)arg, sizeof(struct open_handle_data)))
                return -EFAULT;
            printk(KERN_INFO "vmem ioctl received fd: %d, rank: %d, device_id: %x\n",
                   local_data.ipc_handle.data[0], local_data.rank, local_data.device_id);

            int fd = local_data.ipc_handle.data[0]; // or however the fd is passed
            struct dma_buf *dmabuf = dma_buf_get(fd);
            if (IS_ERR(dmabuf))
                return PTR_ERR(dmabuf);
            
            printk(KERN_INFO "dma_buf_get returned %p\n", dmabuf);

            struct pci_dev *pdev = NULL;
            struct dma_buf_attachment *attach = ERR_PTR(-ENODEV);

            // Iterate all VGA devices to find one that can attach to this dmabuf
            printk(KERN_INFO "vmem: Looking for compatible VGA device...\n");

            // TODO find the right GPU device
            pdev = pci_get_device(0x8086, local_data.device_id, NULL); // Intel GPU PCI ID
            if (!pdev) {
                // Fallback: try to find any display class device if specific ID fails
                printk(KERN_ERR "Failed to find specific GPU pci device, trying to find any display class device\n");
                pdev = pci_get_class(PCI_CLASS_DISPLAY_VGA << 8, NULL);
            }

            if (!pdev) {
                printk(KERN_ERR "Failed to find GPU pci device\n");
                dma_buf_put(dmabuf);
                return -ENODEV;
            }
            
            printk(KERN_INFO "Found GPU pci device: %04x:%02x:%02x.%d\n",
                   pdev->vendor, pdev->bus->number, PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn));
            
            attach = dma_buf_dynamic_attach(dmabuf, &pdev->dev, &vmem_attach_ops, NULL);

            if (IS_ERR(attach)) {
                printk(KERN_ERR "vmem: Failed to attach dma_buf to any GPU device (err: %ld)\n", PTR_ERR(attach));
                dma_buf_put(dmabuf);
                return PTR_ERR(attach);
            }

            // At this point, pdev is valid and holds a ref from pci_get_class/attach loop
            
            // For dynamic attachments, we must lock the reservation object before mapping
            struct sg_table *sgt;

            // Lock the reservation object before mapping
            dma_resv_lock(dmabuf->resv, NULL);
            
            // Try to map the attachment.
            // Note: Some drivers might require dma_buf_pin(attach) before mapping if they don't support dynamic mapping fully, 
            // but for dynamic attachments, map should handle it or fail if not pinned.
            // If -ENOMEM (-12) persists, it might be due to memory fragmentation or limits.
            sgt = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
            
            dma_resv_unlock(dmabuf->resv);

            if (IS_ERR(sgt)) {
                long err = PTR_ERR(sgt);
                printk(KERN_ERR "Failed to map dma_buf attachment: %ld\n", err);
                dma_buf_detach(dmabuf, attach);
                pci_dev_put(pdev);
                dma_buf_put(dmabuf);
                return err;
            }

            struct scatterlist *sg;
            int i = 0;
            // Iterate and print scatter list info
            for_each_sg(sgt->sgl, sg, sgt->nents, i) {
                phys_addr_t phys = sg_phys(sg);
                size_t len = sg->length;
                printk(KERN_INFO "sg %d: phys %pa, len %zu\n", i, &phys, len);
                if (i < 8) {
                    local_data.pfn_list.addrs[i] = phys;
                    local_data.pfn_list.size[i] = len;
                }
            }
            local_data.pfn_list.page_count = sgt->nents;

            dma_resv_lock(dmabuf->resv, NULL);
            dma_buf_unmap_attachment(attach, sgt, DMA_BIDIRECTIONAL);
            dma_resv_unlock(dmabuf->resv);
            
            dma_buf_detach(dmabuf, attach);
            pci_dev_put(pdev);
            dma_buf_put(dmabuf);

            if (copy_to_user((struct open_handle_data __user *)arg, &local_data, sizeof(struct open_handle_data)))
                return -EFAULT;
        }
        break;
    case 1: // Create dma_buf from physical addresses
        {
            struct open_handle_data local_data;
            struct vmem_dmabuf_priv *priv;
            struct dma_buf *dmabuf;
            struct sg_table *sgt;
            struct scatterlist *sg;
            int i, fd;

            printk(KERN_INFO "vmem ioctl cmd 1\n");

            if (copy_from_user(&local_data, (struct open_handle_data __user *)arg, sizeof(struct open_handle_data)))
                return -EFAULT;

            priv = kzalloc(sizeof(*priv), GFP_KERNEL);
            if (!priv)
                return -ENOMEM;

            sgt = kmalloc(sizeof(*sgt), GFP_KERNEL);
            if (!sgt) {
                kfree(priv);
                return -ENOMEM;
            }

            // Assuming 8 pages of 4K for now. This should be dynamic.
            if (sg_alloc_table(sgt, local_data.pfn_list.page_count, GFP_KERNEL)) {
                printk(KERN_ERR "Failed to allocate sg_table\n");
                kfree(sgt);
                kfree(priv);
                return -ENOMEM;
            }
            priv->sgt = sgt;

            for_each_sg(sgt->sgl, sg, sgt->nents, i) {
                // This assumes the user provides valid physical addresses of pages
                sg_set_page(sg, pfn_to_page(PFN_DOWN(local_data.pfn_list.addrs[i])), local_data.pfn_list.size[i], 0);
                sg_dma_address(sg) = local_data.pfn_list.addrs[i];
                sg_dma_len(sg) = local_data.pfn_list.size[i];
            }

            DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
            exp_info.ops = &vmem_dmabuf_ops;
            exp_info.size = 0;
            for (int i = 0; i < local_data.pfn_list.page_count; i++) {
                exp_info.size += local_data.pfn_list.size[i];
            }
            exp_info.flags = O_RDWR;
            exp_info.priv = priv;

            dmabuf = dma_buf_export(&exp_info);
            if (IS_ERR(dmabuf)) {
                printk(KERN_ERR "Failed to export dma_buf: %ld\n", PTR_ERR(dmabuf));
                sg_free_table(sgt);
                kfree(sgt);
                kfree(priv);
                return PTR_ERR(dmabuf);
            }

            fd = dma_buf_fd(dmabuf, O_CLOEXEC);
            if (fd < 0) {
                printk(KERN_ERR "Failed to get dma_buf fd: %d\n", fd);
                dma_buf_put(dmabuf); // This will trigger release
                return fd;
            }

            // Return the fd in the ipc_handle
            memset(&local_data.ipc_handle, 0, sizeof(local_data.ipc_handle));
            memcpy(local_data.ipc_handle.data, &fd, sizeof(fd));

            if (copy_to_user((struct open_handle_data __user *)arg, &local_data, sizeof(struct open_handle_data))) {
                printk(KERN_ERR "Failed to copy data to user\n");
                put_unused_fd(fd);
                // dma_buf_put is implicitly called by fput on the fd
                return -EFAULT;
            }
        }
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

