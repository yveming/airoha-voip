/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AN7581_SLIC_H
#define _AN7581_SLIC_H

#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "an7581_zsi.h"
#include "fxs.h"

#define AN7581_SLIC_DRV_NAME	"an7581-slic"

enum an7581_slic_type {
	AN7581_SLIC_UNKNOWN = 0,
	AN7581_SLIC_LE9641,
	AN7581_SLIC_LE9642,
	AN7581_SLIC_PEF32001,
};

struct an7581_slic {
	struct device *dev;
	struct an7581_zsi *zsi;
	struct mutex lock;
	u8 rcn;
	u8 pcn;
	enum an7581_slic_type type;
	bool initialised;
	struct delayed_work hook_work;
	wait_queue_head_t hook_wait;
	bool hook_offhook;
	bool hook_changed;
	bool ringing;
	unsigned int ring_tick;
	unsigned int ring_on_ticks;
	unsigned int ring_period_ticks;
	unsigned int hook_streak;
	void (*hook_cb)(void *data, bool offhook);
	void *hook_data;
	const struct fxs_slic_ops *ops;
	int bus_slot;
	bool tx_shift;
};

struct an7581_slic_backend {
	const char *name;
	const char *transport;
	enum an7581_slic_type type;
	bool supported;
};

int an7581_slic_zarlink_detect(struct an7581_slic *slic);
int an7581_slic_zarlink_probe(struct an7581_slic *slic);
void an7581_slic_zarlink_remove(struct an7581_slic *slic);
const struct fxs_slic_ops *an7581_slic_zarlink_ops(void);

int an7581_slic_pef32001_detect(struct an7581_slic *slic);

const struct fxs_slic_ops *an7581_slic_get_ops(void **priv);
void an7581_slic_put(void);

#endif /* _AN7581_SLIC_H */
