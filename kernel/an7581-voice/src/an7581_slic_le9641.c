// SPDX-License-Identifier: GPL-2.0-only
/*
 * Microchip/Microsemi Le9641 (VP887) on the Airoha AN7581 ZSI wrapper.
 *
 * The raw-MPI profile data and the line-control sequence are the OEM
 * Le9641 BB/ZSI values (the same tables used by the EcoNet AN7551/AN7581
 * SDK mod-slic3).  The transport is the new an7581-zsi driver; no EN75xx
 * PCM/ZSI/voice code is linked here.
 *
 * This driver initialises the SLIC, brings the linear audio path up on one
 * bus slot, reads the hook switch, provides the 1 s ring / 4 s silence
 * cadence, and exposes /proc/an7581-slic for diagnostics.
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "an7581_zsi.h"

#define DRV_NAME	"an7581-slic"

/* MPI opcodes (read opcode = write opcode + 1) */
#define VP886_R_EC_WRT			0x4a
#define VP886_R_TXSLOT_WRT		0x40
#define VP886_R_TXSLOT_RD		0x41
#define VP886_R_RXSLOT_WRT		0x42
#define VP886_R_RXSLOT_RD		0x43
#define VP886_R_SIGREG_RD		0x4d
#define VP886_R_STATE_WRT		0x56
#define VP886_R_STATE_RD		0x57
#define VP886_R_DEVMODE_RD		0x5f
#define VP886_R_OPFUNC_WRT		0x60
#define VP886_R_GR_WRT			0x82
#define VP886_R_VPGAIN_WRT		0x50
#define VP886_R_OPFUNC_RD		0x61
#define VP886_R_RCNPCN_RD		0x73
#define VP886_R_DCFEED_WRT		0xc6
#define VP886_R_SWCTRL_WRT		0xe6
#define VP886_R_CALCTRL_WRT		0xae

#define VP886_EC_1			0x01
#define VP886_RCN_LE964X		0x08
#define VP886_PCN_LE9641		0x49
#define VP886_PCN_LE9642		0x75

#define VP886_STATE_CODEC		0x20
#define VP886_SS_ACTIVE			0x03
#define VP886_SS_IDLE			0x04
#define VP886_SS_BAL_RING		0x07

#define VP886_SWCTRL_LP			0x05
#define VP886_OPFUNC_LINEAR		0xbf

#define VP886_SIGREG_LEN		4
#define VP886_SIGREG_HOOK		0x01

#define RING_TICK_MS		50
#define HOOK_OFFHOOK_TICK_MS	250
#define RING_ON_MS		1000
#define RING_OFF_MS		4000
#define HOOK_DEBOUNCE		3

#define VP_PROFILE_MPI_LEN		5
#define VP_PROFILE_DATA_START		6

#define LE9642_PCM_CLK_KHZ		2048
#define LE9642_MAX_SLOT			(LE9642_PCM_CLK_KHZ / 64 - 1)

static int bus_slot = 4;
module_param(bus_slot, int, 0444);
MODULE_PARM_DESC(bus_slot, "PCM bus slot for the FXS line (default 4)");

static int feed_ila = 0x07;
module_param(feed_ila, int, 0644);
MODULE_PARM_DESC(feed_ila, "DC feed loop current limit field (mA = 18 + n; OEM IB default 0x07)");

static bool tx_shift = true;
module_param(tx_shift, bool, 0644);
MODULE_PARM_DESC(tx_shift, "shift the ZSI transmit slot down by one slot");

/*
 * OEM Le9641 profiles.  The six-byte VpProfile header is followed by a
 * raw-MPI section whose length is header byte 5; the formatted
 * parameters after it must not be streamed to the chip.
 */
static const u8 dev_profile[] = {
	0x0d, 0xff, 0x00, 0x28, 0x04, 0x14,
	/* DEV_PROFILE_90V_IB_124, ZSI clock slot patched 0x40 -> 0x46 */
	0x46, 0x02, 0x44, 0x46, 0x5e, 0x0c, 0x80,
	0xf6, 0x66, 0x00, 0x64, 0x30, 0x74, 0x30,
	0xe4, 0x04, 0x92, 0x0a, 0xe6, 0x00,
	/* formatted parameters, not streamed */
	0x00, 0x03, 0x00, 0x00, 0x01, 0x65, 0x00, 0x64, 0x52,
	0x64, 0x52, 0xff, 0x66, 0x00, 0x5c, 0x5c, 0x04, 0x3c,
};

