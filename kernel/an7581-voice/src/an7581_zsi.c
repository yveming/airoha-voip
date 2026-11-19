// SPDX-License-Identifier: GPL-2.0-only
/*
 * Airoha AN7581 ZSI transport (HG5582A Le9641).
 *
 * This is deliberately transport-only.  The PCM clock/slot setup lives in
 * an7581-pcm; this driver only configures the ZSI wrapper and reset.
 *
 * The safe register sequence was validated on HG5582A with devmem:
 *
 *   0x1fb00094 = (old & ~0xf) | 5
 *   0x1fa20218 = (old & ~0x003f3300) | 0x00001000
 *   0x1fa201d0 |= 0x00000c01
 *   0x1fbd1000 = 0x0f030313
 *   0x1fbd1010 |= 0x19
 *   pulse 0x1fb00834 bit4 only
 *
 * Do not pulse 0x1fb00834 bits 0 or 17 on this board.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "an7581_zsi.h"

extern bool an7581_pcm_fxs_ready(void);

#define DRV_NAME	"an7581-zsi"

/* ZSI wrapper */
#define ZSI_CFG		0x00
#define ZSI_TXRX	0x04
#define ZSI_CTL		0x08
#define  ZSI_CTL_READ	BIT(0)
#define  ZSI_CTL_TX_ACK	BIT(1)
#define  ZSI_CTL_RX_RDY	BIT(2)
#define ZSI_RX		0x0c
#define ZSI_EN		0x10
#define ZSI_CHAN_SEL	0x14

#define ZSI_CFG_MASK	0xe0ffffefu
#define ZSI_CFG_VAL	0x0f000010u
#define ZSI_EN_VAL	0x19

/* NP SCU (scuclk) */
#define NP_SCU_IFACE_MODE	0x094
#define NP_SCU_RESET		0x834
#define  NP_SCU_RESET_ZSI	BIT(4)

/* Chip SCU */
#define CHIP_SCU_CLKSRC		0x218
#define  CHIP_SCU_CLKSRC_ZSI_MASK	0x002f3300u
#define  CHIP_SCU_CLKSRC_VAL	0x00001000u
#define CHIP_SCU_PINMUX		0x1d0
#define  CHIP_SCU_PINMUX_MASK	0x00000c01u
#define  CHIP_SCU_PINMUX_VAL	0x00000c01u

#define ZSI_POLL_ITERS	20000

static unsigned int gap_us = 5000;
module_param(gap_us, uint, 0644);
MODULE_PARM_DESC(gap_us, "inter-byte gap in microseconds (default 5000)");

/*
 * init_mode=0 performs the full safe SCU/wrapper/reset sequence.
 * init_mode=1 assumes the wrapper was prepared by the bootloader or by
 * manual devmem writes and only programs CFG/EN.  The bootloader does not
 * configure the ZSI interface mode on HG5582A, so use the full sequence by
 * default.
 */
static int init_mode = 0;
module_param(init_mode, int, 0444);
MODULE_PARM_DESC(init_mode, "0=full SCU init, 1=wrapper already configured");

struct an7581_zsi {
	struct device *dev;
	void __iomem *base;
	struct regmap *np_scu;
	struct regmap *chip_scu;
	struct mutex lock;
	bool ready;
};

static struct an7581_zsi *zsi_dbg;

