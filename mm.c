#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/dma-mapping.h>
#include "mm.h"

static struct mmapdr_device *g_mdev;

static void mmapdr_vma_open(struct vm_area_struct *vma)
{
    struct mmapdr_vma_priv *priv = vma->vm_private_data;
    if(priv)
        atomic_inc(&priv->refcount);
}

static void mmapdr_vma_close(struct vm_area_struct *vma)
{
    struct mmapdr_vma_priv *priv = vma->vm_private_data;

    if(!priv)
        return;

    if(atomic_dec_and_test(&priv->refcount)){
        kfree(priv);
    }

    vma->vm_private_data = NULL;
}

static vm_fault_t mmapdr_map_fault(struct vm_fault *vmf)
{
    struct vm_area_struct *vma = vmf->vma;
    struct page *page;
    struct mmapdr_vma_priv *priv = vma->vm_private_data;
    unsigned long offset = vmf->pgoff << PAGE_SHIFT;
    unsigned int idx;

    if(!priv || offset >= BUF_SIZE)
        return VM_FAULT_SIGBUS ;

    idx = offset >> PAGE_SHIFT;

    if(idx >= g_mdev->nr_pages)
        return VM_FAULT_SIGBUS;

    page = g_mdev->pages[idx];
    get_page(page);

    if(vmf_insert_page(vma, vmf->address, page)) {
        put_page(page);
        return VM_FAULT_SIGBUS;
    }

    atomic64_inc(&g_mdev->fault_count);
    atomic64_add(PAGE_SIZE, &g_mdev->bytes_mapped);

    return VM_FAULT_NOPAGE;
}

static const struct vm_operations_struct mmapdr_vm_ops = {
    .open = mmapdr_vma_open,
    .close = mmapdr_vma_close,
    .fault = mmapdr_map_fault,
};

static int mmapdr_mmap(struct file *filep, struct vm_area_struct *vma)
{
    unsigned long size = vma->vm_end - vma->vm_start;
    unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;

    if(size == 0 || size > BUF_SIZE || offset + size > BUF_SIZE)
        return -EINVAL;

    struct mmapdr_vma_priv *priv = kzalloc(sizeof(*priv), GFP_KERNEL);
    if(!priv)
        return -ENOMEM;

    atomic_set(&priv->refcount, 1);
    priv->mapped_bytes = size;
    priv->mdev = g_mdev;

    vma->vm_private_data = priv;
    vma->vm_ops = &mmapdr_vm_ops;
    vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;

    if(vma->vm_flags & VM_WRITE)
        vma->vm_flags |= VM_MIXEDMAP;

    return 0;
}

static int mmapdr_mmap_open(struct inode *inode, struct file *file)
{
    atomic_inc(&g_mdev->open_count);
    return 0;
}

static int mmapdr_mmap_release(struct inode *inode, struct file *file)
{
    atomic_dec(&g_mdev->open_count);
    return 0;
}

static const struct file_operations mmapdr_fops = {
    .owner = THIS_MODULE,
    .open = mmapdr_mmap_open,
    .release = mmapdr_mmap_release,
    .mmap = mmapdr_mmap,
};

static int stats_show(struct seq_file *m, void *v)
{
    seq_printf(m, "Total memory       : %u KiB\n", BUF_SIZE >> 10);
    seq_printf(m, "DMA handle         : 0x%llx\n", (unsigned long long)g_mdev->dma_handle);
    seq_printf(m, "Page faults        : %lld\n", atomic64_read(&g_mdev->fault_count));
    seq_printf(m, "Bytes mapped       : %lld\n", atomic64_read(&g_mdev->bytes_mapped));
    seq_printf(m, "Active opens       : %d\n", atomic_read(&g_mdev->open_count));
    return 0;
}

DEFINE_SHOW_ATTRIBUTE(stats);

static int __init mmapdr_init(void)
{
    int ret;

    g_mdev = kzalloc(sizeof(*g_mdev), GFP_KERNEL);
    if(!g_mdev)
        return -ENOMEM;

    mutex_init(&g_mdev->lock);
    atomic_set(&g_mdev->open_count, 0);
    atomic64_set(&g_mdev->fault_count, 0);
    atomic64_set(&g_mdev->bytes_mapped, 0);

    g_mdev->nr_pages = BUF_SIZE >> PAGE_SHIFT;

    g_mdev->pages = kvmalloc_array(g_mdev->nr_pages, sizeof(struct page*), GFP_KERNEL);
    if(!g_mdev->pages) {
        ret = -ENOMEM;
        goto _err_free_dma;
    }

    for(unsigned int i = 0; i < g_mdev->nr_pages; ++i)
        g_mdev->pages[i] = pfn_to_page((g_mdev->dma_handle >> PAGE_SHIFT) + i);

    ret = alloc_chrdev_region(&g_mdev->devt, 0, 1, DEVICE_NAME);
    if(ret < 0)
        goto _err_free_pages;

    g_mdev->class = class_create(THIS_MODULE, DEVICE_NAME);
    if(IS_ERR(g_mdev->class)) {
        ret = PTR_ERR(g_mdev->class);
        goto _err_unregister;
    }

    cdev_init(&g_mdev->cdev, &mmapdr_fops);
    ret = cdev_add(&g_mdev->cdev, g_mdev->devt, 1);
    if(ret)
        goto _err_class;

    g_mdev->dev = device_create(g_mdev->class, NULL, g_mdev->devt, NULL, DEVICE_NAME);
    if(IS_ERR(g_mdev->dev)) {
        ret = PTR_ERR(g_mdev->dev);
        goto _err_cdev;
    }

    g_mdev->virt_addr = dma_alloc_coherent(g_mdev->dev, BUF_SIZE, &g_mdev->dma_handle, GFP_KERNEL | __GFP_ZERO);
    if(!g_mdev->virt_addr) {
        ret = -ENOMEM;
        goto _err_free_dev;
    }

    g_mdev->debugfs_dir = debugfs_create_dir(DEVICE_NAME, NULL);
    debugfs_create_file("stats", 0444, g_mdev->debugfs_dir, NULL, &stats_fops);

    pr_info("%s: loaded - 63 Kib DMA buffer at 0x%llx\n", DEVICE_NAME, (unsigned long long)g_mdev->dma_handle);

    return 0;

_err_free_dev:
    device_destroy(g_mdev->class, g_mdev->devt);
_err_cdev:
    cdev_del(&g_mdev->cdev);
_err_class:
    class_destroy(g_mdev->class);
_err_unregister:
    unregister_chrdev_region(g_mdev->devt, 1);
_err_free_pages:
    kvfree(g_mdev->pages);
_err_free_dma:
    kfree(g_mdev);
    g_mdev = NULL;
    return ret;
}

static void __exit mmapdr_exit(void)
{
    if (!g_mdev)
        return;

    debugfs_remove_recursive(g_mdev->debugfs_dir);
    cdev_del(&g_mdev->cdev);
    device_destroy(g_mdev->class, g_mdev->devt);
    class_destroy(g_mdev->class);
    unregister_chrdev_region(g_mdev->devt, 1);
    kvfree(g_mdev->pages);
    dma_free_coherent(g_mdev->dev, BUF_SIZE, g_mdev->virt_addr, g_mdev->dma_handle);
    kfree(g_mdev);

    pr_info("%s: unloaded\n", DEVICE_NAME);
}

module_init(mmapdr_init);
module_exit(mmapdr_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Chrinovic M");
MODULE_DESCRIPTION("Mapping device memory into Process's address space");