static const u8 dc_profile[] = {
	0x0d, 0x01, 0x00, 0x0c, 0x02, 0x03,
	0xc6, 0x92, 0x27,
	0x9c, 0x84, 0x58, 0x80, 0x02, 0x00, 0x07,
};

static const u8 ac_profile[] = {
	0xa4, 0x00, 0xf4, 0x4c, 0x01, 0x49,
	0xca, 0xf5, 0x98, 0xaa, 0x7b, 0xab,
	0x2c, 0xa3, 0x25, 0xa5, 0x24, 0xb2, 0x3d, 0x9a, 0x2a, 0xaa, 0xa6, 0x9f,
	0x01, 0x8a, 0x1d, 0x01, 0xa3, 0xa0, 0x2e, 0xb2, 0xb2, 0xba, 0xac, 0xa2,
	0xa6, 0xcb, 0x3b, 0x45, 0x88, 0x2a, 0x20, 0x3c, 0xbc, 0x4e, 0xa6, 0x2b,
	0xa5, 0x2b, 0x3e, 0xba, 0x8f, 0x82, 0xa8, 0x71, 0x80, 0xa9, 0xf0, 0x50,
	0x00, 0x86, 0x2a, 0x42, 0xa1, 0xcb, 0x1b, 0xa3, 0xa8, 0xfb, 0x87, 0xaa,
	0xfb, 0x9f, 0xa9, 0xf0, 0x96, 0x2e, 0x01,
	0x00,
};

static const u8 ring_profile[] = {
	0x0d, 0x04, 0x00, 0x12, 0x01, 0x0c,
	0xc0, 0x08, 0x00, 0x00, 0x00, 0x44, 0x3a, 0x9d, 0x00, 0x00, 0x00, 0x00,
	0xaa, 0x02, 0x0e, 0x00,
};

#define AN7581_SLIC_IOC_MAGIC		'V'

struct an7581_slic_status {
	__u32 id;
	__u32 state;
	__u32 opfunc;
	__u32 hook;
	__u32 tx;
	__u32 rx;
};

struct an7581_slic_event {
	__u32 hook;
	__u32 state;
};

#define AN7581_SLIC_GET_STATUS	_IOR(AN7581_SLIC_IOC_MAGIC, 1, struct an7581_slic_status)
#define AN7581_SLIC_SET_RING	_IOW(AN7581_SLIC_IOC_MAGIC, 2, int)
#define AN7581_SLIC_SET_ACTIVE	_IOW(AN7581_SLIC_IOC_MAGIC, 3, int)
#define AN7581_SLIC_GET_HOOK	_IOR(AN7581_SLIC_IOC_MAGIC, 4, int)

struct an7581_slic {
	struct device *dev;
	struct an7581_zsi *zsi;
	struct mutex lock;
	u8 rcn;
	u8 pcn;
	bool initialised;
	struct miscdevice misc;
	struct delayed_work hook_work;
	wait_queue_head_t hook_wait;
	atomic_t opened;
	bool hook_offhook;
	bool hook_changed;
	bool ringing;
	unsigned int ring_tick;
	unsigned int ring_on_ticks;
	unsigned int ring_period_ticks;
	unsigned int hook_streak;
};

static struct an7581_slic *slic_dbg;

static int slic_cmd(struct an7581_slic *slic, u8 ec, u8 opcode,
		    const u8 *data, size_t len)
{
	return an7581_zsi_write_reg(slic->zsi, ec, opcode, data, len);
}

static int slic_cmd1(struct an7581_slic *slic, u8 ec, u8 opcode, u8 v)
{
	return slic_cmd(slic, ec, opcode, &v, 1);
}

static int slic_stream_profile(struct an7581_slic *slic, u8 ec,
			       const char *what, const u8 *profile,
			       size_t profile_len)
{
	size_t mpi_len = profile[VP_PROFILE_MPI_LEN];
	u8 sel[2] = { VP886_R_EC_WRT, ec };
	int ret;

	if (VP_PROFILE_DATA_START + mpi_len > profile_len)
		return -EINVAL;

	if (ec) {
		ret = an7581_zsi_write(slic->zsi, sel, sizeof(sel));
		if (ret)
			return ret;
	}
	ret = an7581_zsi_write(slic->zsi, profile + VP_PROFILE_DATA_START,
			       mpi_len);
	if (ret)
		dev_err(slic->dev, "profile '%s' failed: %d\n", what, ret);
	return ret;
}

