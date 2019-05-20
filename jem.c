#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/scatterlist.h>
#include <linux/uaccess.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/radix-tree.h>
#include <linux/mutex.h>
#include <linux/fs.h>


#include "jem.h"



#define VERSION_MAJOR 0
#define VERSION_MINOR 0
#define DEVICE_NAME     "jem"



typedef struct
{
	struct class *device_class;
	struct device *file_device;
	int version_major;
} jem_dev_t;

typedef struct
{
	struct device* dev;
} jem_private_data_t;

typedef struct
{
	int dmabuf_fd;
	int _padding00;
	struct dma_buf* dmabuf;
	struct dma_buf_attachment* attachment;
    struct file* file;
} attach_entry_t;




static jem_dev_t jem_dev;
//static int next_id;

RADIX_TREE(dmabuf_entries, GFP_KERNEL);  /* Declare and initialize */
DEFINE_MUTEX(entries_mutex);


void jem_flush_all(void)
{
    struct radix_tree_iter iter;            
    void **slot;  
    attach_entry_t* entry = NULL;

    mutex_lock(&entries_mutex);

    radix_tree_for_each_slot(slot, &dmabuf_entries, &iter, 0)
    {
        entry = (attach_entry_t*)radix_tree_deref_slot(slot);
        if(entry != NULL)
        {
            printk(KERN_INFO "jem: freeing entry (%d)\n", entry->dmabuf_fd);
            
            radix_tree_delete(&dmabuf_entries, entry->dmabuf_fd);

            // Release
            fput(entry->file);
            
            kfree(entry);            
        }
    }

    mutex_unlock(&entries_mutex);
}


long jem_ioctl(struct file *file, unsigned int cmd, ulong arg)
{
    int fd = -1;
    jem_private_data_t* priv = (jem_private_data_t*)file->private_data;
    attach_entry_t* entry = NULL;
    int err = 0;


	switch (cmd)
	{
        case JEM_ATTACH_DMABUF:
            {
                printk(KERN_INFO "jem_ioctl: JEM_ATTACH_DMABUF\n");


                // Get the parameters from user space
                fd = (int)arg;


                // Check if the entry is already attached
                entry = (attach_entry_t*)radix_tree_lookup(&dmabuf_entries, (unsigned long)fd);
                if (entry != NULL)
                {
                    printk(KERN_ALERT "jem_ioctl: ATTACH_DMABUF an entry already exists for fd (%d).\n", fd);
                    return -1;	//TODO: error code
                }


                // Allocate storage for the entry;
                entry = kmalloc(sizeof(attach_entry_t), GFP_KERNEL);
                if (entry == NULL)
                {
                    printk(KERN_ALERT "jem_ioctl: ATTACH_DMABUF kmalloc failed.\n");
                    return -ENOMEM;
                }


                // Attach
                entry->file = fget(fd);


                // Record the entry
                mutex_lock(&entries_mutex);

                err = radix_tree_insert(&dmabuf_entries, (unsigned long)fd, (void*)entry);
                if (err != 0)
                {
                    printk(KERN_ALERT "jem_ioctl: radix_tree_insert failed. (%d)\n", err);
                    mutex_unlock(&entries_mutex);
                    goto A_err0;
                }

                mutex_unlock(&entries_mutex);


                // Return parameters to user
                printk(KERN_INFO "jem_ioctl: JEM_ATTACH_DMABUF OK (ret=%d)\n", fd);
                return fd;


            A_err0:
                kfree(entry);

                return -1;	// TODO: error code
            }
            break;

        case JEM_RELEASE_DMABUF:
            {
                printk(KERN_INFO "jem_ioctl: JEM_RELEASE_DMABUF\n");

                // Get the parameters from user space
                fd = (int)arg;


                // Retreive the entry
                entry = (attach_entry_t*)radix_tree_lookup(&dmabuf_entries, (unsigned long)fd);
                if (entry == NULL)
                {
                    printk(KERN_ALERT "jem_ioctl: RELEASE_DMABUF entry does not exist for fd (%d).\n", fd);
                    return -1;	//TODO: error code
                }

                mutex_lock(&entries_mutex);
                radix_tree_delete(&dmabuf_entries, fd);
                mutex_unlock(&entries_mutex);


                // Release
                fput(entry->file);


                kfree(entry);

                printk(KERN_INFO "jem_ioctl: JEM_RELEASE_DMABUF OK (fd=%d)\n", fd);
                return 0;
            }
            break;

        case JEM_CREATE_FD:
            {
                printk(KERN_INFO "jem_ioctl: JEM_CREATE_FD\n");

                // Get the parameters from user space
                fd = (int)arg;


                // Retreive the entry
                mutex_lock(&entries_mutex);
                entry = (attach_entry_t*)radix_tree_lookup(&dmabuf_entries, (unsigned long)fd);
                if (entry == NULL)
                {
                    printk(KERN_ALERT "jem_ioctl: JEM_CREATE_FD entry does not exist for fd (%d).\n", fd);
                    mutex_unlock(&entries_mutex);
                    return -1;	//TODO: error code
                }
                mutex_unlock(&entries_mutex);


                // Share across process
                err = get_unused_fd_flags(0);
                if (err >= 0)
                {
                    get_file(entry->file);
                    fd_install(err, entry->file);

                    printk(KERN_INFO "jem_ioctl: JEM_CREATE_FD OK (ret=%d)\n", err);
                }
                else
                {
                    printk(KERN_INFO "jem_ioctl: JEM_CREATE_FD FAIL (ret=%d)\n", err);
                }
                
                return err;
            }
            break;

        case JEM_FLUSH_ALL:
            {
                jem_flush_all();
                return 0;
            }
            break;

        default:
            break;
	}

	return -1;
}

