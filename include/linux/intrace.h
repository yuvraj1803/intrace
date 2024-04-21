#ifndef __INTRACE_H__
#define __INTRACE_H__

#include <linux/types.h>
#include <linux/spinlock.h>

struct irq_domain;
struct irq_desc;
struct irq_common_data;

void intrace_init(void);
void intrace_buf_put(struct irq_domain*, struct irq_desc*);

struct intrace_info{
    struct irq_domain*      domain;
    struct irq_desc*        desc;
};

struct intrace_buffer{
    struct intrace_info*     buff;
    u64                      ptr;
    spinlock_t               lock;
};

#endif