#define slic_stream(slic, ec, what, prof) \
	slic_stream_profile((slic), (ec), (what), (prof), sizeof(prof))

static int slic_detect(struct an7581_slic *slic)
{
	u8 id[2];
	int ret;

	ret = an7581_zsi_read_reg(slic->zsi, VP886_EC_1, VP886_R_RCNPCN_RD,
				  id, sizeof(id));
	if (ret)
		return ret;
	slic->rcn = id[0];
	slic->pcn = id[1];
	if (id[0] != VP886_RCN_LE964X ||
	    (id[1] != VP886_PCN_LE9641 && id[1] != VP886_PCN_LE9642)) {
		dev_err(slic->dev, "unexpected chip id %02x %02x\n",
			id[0], id[1]);
		return -ENODEV;
	}
	dev_info(slic->dev, "Le964%c detected (rcn %02x pcn %02x)\n",
		 id[1] == VP886_PCN_LE9642 ? '2' : '1', id[0], id[1]);
	return 0;
}

static int slic_set_feed(struct an7581_slic *slic)
{
	const u8 *dc_mpi = dc_profile + VP_PROFILE_DATA_START;
	u8 dc[2] = { dc_mpi[1], (u8)((dc_mpi[2] & ~0x1f) | (feed_ila & 0x1f)) };
	u8 calctrl[3] = { 0, 0, 0 };
	int ret;

	ret = slic_cmd(slic, VP886_EC_1, VP886_R_DCFEED_WRT, dc, sizeof(dc));
	if (ret)
		return ret;

	/*
	 * The OEM VP-API init leaves the switcher in its low-power mode and
	 * clears CALCTRL.  Writing SWCTRL = 0x6f directly makes the Le9641
	 * ignore the state command and stay in shutdown on this board.
	 */
	ret = slic_cmd1(slic, VP886_EC_1, VP886_R_SWCTRL_WRT,
			VP886_SWCTRL_LP);
	if (ret)
		return ret;
	return slic_cmd(slic, VP886_EC_1, VP886_R_CALCTRL_WRT,
			calctrl, sizeof(calctrl));
}

static int slic_set_state(struct an7581_slic *slic, u8 state, bool codec)
{
	u8 v = state | (codec ? VP886_STATE_CODEC : 0);

	return slic_cmd1(slic, VP886_EC_1, VP886_R_STATE_WRT, v);
}

static int slic_program_slots(struct an7581_slic *slic)
{
	unsigned int tx = bus_slot;
	int ret;

	if (tx_shift)
		tx = tx ? tx - 1 : LE9642_MAX_SLOT;
	ret = slic_cmd1(slic, VP886_EC_1, VP886_R_TXSLOT_WRT, tx);
	if (ret)
		return ret;
	return slic_cmd1(slic, VP886_EC_1, VP886_R_RXSLOT_WRT, bus_slot);
}

static int slic_audio_on(struct an7581_slic *slic)
{
	u8 gr[2] = { 0xa8, 0x71 };	/* OEM -6 dB receive gain */
	int ret;

	ret = slic_set_feed(slic);
	if (ret)
		return ret;
	ret = slic_cmd1(slic, VP886_EC_1, VP886_R_OPFUNC_WRT,
			VP886_OPFUNC_LINEAR);
	if (ret)
		return ret;
	ret = slic_program_slots(slic);
	if (ret)
		return ret;
	ret = slic_set_state(slic, VP886_SS_ACTIVE, true);
	if (ret)
		return ret;
	/* Bring-up values that produced an audible PCM path. */
	ret = slic_cmd(slic, VP886_EC_1, VP886_R_GR_WRT, gr, sizeof(gr));
	if (ret)
		return ret;
	return slic_cmd1(slic, VP886_EC_1, VP886_R_VPGAIN_WRT, 0x00);
}

