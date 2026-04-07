#include <cstdio>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/dma-mapping.h>
#include <linux/version.h>
#include "mmapdr.h"

static struct mmapdr_device *g_mdev; 


static int mmapdr_mmap_open(struct inode *inode, struct file *filei)
{
    atomic_inc(&g_mdev->open_count); 
    return 0; 
}

static int mmapdr_mmap_release(struct inode *inode, struct file *file)
{
    atomic_dec(&g_mdev->open_count);
    return 0;
}

static long mmapdr_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    if(_IOC_TYPE(cmd) != MMAPDR_MAGIC)
        return -ENOTTY; 

    switch (cmd){

        case MMAPDR_IOC_GETINFO: {

            struct mmapdr_info info = {
                .buf_size    = BUF_SIZE,
                .dma_handle  = (u64)g_mdev->dma_handle,
                .nr_pages    = g_mdev->nr_pages,
                .fault_count  = atomic64_read(&g_mdev->fault_count),
                .bytes_mapped = atomic64_read(&g_mdev->bytes_mapped),
            };

            if(copy_to_user((void __user*)arg, &info, sizeof(info))
                return -EFAULT;

            return 0; 
        }

        default:
            return -ENOTTY; 

    }
}

int mmapdr_mmap(struct file *filep, struct vm_area_struct *vma)
{
    struct mmapdr_device *mdev = mmapdr_get_device(); 
    struct mmapdr_vma_priv *priv; 

    unsigned long size   = vma->vm_end - vma->vm_start;
    unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;

    if (!mdev)
        return -ENODEV;
 
    if (size == 0) {
        pr_warn("%s: mmap called with size=0\n", DEVICE_NAME);
        return -EINVAL;
    }

    if (size > BUF_SIZE || offset + size > BUF_SIZE) {
        pr_warn("%s: mmap range [%lu, %lu) exceeds buffer size %u\n",
                DEVICE_NAME, offset, offset + size, BUF_SIZE);
        return -EINVAL;
    }

    priv = kzalloc(sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;

    atomic_set(&priv->refcount, 1);
    priv->mapped_bytes = size;
    priv->mdev         = mdev;

    vma->vm_private_data = priv;
    vma->vm_ops = &mmapdr_vm_ops;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
    vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP | VM_MIXEDMAP);
#else
    vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP | VM_MIXEDMAP;
#endif
 
    pr_debug("%s: mmap() vma=[0x%lx, 0x%lx) pgoff=%lu size=%lu KiB\n",
             DEVICE_NAME,
             vma->vm_start, vma->vm_end,
             vma->vm_pgoff, size >> 10);

    return 0; 
}

static const struct file_operations mmapdr_fops = {
    .owner          = THIS_MODULE,      
    .open           = mmapdr_mmap_open,
    .release        = mmapdr_mmap_release,
    .mmap           = mmapdr_mmap,      
    .unlocked_ioctl = mmapdr_ioctl,
};

static void mmapdr_vma_open(struct vm_area_struct *vma)
{
    struct mmapdr_vma_priv *priv = vma->vm_private_data;
 
    if (priv)
        atomic_inc(&priv->refcount);
}

static void mmapdr_vma_close(struct vm_area_struct *vma)
{
    struct mmapdr_vma_priv *priv = vma->vm_private_data;
 
    if (!priv)
        return;

    if (atomic_dec_and_test(&priv->refcount))
        kfree(priv);
    
    vma->vm_private_data = NULL;
}

