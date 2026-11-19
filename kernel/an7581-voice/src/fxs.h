/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _FXS_H
#define _FXS_H

#include <linux/types.h>
#include <linux/ioctl.h>

#ifdef __KERNEL__
#include <linux/fs.h>
#include <linux/poll.h>
#endif

#define FXS_IOC_MAGIC		'F'
#define FXS_LINE_NAME_MAX	32
#define FXS_MAX_LINES		8

#define FXS_PCM_RATE		8000
#define FXS_PCM_CHANNELS	1
#define FXS_PCM_BITS		16
#define FXS_PCM_FRAME_BYTES	160	/* 10 ms, 8 kHz, s16le, mono */

enum fxs_state {
	FXS_STATE_IDLE = 0,
	FXS_STATE_RINGING,
	FXS_STATE_ACTIVE,
	FXS_STATE_FAULT,
};

enum fxs_event_type {
	FXS_EV_HOOK = 1,	/* value: 0 = on-hook, 1 = off-hook */
	FXS_EV_RING,		/* value: 0 = ring off, 1 = ring on */
	FXS_EV_DTMF,		/* value: ASCII digit */
	FXS_EV_FAULT,		/* value: backend-specific fault code */
};

#define FXS_CAP_RING		(1U << 0)
#define FXS_CAP_HOOK		(1U << 1)
#define FXS_CAP_DTMF_RX		(1U << 2)
#define FXS_CAP_DTMF_TX		(1U << 3)
#define FXS_CAP_CID		(1U << 4)
#define FXS_CAP_LINE_TEST	(1U << 5)
#define FXS_CAP_GAIN		(1U << 6)
#define FXS_CAP_PCM_LOOPBACK	(1U << 7)

struct fxs_caps {
	__u32 version;
	__u32 caps;
	__u32 pcm_rate;
	__u32 pcm_channels;
	__u32 pcm_bits;
	__u32 pcm_frame_bytes;
	__u32 max_events;
	__u32 reserved[8];
};

struct fxs_status {
	__u32 state;
	__u32 hook;		/* 0 = on-hook, 1 = off-hook */
	__u32 ringing;
	__u32 active;
	__u32 reserved[4];
};

struct fxs_profile {
	__u32 version;
	__s32 tx_gain_q15;
	__s32 rx_gain_q15;
	__u32 ring_cadence_ms;	/* 0 = backend default */
	__u32 reserved[5];
};

struct fxs_event {
	__u32 type;
	__u32 value;
	__u32 flags;
	__u32 reserved;
};

#define FXS_IOC_GET_CAPS	_IOR(FXS_IOC_MAGIC, 0x00, struct fxs_caps)
#define FXS_IOC_GET_STATUS	_IOR(FXS_IOC_MAGIC, 0x01, struct fxs_status)
#define FXS_IOC_SET_RING	_IOW(FXS_IOC_MAGIC, 0x02, int)
#define FXS_IOC_SET_ACTIVE	_IOW(FXS_IOC_MAGIC, 0x03, int)
#define FXS_IOC_GET_HOOK	_IOR(FXS_IOC_MAGIC, 0x04, int)
#define FXS_IOC_SET_PROFILE	_IOW(FXS_IOC_MAGIC, 0x05, struct fxs_profile)
#define FXS_IOC_GET_EVENT	_IOR(FXS_IOC_MAGIC, 0x06, struct fxs_event)
#define FXS_IOC_CLAIM		_IOW(FXS_IOC_MAGIC, 0x07, int)
#define FXS_IOC_RELEASE		_IO(FXS_IOC_MAGIC, 0x08)
#define FXS_IOC_GET_LINE_NAME	_IOR(FXS_IOC_MAGIC, 0x09, char[FXS_LINE_NAME_MAX])
#define FXS_IOC_SET_ECHO_CANCEL	_IOW(FXS_IOC_MAGIC, 0x0a, int)

#ifdef __KERNEL__

struct device;
struct fxs_line;

struct fxs_slic_ops {
	u32 caps;
	int (*init)(struct fxs_line *line);
	void (*exit)(struct fxs_line *line);
	int (*set_ring)(struct fxs_line *line, bool on);
	int (*set_active)(struct fxs_line *line, bool on);
	int (*get_hook)(struct fxs_line *line, bool *offhook);
	int (*set_profile)(struct fxs_line *line,
			   const struct fxs_profile *profile);
};

struct fxs_pcm_ops {
	int (*open)(struct fxs_line *line, struct file *file);
	void (*close)(struct fxs_line *line, struct file *file);
	ssize_t (*read)(struct fxs_line *line, struct file *file,
			char __user *buf, size_t count, loff_t *ppos);
	ssize_t (*write)(struct fxs_line *line, struct file *file,
			 const char __user *buf, size_t count, loff_t *ppos);
	__poll_t (*poll)(struct fxs_line *line, struct file *file,
			 poll_table *wait);
	int (*set_echo_cancel)(struct fxs_line *line, bool on);
};

struct fxs_line *fxs_register_line(struct device *dev, const char *name,
				   struct module *owner,
				   const struct fxs_slic_ops *slic,
				   void *slic_priv,
				   const struct fxs_pcm_ops *pcm,
				   void *pcm_priv);
void fxs_unregister_line(struct fxs_line *line);
void fxs_event_push(struct fxs_line *line, const struct fxs_event *event);

#endif /* __KERNEL__ */

#endif /* _FXS_H */
