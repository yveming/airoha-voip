// SPDX-License-Identifier: GPL-2.0-only
/*
 * Zarlink Le964x backend for an7581-slic.
 *
 * The raw-MPI profile data and the line-control sequence are the OEM
 * Le9641 BB/ZSI values (the same tables used by the EcoNet AN7551/AN7581
 * SDK mod-slic3).  The transport is the an7581-zsi driver.
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/ioctl.h>
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

#include "an7581_slic.h"


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

static int feed_ila = 0x07;
module_param(feed_ila, int, 0644);
MODULE_PARM_DESC(feed_ila, "DC feed loop current limit field (mA = 18 + n; OEM IB default 0x07)");

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

int an7581_slic_zarlink_detect(struct an7581_slic *slic)
{
	u8 id[2];
	int ret;

	ret = an7581_zsi_read_reg(slic->zsi, VP886_EC_1, VP886_R_RCNPCN_RD,
				  id, sizeof(id));
	if (ret)
		return ret;

	slic->rcn = id[0];
	slic->pcn = id[1];

	if (id[0] == VP886_RCN_LE964X && id[1] == VP886_PCN_LE9641) {
		slic->type = AN7581_SLIC_LE9641;
		return 0;
	}
	if (id[0] == VP886_RCN_LE964X && id[1] == VP886_PCN_LE9642) {
		slic->type = AN7581_SLIC_LE9642;
		return 0;
	}

	return -ENODEV;
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
	unsigned int tx = slic->bus_slot;
	int ret;

	if (slic->tx_shift)
		tx = tx ? tx - 1 : LE9642_MAX_SLOT;
	ret = slic_cmd1(slic, VP886_EC_1, VP886_R_TXSLOT_WRT, tx);
	if (ret)
		return ret;
	return slic_cmd1(slic, VP886_EC_1, VP886_R_RXSLOT_WRT, slic->bus_slot);
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

	if (slic->type == AN7581_SLIC_UNKNOWN) {
		ret = an7581_slic_zarlink_detect(slic);
		if (ret)
			return ret;
	}

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
		 slic->bus_slot,
		 slic->tx_shift ? (slic->bus_slot ? slic->bus_slot - 1
						 : LE9642_MAX_SLOT)
				: slic->bus_slot);
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
				if (slic->hook_cb)
					slic->hook_cb(slic->hook_data, offhook);
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

static void zarlink_hook_cb(void *data, bool offhook)
{
	struct fxs_event event = {
		.type = FXS_EV_HOOK,
		.value = offhook,
	};

	fxs_event_push(data, &event);
}

static int zarlink_init(struct fxs_line *line)
{
	struct an7581_slic *slic = slic_dbg;
	int ret;

	if (!slic)
		return -ENODEV;

	mutex_lock(&slic->lock);
	ret = slic_init(slic);
	if (!ret) {
		slic->hook_cb = zarlink_hook_cb;
		slic->hook_data = line;
	}
	mutex_unlock(&slic->lock);

	return ret;
}

static void zarlink_exit(struct fxs_line *line)
{
	struct an7581_slic *slic = slic_dbg;

	if (!slic)
		return;

	mutex_lock(&slic->lock);
	if (slic->hook_data == line) {
		slic->hook_cb = NULL;
		slic->hook_data = NULL;
	}
	mutex_unlock(&slic->lock);
}

static int zarlink_set_ring(struct fxs_line *line, bool on)
{
	struct an7581_slic *slic = slic_dbg;
	int ret;

	if (!slic)
		return -ENODEV;

	mutex_lock(&slic->lock);
	if (on) {
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

	return ret;
}

static int zarlink_set_active(struct fxs_line *line, bool on)
{
	struct an7581_slic *slic = slic_dbg;
	int ret;

	if (!slic)
		return -ENODEV;

	mutex_lock(&slic->lock);
	if (on)
		ret = slic_audio_on(slic);
	else
		ret = slic_set_state(slic, VP886_SS_IDLE, false);
	mutex_unlock(&slic->lock);

	return ret;
}

static int zarlink_get_hook(struct fxs_line *line, bool *offhook)
{
	struct an7581_slic *slic = slic_dbg;

	if (!slic)
		return -ENODEV;

	mutex_lock(&slic->lock);
	*offhook = slic->hook_offhook;
	mutex_unlock(&slic->lock);

	return 0;
}

static const struct fxs_slic_ops zarlink_ops = {
	.caps		= FXS_CAP_RING | FXS_CAP_HOOK,
	.init		= zarlink_init,
	.exit		= zarlink_exit,
	.set_ring	= zarlink_set_ring,
	.set_active	= zarlink_set_active,
	.get_hook	= zarlink_get_hook,
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
		ret = an7581_slic_zarlink_detect(slic);
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

int an7581_slic_zarlink_probe(struct an7581_slic *slic)
{
	slic_dbg = slic;
	INIT_DELAYED_WORK(&slic->hook_work, slic_hook_work);
	proc_create(AN7581_SLIC_DRV_NAME, 0644, NULL, &slic_proc_ops);
	schedule_delayed_work(&slic->hook_work, msecs_to_jiffies(50));
	dev_info(slic->dev, "Zarlink backend ready (bus slot %d)\n",
		 slic->bus_slot);
	return 0;
}

void an7581_slic_zarlink_remove(struct an7581_slic *slic)
{
	cancel_delayed_work_sync(&slic->hook_work);
	remove_proc_entry(AN7581_SLIC_DRV_NAME, NULL);
	slic_dbg = NULL;
}

const struct fxs_slic_ops *an7581_slic_zarlink_ops(void)
{
	return &zarlink_ops;
}

