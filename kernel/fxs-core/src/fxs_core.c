// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generic FXS line core.
 *
 * A line is a logical FXS port.  The board/SLIC provider supplies line
 * control (ring, active, hook, profile) and the PCM provider supplies the
 * 8 kHz s16le audio stream.  The core owns the character devices:
 *
 *   /dev/fxsN       control and events
 *   /dev/fxsN-pcm   PCM audio
 *
 * The core does not know whether the line is backed by Le9641, PEF32001,
 * Si3219x or a software dummy.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/idr.h>
#include <linux/kfifo.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/capability.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/device.h>
#include <linux/list.h>

#include "fxs.h"

#define FXS_EVENT_FIFO_SIZE	64

struct fxs_line {
	struct device		*dev;
	struct device		*class_dev;
	struct module		*owner;
	char			*name;
	int			index;

	char			ctl_name[16];
	char			pcm_name[20];

	struct miscdevice	ctl_misc;
	struct miscdevice	pcm_misc;

	const struct fxs_slic_ops *slic;
	void			*slic_priv;
	const struct fxs_pcm_ops *pcm;
	void			*pcm_priv;

	struct mutex		lock;		/* line state */
	spinlock_t		event_lock;	/* event fifo */
	DECLARE_KFIFO_PTR(event_fifo, struct fxs_event);
	wait_queue_head_t	event_wait;
	wait_queue_head_t	release_wait;

	atomic_t		ctl_open;
	atomic_t		pcm_open;
	refcount_t		refs;

	bool			claimed;
	pid_t			claim_pid;
	char			claim_comm[TASK_COMM_LEN];

	u32			state;
	bool			ringing;
	bool			active;
	bool			removing;

	struct list_head	list;
};

static DEFINE_IDA(fxs_ida);
static LIST_HEAD(fxs_lines);
static DEFINE_MUTEX(fxs_lines_lock);

static struct class *fxs_class;

static void fxs_line_put(struct fxs_line *line)
{
	module_put(line->owner);
	if (refcount_dec_and_test(&line->refs))
		wake_up_all(&line->release_wait);
}

static int fxs_ctl_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct fxs_line *line = container_of(misc, struct fxs_line, ctl_misc);

	if (!try_module_get(line->owner))
		return -ENODEV;
	if (!refcount_inc_not_zero(&line->refs)) {
		module_put(line->owner);
		return -ENODEV;
	}
	if (atomic_cmpxchg(&line->ctl_open, 0, 1) != 0) {
		fxs_line_put(line);
		return -EBUSY;
	}

	file->private_data = line;
	return 0;
}

static int fxs_ctl_release(struct inode *inode, struct file *file)
{
	struct fxs_line *line = file->private_data;
	int ret = 0;

	if (!line)
		return 0;

	mutex_lock(&line->lock);
	if (line->ringing && line->slic && line->slic->set_ring)
		ret = line->slic->set_ring(line, false);
	if (line->active && line->slic && line->slic->set_active)
		line->slic->set_active(line, false);
	line->ringing = false;
	line->active = false;
	line->state = FXS_STATE_IDLE;
	line->claimed = false;
	line->claim_pid = 0;
	mutex_unlock(&line->lock);

	atomic_set(&line->ctl_open, 0);
	file->private_data = NULL;
	fxs_line_put(line);

	return ret;
}

static int fxs_pcm_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct fxs_line *line = container_of(misc, struct fxs_line, pcm_misc);
	int ret;

	if (!try_module_get(line->owner))
		return -ENODEV;
	if (!refcount_inc_not_zero(&line->refs)) {
		module_put(line->owner);
		return -ENODEV;
	}
	if (atomic_cmpxchg(&line->pcm_open, 0, 1) != 0) {
		fxs_line_put(line);
		return -EBUSY;
	}
	if (!line->pcm || !line->pcm->open) {
		ret = -ENODEV;
		goto err;
	}

	ret = line->pcm->open(line, file);
	if (ret)
		goto err;

	file->private_data = line;
	return 0;

