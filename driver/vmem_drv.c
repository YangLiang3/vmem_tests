#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/device.h>

#define VMEM_DEV_NAME "vmem"
#define VMEM_DEV_CLASS "vmem_class"
#define VMEM_BUF_SIZE 256

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
static long vmem_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    // demo: no real ioctl
    switch (cmd)
    {
    case 0: // example command
        printk(KERN_INFO "vmem ioctl cmd 0\n");
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
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Demo");
MODULE_DESCRIPTION("Simple vmem driver");
