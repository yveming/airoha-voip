// SPDX-License-Identifier: GPL-2.0-only
/*
 * Airoha AN7581 PCM engine.
 *
 * The register map and 12-byte descriptor layout are taken from the OEM
 * pcm1.ko:
 *
 *   pcmCtrl             0x00
 *   TX slot config      0x04..0x10 and 0x48..0x74
 *   RX slot config      0x14..0x20 and 0x78..0xa4
 *   ISR                 0x24
 *   INTMask             0x28
 *   txPolling           0x2c
 *   rxPolling           0x30
 *   txRingBaseAddr      0x34
 *   rxRingBaseAddr      0x38
 *   txrxRingSizeAndOff  0x3c
 *   txRxDMA             0x40
 *   unknown             0xa8 (descInit writes 0xa0)
 *   txRxChanEnable      0xac
 *
 * Descriptor:
 *   u32 status;    OWN(31) | sample-size(9:0)
 *   u32 ch_valid;  (1 << channels) - 1
 *   u32 buf_addr;  physical address
 *
 * There are 15 descriptors per direction, 0xb4 bytes, stride 0xc.
 *
 * The original diagnostic driver refilled the ring from a 10 ms delayed
 * work item.  With HZ=100 that item can drift against the 10 ms PCM frame
 * clock, and a single late refill inserts a zero-filled descriptor into the
 * middle of a call-progress tone.  The audible result is a regular click.
 * The ring is now processed from the PCM descriptor interrupt when it is
 * available, with a high-resolution kthread fallback (poll_ms, 10 ms by
 * default) only when the interrupt is unavailable.
 *
 * Line echo cancellation uses OSLEC, copied from the airoha_voip vendor
 * tree where the same PCM block already runs it.  The TX frame that was
 * actually handed to the DMA is saved per descriptor and paired with the RX
 * frame from that same descriptor, which is the reference signal OSLEC
 * needs for the 2-wire hybrid echo.
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kfifo.h>
#include <linux/kthread.h>
#include <linux/log2.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/unaligned.h>

#include "oslec.h"

#define DRV_NAME	"an7581-pcm"

#define PCM_CTRL		0x00
#define PCM_ISR			0x24
#define PCM_INTMASK		0x28
#define PCM_TX_POLL		0x2c
#define PCM_RX_POLL		0x30
#define PCM_TX_RING		0x34
#define PCM_RX_RING		0x38
#define PCM_RING_SIZE		0x3c
#define PCM_DMA			0x40
#define PCM_DMA_TX		BIT(0)
#define PCM_DMA_RX		BIT(1)
#define PCM_DESC_CFG		0xa8
#define PCM_CHAN_ENABLE		0xac

#define PCM_CTRL_LOOPBACK	BIT(25)
#define PCM_DESC_OWN		BIT(31)
#define PCM_DESC_COUNT		15
#define PCM_DESC_SIZE		12
#define PCM_FRAME_SIZE		160	/* 80 x 16-bit samples = 10 ms */
#define PCM_SAMPLE_SIZE		80	/* descriptor status: samples, not bytes */
#define PCM_CHANNELS		1
#define PCM_CH_VALID		((1 << PCM_CHANNELS) - 1)
#define PCM_CH_OFFSET		0
#define PCM_BUF_SIZE		PCM_FRAME_SIZE
#define PCM_RING_SIZE_VAL	0x3f
#define PCM_DESC_CFG_VAL	0xa0
#define PCM_FIFO_SIZE		(PCM_FRAME_SIZE * 32)
#define PCM_TX_PREFILL		(PCM_FRAME_SIZE * 4)	/* 40 ms */

/* Same interrupt layout as the EN75xx PCM block. */
#define PCM_ISR_VALID		GENMASK(10, 0)
#define PCM_IRQ_MASK		GENMASK(10, 2)
#define PCM_IRQ_ERR		(BIT(6) | BIT(7) | BIT(8))

#define PCM_EC_TAPS		128	/* 16 ms at 8 kHz */
#define PCM_EC_MODE		(ECHO_CAN_USE_ADAPTION | ECHO_CAN_USE_NLP | \
				 ECHO_CAN_USE_CLIP)

struct an7581_pcm_desc {
	u32 status;
	u32 ch_valid;
	u32 buf_addr;
};

struct an7581_pcm {
	struct device *dev;
	void __iomem *base;
	struct mutex lock;

	void *desc_cpu;
	dma_addr_t desc_dma;
	void *buf_cpu;
	dma_addr_t buf_dma;

	bool streaming;
	bool tone_mode;
	unsigned int tone_phase;

	struct miscdevice misc;
	struct kfifo tx_fifo;
	struct kfifo rx_fifo;
	wait_queue_head_t rx_wait;
	struct task_struct *thread;
	int irq;
	atomic_t opened;

