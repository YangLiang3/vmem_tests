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
#include <linux/kernel.h>
#include "vmem_ioctl.h"



#define vmem_log(dev, fmt, ...) \
    printk(KERN_INFO "vmem: [%s] " fmt, dev_name(dev), ##__VA_ARGS__)

static dev_t vmem_dev;
static struct cdev vmem_cdev;
static struct class *vmem_class;
static char vmem_buf[VMEM_BUF_SIZE];

#define MAX_VMEM_DEVICES 128

/* Fixed BAR layout for the test GPUs on this server.
 * These describe where each GPU's VRAM BAR is physically mapped.
 * Used to validate that sg_dma_address() values fall in expected ranges.
 */
#define VMEM_BAR_4A8_BASE    0x22f000000000ULL  /* GPU 39:00.0 BAR2 start (lspci/sysfs) */
#define VMEM_BAR_4A8_WINDOW  0x800000000ULL     /* 32 GiB (e211 BAR2 size) */
#define VMEM_BAR_4A8_TARGET  0x201000000000ULL

#define VMEM_BAR_490_BASE    0x49000000000ULL
#define VMEM_BAR_490_WINDOW  0x1000000000ULL
#define VMEM_BAR_490_TARGET  0x201800000000ULL
static struct pci_dev *vmem_pdevs[MAX_VMEM_DEVICES];
static int vmem_pdev_count = 0;

static bool vmem_translate_bar_dma_addr(phys_addr_t dma_addr,
                                        phys_addr_t *translated_addr)
{
    if (dma_addr >= VMEM_BAR_4A8_BASE &&
        dma_addr < VMEM_BAR_4A8_BASE + VMEM_BAR_4A8_WINDOW) {
        *translated_addr = VMEM_BAR_4A8_TARGET + (dma_addr - VMEM_BAR_4A8_BASE);
        return true;
    }

    if (dma_addr >= VMEM_BAR_490_BASE &&
        dma_addr < VMEM_BAR_490_BASE + VMEM_BAR_490_WINDOW) {
        *translated_addr = VMEM_BAR_490_TARGET + (dma_addr - VMEM_BAR_490_BASE);
        return true;
    }

    printk(KERN_WARNING
           "vmem: translate miss dma_addr=%pa;"
           " 4a8=[0x%llx,0x%llx) 490=[0x%llx,0x%llx)\n",
           &dma_addr,
           (u64)VMEM_BAR_4A8_BASE,
           (u64)(VMEM_BAR_4A8_BASE + VMEM_BAR_4A8_WINDOW),
           (u64)VMEM_BAR_490_BASE,
           (u64)(VMEM_BAR_490_BASE + VMEM_BAR_490_WINDOW));

    return false;
}

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
    vmem_log(attach->dev, "dma_buf move notify\n");
}

static const struct dma_buf_attach_ops vmem_attach_ops = {
    .allow_peer2peer = true,
    .move_notify = vmem_move_notify,
};

static bool vmem_validate_pfn_list(const struct pfn_list *list)
{
    int max_nents = (int)ARRAY_SIZE(list->addrs);

    if (list->nents <= 0 || list->nents > max_nents)
        return false;

    return true;
}

struct vmem_dmabuf_priv {
    struct sg_table *sgt;
    // We store the raw physical address list directly in private data
    // to avoid constructing a fake sg_table just to copy from it later.
    struct pfn_list pfn_list; 
};

static struct sg_table *vmem_map_dma_buf(struct dma_buf_attachment *attachment,
                                        enum dma_data_direction dir)
{
    struct vmem_dmabuf_priv *priv = attachment->dmabuf->priv;
    struct sg_table *sgt;
    int ret;
    int i;

    vmem_log(attachment->dev, "vmem_map_dma_buf\n");

    if (!priv) {
        return ERR_PTR(-EINVAL);
    }
    
    sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
    if (!sgt)
        return ERR_PTR(-ENOMEM);

