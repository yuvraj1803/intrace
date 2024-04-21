#include <linux/slab.h>
#include <linux/intrace.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/gfp_types.h>

#define INTRACE_BUFFER_NR_PAGES     1
#define INTRACE_BUFFER_RING_SIZE    (INTRACE_BUFFER_NR_PAGES * PAGE_SIZE)
#define INTRACE_BUFFER_NR_ENTRIES   (INTRACE_BUFFER_RING_SIZE / sizeof(struct intrace_info))

static struct intrace_buffer* intrace_buf;

#define INTRACE_BUFFER_ADVANCE()    (intrace_buf->ptr = (intrace_buf->ptr == INTRACE_BUFFER_NR_ENTRIES) ? 0 : intrace_buf->ptr + 1)


void intrace_init(void)
{

    intrace_buf = kzalloc(sizeof(*intrace_buf), GFP_KERNEL);
    if(!intrace_buf){
        goto intrace_fail;
    }

    intrace_buf->buff   = kzalloc(INTRACE_BUFFER_RING_SIZE, GFP_KERNEL);
    intrace_buf->ptr    = 0;
    spin_lock_init(&intrace_buf->lock);

    if(!intrace_buf->buff){
	    kfree(intrace_buf);
	    goto intrace_fail;
    }	    


    pr_info("intrace: Initialized intrace buffer.");
 
    goto out;

intrace_fail:
    pr_info("intrace: FAILED to allocate intrace buffer.");

out:
    return;
}

void intrace_buf_put(struct irq_domain* domain, struct irq_desc* desc)
{

    if(!intrace_buf) return;    // intrace_init() failed earlier.

    spin_lock(&intrace_buf->lock);

    ((struct intrace_info*) (intrace_buf->buff + intrace_buf->ptr))->domain = domain;
    ((struct intrace_info*) (intrace_buf->buff + intrace_buf->ptr))->desc = desc;

    spin_unlock(&intrace_buf->lock);

    INTRACE_BUFFER_ADVANCE();

}


