#include <linux/slab.h>
#include <linux/intrace.h>
#include <linux/printk.h>
#include <linux/types.h>

#define INTRACE_BUFFER_RING_NR_PAGES     1
#define INTRACE_BUFFER_RING_SIZE         (INTRACE_BUFFER_RING_NR_PAGES * PAGE_SIZE)
#define INTRACE_BUFFER_RING_NR_ENTRIES   (INTRACE_BUFFER_RING_SIZE / sizeof(u64))

#define intrace_buf_inc_ptr()            (intrace_buf->ptr = (intrace_buf->ptr + 1) % INTRACE_BUFFER_RING_NR_ENTRIES)
#define intrace_buf_dec_ptr()            (intrace_buf->ptr = ((intrace_buf->ptr - 1) % INTRACE_BUFFER_RING_NR_ENTRIES + INTRACE_BUFFER_RING_NR_ENTRIES)%INTRACE_BUFFER_RING_NR_ENTRIES);

struct intrace_buffer{
    u64* buff;
    u64  ptr;
} *intrace_buf;

void intrace_init(void){

    intrace_buf = kzalloc(sizeof(*intrace_buf), GFP_KERNEL);
    if(!intrace_buf){
        goto intrace_fail;
    }

    intrace_buf->buff = kzalloc(INTRACE_BUFFER_RING_SIZE, GFP_KERNEL);
    intrace_buf->ptr = 0;
    if(!intrace_buf->buff){
	goto intrace_fail_free;
    }	    


    pr_info("intrace: Initialised intrace buffer.");
 
    return;

intrace_fail_free:
    kfree(intrace_buf);

intrace_fail:
    pr_info("intrace: FAILED to allocate intrace buffer.");
    return;
}

void intrace_buf_put(u64 val){

    if(!intrace_buf) return;    // intrace_init() failed earlier.

    intrace_buf->buff[intrace_buf->ptr] = val;

    intrace_buf_inc_ptr();
}

u64 intrace_buf_get(void){

    if(!intrace_buf) return 0;

    intrace_buf_dec_ptr();

    return intrace_buf->buff[intrace_buf->ptr];
}