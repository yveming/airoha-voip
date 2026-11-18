// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal Asterisk channel driver for the AN7581 + Le9641 FXS line.
 *
 * Audio:  /dev/an7581-pcm, 8 kHz signed linear, 16-bit, 10 ms frames.
 *         The fd is registered with Asterisk and polled by the channel
 *         thread; there is no private audio thread.
 * Control:/dev/an7581-slic, hook events plus ring/active ioctls.
 *
 * Locking: Asterisk may hold the channel lock when it calls hangup(), so
 * the channel lock is always taken before pvt->lock.  Never take the
 * channel lock while holding pvt->lock.
 */

#include "asterisk.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "asterisk/channel.h"
#include "asterisk/config.h"
#include "asterisk/dsp.h"
#include "asterisk/causes.h"
#include "asterisk/format_cache.h"
#include "asterisk/frame.h"
#include "asterisk/indications.h"
#include "asterisk/lock.h"
#include "asterisk/logger.h"
#include "asterisk/module.h"
#include "asterisk/pbx.h"
#include "asterisk/stasis_channels.h"
#include "asterisk/utils.h"

#define SLIC_DEV	"/dev/an7581-slic"
#define PCM_DEV		"/dev/an7581-pcm"
#define FRAME_SAMPLES	160
#define VOICE_RATE	8000
#define LEC_SYSFS	"/sys/module/an7581_pcm/parameters/lec_enable"
#define LEC_SYSFS_ALT	"/sys/module/an7581-pcm/parameters/lec_enable"

/*
 * The 2-wire hybrid reflects the locally generated dial tone back into the
 * ADC.  At 450 Hz the reflected tone is strong enough to hide the DTMF
 * rows/columns from ast_dsp's relative-energy test, so digits are never
 * reported while the dial tone is playing.  The EN75xx driver carries the
 * same Q=3 biquad notch for exactly this reason.
 */
struct an7581_notch {
	int32_t b0, b1, a2;
	int16_t x1, x2, y1, y2;
};

#define AN7581_SLIC_IOC_MAGIC	'V'
struct an7581_slic_event {
	uint32_t hook;
	uint32_t state;
};
#define AN7581_SLIC_SET_RING	_IOW(AN7581_SLIC_IOC_MAGIC, 2, int)
#define AN7581_SLIC_SET_ACTIVE	_IOW(AN7581_SLIC_IOC_MAGIC, 3, int)
#define AN7581_SLIC_GET_HOOK	_IOR(AN7581_SLIC_IOC_MAGIC, 4, int)

struct an7581_pvt {
	struct ast_channel *owner;
	struct ast_channel *audio_chan;
	int ctl_fd;
	int pcm_fd;
	int early_media;
	pthread_t monitor_thread;
	int monitor_stop;
	int audio_active;
	int hook;
	int incoming_ringing;
	int wait_onhook;
	int tx_gain_q15;
	int rx_gain_q15;
	int dialtone_notch_hz;
	int dialing;
	int collecting;
	int16_t rx_buf[FRAME_SAMPLES];
	size_t rx_filled;
	struct ast_frame read_frame;
	int dtmf_held;
	int dtmf_pending;
	int dtmf_pending_count;
	int dtmf_gap;
	int dtmf_end_pending;
	struct an7581_notch notch[2];
	unsigned int notch_count;
	struct ast_dsp *dsp;
	ast_mutex_t lock;
	char name[32];
	char pcm_dev[64];
	char slic_dev[64];
	char context[64];
	char exten[64];
};

static struct an7581_pvt *pvt;
static struct ast_channel_tech an7581_tech;

static int gain_db_to_q15(int db)
{
	static const int32_t table[] = { 6538, 7336, 8231, 9235, 10362, 11627, 13045, 14637, 16423, 18427, 20675, 23198, 26029, 29205, 32768, 36766, 41252, 46286, 51934, 58271, 65381 };

	if (db < -14)
		db = -14;
	if (db > 6)
		db = 6;

	return table[db + 14];
}

static void apply_gain(int16_t *samples, size_t count, int32_t gain_q15)
{
	size_t i;

	if (gain_q15 == 32768)
		return;

	for (i = 0; i < count; i++) {
		int32_t v = ((int32_t)samples[i] * gain_q15) >> 15;

		if (v > 32767)
			v = 32767;
		else if (v < -32768)
			v = -32768;
		samples[i] = (int16_t)v;
	}
}

/*
 * OSLEC lives in an7581-pcm, so the channel driver hands the configured
 * value to the module parameter.  This keeps an Asterisk-only restart in
 * sync with an7581.conf; voip-apply still writes the same sysfs file
 * during boot/apply so the setting also works before Asterisk starts.
 */