    // Allocate the scatterlist table based on our stored page count
    ret = sg_alloc_table(sgt, priv->pfn_list.nents, GFP_KERNEL);
    if (ret) {
        kfree(sgt);
        return ERR_PTR(ret);
    }
        
    struct scatterlist *dst_sg = sgt->sgl;
    
    for (i = 0; i < priv->pfn_list.nents; i++) {
        if (!dst_sg) break;
        
        dst_sg->offset = 0; 
        dst_sg->length = priv->pfn_list.size[i];
#ifdef CONFIG_NEED_SG_DMA_LENGTH
        dst_sg->dma_length = priv->pfn_list.size[i];
#endif

        /*
         * For P2P through a PCIe switch, set the DMA address directly to the
         * physical bus address of the remote GPU BAR.  The importing GPU can
         * issue PCIe transactions to this address through the switch without
         * IOMMU translation.
         *
         * We intentionally bypass dma_map_resource() here because:
         * 1. The DMA/IOMMU layer may not correctly handle cross-node P2P BAR
         *    addresses (addresses belonging to a remote device reachable only
         *    through the PCIe switch).
         * 2. For P2P between GPUs on the same switch fabric, the bus address
         *    IS the DMA address — no IOMMU mapping is required.
         *
         * Also set sg_page to ZERO_PAGE as a safety measure — some importers
         * (e.g., GPU KMD) may dereference sg_page() for internal bookkeeping
         * even though only sg_dma_address() is meaningful for device access.
         */
        sg_set_page(dst_sg, ZERO_PAGE(0), priv->pfn_list.size[i], 0);
        sg_dma_address(dst_sg) = (dma_addr_t)priv->pfn_list.addrs[i];
        sg_dma_len(dst_sg) = priv->pfn_list.size[i];

        if (dev_is_pci(attachment->dev)) {
             vmem_log(attachment->dev, "P2P direct map: phys=%llx len=%zu\n", 
                priv->pfn_list.addrs[i], priv->pfn_list.size[i]);
        }

        dst_sg = sg_next(dst_sg);
    }

    sgt->nents = priv->pfn_list.nents; 

    return sgt;
}

static void vmem_unmap_dma_buf(struct dma_buf_attachment *attachment,
                                 struct sg_table *sgt,
                                 enum dma_data_direction dir)
{
    vmem_log(attachment->dev, "vmem_unmap_dma_buf\n");
    /*
     * Since we set sg_dma_address directly (bypassing dma_map_resource),
     * there is no IOMMU/DMA mapping to tear down.  Just free the sg_table.
     */
    sg_free_table(sgt);
    kfree(sgt);
}

