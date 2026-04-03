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
static struct pci_dev *vmem_pdevs[MAX_VMEM_DEVICES];
static int vmem_pdev_count = 0;

struct vmem_bar_swap_rule {
    u32 src_domain;
    u32 src_bus;
    u32 src_device;
    u32 src_function;
    u64 src_bar_base;
    u32 dst_domain;
    u32 dst_bus;
    u32 dst_device;
    u32 dst_function;
    u64 dst_bar_base;
    u64 window_size;
};

static const struct vmem_bar_swap_rule vmem_bar_swap_rules[] = {
    {
        .src_domain = 0x0000,
        .src_bus = 0xb6,
        .src_device = 0x00,
        .src_function = 0x0,
        .src_bar_base = 0x436800000000ULL,
        .dst_domain = 0x0000,
        .dst_bus = 0xb8,
        .dst_device = 0x00,
        .dst_function = 0x0,
        .dst_bar_base = 0x43e000000000ULL,
        .window_size = 0x43e000000000ULL - 0x436800000000ULL,
    },
    {
        .src_domain = 0x0000,
        .src_bus = 0xb8,
        .src_device = 0x00,
        .src_function = 0x0,
        .src_bar_base = 0x43e000000000ULL,
        .dst_domain = 0x0000,
        .dst_bus = 0xb6,
        .dst_device = 0x00,
        .dst_function = 0x0,
        .dst_bar_base = 0x436800000000ULL,
        .window_size = 0x43e000000000ULL - 0x436800000000ULL,
    },
    {
        .src_domain = 0x0001,
        .src_bus = 0xb6,
        .src_device = 0x00,
        .src_function = 0x0,
        .src_bar_base = 0x6b6800000000ULL,
        .dst_domain = 0x0001,
        .dst_bus = 0xb8,
        .dst_device = 0x00,
        .dst_function = 0x0,
        .dst_bar_base = 0x6be000000000ULL,
        .window_size = 0x6be000000000ULL - 0x6b6800000000ULL,
    },
    {
        .src_domain = 0x0001,
        .src_bus = 0xb8,
        .src_device = 0x00,
        .src_function = 0x0,
        .src_bar_base = 0x6be000000000ULL,
        .dst_domain = 0x0001,
        .dst_bus = 0xb6,
        .dst_device = 0x00,
        .dst_function = 0x0,
        .dst_bar_base = 0x6b6800000000ULL,
        .window_size = 0x6be000000000ULL - 0x6b6800000000ULL,
    },
};

static bool vmem_translate_bar_dma_addr(phys_addr_t dma_addr,
                                        phys_addr_t *translated_addr,
                                        const struct vmem_bar_swap_rule **matched_rule)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(vmem_bar_swap_rules); i++) {
        const struct vmem_bar_swap_rule *rule = &vmem_bar_swap_rules[i];
        u64 range_end = rule->src_bar_base + rule->window_size;

        if (dma_addr < rule->src_bar_base || dma_addr >= range_end)
            continue;

        *translated_addr = rule->dst_bar_base + (dma_addr - rule->src_bar_base);
        if (matched_rule)
            *matched_rule = rule;
        return true;
    }

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
        
        // 1. Set length/offset from our stored list 
        dst_sg->offset = 0; 
        dst_sg->length = priv->pfn_list.size[i];
#ifdef CONFIG_NEED_SG_DMA_LENGTH
        dst_sg->dma_length = priv->pfn_list.size[i];
#endif
        
        // We might want to set a dummy page to avoid iterators crashing.
        // For P2P resources (MMIO/LMEM), there might not be a valid struct page.
        // However, many importers assume sg_page(sg) is valid.
        // Ideally we should use PFN_DOWN(phys_addr) if it's system memory, 
        // but for BARs valid struct page might not exist.
        // Leaving page NULL is risky if importers dereference it. 
        // Some drivers set it to a dummy page or use pfn_to_page if appropriate.
        // Here we rely on the importer understanding dma_address is key.

        dma_addr_t dma_addr = dma_map_resource(attachment->dev, 
                                               priv->pfn_list.addrs[i], 
                                               priv->pfn_list.size[i], 
                                               dir, 
                                               DMA_ATTR_SKIP_CPU_SYNC);
        
        if (dma_mapping_error(attachment->dev, dma_addr)) {
            printk(KERN_ERR "vmem: failed to map resource for P2P\n");
            sg_free_table(sgt);
            kfree(sgt);
            return ERR_PTR(-ENOMEM);
        }

        sg_dma_address(dst_sg) = dma_addr;
        sg_dma_len(dst_sg) = priv->pfn_list.size[i];

        if (dev_is_pci(attachment->dev)) {
             vmem_log(attachment->dev, "mapped resource P2P: phys=%llx len=%zu dma=%llx\n", 
                priv->pfn_list.addrs[i], priv->pfn_list.size[i], dma_addr);
        }

        dst_sg = sg_next(dst_sg);
    }

    sgt->nents = priv->pfn_list.nents; 
    ret = 0; 

    return sgt;
}