	/* OSLEC line echo canceller and the TX reference for each descriptor. */
	struct oslec_state *lec;
	bool lec_active;
	s16 tx_desc_ref[PCM_DESC_COUNT][PCM_SAMPLE_SIZE];

	u8 last_rx[PCM_CHANNELS][PCM_FRAME_SIZE];
	u32 rx_peak[PCM_CHANNELS];
	u32 tx_done;
	u32 rx_done;
	u32 tx_underruns;
	u32 rx_overruns;
	u32 dma_errors;
};

static struct an7581_pcm *pcm_dbg;

static bool char_loopback;
module_param(char_loopback, bool, 0444);
MODULE_PARM_DESC(char_loopback, "start /dev/an7581-pcm in internal loopback (test)");

static bool use_irq = true;
module_param(use_irq, bool, 0444);
MODULE_PARM_DESC(use_irq, "Use the PCM descriptor interrupt (default: on, fall back to the kthread poll)");

static unsigned int poll_ms = 10;
module_param(poll_ms, uint, 0444);
MODULE_PARM_DESC(poll_ms, "Fallback kthread poll interval in ms (default: 10, max: 20)");

static bool lec_enable = true;
/* Asterisk runs as the unprivileged "asterisk" user and imports this value
 * from an7581.conf at module load time.  The init script gives the asterisk
 * group write access to this sysfs file. */
module_param(lec_enable, bool, 0664);
MODULE_PARM_DESC(lec_enable, "Enable OSLEC line echo cancellation on the RX path");

static int lec_taps = PCM_EC_TAPS;
module_param(lec_taps, int, 0444);
MODULE_PARM_DESC(lec_taps, "OSLEC length in samples (power of two, 0 disables)");

static int lec_adaption_mode = PCM_EC_MODE;
module_param(lec_adaption_mode, int, 0444);
MODULE_PARM_DESC(lec_adaption_mode,
		 "OSLEC mode bits: adaption=1 NLP=2 CNG=4 clip=8 tx_hpf=16 rx_hpf=32 disable=64");

static inline struct an7581_pcm_desc *pcm_tx_desc(struct an7581_pcm *pcm, int i)
{
	return (struct an7581_pcm_desc *)pcm->desc_cpu + i;
}

static inline struct an7581_pcm_desc *pcm_rx_desc(struct an7581_pcm *pcm, int i)
{
	return (struct an7581_pcm_desc *)pcm->desc_cpu + PCM_DESC_COUNT + i;
}

static inline u8 *pcm_tx_buf(struct an7581_pcm *pcm, int i)
{
	return (u8 *)pcm->buf_cpu + i * PCM_BUF_SIZE;
}

static inline u8 *pcm_rx_buf(struct an7581_pcm *pcm, int i)
{
	return (u8 *)pcm->buf_cpu + (PCM_DESC_COUNT + i) * PCM_BUF_SIZE;
}

static void pcm_fill_pattern(struct an7581_pcm *pcm, int desc)
{
	int ch, i;

	for (ch = 0; ch < PCM_CHANNELS; ch++) {
		u16 *samples = (u16 *)(pcm_tx_buf(pcm, desc) +
				       ch * PCM_FRAME_SIZE);

		for (i = 0; i < PCM_SAMPLE_SIZE; i++)
			samples[i] = 0x1111 * (ch + 1);
	}
}

static const s16 pcm_sine8[8] = {
	0, 23170, 32767, 23170, 0, -23170, -32768, -23170,
};

static void pcm_fill_tone(struct an7581_pcm *pcm, int desc)
{
	s16 *samples = (s16 *)(pcm_tx_buf(pcm, desc) + PCM_CH_OFFSET);
	int i;

	for (i = 0; i < PCM_SAMPLE_SIZE; i++)
		samples[i] = pcm_sine8[(pcm->tone_phase + i) & 7];
	pcm->tone_phase = (pcm->tone_phase + PCM_SAMPLE_SIZE) & 7;
}

static int pcm_lec_alloc(struct an7581_pcm *pcm)
{
	if (pcm->lec || lec_taps <= 0)
		return 0;
	if (lec_taps < 2 || lec_taps > 4096 || !is_power_of_2(lec_taps) ||
	    (lec_adaption_mode & ~0x7f)) {
		dev_warn(pcm->dev, "invalid OSLEC settings, continuing without EC\n");
		return 0;
	}

	pcm->lec = oslec_create(lec_taps, lec_adaption_mode);
	if (!pcm->lec) {
		dev_warn(pcm->dev, "OSLEC allocation failed, continuing without EC\n");
		return 0;
	}
	oslec_flush(pcm->lec);
	pcm->lec_active = READ_ONCE(lec_enable);
	dev_info(pcm->dev, "OSLEC %d taps (%u ms), mode=0x%x\n",
		 lec_taps, lec_taps * 1000 / 8000, lec_adaption_mode);
	return 0;
}