static void vmem_dmabuf_release(struct dma_buf *dmabuf)
{
    struct vmem_dmabuf_priv *priv = dmabuf->priv;
    printk(KERN_INFO "vmem: vmem_dmabuf_release\n");
    // sg_free_table(priv->sgt); // No longer used in priv
    // kfree(priv->sgt);
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
    case VMEM_IOCTL_GET_PFN_LIST:
        printk(KERN_INFO "vmem ioctl cmd 0\n");
        // get the level zero fd from user and print it in kernel log for demo
        {
            struct open_handle_data local_data;
            if (copy_from_user(&local_data, (struct open_handle_data __user *)arg, sizeof(struct open_handle_data)))
                return -EFAULT;
            printk(KERN_INFO "vmem ioctl received fd: %d, dbdf: %04x:%02x:%02x.%x\n",
                   local_data.ipc_handle.data[0],
                   local_data.domain, local_data.bus, local_data.device, local_data.function);

            int fd = local_data.ipc_handle.data[0]; // or however the fd is passed
            struct dma_buf *dmabuf = dma_buf_get(fd);
            if (IS_ERR(dmabuf))
                return PTR_ERR(dmabuf);
            
            printk(KERN_INFO "dma_buf_get returned %p\n", dmabuf);

            struct pci_dev *pdev = NULL;
            struct dma_buf_attachment *attach = ERR_PTR(-ENODEV);
            struct sg_table *sgt = NULL;
            int sgt_nents = 0;
            int export_nents = 0;

            // Iterate all VGA devices to find one that can attach to this dmabuf
            printk(KERN_INFO "vmem: Looking for compatible VGA device...\n");

            // Check pre-found specific Intel GPU devices by BDF first.
            if (vmem_pdev_count > 0) {
                for( int i = 0; i < vmem_pdev_count; i++) {
                    int domain = pci_domain_nr(vmem_pdevs[i]->bus);
                    int bus = vmem_pdevs[i]->bus->number;
                    int dev = PCI_SLOT(vmem_pdevs[i]->devfn);
                    int func = PCI_FUNC(vmem_pdevs[i]->devfn);

                    if (domain == local_data.domain && \
                        bus == local_data.bus && \
                        dev == local_data.device && \
                        func == local_data.function) {
                        pdev = vmem_pdevs[i];
                        vmem_log(&pdev->dev,
                                 "Found matching pre-registered device for dbdf %04x:%02x:%02x.%x\n",
                                 local_data.domain, local_data.bus,
                                 local_data.device, local_data.function);
                        break;
                    }
                }
            }

            if (!pdev) {
                printk(KERN_ERR "vmem: No device found for dbdf %04x:%02x:%02x.%x\n",
                       local_data.domain, local_data.bus, local_data.device, local_data.function);
                dma_buf_put(dmabuf);
                return -ENODEV;
            } else {
                vmem_log(&pdev->dev, "Trying pre-found GPU pci device\n");
                
                attach = dma_buf_dynamic_attach(dmabuf, &pdev->dev, &vmem_attach_ops, NULL);
                if (!IS_ERR(attach)) {
                    // Success!
                    pci_dev_get(pdev); 
                } else {
                    printk(KERN_ERR "vmem: Failed to attach to this device (err: %ld)\n", PTR_ERR(attach));
                    dma_buf_put(dmabuf);
                    return PTR_ERR(attach);
                }
            }

            // At this point, pdev and attach are valid.

            // Lock the reservation object before mapping
            dma_resv_lock(dmabuf->resv, NULL);
            sgt = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
            dma_resv_unlock(dmabuf->resv);

            if (IS_ERR(sgt)) {
                long err = PTR_ERR(sgt);
                printk(KERN_ERR "Failed to map dma_buf attachment: %ld
", err);
                dma_buf_detach(dmabuf, attach);
                dma_buf_put(dmabuf);
                return err;
            }

            struct scatterlist *sg;
            int i = 0;
            sgt_nents = sgt->nents ? sgt->nents : sgt->orig_nents;
            if (sgt_nents <= 0) {
                printk(KERN_ERR "vmem: mapped sg table is empty (nents=%d, orig_nents=%d)
",
                       sgt->nents, sgt->orig_nents);
                dma_resv_lock(dmabuf->resv, NULL);
                dma_buf_unmap_attachment(attach, sgt, DMA_BIDIRECTIONAL);
                dma_resv_unlock(dmabuf->resv);
                dma_buf_detach(dmabuf, attach);
                dma_buf_put(dmabuf);
                return -ENODATA;
            }

            export_nents = min_t(int, sgt_nents,
                                 (int)ARRAY_SIZE(local_data.pfn_list.addrs));
            if (export_nents < sgt_nents) {
                printk(KERN_WARNING "vmem: truncating sg entries from %d to %d
",
                       sgt_nents, export_nents);
            }

            /*
             * Pass the raw DMA addresses from the origin GPU sg_table directly
             * to the caller.  No address translation is performed here; the
             * physical-address remapping (if required) is the responsibility of
             * user-space or the peer node.
             */
            for_each_sg(sgt->sgl, sg, export_nents, i) {
                phys_addr_t dma_addr = sg_dma_address(sg);
                size_t len = sg_dma_len(sg);

                local_data.pfn_list.addrs[i] = dma_addr;
                local_data.pfn_list.size[i] = len;
                vmem_log(&pdev->dev, "sg[%d]: dma_addr=%pa len=%zu
",
                         i, &dma_addr, len);
                if (!((dma_addr >= VMEM_BAR_4A8_BASE &&
                       dma_addr <  VMEM_BAR_4A8_BASE + VMEM_BAR_4A8_WINDOW) ||
                      (dma_addr >= VMEM_BAR_490_BASE &&
                       dma_addr <  VMEM_BAR_490_BASE + VMEM_BAR_490_WINDOW)))
                    printk(KERN_WARNING "vmem: sg[%d] dma_addr=%pa outside known GPU BARs
",
                           i, &dma_addr);
            }
            local_data.pfn_list.nents = export_nents;

            dma_resv_lock(dmabuf->resv, NULL);
            dma_buf_unmap_attachment(attach, sgt, DMA_BIDIRECTIONAL);
            dma_resv_unlock(dmabuf->resv);
            dma_buf_detach(dmabuf, attach);
            dma_buf_put(dmabuf);

            if (copy_to_user((struct open_handle_data __user *)arg, &local_data, sizeof(struct open_handle_data)))
                return -EFAULT;
        }
        break;
    case VMEM_IOCTL_GET_IPC_HANDLE: // Create dma_buf from physical addresses
        {
            struct open_handle_data local_data;
            struct vmem_dmabuf_priv *priv;
            struct dma_buf *dmabuf;
           // struct sg_table *sgt;
           // struct scatterlist *sg;
            int fd;

            printk(KERN_INFO "vmem ioctl cmd 1\n");

            if (copy_from_user(&local_data, (struct open_handle_data __user *)arg, sizeof(struct open_handle_data)))
                return -EFAULT;

            if (!vmem_validate_pfn_list(&local_data.pfn_list)) {
                printk(KERN_ERR "vmem: invalid pfn_list.nents=%d\n", local_data.pfn_list.nents);
                return -EINVAL;
            }

            priv = kzalloc(sizeof(*priv), GFP_KERNEL);
            if (!priv)
                return -ENOMEM;

            /*
             * Translate the received raw PFNs (origin GPU BAR addresses) to the
             * local peer-visible physical addresses using this node's BAR->TARGET
             * mapping.  This is the PA calculation step:
             *   VMEM_BAR_4A8_BASE + offset  ->  VMEM_BAR_4A8_TARGET + offset
             *   VMEM_BAR_490_BASE + offset  ->  VMEM_BAR_490_TARGET + offset
             */
            for (int i = 0; i < local_data.pfn_list.nents; i++) {
                phys_addr_t orig = (phys_addr_t)local_data.pfn_list.addrs[i];
                phys_addr_t translated = orig;
                if (vmem_translate_bar_dma_addr(orig, &translated)) {
                    printk(KERN_INFO "vmem: GET_IPC_HANDLE addr[%d]: %pa -> %pa\n",
                           i, &orig, &translated);
                    local_data.pfn_list.addrs[i] = (unsigned long long)translated;
                } else {
                    printk(KERN_WARNING
                           "vmem: GET_IPC_HANDLE addr[%d]=%pa outside known BAR ranges\n",
                           i, &orig);
                }
            }

            // Store the translated PA list in private data
            memcpy(&priv->pfn_list, &local_data.pfn_list, sizeof(struct pfn_list));

            // No need to allocate sg_table here anymore since we construct it in map_dma_buf
            
            DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
            exp_info.ops = &vmem_dmabuf_ops;
            exp_info.size = 0;
            for (int i = 0; i < local_data.pfn_list.nents; i++) {
                printk(KERN_INFO "export cmd1: phys %llx, len %zu\n", local_data.pfn_list.addrs[i], local_data.pfn_list.size[i]);
                exp_info.size += local_data.pfn_list.size[i];
            }
            exp_info.flags = O_RDWR;
            exp_info.priv = priv;

            dmabuf = dma_buf_export(&exp_info);
            if (IS_ERR(dmabuf)) {
                printk(KERN_ERR "Failed to export dma_buf: %ld\n", PTR_ERR(dmabuf));
                kfree(priv);
                return PTR_ERR(dmabuf);
            }

            fd = dma_buf_fd(dmabuf, O_CLOEXEC);
            if (fd < 0) {
                printk(KERN_ERR "Failed to get dma_buf fd: %d\n", fd);
                dma_buf_put(dmabuf); // This will trigger release
                return fd;
            }

            local_data.fd = fd;

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
    struct pci_dev *pdev = NULL;
    alloc_chrdev_region(&vmem_dev, 0, 1, VMEM_DEV_NAME);
    cdev_init(&vmem_cdev, &vmem_fops);
    cdev_add(&vmem_cdev, vmem_dev, 1);
    vmem_class = class_create(VMEM_DEV_CLASS);
    device_create(vmem_class, NULL, vmem_dev, NULL, VMEM_DEV_NAME);
    
    // Find all matching devices
    while ((pdev = pci_get_device(0x8086, 0xe211, pdev))) {
        if (vmem_pdev_count < MAX_VMEM_DEVICES) {
            // Take an extra reference because pci_get_device will decrement it when passed to next call
            // OR simply: pci_dev_get(pdev);
            // Wait, if I pass pdev to next iteration, pci_get_function puts it. 
            // So if I want to hoard them, I must pci_dev_get(pdev) before continuing loop?
            // Actually, simply doing pci_dev_get(pdev) stores a reference for us.
            pci_dev_get(pdev);
            vmem_pdevs[vmem_pdev_count++] = pdev;
            vmem_log(&pdev->dev, "Found GPU pci device [%d] at %04x:%02x:%02x.%x\n",
                     vmem_pdev_count - 1, pci_domain_nr(pdev->bus), pdev->bus->number,
                     PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn));
        } else {
             printk(KERN_WARNING "vmem: Too many devices found, ignoring extra\n");
             // Don't break, let loop finish to properly refcount the current pdev that would be put by next call? 
             // If I break here, pdev (current) has refcount +1. Correct. 
             // But if I CONTINUE, pci_get_device puts it. 
             // So if I want to stop storing but continue iterating... wait, if I want to stop, I just break and put the current one.
            //  pci_dev_put(pdev);
             break;
        }
    }
    while ((pdev = pci_get_device(0x8086, 0xe210, pdev))) {
        if (vmem_pdev_count < MAX_VMEM_DEVICES) {
            // Take an extra reference because pci_get_device will decrement it when passed to next call
            // OR simply: pci_dev_get(pdev);
            // Wait, if I pass pdev to next iteration, pci_get_function puts it. 
            // So if I want to hoard them, I must pci_dev_get(pdev) before continuing loop?
            // Actually, simply doing pci_dev_get(pdev) stores a reference for us.
            pci_dev_get(pdev);
            vmem_pdevs[vmem_pdev_count++] = pdev;
            vmem_log(&pdev->dev, "Found GPU pci device [%d] at %04x:%02x:%02x.%x\n",
                     vmem_pdev_count - 1, pci_domain_nr(pdev->bus), pdev->bus->number,
                     PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn));
        } else {
             printk(KERN_WARNING "vmem: Too many devices found, ignoring extra\n");
             // Don't break, let loop finish to properly refcount the current pdev that would be put by next call? 
             // If I break here, pdev (current) has refcount +1. Correct. 
             // But if I CONTINUE, pci_get_device puts it. 
             // So if I want to stop storing but continue iterating... wait, if I want to stop, I just break and put the current one.
            //  pci_dev_put(pdev);
             break;
        }
    }
    
    printk(KERN_INFO "vmem driver loaded, found %d devices\n", vmem_pdev_count);
    return 0;
}
static void __exit vmem_exit(void) {
    int i;
    for (i = 0; i < vmem_pdev_count; i++) {
        pci_dev_put(vmem_pdevs[i]);
    }
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