err:
	atomic_set(&line->pcm_open, 0);
	fxs_line_put(line);
	return ret;
}

static int fxs_pcm_release(struct inode *inode, struct file *file)
{
	struct fxs_line *line = file->private_data;

	if (!line)
		return 0;

	if (line->pcm && line->pcm->close)
		line->pcm->close(line, file);

	atomic_set(&line->pcm_open, 0);
	file->private_data = NULL;
	fxs_line_put(line);

	return 0;
}

static ssize_t fxs_ctl_read(struct file *file, char __user *buf,
			    size_t count, loff_t *ppos)
{
	struct fxs_line *line = file->private_data;
	struct fxs_event event;
	unsigned long flags;
	int ret;

	if (!line)
		return -ENODEV;
	if (count < sizeof(event))
		return -EINVAL;

	for (;;) {
		spin_lock_irqsave(&line->event_lock, flags);
		if (kfifo_out(&line->event_fifo, &event, 1) == 1) {
			spin_unlock_irqrestore(&line->event_lock, flags);
			break;
		}
		spin_unlock_irqrestore(&line->event_lock, flags);

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (READ_ONCE(line->removing))
			return 0;

		ret = wait_event_interruptible(line->event_wait,
				!kfifo_is_empty(&line->event_fifo) ||
				READ_ONCE(line->removing));
		if (ret)
			return ret;
		if (READ_ONCE(line->removing) &&
		    kfifo_is_empty(&line->event_fifo))
			return 0;
	}

	if (copy_to_user(buf, &event, sizeof(event)))
		return -EFAULT;

	return sizeof(event);
}

static __poll_t fxs_ctl_poll(struct file *file, poll_table *wait)
{
	struct fxs_line *line = file->private_data;
	__poll_t mask = 0;

	if (!line)
		return EPOLLERR;

	poll_wait(file, &line->event_wait, wait);
	if (!kfifo_is_empty(&line->event_fifo))
		mask |= EPOLLIN;

	return mask;
}

static int fxs_get_caps(struct fxs_line *line, unsigned long arg)
{
	struct fxs_caps caps;

	memset(&caps, 0, sizeof(caps));
	caps.version = 1;
	caps.caps = line->slic ? line->slic->caps : 0;
	caps.pcm_rate = FXS_PCM_RATE;
	caps.pcm_channels = FXS_PCM_CHANNELS;
	caps.pcm_bits = FXS_PCM_BITS;
	caps.pcm_frame_bytes = FXS_PCM_FRAME_BYTES;
	caps.max_events = FXS_EVENT_FIFO_SIZE;

	if (copy_to_user((void __user *)arg, &caps, sizeof(caps)))
		return -EFAULT;

	return 0;
}

static int fxs_get_status(struct fxs_line *line, unsigned long arg)
{
	struct fxs_status status;
	bool hook = false;
	int ret = 0;

	memset(&status, 0, sizeof(status));

	mutex_lock(&line->lock);
	status.state = line->state;
	status.ringing = line->ringing;
	status.active = line->active;
	if (line->slic && line->slic->get_hook)
		ret = line->slic->get_hook(line, &hook);
	status.hook = hook;
	mutex_unlock(&line->lock);

	if (ret)
		return ret;

	if (copy_to_user((void __user *)arg, &status, sizeof(status)))
		return -EFAULT;

	return 0;
}

static int fxs_set_ring(struct fxs_line *line, unsigned long arg)
{
	struct fxs_event event;
	int val;
	int ret;

	if (get_user(val, (int __user *)arg))
		return -EFAULT;
	val = !!val;

	mutex_lock(&line->lock);
	if (!line->slic || !line->slic->set_ring) {
		mutex_unlock(&line->lock);
		return -EOPNOTSUPP;
	}
	ret = line->slic->set_ring(line, val);
	if (!ret) {
		line->ringing = val;
		if (val)
			line->state = FXS_STATE_RINGING;
		else if (line->active)
			line->state = FXS_STATE_ACTIVE;
		else
			line->state = FXS_STATE_IDLE;
	}
	mutex_unlock(&line->lock);

	if (!ret) {
		event.type = FXS_EV_RING;
		event.value = val;
		event.flags = 0;
		event.reserved = 0;
		fxs_event_push(line, &event);
	}

	return ret;
}