static void pcm_lec_free(struct an7581_pcm *pcm)
{
	if (pcm->lec) {
		oslec_free(pcm->lec);
		pcm->lec = NULL;
	}
	pcm->lec_active = false;
}

static void pcm_desc_init(struct an7581_pcm *pcm)
{
	int i;

	memset(pcm->desc_cpu, 0, PCM_DESC_COUNT * 2 * PCM_DESC_SIZE);
	memset(pcm->buf_cpu, 0, PCM_DESC_COUNT * 2 * PCM_BUF_SIZE);

	for (i = 0; i < PCM_DESC_COUNT; i++) {
		struct an7581_pcm_desc *tx = pcm_tx_desc(pcm, i);
		struct an7581_pcm_desc *rx = pcm_rx_desc(pcm, i);

		tx->ch_valid = PCM_CH_VALID;
		tx->buf_addr = pcm->buf_dma + i * PCM_BUF_SIZE;
		rx->ch_valid = PCM_CH_VALID;
		rx->buf_addr = pcm->buf_dma +
			(PCM_DESC_COUNT + i) * PCM_BUF_SIZE;
	}
	dma_wmb();

	writel((u32)(pcm->desc_dma | PCM_DESC_OWN), pcm->base + PCM_TX_RING);
	writel((u32)((pcm->desc_dma + PCM_DESC_COUNT * PCM_DESC_SIZE) |
		     PCM_DESC_OWN), pcm->base + PCM_RX_RING);
	writel(PCM_RING_SIZE_VAL, pcm->base + PCM_RING_SIZE);
	writel(PCM_DESC_CFG_VAL, pcm->base + PCM_DESC_CFG);
	writel(PCM_ISR_VALID, pcm->base + PCM_ISR);
	writel(pcm->irq >= 0 ? PCM_IRQ_MASK : 0, pcm->base + PCM_INTMASK);
}

static void pcm_arm_tx(struct an7581_pcm *pcm, int i, bool pattern)
{
	struct an7581_pcm_desc *d = pcm_tx_desc(pcm, i);

	if (pattern)
		pcm_fill_pattern(pcm, i);
	else
		memset(pcm_tx_buf(pcm, i), 0, PCM_FRAME_SIZE);
	d->status = PCM_DESC_OWN | PCM_SAMPLE_SIZE;
	dma_wmb();
	writel(1, pcm->base + PCM_TX_POLL);
}

static void pcm_arm_rx(struct an7581_pcm *pcm, int i)
{
	struct an7581_pcm_desc *d = pcm_rx_desc(pcm, i);

	d->status = PCM_DESC_OWN | PCM_SAMPLE_SIZE;
	dma_wmb();
	writel(1, pcm->base + PCM_RX_POLL);
}

static void pcm_stop(struct an7581_pcm *pcm);
static int pcm_start(struct an7581_pcm *pcm, bool loopback, bool pattern_tx);

/*
 * Consume completed descriptors.  RX is processed before TX on purpose:
 * the TX reference saved for descriptor N belongs to the frame that the
 * DMA just played in that slot, which is the correct reference for the RX
 * frame that has just completed in the same slot.
 */
static void pcm_process(struct an7581_pcm *pcm)
{
	int j;

	if (!pcm->streaming)
		return;

	for (j = 0; j < PCM_DESC_COUNT; j++) {
		struct an7581_pcm_desc *tx = pcm_tx_desc(pcm, j);
		struct an7581_pcm_desc *rx = pcm_rx_desc(pcm, j);

		dma_rmb();
		if (!(READ_ONCE(rx->status) & PCM_DESC_OWN)) {
			u8 *frame = pcm_rx_buf(pcm, j);
			int ch, i;

			for (ch = 0; ch < PCM_CHANNELS; ch++) {
				s16 *s = (s16 *)(frame + ch * PCM_FRAME_SIZE);
				u32 energy = 0;

				for (i = 0; i < PCM_SAMPLE_SIZE; i++)
					energy += abs(s[i]);
				if (energy > pcm->rx_peak[ch]) {
					pcm->rx_peak[ch] = energy;
					memcpy(pcm->last_rx[ch],
					       frame + ch * PCM_FRAME_SIZE,
					       PCM_FRAME_SIZE);
				}
			}

			if (pcm->lec && pcm->lec_active != READ_ONCE(lec_enable)) {
				pcm->lec_active = READ_ONCE(lec_enable);
				oslec_flush(pcm->lec);
			}
			if (pcm->lec && pcm->lec_active) {
				u8 *base = frame + PCM_CH_OFFSET;

				for (i = 0; i < PCM_SAMPLE_SIZE; i++) {
					s16 tx = pcm->tx_desc_ref[j][i];
					s16 rx = get_unaligned_le16(base + i * 2);

					put_unaligned_le16(oslec_update(pcm->lec,
									tx, rx),
							   base + i * 2);
				}
			}

			pcm->rx_done++;
			if (kfifo_in(&pcm->rx_fifo, frame + PCM_CH_OFFSET,
				     PCM_FRAME_SIZE) < PCM_FRAME_SIZE)
				pcm->rx_overruns++;
			pcm_arm_rx(pcm, j);
			wake_up_interruptible(&pcm->rx_wait);
		}

		dma_rmb();
		if (!(READ_ONCE(tx->status) & PCM_DESC_OWN)) {
			u8 *buf = pcm_tx_buf(pcm, j) + PCM_CH_OFFSET;
			int i;

			pcm->tx_done++;
			if (pcm->tone_mode) {
				pcm_fill_tone(pcm, j);
			} else {
				unsigned int n = kfifo_out(&pcm->tx_fifo, buf,
							   PCM_FRAME_SIZE);

				if (n < PCM_FRAME_SIZE) {
					memset(buf + n, 0, PCM_FRAME_SIZE - n);
					pcm->tx_underruns++;
				}
			}

			for (i = 0; i < PCM_SAMPLE_SIZE; i++)
				pcm->tx_desc_ref[j][i] =
					get_unaligned_le16(buf + i * 2);

			tx->status = PCM_DESC_OWN | PCM_SAMPLE_SIZE;
			dma_wmb();
			writel(1, pcm->base + PCM_TX_POLL);
		}
	}
}

