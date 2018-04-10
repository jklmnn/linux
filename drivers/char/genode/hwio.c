
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/string.h>
#include <linux/dma-mapping.h>

#include "hwio.h"
#include "mem_arch.h"

typedef struct {
    int irq;
    wait_queue_head_t wait;
    int status;
} irq_t;

typedef struct {
    size_t size;
    void *virt;
    dma_addr_t phys;
} dma_t;

typedef struct {
    int type;
    union {
        mmio_range_t mmio;
        irq_t irq;
        dma_t dma;
    };
} hwio_data_t;

static int open_hwio(struct inode *inode, struct file *file)
{
    hwio_data_t *range;

    if(!capable(CAP_SYS_RAWIO))
        return -EPERM;

    file->private_data = kmalloc(sizeof(hwio_data_t), GFP_KERNEL);

    range = file->private_data;
    memset(range, 0, sizeof(hwio_data_t));

    return 0;
}

static const struct vm_operations_struct mmap_mmio_ops = {
#ifndef CONFIG_HAVE_IOREMAP_PROT
    .access = generic_access_phys
#endif //CONFIG_HAVE_IOREMAP_PROT
};

static int mmap_hwio(struct file* file, struct vm_area_struct *vma)
{
    hwio_data_t *hwio = (hwio_data_t*)file->private_data;
    mmio_range_t *range = &(hwio->mmio);
    dma_t *dma = &(hwio->dma);
    size_t size = vma->vm_end - vma->vm_start;
    phys_addr_t offset;

    switch (hwio->type)
    {
        case T_MMIO:
            vma->vm_pgoff = range->phys >> PAGE_SHIFT;
            offset = (phys_addr_t)(vma->vm_pgoff) << PAGE_SHIFT;

            printk("%s vma->vm_pgoff: %lx\n", __func__, vma->vm_pgoff);
            printk("%s offset: %llx\n", __func__, offset);
            printk("%s range: %lx x %zu\n", __func__, range->phys, range->length);

            // check boundaries set by ioctl
            if(size > range->length)
                return -EPERM;

            if(range->phys > offset)
                return -EPERM;

            if(range->phys + range->length < offset + size)
                return -EPERM;


            // check boundaries set by kernel and architecture
            if(offset + (phys_addr_t)size - 1 < offset)
                return -EINVAL;

            if(!valid_mmap_phys_addr_range(vma->vm_pgoff, size))
                return -EINVAL;

            if(!private_mapping_ok(vma))
                return -ENOSYS;

            if(!range_is_allowed(vma->vm_pgoff, size))
                return -EPERM;

            if(!phys_mem_access_prot_allowed(file, vma->vm_pgoff, size, &vma->vm_page_prot))
                return -EINVAL;


            vma->vm_ops = &mmap_mmio_ops;

            if(remap_pfn_range(
                        vma,
                        vma->vm_start,
                        vma->vm_pgoff,
                        size,
                        vma->vm_page_prot
                        ))
                return -EAGAIN;
            break;

        case T_DMA:

            printk("%s: %zu %zu\n", __func__, size, dma->size);
            if(size > dma->size){
                return -EPERM;
            }
            return dma_mmap_coherent(NULL, vma, dma->virt, dma->phys, dma->size);

        default:
            return -EPERM;
    }

    return 0;
}

static irqreturn_t irq_dispatcher(int irq, void *dev_id)
{
    hwio_data_t *hwio = (hwio_data_t*)dev_id;

    if (hwio->type == T_IRQ){
        hwio->irq.status = 1;
        wake_up_all(&(hwio->irq.wait));
    }

    return IRQ_RETVAL(1);
}

