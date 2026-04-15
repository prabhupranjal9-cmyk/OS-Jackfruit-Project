#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/sched/signal.h>
#include "monitor_ioctl.h"
#include <linux/mm.h>

#define DEVICE_NAME "container_monitor"

static struct timer_list monitor_timer;

struct process_info {
    pid_t pid;
    size_t soft;
    size_t hard;
};

static struct process_info proc;


// ================= IOCTL =================
static long monitor_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;

    if (cmd == MONITOR_REGISTER) {
        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;

        proc.pid = req.pid;
        proc.soft = req.soft_limit_bytes;
        proc.hard = req.hard_limit_bytes;

        printk(KERN_INFO "monitor: registered PID=%d\n", proc.pid);
    }

    return 0;
}


// ================= CHECK MEMORY =================

      static void check_memory_usage(void)
{
    struct task_struct *task;

    for_each_process(task) {
        if (task->pid == proc.pid && task->mm) {

            unsigned long rss = get_mm_rss(task->mm) << PAGE_SHIFT;

            printk(KERN_INFO "monitor: PID %d using %lu bytes\n", proc.pid, rss);

            if (rss > proc.soft)
                printk(KERN_INFO "monitor: PID %d exceeded SOFT limit\n", proc.pid);

            if (rss > proc.hard) {
                printk(KERN_INFO "monitor: PID %d exceeded HARD limit -> KILLING\n", proc.pid);
                send_sig(SIGKILL, task, 0);
            }
        }
    }
}


// ================= TIMER =================
static void monitor_timer_fn(struct timer_list *t)
{
    printk(KERN_INFO "monitor: checking...\n");   // 🔥 MUST PRINT

    check_memory_usage();

    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(1000));
}


// ================= FILE OPS =================
static struct file_operations fops = {
    .unlocked_ioctl = monitor_ioctl,
};


// ================= INIT =================
static int __init monitor_init(void)
{
    printk(KERN_INFO "monitor: init\n");

    register_chrdev(240, DEVICE_NAME, &fops);

    timer_setup(&monitor_timer, monitor_timer_fn, 0);
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(1000));

    return 0;
}


// ================= EXIT =================
static void __exit monitor_exit(void)
{
    del_timer(&monitor_timer);
    unregister_chrdev(240, DEVICE_NAME);

    printk(KERN_INFO "monitor: exit\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