static irqreturn_t pcm_irq(int irq, void *data)
{
	struct an7581_pcm *pcm = data;
	u32 status = readl(pcm->base + PCM_ISR);

	if (!(status & PCM_IRQ_MASK))
		return IRQ_NONE;
	return IRQ_WAKE_THREAD;
}

static irqreturn_t pcm_irq_thread(int irq, void *data)
{
	struct an7581_pcm *pcm = data;
	u32 status;

	mutex_lock(&pcm->lock);
	status = readl(pcm->base + PCM_ISR);
	if (status)
		writel(status, pcm->base + PCM_ISR);
	if (status & PCM_IRQ_ERR)
		pcm->dma_errors++;
	pcm_process(pcm);
	mutex_unlock(&pcm->lock);
	return IRQ_HANDLED;
}

/*
 * Fallback poll when the PCM descriptor IRQ is unavailable.  The ring
 * holds 15 x 10 ms descriptors, so 10 ms is enough; poll_ms can be
 * lowered for boards with more scheduling jitter.  usleep_range() is
 * hrtimer based, so this does not drift with CONFIG_HZ=100.
 */
static int pcm_thread(void *data)
{
	struct an7581_pcm *pcm = data;
	unsigned int interval;

	while (!kthread_should_stop()) {
		interval = poll_ms;
		if (interval < 1)
			interval = 1;
		if (interval > 20)
			interval = 20;
		usleep_range(interval * 1000, interval * 1000 + 1000);

		mutex_lock(&pcm->lock);
		if (pcm->streaming) {
			u32 status = readl(pcm->base + PCM_ISR) &
				     PCM_ISR_VALID;

			if (status) {
				writel(status, pcm->base + PCM_ISR);
				if (status & PCM_IRQ_ERR)
					pcm->dma_errors++;
			}
			pcm_process(pcm);
		}
		mutex_unlock(&pcm->lock);
	}
	return 0;
}

static void pcm_tx_prefill(struct an7581_pcm *pcm)
{
	u8 silence[PCM_FRAME_SIZE] = { 0 };
	int i;

	for (i = 0; i < PCM_TX_PREFILL / PCM_FRAME_SIZE; i++)
		kfifo_in(&pcm->tx_fifo, silence, sizeof(silence));
}

static int pcm_start_stream(struct an7581_pcm *pcm, bool tone)
{
	int ret;

	pcm_stop(pcm);
	ret = pcm_lec_alloc(pcm);
	if (ret)
		return ret;

	pcm->tone_mode = tone;
	kfifo_reset(&pcm->tx_fifo);
	kfifo_reset(&pcm->rx_fifo);
	memset(pcm->tx_desc_ref, 0, sizeof(pcm->tx_desc_ref));

	/*
	 * Keep a 40 ms reservoir in front of the 10 ms DMA consumer.  Asterisk
	 * produces 20 ms frames, so without this the producer/consumer phase can
	 * briefly empty the FIFO at a descriptor boundary and insert a zero-filled
	 * frame into an otherwise continuous tone.
	 */
	pcm_tx_prefill(pcm);
	if (pcm->lec)
		oslec_flush(pcm->lec);

	pcm_start(pcm, false, false);
	pcm->streaming = true;
	return 0;
}

static void pcm_stop(struct an7581_pcm *pcm)
{
	u32 v = readl(pcm->base + PCM_DMA);

	pcm->streaming = false;
	writel(0, pcm->base + PCM_INTMASK);
	writel(v & ~(PCM_DMA_TX | PCM_DMA_RX), pcm->base + PCM_DMA);
	wake_up_interruptible(&pcm->rx_wait);
}