static vm_fault_t mmapdr_map_fault(struct vm_fault *vmf)
{
    struct vm_area_struct *vma = vmf->vma; 
    struct mmapdr_vma_priv *priv = vma->vm_private_data; 
    struct mmapdr_device *mdev = mmapdr_get_device(); 
    unsigned long offset;
    unsigned int page_idx;
    struct page *page;
    vm_fault_t fault_ret;

    if (!priv || !mdev) {
        pr_err_ratelimited("%s: fault with NULL priv or mdev\n", DEVICE_NAME);
        return VM_FAULT_SIGBUS;
    }

    /*byte offset */ 
    offset = vmf->pgoff << PAGE_SHIFT; 
    page_idx = vmf->pgoff; 

    if (offset >= BUF_SIZE || page_idx >= mdev->nr_pages)
    {
        pr_warn_ratelimited("%s: fault at out-of-range pgoff=%lu\n",
                            DEVICE_NAME, vmf->pgoff);
        return VM_FAULT_SIGBUS;
    }

    page = mdev->pages[page_idx]; 
    get_page(page); 

    fault_ret = vmf_insert_page(vma, vmf->address, page); 
    
    if (fault_ret & VM_FAULT_ERROR) 
    {
        put_page(page);
        pr_err_ratelimited("%s: vmf_insert_page failed: 0x%x\n",
                           DEVICE_NAME, fault_ret);
        return fault_ret;
    }

    atomic64_inc(&mdev->fault_count);
    atomic64_add(PAGE_SIZE, &mdev->bytes_mapped);

    return VM_FAULT_NOPAGE;
}
const struct vm_operations_struct mmapdr_vm_ops = {
    .open  = mmapdr_vma_open,   
    .close = mmapdr_vma_close,  
    .fault = mmapdr_map_fault,
};


static int stats_show(struct seq_file *m, void *v)
{
    struct mmapdr_device *mdev = mmapdr_get_device();
 
    if (!mdev) 
    {
        seq_puts(m, "Device not initialized\n");
        return 0;
    }
 
    seq_printf(m, "%-22s: %u KiB (%u bytes)\n",
               "DMA buffer size",
               BUF_SIZE >> 10, BUF_SIZE);
 
    seq_printf(m, "%-22s: 0x%llx\n",
               "DMA bus address",
               (unsigned long long)mdev->dma_handle);
 
    seq_printf(m, "%-22s: %u\n",
               "Page count",
               mdev->nr_pages);
 
    seq_printf(m, "%-22s: %lld\n",
               "Page faults (total)",
               atomic64_read(&mdev->fault_count));
 
    seq_printf(m, "%-22s: %lld bytes (%lld KiB)\n",
               "Bytes faulted in",
               atomic64_read(&mdev->bytes_mapped),
               atomic64_read(&mdev->bytes_mapped) >> 10);
 
    seq_printf(m, "%-22s: %d\n",
               "Active opens",
               atomic_read(&mdev->open_count));
 
    {
        s64 mapped = atomic64_read(&mdev->bytes_mapped);
        unsigned int pct = (mapped >= BUF_SIZE) ? 100
                         : (unsigned int)((mapped * 100) / BUF_SIZE);
        seq_printf(m, "%-22s: %u%%\n", "Buffer touched", pct);
    }
 
    return 0;
}

DEFINE_SHOW_ATTRIBUTE(stats);

static int page_map_show(struct seq_file *m, void *v)
{
    struct mmapdr_device *mdev = mmapdr_get_device();
    unsigned int i;
 
    if (!mdev) {
        seq_puts(m, "Device not initialized\n");
        return 0;
    }
 
    seq_printf(m, "%-6s  %-18s  %-18s\n",
               "Index", "Phys Address", "PFN");
    seq_puts(m,   "------  ------------------  ------------------\n");
 
    for (i = 0; i < mdev->nr_pages; i++)
    {
        struct page *page = mdev->pages[i];
        seq_printf(m, "%-6u  0x%016llx  0x%016lx\n",
                   i,
                   (unsigned long long)page_to_phys(page),
                   page_to_pfn(page));
    }
 
    return 0;
}
 
DEFINE_SHOW_ATTRIBUTE(page_map);