static int slic_init(struct an7581_slic *slic)
{
	u8 devmode;
	int ret;

	ret = slic_detect(slic);
	if (ret)
		return ret;

	ret = slic_stream(slic, 0, "dev", dev_profile);
	if (ret)
		return ret;
	ret = slic_stream(slic, 0, "dc", dc_profile);
	if (ret)
		return ret;
	ret = slic_stream(slic, 0, "ac", ac_profile);
	if (ret)
		return ret;
	ret = slic_stream(slic, 0, "ring", ring_profile);
	if (ret)
		return ret;

	/* channel 1 needs its own DC/AC/ring copy */
	ret = slic_stream(slic, VP886_EC_1, "dc-ch", dc_profile);
	if (ret)
		return ret;
	ret = slic_stream(slic, VP886_EC_1, "ac-ch", ac_profile);
	if (ret)
		return ret;
	ret = slic_stream(slic, VP886_EC_1, "ring-ch", ring_profile);
	if (ret)
		return ret;

	ret = an7581_zsi_read_reg(slic->zsi, VP886_EC_1, VP886_R_DEVMODE_RD,
				  &devmode, 1);
	if (ret)
		return ret;
	dev_info(slic->dev, "device profile loaded, devmode=%02x\n", devmode);

	ret = slic_audio_on(slic);
	if (ret)
		return ret;
	slic->initialised = true;
	dev_info(slic->dev, "audio up: bus slot %u, tx slot %u, linear 16-bit\n",
		 bus_slot, tx_shift ? (bus_slot ? bus_slot - 1 : LE9642_MAX_SLOT)
				    : bus_slot);
	return 0;
}

static int slic_read_hook(struct an7581_slic *slic, bool *offhook)
{
	u8 sig[VP886_SIGREG_LEN];
	int ret;

	ret = an7581_zsi_read_reg(slic->zsi, 0, VP886_R_SIGREG_RD,
				  sig, sizeof(sig));
	if (ret)
		return ret;
	*offhook = !!(sig[0] & VP886_SIGREG_HOOK);
	return 0;
}

static void slic_hook_work(struct work_struct *work)
{
	struct an7581_slic *slic = container_of(work, struct an7581_slic,
						 hook_work.work);
	u8 sig[VP886_SIGREG_LEN] = { 0 };
	bool offhook;
	bool in_gap = true;

	if (slic->initialised) {
		mutex_lock(&slic->lock);

		/*
		 * The ring voltage corrupts the SIGREG hook bit, so only
		 * sample the hook during the silent part of the cadence.
		 */
		if (slic->ringing) {
			unsigned int phase = slic->ring_tick %
				slic->ring_period_ticks;

			in_gap = phase >= slic->ring_on_ticks;
			if (phase == 0)
				slic_set_state(slic, VP886_SS_BAL_RING, false);
			else if (phase == slic->ring_on_ticks)
				slic_set_state(slic, VP886_SS_IDLE, true);

			slic->ring_tick++;
			if (!in_gap) {
				mutex_unlock(&slic->lock);
				goto out;
			}
		}

		if (!an7581_zsi_read_reg(slic->zsi, 0, VP886_R_SIGREG_RD,
					 sig, sizeof(sig))) {
			offhook = !!(sig[0] & VP886_SIGREG_HOOK);
			if (offhook == slic->hook_offhook) {
				slic->hook_streak = 0;
			} else if (++slic->hook_streak >= HOOK_DEBOUNCE) {
				slic->hook_streak = 0;
				slic->hook_offhook = offhook;
				if (offhook && slic->ringing) {
					slic->ringing = false;
					slic_set_state(slic, VP886_SS_IDLE, true);
				}
				slic->hook_changed = true;
				wake_up_interruptible(&slic->hook_wait);
				dev_info(slic->dev, "hook event: %s\n",
					 offhook ? "offhook" : "onhook");
			}
		}
		mutex_unlock(&slic->lock);
	}
out:
	{
		unsigned int tick = RING_TICK_MS;

		if (!slic->ringing && slic->hook_offhook)
			tick = HOOK_OFFHOOK_TICK_MS;
		schedule_delayed_work(&slic->hook_work, msecs_to_jiffies(tick));
	}
}

static int slic_dev_open(struct inode *inode, struct file *file)
{
	struct an7581_slic *slic = slic_dbg;

	if (!slic)
		return -ENODEV;
	if (atomic_xchg(&slic->opened, 1))
		return -EBUSY;
	file->private_data = slic;
	return 0;
}