static int pcm_start(struct an7581_pcm *pcm, bool loopback, bool pattern_tx)
{
	u32 ctrl;
	int i;

	pcm_stop(pcm);
	ctrl = readl(pcm->base + PCM_CTRL);
	if (loopback)
		ctrl |= PCM_CTRL_LOOPBACK;
	else
		ctrl &= ~PCM_CTRL_LOOPBACK;
	writel(ctrl, pcm->base + PCM_CTRL);

	pcm_desc_init(pcm);
	for (i = 0; i < PCM_DESC_COUNT; i++) {
		pcm_arm_tx(pcm, i, pattern_tx);
		pcm_arm_rx(pcm, i);
	}

	writel(readl(pcm->base + PCM_DMA) | PCM_DMA_TX | PCM_DMA_RX,
	       pcm->base + PCM_DMA);
	/* Re-kick after the DMA enable, otherwise the initial poll written
	 * while the engine was disabled can be lost (notably on RX). */
	writel(1, pcm->base + PCM_TX_POLL);
	writel(1, pcm->base + PCM_RX_POLL);
	dev_info(pcm->dev, "PCM DMA started (loopback=%d ctrl=%08x dma=%08x irq=%d)\n",
		 loopback, readl(pcm->base + PCM_CTRL),
		 readl(pcm->base + PCM_DMA), pcm->irq);
	return 0;
}

static int pcm_loopback_test(struct an7581_pcm *pcm)
{
	int i, j, seen = 0, rearmed = 0;
	u8 first[16] = { 0 };

	pcm_start(pcm, true, true);
	for (i = 0; i < 1000 && seen < 300; i++) {
		msleep(1);
		for (j = 0; j < PCM_DESC_COUNT; j++) {
			struct an7581_pcm_desc *rx = pcm_rx_desc(pcm, j);
			struct an7581_pcm_desc *tx = pcm_tx_desc(pcm, j);
			u32 st = READ_ONCE(rx->status);

			if (!(READ_ONCE(tx->status) & PCM_DESC_OWN))
				pcm_arm_tx(pcm, j, true);
			if (st & PCM_DESC_OWN)
				continue;
			if (!seen)
				memcpy(first, pcm_rx_buf(pcm, j) + PCM_CH_OFFSET,
				       sizeof(first));
			seen++;
			pcm_arm_rx(pcm, j);
			rearmed++;
		}
	}
	pcm_stop(pcm);

	dev_info(pcm->dev, "PCM loopback: seen=%d rearmed=%d isr=%08x\n",
		 seen, rearmed, readl(pcm->base + PCM_ISR));
	dev_info(pcm->dev, "PCM rx0=%*ph rx160=%*ph rx320=%*ph\n", 16,
		 pcm_rx_buf(pcm, 0), 16,
		 pcm_rx_buf(pcm, 0) + 160, 16,
		 pcm_rx_buf(pcm, 0) + 320);
	return seen > 0 ? 0 : -ETIMEDOUT;
}

static int pcm_tone_test(struct an7581_pcm *pcm, bool loopback)
{
	int i, j;

	pcm->tone_phase = 0;
	pcm_start(pcm, loopback, false);
	dev_info(pcm->dev, "PCM 1 kHz tone for 10 s (loopback=%d)\n", loopback);
	for (i = 0; i < 1000; i++) {
		msleep(10);
		for (j = 0; j < PCM_DESC_COUNT; j++) {
			struct an7581_pcm_desc *tx = pcm_tx_desc(pcm, j);
			struct an7581_pcm_desc *rx = pcm_rx_desc(pcm, j);

			if (!(READ_ONCE(tx->status) & PCM_DESC_OWN)) {
				pcm_fill_tone(pcm, j);
				tx->status = PCM_DESC_OWN | PCM_SAMPLE_SIZE;
				dma_wmb();
				writel(1, pcm->base + PCM_TX_POLL);
			}
			if (!(READ_ONCE(rx->status) & PCM_DESC_OWN))
				pcm_arm_rx(pcm, j);
		}
	}
	pcm_stop(pcm);
	if (loopback)
		dev_info(pcm->dev, "PCM rx0=%*ph rx160=%*ph rx320=%*ph\n", 16,
			 pcm_rx_buf(pcm, 0), 16,
			 pcm_rx_buf(pcm, 0) + 160, 16,
			 pcm_rx_buf(pcm, 0) + 320);
	dev_info(pcm->dev, "PCM tone test finished\n");
	return 0;
}

