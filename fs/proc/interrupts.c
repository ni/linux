#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irqnr.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

/*
 * /proc/interrupts
 */
static void *int_seq_start(struct seq_file *f, loff_t *pos)
{
	return (*pos <= nr_irqs) ? pos : NULL;
}

static void *int_seq_next(struct seq_file *f, void *v, loff_t *pos)
{
	(*pos)++;
	if (*pos > nr_irqs)
		return NULL;
	return pos;
}

static void int_seq_stop(struct seq_file *f, void *v)
{
	/* Nothing to do */
}

static const struct seq_operations int_seq_ops = {
	.start = int_seq_start,
	.next  = int_seq_next,
	.stop  = int_seq_stop,
	.show  = show_interrupts
};

struct interrupts_fd_state {
	atomic_long_t last_irq_change_count;
};

static int interrupts_open(struct inode *inode, struct file *filp)
{
	int res;
	struct interrupts_fd_state *privdata;
	struct seq_file *sf;

	privdata = kzalloc(sizeof(struct interrupts_fd_state), GFP_KERNEL);
	if (!privdata) {
		res = -ENOMEM;
		goto exit;
	}

	res = seq_open(filp, &int_seq_ops);
	if (res) {
		kfree(privdata);
		goto exit;
	}

	sf = filp->private_data;
	sf->private = privdata;

	atomic_long_set(&(privdata->last_irq_change_count),
		get_irq_handler_change_count());

exit:
	return res;
}

static int interrupts_release(struct inode *inode, struct file *filp)
{
	struct seq_file *sf = filp->private_data;

	kfree(sf->private);
	return seq_release(inode, filp);
}

static unsigned int interrupts_poll(struct file *filp,
	struct poll_table_struct *pt)
{
	unsigned int mask = POLLIN | POLLRDNORM;
	long newcount, oldcount;
	struct seq_file *sf = filp->private_data;
	struct interrupts_fd_state *fds = sf->private;

	/* Register for changes to IRQ handlers */
	poll_wait(filp, &irq_handler_change_wq, pt);

	/* Store new change count in priv data */
	newcount = get_irq_handler_change_count();
	oldcount = atomic_long_xchg(
		&(fds->last_irq_change_count), newcount);

	if (newcount != oldcount)
		mask |= POLLERR | POLLPRI;

	return mask;
}

static const struct file_operations proc_interrupts_operations = {
	.open		= interrupts_open,
	.read		= seq_read,
	.poll		= interrupts_poll,
	.llseek		= seq_lseek,
	.release	= interrupts_release,
};

static int __init proc_interrupts_init(void)
{
	proc_create("interrupts", 0, NULL, &proc_interrupts_operations);
	return 0;
}
fs_initcall(proc_interrupts_init);