static int fxs_set_active(struct fxs_line *line, unsigned long arg)
{
	int val;
	int ret;

	if (get_user(val, (int __user *)arg))
		return -EFAULT;
	val = !!val;

	mutex_lock(&line->lock);
	if (!line->slic || !line->slic->set_active) {
		mutex_unlock(&line->lock);
		return -EOPNOTSUPP;
	}
	ret = line->slic->set_active(line, val);
	if (!ret) {
		line->active = val;
		if (val)
			line->state = FXS_STATE_ACTIVE;
		else if (line->ringing)
			line->state = FXS_STATE_RINGING;
		else
			line->state = FXS_STATE_IDLE;
	}
	mutex_unlock(&line->lock);

	return ret;
}

static int fxs_get_hook(struct fxs_line *line, unsigned long arg)
{
	bool hook = false;
	int ret;

	mutex_lock(&line->lock);
	if (!line->slic || !line->slic->get_hook)
		ret = -EOPNOTSUPP;
	else
		ret = line->slic->get_hook(line, &hook);
	mutex_unlock(&line->lock);

	if (ret)
		return ret;

	return put_user(hook ? 1 : 0, (int __user *)arg);
}

static int fxs_set_profile(struct fxs_line *line, unsigned long arg)
{
	struct fxs_profile profile;
	int ret;

	if (copy_from_user(&profile, (void __user *)arg, sizeof(profile)))
		return -EFAULT;
	if (profile.version != 1)
		return -EINVAL;

	mutex_lock(&line->lock);
	if (!line->slic || !line->slic->set_profile)
		ret = -EOPNOTSUPP;
	else
		ret = line->slic->set_profile(line, &profile);
	mutex_unlock(&line->lock);

	return ret;
}

static int fxs_get_event(struct fxs_line *line, unsigned long arg)
{
	struct fxs_event event;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&line->event_lock, flags);
	if (kfifo_out(&line->event_fifo, &event, 1) != 1)
		ret = -EAGAIN;
	spin_unlock_irqrestore(&line->event_lock, flags);

	if (ret)
		return ret;
	if (copy_to_user((void __user *)arg, &event, sizeof(event)))
		return -EFAULT;

	return 0;
}

static int fxs_claim(struct fxs_line *line)
{
	pid_t pid = task_pid_nr(current);
	int ret = 0;

	mutex_lock(&line->lock);
	if (line->claimed && line->claim_pid != pid &&
	    !capable(CAP_SYS_ADMIN)) {
		ret = -EBUSY;
	} else {
		line->claimed = true;
		line->claim_pid = pid;
		get_task_comm(line->claim_comm, current);
	}
	mutex_unlock(&line->lock);

	return ret;
}

static int fxs_release(struct fxs_line *line)
{
	pid_t pid = task_pid_nr(current);
	int ret = 0;

	mutex_lock(&line->lock);
	if (line->claimed && line->claim_pid != pid &&
	    !capable(CAP_SYS_ADMIN))
		ret = -EPERM;
	else {
		line->claimed = false;
		line->claim_pid = 0;
	}
	mutex_unlock(&line->lock);

	return ret;
}

static int fxs_get_line_name(struct fxs_line *line, unsigned long arg)
{
	size_t len = strnlen(line->name, FXS_LINE_NAME_MAX - 1) + 1;

	if (copy_to_user((void __user *)arg, line->name, len))
		return -EFAULT;

	return 0;
}

static int fxs_set_echo_cancel(struct fxs_line *line, unsigned long arg)
{
	int val;

	if (get_user(val, (int __user *)arg))
		return -EFAULT;
	if (!line->pcm || !line->pcm->set_echo_cancel)
		return -EOPNOTSUPP;

	return line->pcm->set_echo_cancel(line, !!val);
}