static void pcm_dump(struct an7581_pcm *pcm)
{
	int i;

	dev_info(pcm->dev,
		 "PCM ctrl=%08x dma=%08x ring=%08x isr=%08x intmask=%08x\n",
		 readl(pcm->base + PCM_CTRL), readl(pcm->base + PCM_DMA),
		 readl(pcm->base + PCM_RING_SIZE),
		 readl(pcm->base + PCM_ISR),
		 readl(pcm->base + PCM_INTMASK));
	dev_info(pcm->dev,
		 "PCM irq=%d tx_done=%u rx_done=%u tx_underruns=%u rx_overruns=%u dma_errors=%u lec=%d/%d\n",
		 pcm->irq, pcm->tx_done, pcm->rx_done, pcm->tx_underruns,
		 pcm->rx_overruns, pcm->dma_errors,
		 pcm->lec ? 1 : 0, pcm->lec_active);
	dev_info(pcm->dev, "PCM txring=%08x rxring=%08x chan=%08x\n",
		 readl(pcm->base + PCM_TX_RING),
		 readl(pcm->base + PCM_RX_RING),
		 readl(pcm->base + PCM_CHAN_ENABLE));
	for (i = 0; i < PCM_DESC_COUNT; i++) {
		dev_info(pcm->dev, "tx[%02d] %08x %08x %08x\n", i,
			 pcm_tx_desc(pcm, i)->status,
			 pcm_tx_desc(pcm, i)->ch_valid,
			 pcm_tx_desc(pcm, i)->buf_addr);
		dev_info(pcm->dev, "rx[%02d] %08x %08x %08x\n", i,
			 pcm_rx_desc(pcm, i)->status,
			 pcm_rx_desc(pcm, i)->ch_valid,
			 pcm_rx_desc(pcm, i)->buf_addr);
	}
}

static ssize_t pcm_proc_write(struct file *file, const char __user *ubuf,
			      size_t count, loff_t *ppos)
{
	struct an7581_pcm *pcm = pcm_dbg;
	char *buf, *cmd;
	int ret = 0;

	if (!pcm)
		return -ENODEV;

	buf = memdup_user_nul(ubuf, count);
	if (IS_ERR(buf))
		return PTR_ERR(buf);
	cmd = strim(buf);

	mutex_lock(&pcm->lock);
	if (!strcmp(cmd, "test")) {
		ret = pcm_loopback_test(pcm);
	} else if (!strcmp(cmd, "loopback")) {
		ret = pcm_start(pcm, true, true);
	} else if (!strcmp(cmd, "start")) {
		ret = pcm_start_stream(pcm, false);
	} else if (!strcmp(cmd, "tone")) {
		ret = pcm_tone_test(pcm, false);
	} else if (!strcmp(cmd, "toneloop")) {
		ret = pcm_tone_test(pcm, true);
	} else if (!strcmp(cmd, "stop")) {
		pcm_stop(pcm);
	} else if (!strcmp(cmd, "dump")) {
		pcm_dump(pcm);
	} else if (!strcmp(cmd, "ec")) {
		dev_info(pcm->dev,
			 "OSLEC taps=%d enable=%d active=%d mode=0x%x\n",
			 lec_taps, READ_ONCE(lec_enable), pcm->lec_active,
			 lec_adaption_mode);
	} else if (!strcmp(cmd, "ecreset")) {
		if (pcm->lec)
			oslec_flush(pcm->lec);
	} else if (!strcmp(cmd, "rx")) {
		int ch;

		dev_info(pcm->dev, "PCM tx_done=%u rx_done=%u\n",
			 pcm->tx_done, pcm->rx_done);
		for (ch = 0; ch < PCM_CHANNELS; ch++)
			dev_info(pcm->dev, "PCM ch%d peak=%u data=%*ph\n", ch,
				 pcm->rx_peak[ch], 32, pcm->last_rx[ch]);
	} else if (!strcmp(cmd, "rxreset")) {
		memset(pcm->rx_peak, 0, sizeof(pcm->rx_peak));
		memset(pcm->last_rx, 0, sizeof(pcm->last_rx));
	} else {
		ret = -EINVAL;
	}
	mutex_unlock(&pcm->lock);

	kfree(buf);
	return ret ? ret : count;
}

static int pcm_dev_open(struct inode *inode, struct file *file)
{
	struct an7581_pcm *pcm = pcm_dbg;
	int ret;

	if (!pcm)
		return -ENODEV;
	if (atomic_xchg(&pcm->opened, 1))
		return -EBUSY;

	mutex_lock(&pcm->lock);
	pcm->tone_mode = false;
	kfifo_reset(&pcm->tx_fifo);
	kfifo_reset(&pcm->rx_fifo);
	pcm->tx_done = 0;
	pcm->rx_done = 0;
	pcm->tx_underruns = 0;
	pcm->rx_overruns = 0;
	pcm->dma_errors = 0;
	ret = pcm_lec_alloc(pcm);
	if (ret) {
		mutex_unlock(&pcm->lock);
		atomic_set(&pcm->opened, 0);
		return ret;
	}
	memset(pcm->tx_desc_ref, 0, sizeof(pcm->tx_desc_ref));
	if (pcm->lec)
		oslec_flush(pcm->lec);
	pcm_start(pcm, char_loopback, char_loopback);
	pcm_tx_prefill(pcm);
	pcm->streaming = true;
	mutex_unlock(&pcm->lock);
	return 0;
}