static long ioctl_hwio(struct file *file, unsigned int cmd, unsigned long arg)
{
    int ret;
    hwio_data_t *hwio = (hwio_data_t*)file->private_data;
    mmio_range_t *range = &(hwio->mmio);
    irq_t *irq = &(hwio->irq);
    dma_t *dma = &(hwio->dma);

    printk("%s\n", __func__);

    // if the range is already set it cannot be changed anymore
    if(hwio->type != T_UNCONFIGURED)
        return -EACCES;

    switch (cmd)
    {
        case MMIO_SET_RANGE:
            if(copy_from_user(range, (mmio_range_t*)arg, sizeof(mmio_range_t))){
                return -EACCES;
            }
            // extend size to full pages since we can only mmap pages
            range->length = (range->length / PAGE_SIZE * PAGE_SIZE) + !!(range->length % PAGE_SIZE) * PAGE_SIZE;
            hwio->type = T_MMIO;
            break;

        case IRQ_SET:
            if(copy_from_user(&(irq->irq), (int*)arg, sizeof(int))){
                return -EACCES;
            }
            printk("%s requesting irq %d\n", __func__, irq->irq);
            ret = request_irq(irq->irq, irq_dispatcher, IRQF_SHARED | IRQF_NO_SUSPEND, __func__, (void*)hwio);
            if (ret < 0){
                printk(KERN_ALERT "%s: requesting irq %d failed with %d\n", __func__, irq->irq, ret);
                return ret;
            }
            init_waitqueue_head(&(irq->wait));
            disable_irq(irq->irq);
            hwio->type = T_IRQ;
            break;

        case DMA_SET:
            if(copy_from_user(&(dma->size), (size_t*)arg, sizeof(size_t))){
                return -EACCES;
            }
            dma->virt = dma_alloc_coherent(NULL, dma->size, &(dma->phys), GFP_KERNEL);
            if(!dma->virt){
                printk(KERN_ALERT "%s: failed to allocate dma memory\n", __func__);
                return -ENOMEM;
            }
            // extend size to full pages since we can only mmap pages
            dma->size = (dma->size / PAGE_SIZE * PAGE_SIZE) + !!(dma->size % PAGE_SIZE) * PAGE_SIZE;
            printk("%s allocated %zu byte dma memory @ %#llx\n", __func__, dma->size, dma->phys);
            hwio->type = T_DMA;
            break;

        default:
            printk(KERN_ALERT "%s invalid command %d\n", __func__, cmd);
            return -EINVAL;
    }

    return 0;
}

ssize_t read_hwio (struct file *file, char __user *data, size_t size, loff_t *__attribute__((unused))offset)
{
    hwio_data_t *hwio = (hwio_data_t*)(file->private_data);
    printk("%s\n", __func__);

    switch (hwio->type)
    {
        case T_IRQ:
            enable_irq(hwio->irq.irq);
            wait_event_interruptible(hwio->irq.wait, hwio->irq.status);
            disable_irq(hwio->irq.irq);
            hwio->irq.status = 0;
            break;

        case T_DMA:
            if(size < sizeof(dma_addr_t)){
                printk(KERN_ALERT "%s: insufficient size %zu\n", __func__, size);
                return -EINVAL;
            }
            if(copy_to_user((void *)data, &(hwio->dma.phys), sizeof(dma_addr_t))){
                return -EACCES;
            }
            return sizeof(dma_addr_t);
            break;

        default:
            return -EACCES;
    }

    return 0;
}

static int close_hwio(struct inode *inode, struct file *file)
{
    hwio_data_t *hwio = (hwio_data_t*)(file->private_data);
    printk("%s\n", __func__);

    switch (hwio->type){
        case T_IRQ:
            free_irq(hwio->irq.irq, hwio);
            break;

        default:
            break;
    }
    if (hwio->type == T_IRQ)
    {
        free_irq(hwio->irq.irq, hwio);
    }

    kfree(file->private_data);
    return 0;
}

static const struct file_operations hwio_fops = {
    .open = open_hwio,
    .mmap = mmap_hwio,
    .read = read_hwio,
    .unlocked_ioctl = ioctl_hwio,
    .release = close_hwio
};

static struct miscdevice hwio_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "hwio",
    .fops = &hwio_fops
};

static int __init hwio_init(void)
{
    misc_register(&hwio_dev);
    printk(KERN_INFO "hwio module registered\n");
    return 0;
}

static void __exit hwio_exit(void)
{
    misc_deregister(&hwio_dev);
    printk(KERN_INFO "hwio module unregistered\n");
}


module_init(hwio_init);
module_exit(hwio_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Johannes Kliemann <jk@jkliemann.de>");
MODULE_DESCRIPTION("Provide hardware io resources in user space");