static void vmem_unmap_dma_buf(struct dma_buf_attachment *attachment,
                                 struct sg_table *sgt,
                                 enum dma_data_direction dir)
{
    vmem_log(attachment->dev, "vmem_unmap_dma_buf\n");
    // We used dma_map_resource, so theoretically we should use dma_unmap_resource.
    // However, dma_unmap_sgtable usually calls dma_unmap_sg... which expects normal mappings.
    // Since we manually constructed this, we should manually iterate and unmap if we want to be 100% correct,
    // or rely on dma_unmap_resource loops.
    // Standard dma_unmap_sgtable might NOT work correctly if we mixed dma_map_resource manually.
    
    struct scatterlist *sg;
    int i;
    for_each_sg(sgt->sgl, sg, sgt->nents, i) {
         dma_unmap_resource(attachment->dev, sg_dma_address(sg), sg_dma_len(sg), dir, DMA_ATTR_SKIP_CPU_SYNC);
    }
    
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
            printk(KERN_INFO "vmem ioctl received fd: %d, rank: %d, device_id: %x, dbdf: %04x:%02x:%02x.%x\n",
                   local_data.ipc_handle.data[0], local_data.rank, local_data.device_id,
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
                                 "Found matching pre-registered device for rank %d and dbdf %04x:%02x:%02x.%x\n",
                                 local_data.rank, local_data.domain, local_data.bus,
                                 local_data.device, local_data.function);
                        break;
                    }
                }
                // Fallback to rank-based modulo selection for asymmetric node topologies.
                if (!pdev) {
                    int selected_idx = local_data.rank % vmem_pdev_count;
                    pdev = vmem_pdevs[selected_idx];
                    vmem_log(&pdev->dev,
                             "Fallback selected device index %d for rank %d\n",
                             selected_idx, local_data.rank);
                }
            }

            if (!pdev) {
                printk(KERN_ERR "vmem: No device found for rank %d\n", local_data.rank);
                dma_buf_put(dmabuf);
                return -ENODEV;
            } else {
                vmem_log(&pdev->dev, "Trying pre-found GPU pci device for rank %d\n", local_data.rank);
                
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

            uint64_t bar2_start = 0;
            if(pdev) {
                struct resource *res = &pdev->resource[2];
                bar2_start = res->start;
                vmem_log(&pdev->dev, "Device BAR2 resource: start=%pa\n", &bar2_start);
                
            }

            // At this point, pdev and attach are valid.

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
                dma_buf_put(dmabuf);
                return err;
            }

            struct scatterlist *sg;
            int i = 0;
            sgt_nents = sgt->nents ? sgt->nents : sgt->orig_nents;
            if (sgt_nents <= 0) {
                printk(KERN_ERR "vmem: mapped sg table is empty (nents=%d, orig_nents=%d)\n",
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
                printk(KERN_WARNING "vmem: truncating sg entries from %d to %d\n",
                       sgt_nents, export_nents);
            }
            // Iterate and print scatter list info
            for_each_sg(sgt->sgl, sg, export_nents, i) {
                phys_addr_t phys = sg_phys(sg);
                size_t len = sg->length;
                phys_addr_t dma_addr = sg_dma_address(sg);
                phys_addr_t translated_dma_addr = dma_addr;
                const struct vmem_bar_swap_rule *matched_rule = NULL;
                unsigned int offset = sg->offset;
                vmem_log(&pdev->dev, "[rank %d] sg->nents %d: phys %pa, len %zu, dma_addr %pa, offset %u\n",
                       local_data.rank,
                       i, &phys, len, &dma_addr, offset);

                if (vmem_translate_bar_dma_addr(dma_addr, &translated_dma_addr, &matched_rule)) {
                    vmem_log(&pdev->dev,
                             "Translated dma_addr from %pa to %pa using BAR swap %04x:%02x:%02x.%x -> %04x:%02x:%02x.%x\n",
                             &dma_addr, &translated_dma_addr,
                             matched_rule->src_domain, matched_rule->src_bus,
                             matched_rule->src_device, matched_rule->src_function,
                             matched_rule->dst_domain, matched_rule->dst_bus,
                             matched_rule->dst_device, matched_rule->dst_function);
                    dma_addr = translated_dma_addr;
                }

                if (i < 8) {
                    local_data.pfn_list.addrs[i] = dma_addr;
                    local_data.pfn_list.size[i] = len;
                          vmem_log(&pdev->dev, "Calculated P2P addr for rank %d: %llx (dma_addr %pa)\n",
                              local_data.rank, local_data.pfn_list.addrs[i], &dma_addr);
                }
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

            priv = kzalloc(sizeof(*priv), GFP_KERNEL);
            if (!priv)
                return -ENOMEM;

            // Store the pfn_list directly in private data
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