static int hexdump_show(struct seq_file *m, void *v)
{
    struct mmapdr_device *mdev = mmapdr_get_device();
    const u8 *buf;
    unsigned int i, j;
 
    if (!mdev || !mdev->virt_addr) {
        seq_puts(m, "Buffer not allocated\n");
        return 0;
    }
 
    buf = (const u8 *)mdev->virt_addr;
 
    seq_printf(m, "First %u bytes of DMA buffer at virt=0x%px bus=0x%llx:\n\n",
               HEXDUMP_BYTES,
               mdev->virt_addr,
               (unsigned long long)mdev->dma_handle);
 
    for (i = 0; i < HEXDUMP_BYTES; i += BYTES_PER_LINE) {
        seq_printf(m, "%04x:  ", i);
 
        for (j = 0; j < BYTES_PER_LINE; j++) {
            if (i + j < HEXDUMP_BYTES)
                seq_printf(m, "%02x ", buf[i + j]);
            else
                seq_puts(m, "   ");
 
            if (j == 7)
                seq_puts(m, " ");
        }
 
        seq_puts(m, " |");
        for (j = 0; j < BYTES_PER_LINE && i + j < HEXDUMP_BYTES; j++) {
            u8 c = buf[i + j];
            seq_printf(m, "%c", (c >= 0x20 && c < 0x7f) ? c : '.');
        }
        seq_puts(m, "|\n");
    }
 
    return 0;
}
 
DEFINE_SHOW_ATTRIBUTE(hexdump);
 
int mmapdr_debugfs_init(struct mmapdr_device *mdev)
{
    mdev->debugfs_dir = debugfs_create_dir(DEVICE_NAME, NULL);
 
    if (IS_ERR(mdev->debugfs_dir)) {
        mdev->debugfs_dir = NULL;
        return PTR_ERR(mdev->debugfs_dir);
    }
    debugfs_create_file("stats",    0444, mdev->debugfs_dir, NULL, &stats_fops);
    debugfs_create_file("page_map", 0444, mdev->debugfs_dir, NULL, &page_map_fops);
    debugfs_create_file("hexdump",  0444, mdev->debugfs_dir, NULL, &hexdump_fops);
 
    pr_info("%s: debugfs entries at /sys/kernel/debug/%s/\n",
            DEVICE_NAME, DEVICE_NAME);
 
    return 0;
}
 
void mmapdr_debugfs_remove(struct mmapdr_device *mdev)
{
    if (mdev->debugfs_dir) {
        debugfs_remove_recursive(mdev->debugfs_dir);
        mdev->debugfs_dir = NULL;
    }
}
 