static void apply_echo_cancellation(int enable)
{
	static const char *const paths[] = { LEC_SYSFS, LEC_SYSFS_ALT };
	char value = enable ? '1' : '0';
	unsigned int i;

	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		int fd = open(paths[i], O_WRONLY);

		if (fd < 0)
			continue;
		if (write(fd, &value, sizeof(value)) == sizeof(value)) {
			close(fd);
			return;
		}
		close(fd);
	}
	ast_debug(3, "AN7581: cannot apply echo_cancellation=%d\n", enable);
}

static void add_dialtone_notch(struct an7581_pvt *p, unsigned int freq)
{
	struct an7581_notch *notch;
	double w0, alpha, a0;

	/* The DTMF row frequencies start at 697 Hz.  Leave that band alone. */
	if (!freq || freq >= 697 ||
	    p->notch_count == sizeof(p->notch) / sizeof(p->notch[0]))
		return;

	notch = &p->notch[p->notch_count];
	w0 = 2.0 * M_PI * freq / VOICE_RATE;
	alpha = sin(w0) / 6.0;	/* Q = 3 */
	a0 = 1.0 + alpha;
	notch->b0 = lround(16384.0 / a0);
	notch->b1 = lround(-32768.0 * cos(w0) / a0);
	notch->a2 = lround(16384.0 * (1.0 - alpha) / a0);
	notch->x1 = notch->x2 = notch->y1 = notch->y2 = 0;
	p->notch_count++;
}

static void apply_notch(struct an7581_pvt *p, int16_t *samples, size_t count)
{
	size_t i;
	unsigned int j;

	for (i = 0; i < count; i++) {
		int32_t x0 = samples[i], y0;

		for (j = 0; j < p->notch_count; j++) {
			struct an7581_notch *n = &p->notch[j];
			int64_t acc = (int64_t)n->b0 * x0 +
				(int64_t)n->b1 * n->x1 +
				(int64_t)n->b0 * n->x2 -
				(int64_t)n->b1 * n->y1 -
				(int64_t)n->a2 * n->y2;

			y0 = acc >> 14;
			if (y0 > 32767)
				y0 = 32767;
			else if (y0 < -32768)
				y0 = -32768;
			n->x2 = n->x1;
			n->x1 = x0;
			n->y2 = n->y1;
			n->y1 = y0;
			x0 = y0;
		}
		samples[i] = x0;
	}
}