static int fxs_do_ioctl(struct fxs_line *line, unsigned int cmd,
			unsigned long arg, bool control)
{
	switch (cmd) {
	case FXS_IOC_GET_CAPS:
		return fxs_get_caps(line, arg);
	case FXS_IOC_GET_STATUS:
		return fxs_get_status(line, arg);
	case FXS_IOC_GET_EVENT:
		return fxs_get_event(line, arg);
	case FXS_IOC_GET_LINE_NAME:
		return fxs_get_line_name(line, arg);
	case FXS_IOC_SET_RING:
		if (!control)
			return -ENOTTY;
		return fxs_set_ring(line, arg);
	case FXS_IOC_SET_ACTIVE:
		if (!control)
			return -ENOTTY;
		return fxs_set_active(line, arg);
	case FXS_IOC_GET_HOOK:
		if (!control)
			return -ENOTTY;
		return fxs_get_hook(line, arg);
	case FXS_IOC_SET_PROFILE:
		if (!control)
			return -ENOTTY;
		return fxs_set_profile(line, arg);
	case FXS_IOC_CLAIM:
		if (!control)
			return -ENOTTY;
		return fxs_claim(line);
	case FXS_IOC_RELEASE:
		if (!control)
			return -ENOTTY;
		return fxs_release(line);
	case FXS_IOC_SET_ECHO_CANCEL:
		if (!control)
			return -ENOTTY;
		return fxs_set_echo_cancel(line, arg);
	default:
		return -ENOTTY;
	}
}

static long fxs_ctl_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	struct fxs_line *line = file->private_data;

	if (!line)
		return -ENODEV;

	return fxs_do_ioctl(line, cmd, arg, true);
}

static long fxs_pcm_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	struct fxs_line *line = file->private_data;

	if (!line)
		return -ENODEV;

	return fxs_do_ioctl(line, cmd, arg, false);
}

static ssize_t fxs_pcm_read(struct file *file, char __user *buf,
			    size_t count, loff_t *ppos)
{
	struct fxs_line *line = file->private_data;

	if (!line || !line->pcm || !line->pcm->read)
		return -ENODEV;

	return line->pcm->read(line, file, buf, count, ppos);
}

static ssize_t fxs_pcm_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct fxs_line *line = file->private_data;

	if (!line || !line->pcm || !line->pcm->write)
		return -ENODEV;

	return line->pcm->write(line, file, buf, count, ppos);
}

static __poll_t fxs_pcm_poll(struct file *file, poll_table *wait)
{
	struct fxs_line *line = file->private_data;

	if (!line || !line->pcm || !line->pcm->poll)
		return EPOLLERR;

	return line->pcm->poll(line, file, wait);
}

static const struct file_operations fxs_ctl_fops = {
	.owner		= THIS_MODULE,
	.open		= fxs_ctl_open,
	.release	= fxs_ctl_release,
	.read		= fxs_ctl_read,
	.poll		= fxs_ctl_poll,
	.unlocked_ioctl	= fxs_ctl_ioctl,
	.llseek		= noop_llseek,
};

static const struct file_operations fxs_pcm_fops = {
	.owner		= THIS_MODULE,
	.open		= fxs_pcm_open,
	.release	= fxs_pcm_release,
	.read		= fxs_pcm_read,
	.write		= fxs_pcm_write,
	.poll		= fxs_pcm_poll,
	.unlocked_ioctl	= fxs_pcm_ioctl,
	.llseek		= noop_llseek,
};

