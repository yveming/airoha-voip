// SPDX-License-Identifier: GPL-2.0-only
/*
 * AN7581 FXS provider for fxs-core.
 *
 * The PCM engine and the SLIC backend keep their register-level code; this
 * file binds the PCM engine and whichever SLIC backend detects the hardware
 * to one generic FXS line.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/poll.h>

#include "fxs.h"

#include "an7581_pcm.h"

/* an7581-slic.c */
extern const struct fxs_slic_ops *an7581_slic_get_ops(void **priv);
extern void an7581_slic_put(void);

static int an7581_pcm_open(struct fxs_line *line, struct file *file)
{
	return an7581_pcm_fxs_open(file);
}

static void an7581_pcm_close(struct fxs_line *line, struct file *file)
{
	an7581_pcm_fxs_close(file);
}

static ssize_t an7581_pcm_read(struct fxs_line *line, struct file *file,
			       char __user *buf, size_t count, loff_t *ppos)
{
	return an7581_pcm_fxs_read(file, buf, count, ppos);
}

static ssize_t an7581_pcm_write(struct fxs_line *line, struct file *file,
				const char __user *buf, size_t count,
				loff_t *ppos)
{
	return an7581_pcm_fxs_write(file, buf, count, ppos);
}

static __poll_t an7581_pcm_poll(struct fxs_line *line, struct file *file,
				poll_table *wait)
{
	return an7581_pcm_fxs_poll(file, wait);
}

static int an7581_pcm_set_echo_cancel(struct fxs_line *line, bool on)
{
	return an7581_pcm_fxs_set_echo_cancel(on);
}

static const struct fxs_pcm_ops an7581_pcm_ops = {
	.open		= an7581_pcm_open,
	.close		= an7581_pcm_close,
	.read		= an7581_pcm_read,
	.write		= an7581_pcm_write,
	.poll		= an7581_pcm_poll,
	.set_echo_cancel = an7581_pcm_set_echo_cancel,
};

static char *line_name = "line1";
module_param(line_name, charp, 0444);
MODULE_PARM_DESC(line_name, "FXS line name exposed through fxs-core");

static struct fxs_line *an7581_line;

static int __init an7581_fxs_init(void)
{
	const struct fxs_slic_ops *slic_ops;
	void *slic_priv = NULL;

	if (!an7581_pcm_fxs_ready())
		return -ENODEV;

	slic_ops = an7581_slic_get_ops(&slic_priv);
	if (IS_ERR(slic_ops)) {
		pr_err("an7581-fxs: no SLIC ops\n");
		return PTR_ERR(slic_ops);
	}

	an7581_line = fxs_register_line(NULL, line_name, THIS_MODULE,
					slic_ops, slic_priv,
					&an7581_pcm_ops, NULL);
	if (IS_ERR(an7581_line)) {
		an7581_slic_put();
		return PTR_ERR(an7581_line);
	}

	pr_info("an7581-fxs: %s line\n", line_name);
	return 0;
}

static void __exit an7581_fxs_exit(void)
{
	fxs_unregister_line(an7581_line);
	an7581_line = NULL;
	an7581_slic_put();
}

module_init(an7581_fxs_init);
module_exit(an7581_fxs_exit);

MODULE_INFO(author, "ming");
MODULE_INFO(description, "AN7581 FXS line provider");
MODULE_LICENSE("GPL");
