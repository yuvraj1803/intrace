#include <linux/slab.h>
#include <linux/intrace.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/debugfs.h>
#include <linux/gfp_types.h>
#include <linux/fs.h>
#include <linux/kernel.h>

#include <asm/uaccess.h>

#define INTRACE_BUFFER_NR_PAGES              1
#define INTRACE_BUFFER_RING_SIZE             (INTRACE_BUFFER_NR_PAGES * PAGE_SIZE)
#define INTRACE_BUFFER_NR_ENTRIES            (INTRACE_BUFFER_RING_SIZE / sizeof(struct intrace_info))

#define INTRACE_BUFFER_ADVANCE()                                     (intracer->ptr = (intracer->ptr == INTRACE_BUFFER_NR_ENTRIES) ? 0 : intracer->ptr + 1)
#define DEFINE_INTRACE_DEBUGFS_FILE(_name, _data, _mode, _fops)      {.name=(const char*)_name, .data=(void*)_data, .mode=(umode_t)_mode, .fops=(struct file_operations*)_fops, .file=(struct dentry*)NULL}

static bool intrace_enabled;
static struct intrace_tracer* intracer;


bool is_intrace_enabled(void){
    return intrace_enabled;
}

void disable_intrace(void){
    intrace_enabled = false;
}

void enable_intrace(void){
    intrace_enabled = true;
}

ssize_t intrace_change_state_write(struct file * filep, const char __user * ubuf, size_t cnt, loff_t * ppos){

    char command[10];
    ssize_t r = cnt;

    if(cnt > 10) cnt = 10; // we only entertain "enable" or "disable" as inputs.

    if(copy_from_user(command, ubuf, cnt)){
        return -EINVAL;
    }

    command[cnt] = 0;

    char* __command = strim(command);

    if(!strcmp(__command, "enable"))  enable_intrace();
    if(!strcmp(__command, "disable")) disable_intrace();

    *ppos += r;
    return r;

}


static struct file_operations intrace_change_state_fops = {
    .write = intrace_change_state_write,
};

ssize_t intrace_state_read(struct file *file , char __user * ubuf, size_t cnt, loff_t* ppos){

    int r;
    char state[10]; // can either be enabled or disabled

    r = sprintf(state, "%s\n",is_intrace_enabled() ? "enabled" : "disabled");

    return simple_read_from_buffer(ubuf, cnt, ppos, state, r);
}


static struct file_operations intrace_state_fops = {
    .read = intrace_state_read,
};

static struct intrace_debugfs_file intrace_debugfs_files[] = {
    DEFINE_INTRACE_DEBUGFS_FILE("state", 0, 400, &intrace_state_fops),
    DEFINE_INTRACE_DEBUGFS_FILE("change_state", 0, 400, &intrace_change_state_fops)
};



static void __init intrace_debugfs_init(void){

    intracer->dir = debugfs_create_dir("intrace", NULL);
    if(intracer->dir == ERR_PTR(-ENODEV)){
        goto intrace_debugfs_fail;
    }

    for(int i = 0; i < ARRAY_SIZE(intrace_debugfs_files); i++){
        struct dentry* file = debugfs_create_file(
            intrace_debugfs_files[i].name,
            intrace_debugfs_files[i].mode,
            intracer->dir,  // parent directory in debugfs.
            intrace_debugfs_files[i].data,
            intrace_debugfs_files[i].fops
        );

        if(file == ERR_PTR(-ENODEV)){ // we free all the files and abort even if one file fails to get into debugfs.
            pr_info("intrace: failed to create file intrace/%s under debugfs. debugfs seems to be disabled.\n", intrace_debugfs_files[i].name);
            goto intrace_debugfs_free_everything;
        }

        intrace_debugfs_files[i].file = file;
    }

    goto out;

intrace_debugfs_fail:
    pr_info("intrace: Failed to create debugfs object. debugfs seems to be disabled.\n");

intrace_debugfs_free_everything:
    debugfs_remove_recursive(intracer->dir);    // if the dentry passed is NULL or an error pointer, nothing will be done. so no need to check the args against anything.

out:
    return;

}

static int __init intrace_init(void)
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

    intrace_debugfs_init();
    enable_intrace();

    pr_info("intrace: Initialized intrace buffer.");
 
    goto out;

intrace_fail:
    pr_info("intrace: FAILED to allocate intrace buffer.");

out:
    return 0;
}

late_initcall(intrace_init)

void intrace_buf_put(struct irq_domain* domain, struct irq_desc* desc)
{

    if(!intracer) return;    // intrace_init() failed earlier.

    spin_lock(&intracer->lock);

    ((struct intrace_info*) (intracer->buff + intracer->ptr))->domain = domain;
    ((struct intrace_info*) (intracer->buff + intracer->ptr))->desc = desc;

    spin_unlock(&intracer->lock);

    INTRACE_BUFFER_ADVANCE();

}