struct fxs_line *fxs_register_line(struct device *dev, const char *name,
				   struct module *owner,
				   const struct fxs_slic_ops *slic,
				   void *slic_priv,
				   const struct fxs_pcm_ops *pcm,
				   void *pcm_priv)
{
	struct fxs_line *line;
	int id;
	int ret;

	if (!name || !slic)
		return ERR_PTR(-EINVAL);

	line = kzalloc(sizeof(*line), GFP_KERNEL);
	if (!line)
		return ERR_PTR(-ENOMEM);

	line->name = kstrdup(name, GFP_KERNEL);
	if (!line->name) {
		ret = -ENOMEM;
		goto err_free;
	}

	line->dev = dev;
	line->owner = owner ? owner : THIS_MODULE;
	line->slic = slic;
	line->slic_priv = slic_priv;
	line->pcm = pcm;
	line->pcm_priv = pcm_priv;
	line->state = FXS_STATE_IDLE;
	mutex_init(&line->lock);
	spin_lock_init(&line->event_lock);
	init_waitqueue_head(&line->event_wait);
	init_waitqueue_head(&line->release_wait);
	atomic_set(&line->ctl_open, 0);
	atomic_set(&line->pcm_open, 0);
	refcount_set(&line->refs, 1);

	ret = kfifo_alloc(&line->event_fifo, FXS_EVENT_FIFO_SIZE, GFP_KERNEL);
	if (ret)
		goto err_name;

	id = ida_alloc_max(&fxs_ida, FXS_MAX_LINES - 1, GFP_KERNEL);
	if (id < 0) {
		ret = id;
		goto err_fifo;
	}
	line->index = id;

	snprintf(line->ctl_name, sizeof(line->ctl_name), "fxs%d", id);
	snprintf(line->pcm_name, sizeof(line->pcm_name), "fxs%d-pcm", id);

	line->ctl_misc.minor = MISC_DYNAMIC_MINOR;
	line->ctl_misc.name = line->ctl_name;
	line->ctl_misc.fops = &fxs_ctl_fops;
	line->ctl_misc.mode = 0600;
	line->ctl_misc.parent = dev;

	line->pcm_misc.minor = MISC_DYNAMIC_MINOR;
	line->pcm_misc.name = line->pcm_name;
	line->pcm_misc.fops = &fxs_pcm_fops;
	line->pcm_misc.mode = 0600;
	line->pcm_misc.parent = dev;

	ret = misc_register(&line->ctl_misc);
	if (ret)
		goto err_ida;

	ret = misc_register(&line->pcm_misc);
	if (ret)
		goto err_ctl;

	mutex_lock(&fxs_lines_lock);
	list_add_tail(&line->list, &fxs_lines);
	mutex_unlock(&fxs_lines_lock);

	if (slic->init) {
		ret = slic->init(line);
		if (ret)
			goto err_slic;
	}

	line->class_dev = device_create(fxs_class, dev, 0, line,
					"fxs%d", id);
	if (IS_ERR(line->class_dev)) {
		ret = PTR_ERR(line->class_dev);
		line->class_dev = NULL;
		goto err_slic;
	}

	if (dev)
		dev_info(dev, "fxs-core: registered %s (%s, %s)\n",
			 line->name, line->ctl_name, line->pcm_name);
	else
		pr_info("fxs-core: registered %s (%s, %s)\n",
			line->name, line->ctl_name, line->pcm_name);

	return line;

err_slic:
	if (slic->exit)
		slic->exit(line);
	mutex_lock(&fxs_lines_lock);
	list_del(&line->list);
	mutex_unlock(&fxs_lines_lock);
	misc_deregister(&line->pcm_misc);
err_ctl:
	misc_deregister(&line->ctl_misc);
err_ida:
	ida_free(&fxs_ida, line->index);
err_fifo:
	kfifo_free(&line->event_fifo);
err_name:
	kfree(line->name);
err_free:
	kfree(line);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(fxs_register_line);

void fxs_unregister_line(struct fxs_line *line)
{
	if (!line)
		return;

	mutex_lock(&fxs_lines_lock);
	list_del(&line->list);
	mutex_unlock(&fxs_lines_lock);

	WRITE_ONCE(line->removing, true);
	wake_up_all(&line->event_wait);

	if (line->class_dev)
		device_unregister(line->class_dev);
	misc_deregister(&line->pcm_misc);
	misc_deregister(&line->ctl_misc);

	if (!refcount_dec_and_test(&line->refs))
		wait_event(line->release_wait,
			   refcount_read(&line->refs) == 0);

	if (line->slic && line->slic->exit)
		line->slic->exit(line);

	ida_free(&fxs_ida, line->index);
	kfifo_free(&line->event_fifo);
	kfree(line->name);
	kfree(line);
}
EXPORT_SYMBOL_GPL(fxs_unregister_line);

void fxs_event_push(struct fxs_line *line, const struct fxs_event *event)
{
	unsigned long flags;
	struct fxs_event dropped;
	unsigned int in;
	unsigned int out;

	if (!line || !event)
		return;

	spin_lock_irqsave(&line->event_lock, flags);
	in = kfifo_in(&line->event_fifo, event, 1);
	if (in != 1) {
		out = kfifo_out(&line->event_fifo, &dropped, 1);
		if (out == 1)
			in = kfifo_in(&line->event_fifo, event, 1);
	}
	spin_unlock_irqrestore(&line->event_lock, flags);

	wake_up_interruptible(&line->event_wait);
}
EXPORT_SYMBOL_GPL(fxs_event_push);

static const char *fxs_state_name(u32 state)
{
	switch (state) {
	case FXS_STATE_IDLE:
		return "idle";
	case FXS_STATE_RINGING:
		return "ringing";
	case FXS_STATE_ACTIVE:
		return "active";
	case FXS_STATE_FAULT:
		return "fault";
	default:
		return "unknown";
	}
}

static ssize_t line_name_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct fxs_line *line = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", line->name);
}
static DEVICE_ATTR_RO(line_name);