static double goertzel_energy(const int16_t *samples, int n, double freq)
{
	double w = 2.0 * M_PI * freq / VOICE_RATE;
	double coeff = 2.0 * cos(w);
	double s0 = 0, s1 = 0, s2 = 0;
	int i;

	for (i = 0; i < n; i++) {
		s0 = samples[i] + coeff * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

/*
 * One-frame DTMF detector.  Some analogue phones emit a single 20 ms
 * burst even when the key is held; ast_dsp wants two 12.75 ms blocks of
 * the same digit before it reports BEGIN, so those digits were lost.
 * The Goertzel bins below look at the whole 20 ms frame and accept a
 * single frame, then emit BEGIN/END directly to the channel.
 */
#define DTMF_MIN_ENERGY		1.0e8

static char dtmf_detect_custom(const int16_t *samples)
{
	static const double rows[] = { 697, 770, 852, 941 };
	static const double cols[] = { 1209, 1336, 1477, 1633 };
	static const char map[] = "123A" "456B" "789C" "*0#D";
	double re[4], ce[4];
	double best_r = 0, best_c = 0, second_r = 0, second_c = 0;
	int br = 0, bc = 0, i;

	for (i = 0; i < 4; i++) {
		re[i] = goertzel_energy(samples, FRAME_SAMPLES, rows[i]);
		ce[i] = goertzel_energy(samples, FRAME_SAMPLES, cols[i]);
	}
	for (i = 0; i < 4; i++) {
		if (re[i] > best_r) {
			second_r = best_r;
			best_r = re[i];
			br = i;
		} else if (re[i] > second_r) {
			second_r = re[i];
		}
		if (ce[i] > best_c) {
			second_c = best_c;
			best_c = ce[i];
			bc = i;
		} else if (ce[i] > second_c) {
			second_c = ce[i];
		}
	}

	if (best_r < DTMF_MIN_ENERGY || best_c < DTMF_MIN_ENERGY)
		return 0;

	{
		double qr = second_r > 0 ? best_r / second_r : 1000.0;
		double qc = second_c > 0 ? best_c / second_c : 1000.0;
		double quality = qr < qc ? qr : qc;

		/* DTMF twist limit, same order as ast_dsp's relaxed mode. */
		if (best_c > best_r * 6.31 || best_r > best_c * 6.31)
			return 0;
		/*
		 * A real DTMF pair dominates the other rows/columns by a wide
		 * margin.  2x was low enough for speech formants to produce a
		 * false digit; the phone's real '1' frame is >8x on the weaker
		 * axis.  Use 4x (12 dB).
		 */
		if (quality < 4.0)
			return 0;
	}

	return map[br * 4 + bc];
}

static int an7581_start_audio(struct ast_channel *chan)
{
	struct an7581_pvt *p = ast_channel_tech_pvt(chan);

	if (!p)
		return -1;

	ast_mutex_lock(&p->lock);
	if (p->owner != chan) {
		ast_mutex_unlock(&p->lock);
		return -1;
	}
	if (__atomic_load_n(&p->audio_chan, __ATOMIC_ACQUIRE) &&
	    __atomic_load_n(&p->audio_chan, __ATOMIC_ACQUIRE) != chan) {
		ast_log(LOG_WARNING, "AN7581: PCM path is busy; refusing start for %s\n",
			ast_channel_name(chan));
		ast_mutex_unlock(&p->lock);
		return -1;
	}
	p->audio_active = 0;
	__atomic_store_n(&p->audio_chan, NULL, __ATOMIC_RELEASE);
	if (p->pcm_fd >= 0) {
		close(p->pcm_fd);
		p->pcm_fd = -1;
	}
	if (p->dsp) {
		ast_dsp_free(p->dsp);
		p->dsp = NULL;
	}

	p->pcm_fd = open(p->pcm_dev, O_RDWR | O_NONBLOCK);
	if (p->pcm_fd < 0) {
		ast_log(LOG_ERROR, "AN7581: cannot open %s: %s\n", p->pcm_dev,
			strerror(errno));
		ast_mutex_unlock(&p->lock);
		return -1;
	}

	/* an7581-pcm starts streaming and pre-fills TX silence on open. */
	p->audio_active = 1;
	__atomic_store_n(&p->audio_chan, chan, __ATOMIC_RELEASE);
	p->rx_filled = 0;
	p->dtmf_held = 0;
	p->dtmf_pending = 0;
	p->dtmf_pending_count = 0;
	p->dtmf_gap = 1;
	p->dtmf_end_pending = 0;

	memset(&p->read_frame, 0, sizeof(p->read_frame));
	p->read_frame.frametype = AST_FRAME_VOICE;
	p->read_frame.subclass.format = ast_format_slin;
	p->read_frame.src = "an7581";
	p->read_frame.data.ptr = p->rx_buf;
	p->read_frame.datalen = sizeof(p->rx_buf);
	p->read_frame.samples = FRAME_SAMPLES;

	p->dsp = ast_dsp_new();
	if (p->dsp) {
		ast_dsp_set_features(p->dsp, DSP_FEATURE_DIGIT_DETECT);
		ast_dsp_set_digitmode(p->dsp, DSP_DIGITMODE_DTMF |
				  DSP_DIGITMODE_RELAXDTMF);
	} else {
		ast_log(LOG_WARNING, "AN7581: ast_dsp_new failed; inband DTMF disabled\n");
	}
	p->notch_count = 0;
	if (p->dialtone_notch_hz > 0)
		add_dialtone_notch(p, p->dialtone_notch_hz);

	/* Asterisk polls this fd from ast_waitfor(); no audio thread needed. */
	ast_channel_set_fd(chan, 0, p->pcm_fd);
	ast_mutex_unlock(&p->lock);
	return 0;
}

static void an7581_stop_audio_locked(struct an7581_pvt *p,
				      struct ast_channel *chan)
{
	p->audio_active = 0;
	if (__atomic_load_n(&p->audio_chan, __ATOMIC_ACQUIRE) == chan)
		__atomic_store_n(&p->audio_chan, NULL, __ATOMIC_RELEASE);
	ast_channel_set_fd(chan, 0, -1);
	if (p->pcm_fd >= 0) {
		close(p->pcm_fd);
		p->pcm_fd = -1;
	}
	if (p->dsp) {
		ast_dsp_free(p->dsp);
		p->dsp = NULL;
	}
	p->rx_filled = 0;
	p->dtmf_end_pending = 0;
}

static struct ast_channel *an7581_new(struct an7581_pvt *p, int state,
				      const struct ast_assigned_ids *assignedids,
				      const struct ast_channel *requestor,
				      const char *context, const char *exten)
{
	struct ast_format_cap *caps;
	struct ast_channel *chan;

	caps = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT);
	if (!caps)
		return NULL;

	chan = ast_channel_alloc(1, state, NULL, NULL, NULL, exten, context,
				 assignedids, requestor, 0, "AN7581/%s",
				 p->name);
	if (!chan) {
		ao2_ref(caps, -1);
		return NULL;
	}

	ast_channel_stage_snapshot(chan);
	ast_channel_tech_set(chan, &an7581_tech);
	ast_channel_set_readformat(chan, ast_format_slin);
	ast_channel_set_writeformat(chan, ast_format_slin);
	ast_format_cap_append(caps, ast_format_slin, 0);
	ast_channel_nativeformats_set(chan, caps);
	ao2_ref(caps, -1);
	ast_channel_tech_pvt_set(chan, p);
	__atomic_store_n(&p->early_media, 0, __ATOMIC_RELEASE);
	pbx_builtin_setvar_helper(chan, "AN7581_EARLY_MEDIA", "0");
	pbx_builtin_setvar_helper(chan, "AN7581_ANSWERED", "0");
	ast_channel_stage_snapshot_done(chan);
	ast_channel_unlock(chan);

	return chan;
}

static void an7581_start_outgoing(struct an7581_pvt *p)
{
	struct ast_channel *chan;
	int on = 1;

	ast_mutex_lock(&p->lock);
	if (p->owner || p->wait_onhook) {
		ast_mutex_unlock(&p->lock);
		return;
	}
	chan = an7581_new(p, AST_STATE_UP, NULL, NULL, p->context, p->exten);
	if (!chan) {
		ast_mutex_unlock(&p->lock);
		return;
	}
	/*
	 * Activate the line before publishing the owner.  The monitor
	 * thread takes the same lock, so it cannot observe the new channel
	 * before the SLIC is in the active state.
	 */
	ioctl(p->ctl_fd, AN7581_SLIC_SET_ACTIVE, &on);
	p->owner = chan;
	__atomic_store_n(&p->dialing, 1, __ATOMIC_RELAXED);
	__atomic_store_n(&p->collecting, 1, __ATOMIC_RELAXED);
	ast_mutex_unlock(&p->lock);

	if (an7581_start_audio(chan)) {
		ast_hangup(chan);
		return;
	}
	if (ast_pbx_start(chan)) {
		ast_log(LOG_ERROR, "AN7581: ast_pbx_start failed\n");
		ast_hangup(chan);
	}
}

static void *an7581_monitor_thread(void *data)
{
	struct an7581_pvt *p = data;
	struct pollfd pfd;
	struct an7581_slic_event ev;
	int hook = 0;
	int answer = 0, outgoing = 0, hangup = 0;
	struct ast_channel *chan = NULL;

	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = p->ctl_fd;
	pfd.events = POLLIN;

	if (!ioctl(p->ctl_fd, AN7581_SLIC_GET_HOOK, &hook))
		p->hook = hook;

	/* If Asterisk is started while the phone is already offhook, start
	 * the call immediately instead of waiting for a hook transition. */
	if (p->hook)
		an7581_start_outgoing(p);

	while (!__atomic_load_n(&p->monitor_stop, __ATOMIC_RELAXED)) {
		int ret = poll(&pfd, 1, 500);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (ret == 0)
			continue;
		if (!(pfd.revents & POLLIN))
			continue;
		if (read(p->ctl_fd, &ev, sizeof(ev)) != sizeof(ev))
			continue;

		answer = outgoing = hangup = 0;
		chan = NULL;

		ast_mutex_lock(&p->lock);
		p->hook = ev.hook;
		if (p->hook) {
			if (p->incoming_ringing && p->owner) {
				answer = 1;
				chan = p->owner;
				ast_channel_ref(chan);
				p->incoming_ringing = 0;
			} else if (!p->owner && !p->wait_onhook) {
				outgoing = 1;
			}
		} else {
			p->wait_onhook = 0;
			if (p->owner) {
				hangup = 1;
				chan = p->owner;
				ast_channel_ref(chan);
				p->owner = NULL;
				p->incoming_ringing = 0;
				/*
				 * Release the PCM fd while the line lock is held so a
				 * quick re-offhook cannot race with the old fd.
				 */
				an7581_stop_audio_locked(p, chan);
			}
		}
		ast_mutex_unlock(&p->lock);

		if (answer) {
			int off = 0, on = 1;

			ioctl(p->ctl_fd, AN7581_SLIC_SET_RING, &off);
			ioctl(p->ctl_fd, AN7581_SLIC_SET_ACTIVE, &on);
			if (an7581_start_audio(chan)) {
				ast_log(LOG_ERROR, "AN7581: cannot start audio on FXS answer\n");
				ast_softhangup(chan, AST_SOFTHANGUP_DEV);
			} else {
				ast_setstate(chan, AST_STATE_UP);
				ast_queue_control(chan, AST_CONTROL_ANSWER);
				ast_log(LOG_NOTICE, "AN7581: FXS answered incoming call\n");
			}
		}
		if (outgoing)
			an7581_start_outgoing(p);
		if (hangup) {
			int off = 0, idle = 0;

			ioctl(p->ctl_fd, AN7581_SLIC_SET_RING, &off);
			ioctl(p->ctl_fd, AN7581_SLIC_SET_ACTIVE, &idle);
			ast_log(LOG_NOTICE, "AN7581: FXS went onhook\n");
			ast_softhangup(chan, AST_SOFTHANGUP_EXPLICIT);
		}
		if (chan)
			ast_channel_unref(chan);
	}
	return NULL;
}

static struct ast_channel *an7581_requester(const char *type,
					    struct ast_format_cap *cap,
					    const struct ast_assigned_ids *assignedids,
					    const struct ast_channel *requestor,
					    const char *addr, int *cause)
{
	struct ast_channel *chan;
	int on = 1;
	int offhook;

	if (!pvt)
		return NULL;

	if (!ast_format_cap_iscompatible(cap, an7581_tech.capabilities)) {
		*cause = AST_CAUSE_BEARERCAPABILITY_NOTAVAIL;
		return NULL;
	}

	ast_mutex_lock(&pvt->lock);
	if (pvt->owner) {
		ast_mutex_unlock(&pvt->lock);
		*cause = AST_CAUSE_BUSY;
		return NULL;
	}
	chan = an7581_new(pvt, AST_STATE_RING, assignedids, requestor, NULL, NULL);
	if (!chan) {
		ast_mutex_unlock(&pvt->lock);
		*cause = AST_CAUSE_CONGESTION;
		return NULL;
	}
	/*
	 * Put the SLIC in the right state before publishing the owner.
	 * The hook monitor takes the same lock, so an off-hook event
	 * cannot answer a line that has not started ringing yet.
	 */
	offhook = pvt->hook;
	if (offhook) {
		ioctl(pvt->ctl_fd, AN7581_SLIC_SET_ACTIVE, &on);
		pvt->incoming_ringing = 0;
	} else {
		ioctl(pvt->ctl_fd, AN7581_SLIC_SET_RING, &on);
		pvt->incoming_ringing = 1;
	}
	pvt->owner = chan;
	pvt->wait_onhook = 0;
	ast_mutex_unlock(&pvt->lock);

	if (offhook) {
		if (an7581_start_audio(chan)) {
			ast_log(LOG_ERROR, "AN7581: cannot start audio for off-hook incoming call\n");
			ast_hangup(chan);
			*cause = AST_CAUSE_CONGESTION;
			return NULL;
		}
		ast_setstate(chan, AST_STATE_UP);
		ast_queue_control(chan, AST_CONTROL_ANSWER);
	} else {
		ast_log(LOG_NOTICE, "AN7581: ringing FXS for incoming call\n");
	}

	return chan;
}

static int an7581_hangup(struct ast_channel *chan)
{
	struct an7581_pvt *p = ast_channel_tech_pvt(chan);
	int off = 0, idle = 0;
	bool current;

	if (!p)
		return -1;

	ast_mutex_lock(&p->lock);
	current = (p->owner == chan);
	if (current || !p->owner) {
		an7581_stop_audio_locked(p, chan);
		ioctl(p->ctl_fd, AN7581_SLIC_SET_RING, &off);
		ioctl(p->ctl_fd, AN7581_SLIC_SET_ACTIVE, &idle);
		p->owner = NULL;
		__atomic_store_n(&p->dialing, 0, __ATOMIC_RELAXED);
		__atomic_store_n(&p->collecting, 0, __ATOMIC_RELAXED);
		p->incoming_ringing = 0;
		__atomic_store_n(&p->early_media, 0, __ATOMIC_RELEASE);
		/*
		 * Only a remote hangup while the phone is still off-hook
		 * needs to block redial until the next on-hook.  If the
		 * monitor already saw the on-hook, a quick re-offhook must
		 * be allowed through.
		 */
		p->wait_onhook = current ? (p->hook ? 1 : 0) : 0;
	}
	ast_mutex_unlock(&p->lock);

	ast_channel_tech_pvt_set(chan, NULL);
	return 0;
}

static int an7581_answer(struct ast_channel *chan)
{
	return 0;
}

static struct ast_frame *an7581_read(struct ast_channel *chan)
{
	struct an7581_pvt *p = ast_channel_tech_pvt(chan);
	ssize_t n;

	if (!p)
		return &ast_null_frame;

	ast_mutex_lock(&p->lock);
	if (!p->audio_active || p->pcm_fd < 0 ||
	    __atomic_load_n(&p->audio_chan, __ATOMIC_ACQUIRE) != chan) {
		ast_mutex_unlock(&p->lock);
		return &ast_null_frame;
	}

	if (p->dtmf_end_pending) {
		memset(&p->read_frame, 0, sizeof(p->read_frame));
		p->read_frame.frametype = AST_FRAME_DTMF_END;
		p->read_frame.subclass.integer = p->dtmf_end_pending;
		p->read_frame.src = "an7581";
		p->dtmf_end_pending = 0;
		ast_mutex_unlock(&p->lock);
		return &p->read_frame;
	}

	while (p->rx_filled < sizeof(p->rx_buf)) {
		n = read(p->pcm_fd, (char *)p->rx_buf + p->rx_filled,
			 sizeof(p->rx_buf) - p->rx_filled);
		if (n > 0) {
			p->rx_filled += n;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			break;
		break;
	}

	if (p->rx_filled < sizeof(p->rx_buf)) {
		ast_mutex_unlock(&p->lock);
		return &ast_null_frame;
	}

	apply_gain(p->rx_buf, FRAME_SAMPLES, p->tx_gain_q15);
	if (__atomic_load_n(&p->dialing, __ATOMIC_RELAXED) && p->notch_count)
		apply_notch(p, p->rx_buf, FRAME_SAMPLES);

	if (__atomic_load_n(&p->collecting, __ATOMIC_RELAXED)) {
		char digit = dtmf_detect_custom(p->rx_buf);

		if (digit) {
			if (digit == p->dtmf_held) {
				p->dtmf_gap = 0;
				p->dtmf_pending = 0;
				p->dtmf_pending_count = 0;
			} else {
				if (digit == p->dtmf_pending) {
					if (p->dtmf_pending_count < 2)
						p->dtmf_pending_count++;
				} else {
					p->dtmf_pending = digit;
					p->dtmf_pending_count = 1;
				}
				if (p->dtmf_pending_count >= 2 && p->dtmf_gap >= 1) {
					ast_verb(3, "AN7581: DTMF custom '%c'\n", digit);
					__atomic_store_n(&p->dialing, 0, __ATOMIC_RELAXED);
					p->dtmf_held = digit;
					p->dtmf_gap = 0;
					p->dtmf_pending = 0;
					p->dtmf_pending_count = 0;
					p->dtmf_end_pending = digit;
					p->rx_filled = 0;
					memset(&p->read_frame, 0, sizeof(p->read_frame));
					p->read_frame.frametype = AST_FRAME_DTMF_BEGIN;
					p->read_frame.subclass.integer = digit;
					p->read_frame.src = "an7581";
					ast_mutex_unlock(&p->lock);
					return &p->read_frame;
				}
			}
		} else {
			p->dtmf_pending = 0;
			p->dtmf_pending_count = 0;
			if (p->dtmf_gap < 3)
				p->dtmf_gap++;
			if (p->dtmf_gap >= 2)
				p->dtmf_held = 0;
		}
	}

	p->rx_filled = 0;
	memset(&p->read_frame, 0, sizeof(p->read_frame));
	p->read_frame.frametype = AST_FRAME_VOICE;
	p->read_frame.subclass.format = ast_format_slin;
	p->read_frame.src = "an7581";
	p->read_frame.data.ptr = p->rx_buf;
	p->read_frame.datalen = sizeof(p->rx_buf);
	p->read_frame.samples = FRAME_SAMPLES;

	if (!__atomic_load_n(&p->collecting, __ATOMIC_RELAXED) && p->dsp) {
		struct ast_frame *dspf = ast_dsp_process(chan, p->dsp, &p->read_frame);

		if (dspf == &ast_null_frame) {
			ast_mutex_unlock(&p->lock);
			return &ast_null_frame;
		}
		if (dspf && dspf != &p->read_frame) {
			/* dsp->f is private to the DSP object; copy it out. */
			p->read_frame = *dspf;
			p->read_frame.mallocd = 0;
			p->read_frame.data.ptr = NULL;
			p->read_frame.datalen = 0;
		}
	}

	ast_mutex_unlock(&p->lock);
	return &p->read_frame;
}

static int an7581_write(struct ast_channel *chan, struct ast_frame *f)
{
	struct an7581_pvt *p = ast_channel_tech_pvt(chan);
	int16_t tmp[FRAME_SAMPLES];
	int ret = 0;

	if (!p || f->frametype != AST_FRAME_VOICE || !f->data.ptr ||
	    !f->datalen)
		return 0;

	ast_mutex_lock(&p->lock);
	if (__atomic_load_n(&p->audio_chan, __ATOMIC_ACQUIRE) != chan) {
		ast_mutex_unlock(&p->lock);
		return 0;
	}
	if (p->pcm_fd >= 0) {
		ssize_t n;

		if (p->rx_gain_q15 != 32768 &&
		    f->datalen <= sizeof(tmp) && (f->datalen % 2) == 0) {
			memcpy(tmp, f->data.ptr, f->datalen);
			apply_gain(tmp, f->datalen / 2, p->rx_gain_q15);
			n = write(p->pcm_fd, tmp, f->datalen);
		} else {
			n = write(p->pcm_fd, f->data.ptr, f->datalen);
		}
		if (n < 0 && errno != EAGAIN)
			ret = -1;
	}
	ast_mutex_unlock(&p->lock);
	return ret;
}

static void an7581_play_ringback(struct ast_channel *chan)
{
	struct ast_tone_zone_sound *ts;

	ts = ast_get_indication_tone(ast_channel_zone(chan), "ringback");
	if (!ts)
		ts = ast_get_indication_tone(ast_channel_zone(chan), "ring");
	if (!ts) {
		ast_log(LOG_WARNING, "AN7581: no ringback indication tone\n");
		return;
	}

	ast_playtones_start(chan, 0, ts->data, 1);
	ast_tone_zone_sound_unref(ts);
}

static int an7581_indicate(struct ast_channel *chan, int cond,
			   const void *data, size_t datalen)
{
	struct an7581_pvt *p = ast_channel_tech_pvt(chan);

	switch (cond) {
	case AST_CONTROL_RINGING:
		if (p) {
			__atomic_store_n(&p->collecting, 0, __ATOMIC_RELAXED);
			/* A 180 after a 183 with SDP must not replace early media. */
			if (__atomic_load_n(&p->early_media, __ATOMIC_ACQUIRE))
				return 0;
		}
		pbx_builtin_setvar_helper(chan, "AN7581_EARLY_MEDIA", "0");
		an7581_play_ringback(chan);
		return 0;
	case AST_CONTROL_PROGRESS:
		if (p) {
			__atomic_store_n(&p->collecting, 0, __ATOMIC_RELAXED);
			__atomic_store_n(&p->early_media, 1, __ATOMIC_RELEASE);
		}
		pbx_builtin_setvar_helper(chan, "AN7581_EARLY_MEDIA", "1");
		ast_playtones_stop(chan);
		return 0;
	case AST_CONTROL_ANSWER:
		pbx_builtin_setvar_helper(chan, "AN7581_ANSWERED", "1");
		ast_playtones_stop(chan);
		if (p) {
			__atomic_store_n(&p->dialing, 0, __ATOMIC_RELAXED);
			__atomic_store_n(&p->collecting, 0, __ATOMIC_RELAXED);
		}
		return 0;
	case AST_CONTROL_HANGUP:
	case AST_CONTROL_BUSY:
	case AST_CONTROL_CONGESTION:
		ast_playtones_stop(chan);
		if (p) {
			__atomic_store_n(&p->dialing, 0, __ATOMIC_RELAXED);
			__atomic_store_n(&p->collecting, 0, __ATOMIC_RELAXED);
		}
		return 0;
	default:
		return -1;
	}
}

static int an7581_fixup(struct ast_channel *oldchan, struct ast_channel *newchan)
{
	struct an7581_pvt *p = ast_channel_tech_pvt(newchan);

	if (p) {
		ast_mutex_lock(&p->lock);
		if (__atomic_load_n(&p->audio_chan, __ATOMIC_ACQUIRE) == oldchan)
			__atomic_store_n(&p->audio_chan, newchan, __ATOMIC_RELEASE);
		p->owner = newchan;
		ast_mutex_unlock(&p->lock);
	}
	return 0;
}

static int an7581_devicestate(const char *dev)
{
	int state;

	if (!pvt)
		return AST_DEVICE_UNKNOWN;

	ast_mutex_lock(&pvt->lock);
	state = pvt->owner ? AST_DEVICE_INUSE : AST_DEVICE_NOT_INUSE;
	ast_mutex_unlock(&pvt->lock);
	return state;
}

static int load_module(void)
{
	pvt = ast_calloc(1, sizeof(*pvt));
	if (!pvt)
		return AST_MODULE_LOAD_DECLINE;

	ast_mutex_init(&pvt->lock);
	pvt->tx_gain_q15 = 32768;
	pvt->rx_gain_q15 = 32768;
	pvt->dialtone_notch_hz = 450;
	ast_copy_string(pvt->name, "line1", sizeof(pvt->name));
	ast_copy_string(pvt->pcm_dev, PCM_DEV, sizeof(pvt->pcm_dev));
	ast_copy_string(pvt->slic_dev, SLIC_DEV, sizeof(pvt->slic_dev));
	ast_copy_string(pvt->context, "from-analog", sizeof(pvt->context));
	ast_copy_string(pvt->exten, "s", sizeof(pvt->exten));
	{
		struct ast_flags config_flags = { 0 };
		struct ast_config *cfg = ast_config_load("an7581.conf", config_flags);
		if (cfg && cfg != CONFIG_STATUS_FILEINVALID) {
			const char *v;
			if ((v = ast_variable_retrieve(cfg, "general", "pcm_dev")))
				ast_copy_string(pvt->pcm_dev, v, sizeof(pvt->pcm_dev));
			if ((v = ast_variable_retrieve(cfg, "general", "slic_dev")))
				ast_copy_string(pvt->slic_dev, v, sizeof(pvt->slic_dev));
			if ((v = ast_variable_retrieve(cfg, "general", "context")))
				ast_copy_string(pvt->context, v, sizeof(pvt->context));
			if ((v = ast_variable_retrieve(cfg, "general", "exten")))
				ast_copy_string(pvt->exten, v, sizeof(pvt->exten));
			if ((v = ast_variable_retrieve(cfg, "general", "line_name")))
				ast_copy_string(pvt->name, v, sizeof(pvt->name));
			if ((v = ast_variable_retrieve(cfg, "general", "tx_gain")))
				pvt->tx_gain_q15 = gain_db_to_q15(atoi(v));
			if ((v = ast_variable_retrieve(cfg, "general", "rx_gain")))
				pvt->rx_gain_q15 = gain_db_to_q15(atoi(v));
			if ((v = ast_variable_retrieve(cfg, "general", "dialtone_notch")))
				pvt->dialtone_notch_hz = atoi(v);
			if ((v = ast_variable_retrieve(cfg, "general", "echo_cancellation")))
				apply_echo_cancellation(atoi(v) ? 1 : 0);
			ast_config_destroy(cfg);
		}
	}
	pvt->pcm_fd = -1;
	pvt->ctl_fd = open(pvt->slic_dev, O_RDWR);
	if (pvt->ctl_fd < 0) {
		ast_log(LOG_ERROR, "AN7581: cannot open %s: %s\n", pvt->slic_dev,
			strerror(errno));
		goto fail;
	}

	an7581_tech.capabilities = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT);
	if (!an7581_tech.capabilities)
		goto fail_close;
	ast_format_cap_append(an7581_tech.capabilities, ast_format_slin, 0);
	ast_format_cap_append(an7581_tech.capabilities, ast_format_alaw, 0);
	ast_format_cap_append(an7581_tech.capabilities, ast_format_ulaw, 0);
	ast_format_cap_append(an7581_tech.capabilities, ast_format_g722, 0);

	if (ast_channel_register(&an7581_tech)) {
		ast_log(LOG_ERROR, "AN7581: failed to register channel tech\n");
		goto fail_caps;
	}

	if (ast_pthread_create_background(&pvt->monitor_thread, NULL,
					  an7581_monitor_thread, pvt)) {
		ast_channel_unregister(&an7581_tech);
		goto fail_caps;
	}

	ast_log(LOG_NOTICE, "AN7581 FXS channel driver loaded\n");
	return AST_MODULE_LOAD_SUCCESS;

fail_caps:
	ao2_ref(an7581_tech.capabilities, -1);
	an7581_tech.capabilities = NULL;
fail_close:
	close(pvt->ctl_fd);
fail:
	ast_mutex_destroy(&pvt->lock);
	ast_free(pvt);
	pvt = NULL;
	return AST_MODULE_LOAD_DECLINE;
}

static int unload_module(void)
{
	if (!pvt)
		return 0;
	if (pvt->owner)
		return -1;

	pvt->monitor_stop = 1;
	pthread_join(pvt->monitor_thread, NULL);

	ast_channel_unregister(&an7581_tech);
	ao2_ref(an7581_tech.capabilities, -1);
	an7581_tech.capabilities = NULL;
	ast_mutex_lock(&pvt->lock);
	if (pvt->pcm_fd >= 0) {
		close(pvt->pcm_fd);
		pvt->pcm_fd = -1;
	}
	if (pvt->dsp) {
		ast_dsp_free(pvt->dsp);
		pvt->dsp = NULL;
	}
	pvt->audio_active = 0;
	ast_mutex_unlock(&pvt->lock);
	close(pvt->ctl_fd);
	ast_mutex_destroy(&pvt->lock);
	ast_free(pvt);
	pvt = NULL;
	return 0;
}

static int an7581_call(struct ast_channel *chan, const char *addr, int timeout)
{
	struct an7581_pvt *p = ast_channel_tech_pvt(chan);
	int hook;

	if (!p)
		return -1;

	ast_mutex_lock(&p->lock);
	hook = p->hook;
	ast_mutex_unlock(&p->lock);

	/*
	 * an7581_requester() has already started ringing for an on-hook
	 * line.  Tell the Dial application about it so it can send 180
	 * Ringing to the caller.  If the phone is already off-hook the
	 * requester has answered it and started audio; do not report it
	 * as ringing again.
	 */
	if (!hook)
		ast_queue_control(chan, AST_CONTROL_RINGING);

	return 0;
}

static struct ast_frame *an7581_exception(struct ast_channel *chan)
{
	/*
	 * The hook monitor owns hook state transitions; the channel core
	 * only needs a null frame to suppress the "no exception handler"
	 * warning.
	 */
	return &ast_null_frame;
}

static int an7581_digit_end(struct ast_channel *chan, char digit,
			    unsigned int duration)
{
	/*
	 * DTMF toward the subscriber is generated as audio by Asterisk's
	 * tone code; the SLIC has no out-of-band DTMF path.
	 */
	return 0;
}

static struct ast_channel_tech an7581_tech = {
	.type = "AN7581",
	.description = "AN7581 Le9641 FXS",
	.requester = an7581_requester,
	.call = an7581_call,
	.hangup = an7581_hangup,
	.answer = an7581_answer,
	.read = an7581_read,
	.write = an7581_write,
	.exception = an7581_exception,
	.indicate = an7581_indicate,
	.fixup = an7581_fixup,
	.send_digit_end = an7581_digit_end,
	.devicestate = an7581_devicestate,
};

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER,
	"AN7581 Le9641 FXS Channel Driver",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_CHANNEL_DRIVER,
);