static int slic_dev_release(struct inode *inode, struct file *file)
{
	struct an7581_slic *slic = file->private_data;

	if (slic)
		atomic_set(&slic->opened, 0);
	return 0;
}

static ssize_t slic_dev_read(struct file *file, char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	struct an7581_slic *slic = file->private_data;
	struct an7581_slic_event ev;
	int ret;

	if (!slic)
		return -ENODEV;
	if (count < sizeof(ev))
		return -EINVAL;
	if (file->f_flags & O_NONBLOCK) {
		if (!READ_ONCE(slic->hook_changed))
			return -EAGAIN;
	} else {
		ret = wait_event_interruptible(slic->hook_wait,
					       READ_ONCE(slic->hook_changed));
		if (ret)
			return ret;
	}
	mutex_lock(&slic->lock);
	ev.hook = slic->hook_offhook;
	ev.state = 0;
	slic->hook_changed = false;
	mutex_unlock(&slic->lock);
	if (copy_to_user(ubuf, &ev, sizeof(ev)))
		return -EFAULT;
	return sizeof(ev);
}

static __poll_t slic_dev_poll(struct file *file, poll_table *wait)
{
	struct an7581_slic *slic = file->private_data;
	__poll_t mask = 0;

	if (!slic)
		return EPOLLERR;
	poll_wait(file, &slic->hook_wait, wait);
	if (READ_ONCE(slic->hook_changed))
		mask |= EPOLLIN | EPOLLRDNORM;
	return mask;
}

static long slic_dev_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	struct an7581_slic *slic = file->private_data;
	struct an7581_slic_status st;
	u8 state = 0, opfunc = 0, tx = 0, rx = 0;
	int val, ret = 0;

	if (!slic)
		return -ENODEV;

	switch (cmd) {
	case AN7581_SLIC_GET_STATUS:
		mutex_lock(&slic->lock);
		an7581_zsi_read_reg(slic->zsi, VP886_EC_1, VP886_R_STATE_RD,
				    &state, 1);
		an7581_zsi_read_reg(slic->zsi, VP886_EC_1, VP886_R_OPFUNC_RD,
				    &opfunc, 1);
		an7581_zsi_read_reg(slic->zsi, VP886_EC_1, VP886_R_TXSLOT_RD,
				    &tx, 1);
		an7581_zsi_read_reg(slic->zsi, VP886_EC_1, VP886_R_RXSLOT_RD,
				    &rx, 1);
		memset(&st, 0, sizeof(st));
		st.id = (slic->rcn << 8) | slic->pcn;
		st.state = state;
		st.opfunc = opfunc;
		st.hook = slic->hook_offhook;
		st.tx = tx;
		st.rx = rx;
		mutex_unlock(&slic->lock);
		if (copy_to_user((void __user *)arg, &st, sizeof(st)))
			return -EFAULT;
		break;
	case AN7581_SLIC_SET_RING:
		if (get_user(val, (int __user *)arg))
			return -EFAULT;
		mutex_lock(&slic->lock);
		if (val) {
			slic->ringing = true;
			slic->ring_tick = 0;
			slic->ring_on_ticks = RING_ON_MS / RING_TICK_MS;
			slic->ring_period_ticks =
				(RING_ON_MS + RING_OFF_MS) / RING_TICK_MS;
			slic->hook_streak = 0;
			ret = slic_set_state(slic, VP886_SS_BAL_RING, false);
		} else {
			slic->ringing = false;
			slic->hook_streak = 0;
			ret = slic_set_state(slic, VP886_SS_IDLE, true);
		}
		mutex_unlock(&slic->lock);
		break;
	case AN7581_SLIC_SET_ACTIVE:
		if (get_user(val, (int __user *)arg))
			return -EFAULT;
		mutex_lock(&slic->lock);
		if (val)
			ret = slic_audio_on(slic);
		else
			ret = slic_set_state(slic, VP886_SS_IDLE, false);
		mutex_unlock(&slic->lock);
		break;
	case AN7581_SLIC_GET_HOOK:
		val = slic->hook_offhook;
		if (copy_to_user((int __user *)arg, &val, sizeof(val)))
			return -EFAULT;
		break;
	default:
		ret = -ENOTTY;
	}
	return ret;
}

