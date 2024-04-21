#include <linux/slab.h>
#include <linux/intrace.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/gfp_types.h>

#define INTRACE_BUFFER_NR_PAGES     1
#define INTRACE_BUFFER_RING_SIZE    (INTRACE_BUFFER_NR_PAGES * PAGE_SIZE)
#define INTRACE_BUFFER_NR_ENTRIES   (INTRACE_BUFFER_RING_SIZE / sizeof(struct intrace_info))

static struct intrace_tracer* intracer;

#define INTRACE_BUFFER_ADVANCE()    (intracer->ptr = (intracer->ptr == INTRACE_BUFFER_NR_ENTRIES) ? 0 : intracer->ptr + 1)


void intrace_init(void)
{

    intracer = kzalloc(sizeof(*intracer), GFP_KERNEL);
    if(!intracer){
        goto intrace_fail;
    }

    intracer->buff   = kzalloc(INTRACE_BUFFER_RING_SIZE, GFP_KERNEL);
    intracer->ptr    = 0;
    spin_lock_init(&intracer->lock);

    if(!intracer->buff){
	    kfree(intracer);
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

    if(!intracer) return;    // intrace_init() failed earlier.

    spin_lock(&intracer->lock);

    ((struct intrace_info*) (intracer->buff + intracer->ptr))->domain = domain;
    ((struct intrace_info*) (intracer->buff + intracer->ptr))->desc = desc;

    spin_unlock(&intracer->lock);

    INTRACE_BUFFER_ADVANCE();

}


