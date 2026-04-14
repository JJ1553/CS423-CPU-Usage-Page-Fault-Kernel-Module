#define LINUX

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/cdev.h>
#include <workqueue.h>


#include "mp3_given.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("JaJenks2 <JaJenks2@illinois.edu>");
MODULE_DESCRIPTION("CS-423 MP3");

// Forward declarations
static int __init mp3_init(void);
static void __exit mp3_exit(void);

#define DEBUG 1
#define MP3_BUFFER_SIZE (128 * PAGE_SIZE) //128 pages

struct mp3_proc {
    pid_t pid;
    struct list_head list;
};


static struct proc_dir_entry *mp3_dir;
static struct proc_dir_entry *mp3_status;
static struct delayed_work mp3_work;
static void *mp3_buffer;
static unsigned long sample_index;
static unsigned long max_samples = 12000;
static struct cdev mp3_cdev;

static dev_t mp3_dev = MKDEV(423, 0);

static LIST_HEAD(mp3_proc_list);
static DEFINE_MUTEX(mp3_lock);


/*
	Free vmalloc profiler buffer and clear reserved bits for all pages.
	Safe to call multiple times because it checks mp3_buffer for NULL.
*/
static void mp3_free_buffer(void)
{
    size_t i;

    if (!mp3_buffer)
        return;

    for (i = 0; i < MP3_BUFFER_SIZE; i += PAGE_SIZE) {
        struct page *page = vmalloc_to_page((char *)mp3_buffer + i);
        if (page)
            ClearPageReserved(page);
    }

    vfree(mp3_buffer);
    mp3_buffer = NULL;
}

static int mp3_open(struct inode *inode, struct file *file) {
	return 0;
}

static int mp3_release(struct inode *inode, struct file *file) {
	return 0;
}

/*
	Character device mmap callback.
	Maps the vmalloc profiler buffer into user space one page at a time,
	because vmalloc memory is virtually contiguous but not necessarily
	physically contiguous.
*/
static int mp3_mmap(struct file *file, struct vm_area_struct *vma) {
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long current_addr = vma->vm_start;
	unsigned long pfn;
	char *kaddr = (char *)mp3_buffer + offset;
	

	if(!mp3_buffer)
		return -ENOMEM;

	if (offset >= MP3_BUFFER_SIZE)
		return -EINVAL;

	if(size > MP3_BUFFER_SIZE - offset)
		return -EINVAL;


	// Remap each vmalloc page individually into the user VMA range.
	while(size > 0) {
		pfn = vmalloc_to_pfn(kaddr);
		
		if(remap_pfn_range(vma, current_addr, pfn, PAGE_SIZE, vma->vm_page_prot))
			return -EFAULT;
		
		kaddr += PAGE_SIZE;
		current_addr += PAGE_SIZE;

		if(size > PAGE_SIZE)
			size -= PAGE_SIZE;
		else
			size = 0;
	}

	return 0;
}


static const struct file_operations mp3_fops = {
	.owner = THIS_MODULE,
	.open = mp3_open,
	.release = mp3_release,
	.mmap = mp3_mmap,
};


/*
	Proc read callback for /proc/mp3/status.
	Prints one registered PID per line.
*/
static int mp3_status_show(struct seq_file *m, void *v) {

    struct mp3_proc *entry;

    mutex_lock(&mp3_lock);
    list_for_each_entry(entry, &mp3_proc_list, list)
        seq_printf(m, "%d\n", entry->pid);
    mutex_unlock(&mp3_lock);

    return 0;
}

/* Open callback for /proc/mp3/status. */
static int mp3_status_open(struct inode *inode, struct file *file) {
	return single_open(file, mp3_status_show, NULL);
}