static int pcm_dev_release(struct inode *inode, struct file *file)
{
	struct an7581_pcm *pcm = pcm_dbg;

	if (!pcm)
		return 0;
	mutex_lock(&pcm->lock);
	pcm_stop(pcm);
	kfifo_reset(&pcm->tx_fifo);
	kfifo_reset(&pcm->rx_fifo);
	mutex_unlock(&pcm->lock);
	atomic_set(&pcm->opened, 0);
	return 0;
}

static ssize_t pcm_dev_read(struct file *file, char __user *ubuf,
			    size_t count, loff_t *ppos)
{
	struct an7581_pcm *pcm = pcm_dbg;
	unsigned int copied;
	int ret;

	if (!pcm)
		return -ENODEV;
	if (count == 0)
		return 0;

	if (file->f_flags & O_NONBLOCK) {
		if (kfifo_len(&pcm->rx_fifo) < PCM_FRAME_SIZE &&
		    READ_ONCE(pcm->streaming))
			return -EAGAIN;
	} else {
		ret = wait_event_interruptible(pcm->rx_wait,
				       kfifo_len(&pcm->rx_fifo) >= PCM_FRAME_SIZE ||
				       !READ_ONCE(pcm->streaming));
		if (ret)
			return ret;
	}
	if (!READ_ONCE(pcm->streaming) && !kfifo_len(&pcm->rx_fifo))
		return 0;

	ret = kfifo_to_user(&pcm->rx_fifo, ubuf, count, &copied);
	return ret ? ret : copied;
}

static ssize_t pcm_dev_write(struct file *file, const char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	struct an7581_pcm *pcm = pcm_dbg;
	unsigned int copied = 0;
	int ret;

	if (!pcm)
		return -ENODEV;
	if (count == 0)
		return 0;

	if (!READ_ONCE(pcm->streaming)) {
		mutex_lock(&pcm->lock);
		if (!pcm->streaming) {
			ret = pcm_start_stream(pcm, false);
			if (ret) {
				mutex_unlock(&pcm->lock);
				return ret;
			}
		}
		mutex_unlock(&pcm->lock);
	}

	ret = kfifo_from_user(&pcm->tx_fifo, ubuf, count, &copied);
	return ret ? ret : copied;
}

static __poll_t pcm_dev_poll(struct file *file, poll_table *wait)
{
	struct an7581_pcm *pcm = pcm_dbg;
	__poll_t mask = 0;

	if (!pcm)
		return EPOLLERR;
	poll_wait(file, &pcm->rx_wait, wait);
	if (kfifo_len(&pcm->rx_fifo) >= PCM_FRAME_SIZE ||
	    !READ_ONCE(pcm->streaming))
		mask |= EPOLLIN | EPOLLRDNORM;
	return mask;
}

static const struct file_operations pcm_fops = {
	.owner = THIS_MODULE,
	.open = pcm_dev_open,
	.release = pcm_dev_release,
	.read = pcm_dev_read,
	.write = pcm_dev_write,
	.poll = pcm_dev_poll,
};

static ssize_t pcm_proc_read(struct file *file, char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	struct an7581_pcm *pcm = pcm_dbg;
	ssize_t ret;

	if (!pcm)
		return -ENODEV;
	mutex_lock(&pcm->lock);
	ret = simple_read_from_buffer(ubuf, count, ppos, pcm->last_rx,
				      sizeof(pcm->last_rx));
	mutex_unlock(&pcm->lock);
	return ret;
}

static const struct proc_ops pcm_proc_ops = {
	.proc_read = pcm_proc_read,
	.proc_write = pcm_proc_write,
};