static const struct file_operations slic_dev_fops = {
	.owner = THIS_MODULE,
	.open = slic_dev_open,
	.release = slic_dev_release,
	.read = slic_dev_read,
	.poll = slic_dev_poll,
	.unlocked_ioctl = slic_dev_ioctl,
};

static void slic_dump(struct an7581_slic *slic)
{
	u8 devmode = 0, state = 0, opfunc = 0, tx = 0, rx = 0;
	u8 sig[VP886_SIGREG_LEN] = { 0 };

	an7581_zsi_read_reg(slic->zsi, VP886_EC_1, VP886_R_DEVMODE_RD,
			    &devmode, 1);
	an7581_zsi_read_reg(slic->zsi, VP886_EC_1, VP886_R_STATE_RD,
			    &state, 1);
	an7581_zsi_read_reg(slic->zsi, VP886_EC_1, VP886_R_OPFUNC_RD,
			    &opfunc, 1);
	an7581_zsi_read_reg(slic->zsi, VP886_EC_1, VP886_R_TXSLOT_RD,
			    &tx, 1);
	an7581_zsi_read_reg(slic->zsi, VP886_EC_1, VP886_R_RXSLOT_RD,
			    &rx, 1);
	an7581_zsi_read_reg(slic->zsi, 0, VP886_R_SIGREG_RD,
			    sig, sizeof(sig));

	dev_info(slic->dev,
		 "slic id=%02x/%02x devmode=%02x state=%02x opfunc=%02x tx=%u rx=%u sig=%*ph\n",
		 slic->rcn, slic->pcn, devmode, state, opfunc, tx, rx,
		 VP886_SIGREG_LEN, sig);
}

static ssize_t slic_proc_write(struct file *file, const char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	struct an7581_slic *slic = slic_dbg;
	char *buf, *cmd;
	int ret = 0;

	if (!slic)
		return -ENODEV;

	buf = memdup_user_nul(ubuf, count);
	if (IS_ERR(buf))
		return PTR_ERR(buf);
	cmd = strim(buf);

	mutex_lock(&slic->lock);
	if (!strcmp(cmd, "detect")) {
		ret = slic_detect(slic);
	} else if (!strcmp(cmd, "init")) {
		ret = slic_init(slic);
	} else if (!strcmp(cmd, "audio")) {
		ret = slic_audio_on(slic);
	} else if (!strcmp(cmd, "onhook")) {
		ret = slic_set_state(slic, VP886_SS_IDLE, false);
	} else if (!strcmp(cmd, "offhook")) {
		ret = slic_audio_on(slic);
	} else if (!strcmp(cmd, "ring")) {
		ret = slic_set_state(slic, VP886_SS_BAL_RING, false);
	} else if (!strcmp(cmd, "ringstop")) {
		ret = slic_set_state(slic, VP886_SS_IDLE, true);
	} else if (!strcmp(cmd, "hook")) {
		bool offhook;
		ret = slic_read_hook(slic, &offhook);
		if (!ret)
			dev_info(slic->dev, "hook=%s\n",
				 offhook ? "offhook" : "onhook");
	} else if (!strcmp(cmd, "dump")) {
		slic_dump(slic);
	} else {
		ret = -EINVAL;
	}
	mutex_unlock(&slic->lock);

	kfree(buf);
	return ret ? ret : count;
}