static ssize_t state_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct fxs_line *line = dev_get_drvdata(dev);
	const char *name;

	mutex_lock(&line->lock);
	name = fxs_state_name(line->state);
	mutex_unlock(&line->lock);

	return sysfs_emit(buf, "%s\n", name);
}
static DEVICE_ATTR_RO(state);

static ssize_t hook_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct fxs_line *line = dev_get_drvdata(dev);
	bool hook = false;
	int ret = -EOPNOTSUPP;

	mutex_lock(&line->lock);
	if (line->slic && line->slic->get_hook)
		ret = line->slic->get_hook(line, &hook);
	mutex_unlock(&line->lock);

	if (ret)
		return sysfs_emit(buf, "unknown\n");

	return sysfs_emit(buf, "%s\n", hook ? "offhook" : "onhook");
}
static DEVICE_ATTR_RO(hook);

static ssize_t ringing_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct fxs_line *line = dev_get_drvdata(dev);
	bool ringing;

	mutex_lock(&line->lock);
	ringing = line->ringing;
	mutex_unlock(&line->lock);

	return sysfs_emit(buf, "%d\n", ringing);
}
static DEVICE_ATTR_RO(ringing);

static ssize_t active_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct fxs_line *line = dev_get_drvdata(dev);
	bool active;

	mutex_lock(&line->lock);
	active = line->active;
	mutex_unlock(&line->lock);

	return sysfs_emit(buf, "%d\n", active);
}
static DEVICE_ATTR_RO(active);

static struct attribute *fxs_line_attrs[] = {
	&dev_attr_line_name.attr,
	&dev_attr_state.attr,
	&dev_attr_hook.attr,
	&dev_attr_ringing.attr,
	&dev_attr_active.attr,
	NULL,
};
ATTRIBUTE_GROUPS(fxs_line);

static int __init fxs_core_init(void)
{
	fxs_class = class_create("fxs");
	if (IS_ERR(fxs_class))
		return PTR_ERR(fxs_class);
	fxs_class->dev_groups = fxs_line_groups;

	pr_info("fxs-core: generic FXS line core\n");
	return 0;
}

static void __exit fxs_core_exit(void)
{
	struct fxs_line *line;

	while (true) {
		mutex_lock(&fxs_lines_lock);
		line = list_first_entry_or_null(&fxs_lines,
						struct fxs_line, list);
		mutex_unlock(&fxs_lines_lock);
		if (!line)
			break;
		fxs_unregister_line(line);
	}
	class_destroy(fxs_class);
	fxs_class = NULL;
	pr_info("fxs-core: unloaded\n");
}

module_init(fxs_core_init);
module_exit(fxs_core_exit);

MODULE_INFO(author, "ming");
MODULE_INFO(description, "Generic FXS line core");
MODULE_LICENSE("GPL");
