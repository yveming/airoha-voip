// SPDX-License-Identifier: GPL-2.0-only
/*
 * Software-only FXS line used to exercise fxs-core without hardware.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/fs.h>

#include "fxs.h"

static bool dummy_hook;
module_param(dummy_hook, bool, 0644);
MODULE_PARM_DESC(dummy_hook, "Simulated hook state (0 = on-hook, 1 = off-hook)");

static int dummy_set_ring(struct fxs_line *line, bool on)
{
	pr_info("fxs-dummy: ring %s\n", on ? "on" : "off");
	return 0;
}

static int dummy_set_active(struct fxs_line *line, bool on)
{
	pr_info("fxs-dummy: active %s\n", on ? "on" : "off");
	return 0;
}

static int dummy_get_hook(struct fxs_line *line, bool *offhook)
{
	*offhook = dummy_hook;
	return 0;
}

static int dummy_set_profile(struct fxs_line *line,
			     const struct fxs_profile *profile)
{
	return 0;
}

static int dummy_pcm_open(struct fxs_line *line, struct file *file)
{
	pr_info("fxs-dummy: PCM open\n");
	return 0;
}

static void dummy_pcm_close(struct fxs_line *line, struct file *file)
{
	pr_info("fxs-dummy: PCM close\n");
}

static ssize_t dummy_pcm_read(struct fxs_line *line, struct file *file,
			      char __user *buf, size_t count, loff_t *ppos)
{
	if (count < FXS_PCM_FRAME_BYTES)
		return -EINVAL;
	if (clear_user(buf, FXS_PCM_FRAME_BYTES))
		return -EFAULT;
	return FXS_PCM_FRAME_BYTES;
}

static ssize_t dummy_pcm_write(struct fxs_line *line, struct file *file,
			       const char __user *buf, size_t count,
			       loff_t *ppos)
{
	return count;
}

static __poll_t dummy_pcm_poll(struct fxs_line *line, struct file *file,
			       poll_table *wait)
{
	return EPOLLIN | EPOLLOUT;
}

static const struct fxs_slic_ops dummy_slic_ops = {
	.caps		= FXS_CAP_RING | FXS_CAP_HOOK | FXS_CAP_GAIN,
	.set_ring	= dummy_set_ring,
	.set_active	= dummy_set_active,
	.get_hook	= dummy_get_hook,
	.set_profile	= dummy_set_profile,
};

static const struct fxs_pcm_ops dummy_pcm_ops = {
	.open	= dummy_pcm_open,
	.close	= dummy_pcm_close,
	.read	= dummy_pcm_read,
	.write	= dummy_pcm_write,
	.poll	= dummy_pcm_poll,
};

static struct fxs_line *dummy_line;

static int __init fxs_dummy_init(void)
{
	dummy_line = fxs_register_line(NULL, "dummy0", THIS_MODULE,
				       &dummy_slic_ops, NULL,
				       &dummy_pcm_ops, NULL);
	if (IS_ERR(dummy_line))
		return PTR_ERR(dummy_line);

	return 0;
}

static void __exit fxs_dummy_exit(void)
{
	fxs_unregister_line(dummy_line);
	dummy_line = NULL;
}

module_init(fxs_dummy_init);
module_exit(fxs_dummy_exit);

MODULE_INFO(author, "ming");
MODULE_INFO(description, "FXS dummy line for testing");
MODULE_LICENSE("GPL");
