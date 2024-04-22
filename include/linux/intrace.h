#ifndef __INTRACE_H__
#define __INTRACE_H__

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/debugfs.h>
#include <linux/fs.h>

struct irq_domain;
struct irq_desc;
struct irq_common_data;

void intrace_buf_put(struct irq_domain*, struct irq_desc*);

struct intrace_info{
    struct irq_domain*      domain;
    struct irq_desc*        desc;
};

struct intrace_tracer{
    struct intrace_info*     buff;
    unsigned long long       ptr;
    spinlock_t               lock;
    struct dentry*           dir;   // dir in debugfs.
};

struct intrace_debugfs_file{
    const char* name;
    void* data;
    umode_t mode;
    const struct file_operations* fops;  
    struct dentry*          file;   // file in debugfs under intrace_tracer->dir (parent)
};

#endif