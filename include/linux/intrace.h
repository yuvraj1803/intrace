#ifndef __INTRACE_H__
#define __INTRACE_H__

#include <linux/types.h>


void intrace_init(void);
void intrace_buf_put(u64 val);
u64 intrace_buf_get(void);

#endif