static int zsi_wait(struct an7581_zsi *zsi, u32 bit)
{
	unsigned int i;

	for (i = 0; i < ZSI_POLL_ITERS; i++) {
		if (readl(zsi->base + ZSI_CTL) & bit)
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static void zsi_wrapper_config(struct an7581_zsi *zsi)
{
	u32 cfg = readl(zsi->base + ZSI_CFG);

	writel(0, zsi->base + ZSI_CHAN_SEL);
	writel((cfg & ZSI_CFG_MASK) | ZSI_CFG_VAL, zsi->base + ZSI_CFG);
	writel(readl(zsi->base + ZSI_EN) | ZSI_EN_VAL, zsi->base + ZSI_EN);
}

static int zsi_write_byte(struct an7581_zsi *zsi, u8 v)
{
	u32 ctl;
	int ret;

	writel(v, zsi->base + ZSI_TXRX);
	ret = zsi_wait(zsi, ZSI_CTL_TX_ACK);
	if (ret)
		return ret;

	/* OEM spi_wraper_write_cmd(): preserve the other CTL bits. */
	ctl = readl(zsi->base + ZSI_CTL);
	writel(ctl | ZSI_CTL_TX_ACK, zsi->base + ZSI_CTL);
	usleep_range(gap_us, gap_us + 1000);
	return 0;
}

static int zsi_read_byte(struct an7581_zsi *zsi, u8 *v)
{
	u32 ctl;
	int ret;

	ctl = readl(zsi->base + ZSI_CTL);
	writel(ctl | ZSI_CTL_READ, zsi->base + ZSI_CTL);
	ret = zsi_wait(zsi, ZSI_CTL_RX_RDY);
	if (ret)
		return ret;

	*v = readl(zsi->base + ZSI_RX) & 0xff;
	ctl = readl(zsi->base + ZSI_CTL);
	writel(ctl | ZSI_CTL_RX_RDY, zsi->base + ZSI_CTL);
	usleep_range(gap_us, gap_us + 1000);
	return 0;
}

int an7581_zsi_write(struct an7581_zsi *zsi, const u8 *buf, size_t len)
{
	size_t i;
	int ret = 0;

	if (!zsi || !zsi->ready)
		return -EIO;

	mutex_lock(&zsi->lock);
	for (i = 0; i < len; i++) {
		ret = zsi_write_byte(zsi, buf[i]);
		if (ret)
			break;
	}
	mutex_unlock(&zsi->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(an7581_zsi_write);

static int zsi_select_ec(struct an7581_zsi *zsi, u8 ec)
{
	/* EC 0 means a device-level command: no EC register write. */
	if (!ec || ec == 0x4b)
		return 0;
	return zsi_write_byte(zsi, 0x4a) ?: zsi_write_byte(zsi, ec);
}

int an7581_zsi_read_reg(struct an7581_zsi *zsi, u8 ec, u8 reg,
			u8 *buf, size_t len)
{
	size_t i;
	int ret;

	if (!zsi || !zsi->ready)
		return -EIO;

	mutex_lock(&zsi->lock);
	ret = zsi_select_ec(zsi, ec);
	if (ret)
		goto out;
	ret = zsi_write_byte(zsi, reg);
	if (ret)
		goto out;
	ret = zsi_write_byte(zsi, 0x06);
	if (ret)
		goto out;
	for (i = 0; i < len; i++) {
		ret = zsi_read_byte(zsi, &buf[i]);
		if (ret)
			goto out;
	}
out:
	mutex_unlock(&zsi->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(an7581_zsi_read_reg);

int an7581_zsi_write_reg(struct an7581_zsi *zsi, u8 ec, u8 reg,
			 const u8 *buf, size_t len)
{
	size_t i;
	int ret;

	if (!zsi || !zsi->ready)
		return -EIO;

	mutex_lock(&zsi->lock);
	ret = zsi_select_ec(zsi, ec);
	if (ret)
		goto out;
	ret = zsi_write_byte(zsi, reg);
	if (ret)
		goto out;
	for (i = 0; i < len; i++) {
		ret = zsi_write_byte(zsi, buf[i]);
		if (ret)
			goto out;
	}
out:
	mutex_unlock(&zsi->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(an7581_zsi_write_reg);

struct an7581_zsi *an7581_zsi_get(struct device *dev, const char *phandle)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct an7581_zsi *zsi = NULL;

	if (dev && dev->of_node) {
		np = of_parse_phandle(dev->of_node, phandle, 0);
		if (np) {
			pdev = of_find_device_by_node(np);
			of_node_put(np);
			if (pdev)
				zsi = platform_get_drvdata(pdev);
		}
	}
	if (!zsi)
		zsi = zsi_dbg;
	return zsi;
}
EXPORT_SYMBOL_GPL(an7581_zsi_get);

void an7581_zsi_put(struct an7581_zsi *zsi)
{
}
EXPORT_SYMBOL_GPL(an7581_zsi_put);

static int zsi_hw_init(struct an7581_zsi *zsi)
{
	int ret;

	if (init_mode == 0) {
		if (zsi->np_scu) {
			ret = regmap_update_bits(zsi->np_scu, NP_SCU_IFACE_MODE,
						 0xf, 0x5);
			if (ret)
				return ret;
		}
		if (zsi->chip_scu) {
			ret = regmap_update_bits(zsi->chip_scu, CHIP_SCU_CLKSRC,
						 CHIP_SCU_CLKSRC_ZSI_MASK,
						 CHIP_SCU_CLKSRC_VAL);
			if (ret)
				return ret;
			ret = regmap_update_bits(zsi->chip_scu, CHIP_SCU_PINMUX,
						 CHIP_SCU_PINMUX_MASK,
						 CHIP_SCU_PINMUX_VAL);
			if (ret)
				return ret;
		}
		zsi_wrapper_config(zsi);
		if (zsi->np_scu) {
			ret = regmap_update_bits(zsi->np_scu, NP_SCU_RESET,
						 NP_SCU_RESET_ZSI,
						 NP_SCU_RESET_ZSI);
			if (ret)
				return ret;
			usleep_range(5000, 6000);
			ret = regmap_update_bits(zsi->np_scu, NP_SCU_RESET,
						 NP_SCU_RESET_ZSI, 0);
			if (ret)
				return ret;
			usleep_range(5000, 6000);
		}
	}
	zsi_wrapper_config(zsi);
	zsi->ready = true;
	return 0;
}

static ssize_t zsi_proc_write(struct file *file, const char __user *ubuf,
			      size_t count, loff_t *ppos)
{
	struct an7581_zsi *zsi = zsi_dbg;
	char *buf, *cmd, *p;
	u8 ec, reg, data[16];
	int len, i, n;
	int ret = 0;

	if (!zsi)
		return -ENODEV;

	buf = memdup_user_nul(ubuf, count);
	if (IS_ERR(buf))
		return PTR_ERR(buf);
	cmd = strim(buf);
	if (!*cmd)
		goto out;

	if (!strcmp(cmd, "id")) {
		u8 id[2] = { 0 };
		ret = an7581_zsi_read_reg(zsi, 1, 0x73, id, sizeof(id));
		if (ret)
			dev_err(zsi->dev, "ID read failed: %d\n", ret);
		else
			dev_info(zsi->dev, "AN7581 ZSI ID: %02x %02x\n",
				 id[0], id[1]);
		goto out;
	}

	if (!strcmp(cmd, "dump")) {
		dev_info(zsi->dev, "ZSI cfg=%08x ctl=%08x en=%08x sel=%08x\n",
			 readl(zsi->base + ZSI_CFG),
			 readl(zsi->base + ZSI_CTL),
			 readl(zsi->base + ZSI_EN),
			 readl(zsi->base + ZSI_CHAN_SEL));
		goto out;
	}

	p = cmd;
	cmd = strsep(&p, " ");
	if (!strcmp(cmd, "r")) {
		char *a = strsep(&p, " ");
		char *b = strsep(&p, " ");
		char *c = strsep(&p, " ");
		if (!a || !b || !c ||
		    kstrtou8(a, 16, &ec) || kstrtou8(b, 16, &reg) ||
		    kstrtoint(c, 10, &len) || len <= 0 || len > sizeof(data)) {
			ret = -EINVAL;
			goto out;
		}
		ret = an7581_zsi_read_reg(zsi, ec, reg, data, len);
		if (ret) {
			dev_err(zsi->dev, "read %02x/%02x failed: %d\n",
				ec, reg, ret);
			goto out;
		}
		for (i = 0; i < len; i++)
			dev_info(zsi->dev, "r %02x/%02x[%d]=%02x\n",
				 ec, reg, i, data[i]);
		goto out;
	}

	if (!strcmp(cmd, "w")) {
		char *a = strsep(&p, " ");
		char *b = strsep(&p, " ");
		if (!a || !b ||
		    kstrtou8(a, 16, &ec) || kstrtou8(b, 16, &reg)) {
			ret = -EINVAL;
			goto out;
		}
		n = 0;
		while (p && *p && n < ARRAY_SIZE(data)) {
			char *t = strsep(&p, " ");
			if (!*t)
				continue;
			if (kstrtou8(t, 16, &data[n])) {
				ret = -EINVAL;
				goto out;
			}
			n++;
		}
		if (!n) {
			ret = -EINVAL;
			goto out;
		}
		ret = an7581_zsi_write_reg(zsi, ec, reg, data, n);
		if (ret)
			dev_err(zsi->dev, "write %02x/%02x failed: %d\n",
				ec, reg, ret);
		goto out;
	}

	ret = -EINVAL;
out:
	kfree(buf);
	return ret ? ret : count;
}

static const struct proc_ops zsi_proc_ops = {
	.proc_write = zsi_proc_write,
};

static int an7581_zsi_probe(struct platform_device *pdev)
{
	struct an7581_zsi *zsi;
	int ret;

	zsi = devm_kzalloc(&pdev->dev, sizeof(*zsi), GFP_KERNEL);
	if (!zsi)
		return -ENOMEM;

	zsi->dev = &pdev->dev;
	mutex_init(&zsi->lock);
	zsi->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(zsi->base))
		return PTR_ERR(zsi->base);
	zsi->np_scu = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						      "airoha,scu");
	if (IS_ERR(zsi->np_scu))
		zsi->np_scu = NULL;
	zsi->chip_scu = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
							"airoha,chip-scu");
	if (IS_ERR(zsi->chip_scu))
		zsi->chip_scu = NULL;

	if (!an7581_pcm_fxs_ready())
		return -EPROBE_DEFER;

	platform_set_drvdata(pdev, zsi);
	zsi_dbg = zsi;

	ret = zsi_hw_init(zsi);
	if (ret) {
		dev_err(&pdev->dev, "ZSI init failed: %d\n", ret);
		zsi_dbg = NULL;
		return ret;
	}

	proc_create(DRV_NAME, 0644, NULL, &zsi_proc_ops);
	of_platform_populate(pdev->dev.of_node, NULL, NULL, &pdev->dev);
	dev_info(&pdev->dev, "AN7581 ZSI transport ready (init_mode=%d)\n",
		 init_mode);
	return 0;
}

static void an7581_zsi_remove(struct platform_device *pdev)
{
	of_platform_depopulate(&pdev->dev);
	remove_proc_entry(DRV_NAME, NULL);
	zsi_dbg = NULL;
}

static const struct of_device_id an7581_zsi_of_match[] = {
	{ .compatible = "airoha,an7581-zsi" },
	/* Older Econet/Airoha DTs use the SoC-family compatible. */
	{ .compatible = "econet,an7581-zsi" },
	{ }
};
MODULE_DEVICE_TABLE(of, an7581_zsi_of_match);

static struct platform_driver an7581_zsi_driver = {
	.probe = an7581_zsi_probe,
	.remove = an7581_zsi_remove,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = an7581_zsi_of_match,
	},
};
module_platform_driver(an7581_zsi_driver);

MODULE_DESCRIPTION("Airoha AN7581 ZSI transport");
MODULE_LICENSE("GPL");