static int an7581_pcm_probe(struct platform_device *pdev)
{
	struct an7581_pcm *pcm;
	int ret;

	pcm = devm_kzalloc(&pdev->dev, sizeof(*pcm), GFP_KERNEL);
	if (!pcm)
		return -ENOMEM;

	pcm->dev = &pdev->dev;
	pcm->irq = -1;
	mutex_init(&pcm->lock);
	init_waitqueue_head(&pcm->rx_wait);
	atomic_set(&pcm->opened, 0);
	ret = kfifo_alloc(&pcm->tx_fifo, PCM_FIFO_SIZE, GFP_KERNEL);
	if (ret)
		return ret;
	ret = kfifo_alloc(&pcm->rx_fifo, PCM_FIFO_SIZE, GFP_KERNEL);
	if (ret)
		return ret;
	pcm->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(pcm->base))
		return PTR_ERR(pcm->base);

	ret = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	pcm->desc_cpu = dma_alloc_coherent(&pdev->dev,
					   PCM_DESC_COUNT * 2 * PCM_DESC_SIZE,
					   &pcm->desc_dma, GFP_KERNEL);
	if (!pcm->desc_cpu)
		return -ENOMEM;

	pcm->buf_cpu = dma_alloc_coherent(&pdev->dev,
					  PCM_DESC_COUNT * 2 * PCM_BUF_SIZE,
					  &pcm->buf_dma, GFP_KERNEL);
	if (!pcm->buf_cpu)
		return -ENOMEM;

	if (use_irq) {
		pcm->irq = platform_get_irq_optional(pdev, 0);
		if (pcm->irq == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		if (pcm->irq >= 0) {
			ret = devm_request_threaded_irq(&pdev->dev, pcm->irq,
							pcm_irq, pcm_irq_thread,
							IRQF_ONESHOT,
							dev_name(&pdev->dev), pcm);
			if (ret) {
				dev_warn(&pdev->dev,
					 "PCM IRQ %d unavailable (%d), using kthread poll\n",
					 pcm->irq, ret);
				pcm->irq = -1;
			}
		}
	}

	if (pcm->irq < 0) {
		pcm->thread = kthread_run(pcm_thread, pcm, "an7581-pcm");
		if (IS_ERR(pcm->thread)) {
			ret = PTR_ERR(pcm->thread);
			pcm->thread = NULL;
			dev_err(&pdev->dev, "cannot start PCM thread: %d\n", ret);
			return ret;
		}
		sched_set_fifo(pcm->thread);
	}

	platform_set_drvdata(pdev, pcm);
	pcm_dbg = pcm;
	proc_create(DRV_NAME, 0644, NULL, &pcm_proc_ops);
	pcm->misc.minor = MISC_DYNAMIC_MINOR;
	pcm->misc.name = DRV_NAME;
	pcm->misc.fops = &pcm_fops;
	ret = misc_register(&pcm->misc);
	if (ret) {
		remove_proc_entry(DRV_NAME, NULL);
		if (pcm->thread)
			kthread_stop(pcm->thread);
		pcm->thread = NULL;
		return ret;
	}
	dev_info(&pdev->dev,
		 "AN7581 PCM ready (/dev/%s), irq=%d, refill=%s, OSLEC=%s\n",
		 DRV_NAME, pcm->irq,
		 pcm->irq >= 0 ? "irq" : "kthread-poll",
		 READ_ONCE(lec_enable) ? "on" : "off");
	return 0;
}

static void an7581_pcm_remove(struct platform_device *pdev)
{
	struct an7581_pcm *pcm = platform_get_drvdata(pdev);

	misc_deregister(&pcm->misc);
	remove_proc_entry(DRV_NAME, NULL);
	if (pcm->thread)
		kthread_stop(pcm->thread);
	mutex_lock(&pcm->lock);
	pcm_stop(pcm);
	mutex_unlock(&pcm->lock);
	pcm_lec_free(pcm);
	kfifo_free(&pcm->tx_fifo);
	kfifo_free(&pcm->rx_fifo);
	pcm_dbg = NULL;
}

static const struct of_device_id an7581_pcm_of_match[] = {
	{ .compatible = "airoha,an7581-pcm" },
	/* Older HG5582A DTs used the EN75xx-era board compatible. */
	{ .compatible = "airoha,en7581-pcm" },
	{ }
};
MODULE_DEVICE_TABLE(of, an7581_pcm_of_match);

static struct platform_driver an7581_pcm_driver = {
	.probe = an7581_pcm_probe,
	.remove = an7581_pcm_remove,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = an7581_pcm_of_match,
	},
};

static struct platform_device *an7581_pcm_fallback;

static int __init an7581_pcm_init(void)
{
	struct resource res = {
		.start = 0x1fbd0000,
		.end = 0x1fbd0fff,
		.flags = IORESOURCE_MEM,
	};
	struct device_node *np;
	int ret;

	/* The first flashed 6.18.60 image predates the PCM DT node. */
	np = of_find_compatible_node(NULL, NULL, "airoha,an7581-pcm");
	if (!np)
		np = of_find_compatible_node(NULL, NULL, "airoha,en7581-pcm");
	if (np) {
		of_node_put(np);
	} else {
		an7581_pcm_fallback = platform_device_register_simple(
			DRV_NAME, -1, &res, 1);
		if (IS_ERR(an7581_pcm_fallback))
			return PTR_ERR(an7581_pcm_fallback);
	}

	ret = platform_driver_register(&an7581_pcm_driver);
	if (ret) {
		if (an7581_pcm_fallback)
			platform_device_unregister(an7581_pcm_fallback);
		return ret;
	}
	return 0;
}

static void __exit an7581_pcm_exit(void)
{
	platform_driver_unregister(&an7581_pcm_driver);
	if (an7581_pcm_fallback)
		platform_device_unregister(an7581_pcm_fallback);
}

module_init(an7581_pcm_init);
module_exit(an7581_pcm_exit);

MODULE_DESCRIPTION("Airoha AN7581 PCM engine with OSLEC");
MODULE_LICENSE("GPL");
