/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AN7581_PCM_H
#define _AN7581_PCM_H

#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/types.h>

bool an7581_pcm_fxs_ready(void);
int an7581_pcm_fxs_open(struct file *file);
void an7581_pcm_fxs_close(struct file *file);
ssize_t an7581_pcm_fxs_read(struct file *file, char __user *buf,
			    size_t count, loff_t *ppos);
ssize_t an7581_pcm_fxs_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos);
__poll_t an7581_pcm_fxs_poll(struct file *file, poll_table *wait);
int an7581_pcm_fxs_set_echo_cancel(bool on);

#endif /* _AN7581_PCM_H */