static int jem_open(struct inode *inode, struct file *file)
{
	int ret = 0;
	jem_private_data_t* priv;

	printk(KERN_INFO "jem_open\n");

	priv = kmalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
	{
		return -ENOMEM;
	}

	priv->dev = jem_dev.file_device;
	file->private_data = priv;

	return ret;
}

static int jem_release(struct inode *inode, struct file *file)
{
	int ret = 0;
	jem_private_data_t* priv = file->private_data;

	printk(KERN_INFO "jem_release\n");

	kfree(priv);

	return ret;
}

struct file_operations jem_fops = {
	.owner = THIS_MODULE,
	.open = jem_open,
	.release = jem_release,
	.unlocked_ioctl = jem_ioctl,
};



static int jem_init(void)
{
	int ret;


	printk(KERN_INFO "jem_init\n");

	
	memset(&jem_dev, 0, sizeof(jem_dev));


	ret = register_chrdev(VERSION_MAJOR, DEVICE_NAME, &jem_fops);
	if (ret < 0)
	{
		printk(KERN_ALERT "jem: can't register major for device\n");
		return ret;
	}

	jem_dev.version_major = ret;

	jem_dev.device_class = class_create(THIS_MODULE, DEVICE_NAME);
	if (!jem_dev.device_class)
	{
		printk(KERN_ALERT "jem: failed to create class\n");
		return -EFAULT;
	}

	jem_dev.file_device = device_create(jem_dev.device_class,
		NULL,
		MKDEV(jem_dev.version_major, VERSION_MINOR),
		NULL,
		DEVICE_NAME);
	if (!jem_dev.file_device)
	{
		printk(KERN_ALERT "jem: failed to create device %s", DEVICE_NAME);
		return -EFAULT;
	}

	return 0;
}
static void jem_exit(void)
{

	printk(KERN_INFO "jem_exit\n");

    // Cleanup      
    jem_flush_all();


    // Destroy device
	device_destroy(jem_dev.device_class,
		MKDEV(jem_dev.version_major, VERSION_MINOR));

	class_destroy(jem_dev.device_class);

	unregister_chrdev(jem_dev.version_major, DEVICE_NAME);
}


MODULE_LICENSE("Dual BSD/GPL");

module_init(jem_init);
module_exit(jem_exit);