static int __init mmapdr_init(void)
{
    int ret; 
    pr_info("%s: initializing driver\n", DEVICE_NAME);

    g_mdev = kzalloc(sizeof(*g_mdev), GFP_KERNEL);
    if (!g_mdev)
        return -ENOMEM;
 
    atomic_set(&g_mdev->open_count, 0);
    atomic64_set(&g_mdev->fault_count, 0);
    atomic64_set(&g_mdev->bytes_mapped, 0);
    
    ret = alloc_chrdev_region(&g_mdev->devt, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("%s: failed to allocate chrdev region: %d\n", DEVICE_NAME, ret);
        goto err_free_mdev;
    }
 
    pr_info("%s: allocated major=%d minor=%d\n",
            DEVICE_NAME,
            MAJOR(g_mdev->devt),  
            MINOR(g_mdev->devt)); 

    cdev_init(&g_mdev->cdev, &mmapdr_fops);
    g_mdev->cdev.owner = THIS_MODULE;
 
    ret = cdev_add(&g_mdev->cdev, g_mdev->devt, 1);
    if (ret)
    {
        pr_err("%s: cdev_add failed: %d\n", DEVICE_NAME, ret);
        goto err_unregister_chrdev;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    g_mdev->class = class_create(DEVICE_NAME);
#else
    g_mdev->class = class_create(THIS_MODULE, DEVICE_NAME);
#endif
 
    if (IS_ERR(g_mdev->class)) 
    {
        ret = PTR_ERR(g_mdev->class);
        pr_err("%s: class_create failed: %d\n", DEVICE_NAME, ret);
        goto err_del_cdev;
    }
 
    g_mdev->dev = device_create(g_mdev->class, NULL, g_mdev->devt,
                                NULL, DEVICE_NAME);
    if (IS_ERR(g_mdev->dev))
    {
        ret = PTR_ERR(g_mdev->dev);
        pr_err("%s: device_create failed: %d\n", DEVICE_NAME, ret);
        goto err_destroy_class;
    }

    g_mdev->virt_addr = dma_alloc_coherent(g_mdev->dev,
                                            BUF_SIZE,
                                            &g_mdev->dma_handle,
                                            GFP_KERNEL | __GFP_ZERO);

    if (!g_mdev->virt_addr) {
        pr_err("%s: dma_alloc_coherent failed\n", DEVICE_NAME);
        ret = -ENOMEM;
        goto err_destroy_device;
    }

    pr_info("%s: DMA buffer: virt=0x%px bus=0x%llx size=%u KiB\n",
            DEVICE_NAME,
            g_mdev->virt_addr,
            (unsigned long long)g_mdev->dma_handle,
            BUF_SIZE >> 10);

    g_mdev->nr_pages = BUF_SIZE >> PAGE_SHIFT;

    g_mdev->pages = kvmalloc_array(g_mdev->nr_pages,
                                    sizeof(struct page *),
                                    GFP_KERNEL);
    if (!g_mdev->pages) {
        pr_err("%s: failed to allocate pages array\n", DEVICE_NAME);
        ret = -ENOMEM;
        goto err_free_dma;
    }

    for(unsigned int i= 0; i < g_mdev->nr_pages; i++)
    {
        g_mdev->pages[i] = pfn_page((g_mdev->dma_handle >> PAGE_SHIFT) + 1);
    }

    ret = mmapdr_debugfs_init(g_mdev);
    if (ret) {
        /* debugfs failures are non-fatal — driver still works without it */
        pr_warn("%s: debugfs init failed: %d (continuing)\n", DEVICE_NAME, ret);
    }
 
    pr_info("%s: loaded successfully — %u pages, DMA bus addr 0x%llx\n",
            DEVICE_NAME, g_mdev->nr_pages,
            (unsigned long long)g_mdev->dma_handle);
 
    return 0;
 
err_free_dma:
    dma_free_coherent(g_mdev->dev, BUF_SIZE,
                      g_mdev->virt_addr, g_mdev->dma_handle);
err_destroy_device:
    device_destroy(g_mdev->class, g_mdev->devt);
err_destroy_class:
    class_destroy(g_mdev->class);
err_del_cdev:
    cdev_del(&g_mdev->cdev);
err_unregister_chrdev:
    unregister_chrdev_region(g_mdev->devt, 1);
err_free_mdev:
    kfree(g_mdev);
    g_mdev = NULL;
    return ret;
 
}

static void __exit mmapdr_exit(void)
{
    if (!g_mdev)
        return;
 
    mmapdr_debugfs_remove(g_mdev);
 
    /*
     * kvfree() is the counterpart to kvmalloc_array(): it handles both
     * kmalloc'd and vmalloc'd memory correctly (checks the pointer range).
     */
    kvfree(g_mdev->pages);
 
    /*
     * dma_free_coherent() must be called with the same device, size, and
     * dma_handle that were passed to dma_alloc_coherent(). It unmaps the
     * IOMMU mapping (if any) and returns the pages to the system.
     * Must be called before device_destroy() while g_mdev->dev is still valid.
     */
    dma_free_coherent(g_mdev->dev, BUF_SIZE,
                      g_mdev->virt_addr, g_mdev->dma_handle);
 
    device_destroy(g_mdev->class, g_mdev->devt);
    class_destroy(g_mdev->class);
    cdev_del(&g_mdev->cdev);
    unregister_chrdev_region(g_mdev->devt, 1);
    kfree(g_mdev);
 
    pr_info("%s: unloaded\n", DEVICE_NAME);
}
 
module_init(mmapdr_init);
module_exit(mmapdr_exit);
 
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Chrinovic M");
MODULE_DESCRIPTION("Demand-paged mapping of a DMA coherent buffer into userspace");
MODULE_VERSION("2.0");