/*
	Proc write callback handling registration and deregistration commands.
	R <PID>: add PID to tracking list and start delayed work if this is the
	first entry.
	U <PID>: remove PID from tracking list and stop delayed work when list
	becomes empty.
*/
static ssize_t mp3_status_write(struct file *file, const char __user *buffer,
				size_t count, loff_t *ppos)
{
char *buf;
size_t len;
pid_t pid;
int ret;
struct mp3_proc *entry;
struct mp3_proc *tmp;
bool found = false, is_empty = false;


	if (count == 0)
		return 0;

	buf = memdup_user_nul(buffer, count);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	len = strnlen(buf, count + 1);
	if (len > 0 && buf[len - 1] == '\n')
		buf[len - 1] = '\0';

	switch (buf[0]) {

		case 'R':

			ret = sscanf(buf, "R %d", &pid);
			if (ret != 1 || pid <= 0) {
				kfree(buf);
				return -EINVAL;
			}

        	mutex_lock(&mp3_lock);
			// Reject duplicate registration requests for the same PID.
            list_for_each_entry(tmp, &mp3_proc_list, list) {
               if(tmp->pid == pid) {
                  pr_debug( "PID %d already exists in the list\n", pid);
                  mutex_unlock(&mp3_lock);
                  kfree(buf);
                  return count;
               }
            }
            //if new PID, create a new entry and add it to the list
            entry = kmalloc(sizeof(*entry), GFP_KERNEL);

            if (!entry) {
               mutex_unlock(&mp3_lock);
               kfree(buf);
               return -ENOMEM;
            }

			is_empty = list_empty(&mp3_proc_list);

            entry->pid = pid;
            list_add_tail(&entry->list, &mp3_proc_list); //add the new entry to the end of the list
         	mutex_unlock(&mp3_lock);

			// Start delayed sampling only when transitioning from 0 to 1 tracked tasks.
			if(is_empty) {
				schedule_delayed_work(&mp3_work, msecs_to_jiffies(50));
			}

		break;

		case 'U':
			ret = sscanf(buf, "U %d", &pid);
			if (ret != 1 || pid <= 0) {
				kfree(buf);
				return -EINVAL;
			}

			mutex_lock(&mp3_lock);
			// Locate matching PID, remove node from list, and free its memory.
			list_for_each_entry_safe(entry, tmp, &mp3_proc_list, list) {
				if (entry->pid == pid) {
					list_del(&entry->list);
					kfree(entry);
					found = true;
					break;
				}
			}

			if (!found) {
				mutex_unlock(&mp3_lock);
				kfree(buf);
				return -ESRCH;
			}

			is_empty = list_empty(&mp3_proc_list);
			mutex_unlock(&mp3_lock);

			// Stop delayed sampling when the tracked process list becomes empty.
			if(is_empty) {
				cancel_delayed_work_sync(&mp3_work);
			}
			
			#if DEBUG
				pr_info("MP3 deregistered: pid=%d\n", pid);
			#endif
		break;

		default:
			kfree(buf);
			return -EINVAL;
	}

	kfree(buf);
	return count;
}


static void mp3_work_fn(struct work_struct *work) {
	struct mp3_proc *entry, *tmp;
	int ret = 0;
	unsigned long min_flt, maj_flt, utime, stime;
	unsigned long total_min = 0, total_maj = 0, total_cpu = 0;
	bool is_empty = false;

	/*
		Sample all registered tasks and aggregate one combined sample:
		soft faults, hard faults, and cpu time (utime + stime).
	*/
	mutex_lock(&mp3_lock);
	list_for_each_entry_safe(entry, tmp, &mp3_proc_list, list) {
		ret = get_cpu_use( entry->pid, &min_flt, &maj_flt, &utime, &stime);
		if (ret == 0) {
			total_min += min_flt;
			total_maj += maj_flt;
			total_cpu += utime + stime;
		} 
		else {
			pr_err("Failed to get CPU usage for PID %d: error code %d\n", entry->pid, ret);
			list_del(&entry->list);
			kfree(entry);
		}
	}

	/* Write one sample as four unsigned long values in queue order. */
	unsigned long *buffer = (unsigned long *)mp3_buffer;
	size_t base = sample_index * 4; //4 unsigned long values per sample
	buffer[base] = jiffies;
	buffer[base + 1] = total_min;
	buffer[base + 2] = total_maj;
	buffer[base + 3] = total_cpu;
	// Advance circular index so future samples overwrite oldest entries.
	sample_index = (sample_index + 1) % max_samples;

	is_empty = list_empty(&mp3_proc_list);

	mutex_unlock(&mp3_lock);

	/* Continue periodic sampling at 20Hz while list is non-empty. */
	if(!is_empty) {
		schedule_delayed_work(&mp3_work, msecs_to_jiffies(50)); //reschedule the work to run again after 50ms (20 times per second)
	}
}

