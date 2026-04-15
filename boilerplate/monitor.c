#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>

#define DEVICE_NAME "container_monitor"

#define CMD_REGISTER 1
#define CMD_GET_STATS 2

struct stats {
    pid_t pid;
    unsigned long rss;
};

static dev_t dev_num;
static struct cdev monitor_cdev;
static struct class *monitor_class;

static pid_t monitored_pid = -1;

static long monitor_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct stats s;
    struct task_struct *task;

    switch (cmd) {
    case CMD_REGISTER:
        if (copy_from_user(&monitored_pid, (pid_t *)arg, sizeof(pid_t)))
            return -EFAULT;
        printk(KERN_INFO "monitor: registered pid %d\n", monitored_pid);
        break;

    case CMD_GET_STATS:
        s.pid = monitored_pid;
        s.rss = 0;

        if (monitored_pid > 0) {
            task = pid_task(find_vpid(monitored_pid), PIDTYPE_PID);
            if (task && task->mm)
      s.rss = task->mm->total_vm << (PAGE_SHIFT - 10);  // KB
  }

        if (copy_to_user((void *)arg, &s, sizeof(s)))
            return -EFAULT;
        break;

    default:
        return -EINVAL;
    }

    return 0;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

static int __init monitor_init(void)
{
    alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);

    cdev_init(&monitor_cdev, &fops);
    cdev_add(&monitor_cdev, dev_num, 1);

    monitor_class = class_create(DEVICE_NAME);
    device_create(monitor_class, NULL, dev_num, NULL, DEVICE_NAME);

    printk(KERN_INFO "monitor loaded\n");
    return 0;
}

static void __exit monitor_exit(void)
{
    device_destroy(monitor_class, dev_num);
    class_destroy(monitor_class);
    cdev_del(&monitor_cdev);
    unregister_chrdev_region(dev_num, 1);
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