static ssize_t slic_proc_read(struct file *file, char __user *ubuf,
			      size_t count, loff_t *ppos)
{
	struct an7581_slic *slic = slic_dbg;
	char buf[128];
	u8 state = 0, opfunc = 0, tx = 0, rx = 0;
	u8 sig[VP886_SIGREG_LEN] = { 0 };
	bool offhook = false;
	int len;

	if (!slic)
		return -ENODEV;

	mutex_lock(&slic->lock);
	an7581_zsi_read_reg(slic->zsi, VP886_EC_1, VP886_R_STATE_RD,
			    &state, 1);
	an7581_zsi_read_reg(slic->zsi, VP886_EC_1, VP886_R_OPFUNC_RD,
			    &opfunc, 1);
	an7581_zsi_read_reg(slic->zsi, VP886_EC_1, VP886_R_TXSLOT_RD,
			    &tx, 1);
	an7581_zsi_read_reg(slic->zsi, VP886_EC_1, VP886_R_RXSLOT_RD,
			    &rx, 1);
	an7581_zsi_read_reg(slic->zsi, 0, VP886_R_SIGREG_RD,
			    sig, sizeof(sig));
	offhook = !!(sig[0] & VP886_SIGREG_HOOK);
	len = scnprintf(buf, sizeof(buf),
			"id=%02x/%02x state=%02x opfunc=%02x tx=%u rx=%u hook=%s\n",
			slic->rcn, slic->pcn, state, opfunc, tx, rx,
			offhook ? "offhook" : "onhook");
	mutex_unlock(&slic->lock);

	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

static const struct proc_ops slic_proc_ops = {
	.proc_read = slic_proc_read,
	.proc_write = slic_proc_write,
};

static int an7581_slic_probe(struct platform_device *pdev)
{
	struct an7581_slic *slic;
	u32 slots[2];
	int ret;

	slic = devm_kzalloc(&pdev->dev, sizeof(*slic), GFP_KERNEL);
	if (!slic)
		return -ENOMEM;

	slic->dev = &pdev->dev;
	mutex_init(&slic->lock);
	init_waitqueue_head(&slic->hook_wait);
	INIT_DELAYED_WORK(&slic->hook_work, slic_hook_work);
	atomic_set(&slic->opened, 0);
	slic->zsi = an7581_zsi_get(&pdev->dev, "airoha,zsi");
	if (!slic->zsi)
		return -EPROBE_DEFER;

	if (pdev->dev.of_node &&
	    !of_property_read_u32_array(pdev->dev.of_node, "airoha,bus-slots",
					slots, 1))
		bus_slot = slots[0];
	if (pdev->dev.of_node &&
	    of_property_read_bool(pdev->dev.of_node, "airoha,no-zsi-tx-shift"))
		tx_shift = false;

	platform_set_drvdata(pdev, slic);
	slic_dbg = slic;
	proc_create(DRV_NAME, 0644, NULL, &slic_proc_ops);

	slic->misc.minor = MISC_DYNAMIC_MINOR;
	slic->misc.name = "an7581-slic";
	slic->misc.fops = &slic_dev_fops;
	ret = misc_register(&slic->misc);
	if (ret) {
		remove_proc_entry(DRV_NAME, NULL);
		slic_dbg = NULL;
		return ret;
	}
	schedule_delayed_work(&slic->hook_work, msecs_to_jiffies(50));

	dev_info(&pdev->dev, "AN7581 Le9641 SLIC ready (bus slot %d, /dev/an7581-slic)\n",
		 bus_slot);
	return 0;
}

static void an7581_slic_remove(struct platform_device *pdev)
{
	struct an7581_slic *slic = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&slic->hook_work);
	misc_deregister(&slic->misc);
	remove_proc_entry(DRV_NAME, NULL);
	slic_dbg = NULL;
}

static const struct of_device_id an7581_slic_of_match[] = {
	{ .compatible = "fiberhome,hg5582a-le9641" },
	{ .compatible = "microsemi,le9641" },
	{ }
};
MODULE_DEVICE_TABLE(of, an7581_slic_of_match);

static struct platform_driver an7581_slic_driver = {
	.probe = an7581_slic_probe,
	.remove = an7581_slic_remove,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = an7581_slic_of_match,
	},
};

static struct platform_device *an7581_slic_fallback;

static int __init an7581_slic_init(void)
{
	struct device_node *np;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "fiberhome,hg5582a-le9641");
	if (!np)
		np = of_find_compatible_node(NULL, NULL, "microsemi,le9641");
	if (np) {
		of_node_put(np);
	} else {
		an7581_slic_fallback = platform_device_register_simple(
			DRV_NAME, -1, NULL, 0);
		if (IS_ERR(an7581_slic_fallback))
			return PTR_ERR(an7581_slic_fallback);
	}

	ret = platform_driver_register(&an7581_slic_driver);
	if (ret) {
		if (an7581_slic_fallback)
			platform_device_unregister(an7581_slic_fallback);
		return ret;
	}
	return 0;
}

static void __exit an7581_slic_exit(void)
{
	platform_driver_unregister(&an7581_slic_driver);
	if (an7581_slic_fallback)
		platform_device_unregister(an7581_slic_fallback);
}

module_init(an7581_slic_init);
module_exit(an7581_slic_exit);

MODULE_DESCRIPTION("Airoha AN7581 Le9641 SLIC driver");
MODULE_LICENSE("GPL");