static const struct proc_ops mp3_proc_ops = {
	.proc_open		= mp3_status_open,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release	= single_release,
	.proc_write		= mp3_status_write,
};

/*
	Module initialization.
	Creates proc entries, allocates and initializes profiler buffer,
	initializes delayed work, and registers the MP3 character device.
*/
int __init mp3_init(void)
{
	int ret;
	size_t i;

	#ifdef DEBUG
	printk(KERN_ALERT "MP3 MODULE LOADING\n");
	#endif

	// Create /proc/mp3 directory.
	mp3_dir = proc_mkdir("mp3", NULL); 
	if (!mp3_dir)
		return -ENOMEM;

	// Create /proc/mp3/status read/write proc entry.
	mp3_status = proc_create("status", 0666, mp3_dir, &mp3_proc_ops); 
	if (!mp3_status) {
		proc_remove(mp3_dir);
		return -ENOMEM;
	}

	mp3_buffer = vmalloc(MP3_BUFFER_SIZE);
	if(!mp3_buffer) {
		proc_remove(mp3_status);
		proc_remove(mp3_dir);
		return -ENOMEM;
	}

	memset(mp3_buffer, -1, MP3_BUFFER_SIZE);

	for(i = 0 ; i < MP3_BUFFER_SIZE; i += PAGE_SIZE) {
		struct page *page = vmalloc_to_page((char*)mp3_buffer + i);
		if(page)
			SetPageReserved(page);
	}
	sample_index = 0;
	INIT_DELAYED_WORK(&mp3_work, mp3_work_fn);

	ret = register_chrdev_region(mp3_dev, 1, "mp3");
	if (ret < 0) {
		mp3_free_buffer();
		proc_remove(mp3_status);
		proc_remove(mp3_dir);
		return ret;
	}

	cdev_init(&mp3_cdev, &mp3_fops);
	mp3_cdev.owner = THIS_MODULE;
	ret = cdev_add(&mp3_cdev, mp3_dev, 1);
	if (ret < 0) {
		unregister_chrdev_region(mp3_dev, 1);
		mp3_free_buffer();
		proc_remove(mp3_status);
		proc_remove(mp3_dir);
		return ret;
	}


	printk(KERN_ALERT "MP3 MODULE LOADED\n");
	return 0;   
}

/*
	Module cleanup.
	Unregisters character device, cancels delayed work, frees all list nodes,
	removes proc entries, and releases profiler buffer pages.
*/
void __exit mp3_exit(void)
{
   #ifdef DEBUG
   printk(KERN_ALERT "MP3 MODULE UNLOADING\n");
   #endif
   struct mp3_proc *entry, *tmp;

   cdev_del(&mp3_cdev);

   unregister_chrdev_region(mp3_dev, 1);

   cancel_delayed_work_sync(&mp3_work);
   
	mutex_lock(&mp3_lock);
    list_for_each_entry_safe(entry, tmp, &mp3_proc_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&mp3_lock);


   	if (mp3_status)
		proc_remove(mp3_status);
	if (mp3_dir)
		proc_remove(mp3_dir);

	mp3_free_buffer();


   printk(KERN_ALERT "MP3 MODULE UNLOADED\n");
}

// Register init and exit funtions
module_init(mp3_init);
module_exit(mp3_exit);
