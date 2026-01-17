#ifndef MM_H 
#define MM_H

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/mutex.h>
#include <linux/vmalloc.h>

#define DEVICE_NAME "mmpdr"

#define BUF_SIZE (64 * 1024UL) //64 kib 

struct mmapdr_vma_priv
{
    struct mmapdr_device *mdev;

    /*track num of VMAs sharing this private data */ 
    atomic_t refcount; 

    /*size of the mapped region in bytes */ 
    unsigned long mapped_bytes; 
}; 

struct mmapdr_device
{
    /*device */ 
    struct cdev cdev; 
    struct device *dev ; 
    struct class *class; 
    dev_t devt; 

    /*memory*/  
    void *virt_addr; 
    dma_addr_t dma_handle; 
    struct page **pages; 
    unsigned int nr_pages; 

    struct mutex lock; 
    atomic_t  open_count; 

    /*stats */ 
    atomic64_t fault_count; 
    atomic64_t bytes_mapped; 

    /*debug fs*/ 
    struct dentry *debugfs_dir 
}; 




#endif
