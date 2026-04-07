#ifndef _MMAPDR_H
#define _MMAPDR_H

#include <linux/cdev.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/dma-mapping.h>
#include <linux/debugfs.h>


#define DEVICE_NAME     "mmapdr"
#define BUF_SIZE        (64 * 1024)
#define MMAPDR_MAGIC    0xDA

#define HEXDUMP_BYTES  256
#define BYTES_PER_LINE  16

struct mmapdr_info {
    __u32  buf_size;
    __u64  dma_handle;
    __u32  nr_pages;
    __s64  fault_count;
    __s64  bytes_mapped;
};

#define MMAPDR_IOC_GETINFO  _IOR(MMAPDR_MAGIC, 0x01, struct mmapdr_info)
#define MMAPDR_IOC_FILL     _IOW(MMAPDR_MAGIC, 0x02, struct mmapdr_fill)


/*per device state*/ 
struct mmapdr_device {
    dev_t devt;
    struct cdev cdev;
    struct class *class;
    struct device *dev;
 
    void *virt_addr;
    dma_addr_t dma_handle;
    struct page **pages;
    unsigned int nr_pages;
 
    atomic_t open_count;
    atomic64_t  fault_count;
    atomic64_t  bytes_mapped;
 
    struct dentry *debugfs_dir;
};
 
struct mmapdr_vma_priv{
    atomic_t             refcount;
    unsigned long        mapped_bytes;
    struct mmapdr_device *mdev;
};

struct mmapdr_device *mmapdr_get_device(void)
{
    return g_mdev;
}

extern const struct vm_operations mmapdr_vm_ops; 
int mmapdr_mmap(struct file *filep, struct vm_area_struct *vma); 
int mmapdr_debugfs_init(struct mmapdr_device *mdev);
void mmapdr_debugfs_remove(struct mmapdr_device *mdev);

#endif
