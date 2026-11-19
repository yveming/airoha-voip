// SPDX-License-Identifier: GPL-2.0-only
/*
 * Airoha AN7581 SLIC core.
 *
 * One module owns the SLIC backend table.  Backends are selected by the
 * transport and, for Zarlink ZSI parts, by a safe RCN/PCN read.  The
 * provider (an7581-fxs) only asks for the selected fxs_slic_ops.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "an7581_slic.h"

static char *slic_type = "auto";
module_param(slic_type, charp, 0444);
MODULE_PARM_DESC(slic_type, "SLIC type: auto, le9641, le9642, pef32001");

static char *transport = "ZSI";
module_param(transport, charp, 0444);
MODULE_PARM_DESC(transport, "SLIC transport: ZSI or CSI");

static int bus_slot = 4;
module_param(bus_slot, int, 0444);
MODULE_PARM_DESC(bus_slot, "PCM bus slot for the FXS line (default 4)");

static bool tx_shift = true;
module_param(tx_shift, bool, 0644);
MODULE_PARM_DESC(tx_shift, "shift the ZSI transmit slot down by one slot");

static const struct an7581_slic_backend an7581_slic_backends[] = {
	{
		.name = "le9641",
		.transport = "ZSI",
		.type = AN7581_SLIC_LE9641,
		.supported = true,
	},
	{
		.name = "le9642",
		.transport = "ZSI",
		.type = AN7581_SLIC_LE9642,
		.supported = false,
	},
	{
		.name = "pef32001",
		.transport = "CSI",
		.type = AN7581_SLIC_PEF32001,
		.supported = false,
	},
};

static struct an7581_slic *an7581_slic_dev;

static const struct an7581_slic_backend *
an7581_slic_lookup(enum an7581_slic_type type)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(an7581_slic_backends); i++) {
		if (an7581_slic_backends[i].type == type)
			return &an7581_slic_backends[i];
	}

	return NULL;
}

const struct fxs_slic_ops *an7581_slic_get_ops(void **priv)
{
	if (!try_module_get(THIS_MODULE))
		return ERR_PTR(-ENODEV);

	if (!an7581_slic_dev || !an7581_slic_dev->ops) {
		module_put(THIS_MODULE);
		return ERR_PTR(-ENODEV);
	}

	if (priv)
		*priv = an7581_slic_dev;

	return an7581_slic_dev->ops;
}
EXPORT_SYMBOL_GPL(an7581_slic_get_ops);

void an7581_slic_put(void)
{
	module_put(THIS_MODULE);
}
EXPORT_SYMBOL_GPL(an7581_slic_put);

static int an7581_slic_probe(struct platform_device *pdev)
{
	const struct an7581_slic_backend *backend;
	struct an7581_slic *slic;
	u32 slots[2];
	int ret;

	slic = devm_kzalloc(&pdev->dev, sizeof(*slic), GFP_KERNEL);
	if (!slic)
		return -ENOMEM;

	slic->dev = &pdev->dev;
	mutex_init(&slic->lock);
	init_waitqueue_head(&slic->hook_wait);

	slic->zsi = an7581_zsi_get(&pdev->dev, "airoha,zsi");
	if (!slic->zsi)
		return -EPROBE_DEFER;

	slic->bus_slot = bus_slot;
	slic->tx_shift = tx_shift;
	if (pdev->dev.of_node &&
	    !of_property_read_u32_array(pdev->dev.of_node, "airoha,bus-slots",
					slots, 1))
		slic->bus_slot = slots[0];
	if (pdev->dev.of_node &&
	    of_property_read_bool(pdev->dev.of_node, "airoha,no-zsi-tx-shift"))
		slic->tx_shift = false;

	if (!strcmp(transport, "CSI")) {
		ret = an7581_slic_pef32001_detect(slic);
		dev_info(&pdev->dev, "pef32001 unsupported\n");
		return ret ? ret : -EOPNOTSUPP;
	}

	if (strcmp(transport, "ZSI")) {
		dev_err(&pdev->dev, "unknown transport %s\n", transport);
		return -EINVAL;
	}

	ret = an7581_slic_zarlink_detect(slic);
	if (ret) {
		if (slic->rcn || slic->pcn)
			dev_err(&pdev->dev, "unknown SLIC id %02x %02x\n",
				slic->rcn, slic->pcn);
		else
			dev_err(&pdev->dev, "no SLIC detected\n");
		return ret;
	}

	backend = an7581_slic_lookup(slic->type);
	if (!backend) {
		dev_err(&pdev->dev, "unknown SLIC id %02x %02x\n",
			slic->rcn, slic->pcn);
		return -ENODEV;
	}

	if (!backend->supported) {
		dev_info(&pdev->dev, "%s detected, unsupported\n", backend->name);
		return -EOPNOTSUPP;
	}

	if (strcmp(slic_type, "auto") && strcmp(slic_type, backend->name)) {
		dev_info(&pdev->dev, "%s detected, requested %s\n",
			 backend->name, slic_type);
		return -EOPNOTSUPP;
	}

	dev_info(&pdev->dev, "%s detected\n", backend->name);

	slic->type = backend->type;
	slic->ops = an7581_slic_zarlink_ops();

	ret = an7581_slic_zarlink_probe(slic);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, slic);
	an7581_slic_dev = slic;

	return 0;
}

static void an7581_slic_remove(struct platform_device *pdev)
{
	struct an7581_slic *slic = platform_get_drvdata(pdev);

	if (!slic)
		return;

	an7581_slic_dev = NULL;
	an7581_slic_zarlink_remove(slic);
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
		.name = AN7581_SLIC_DRV_NAME,
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
			AN7581_SLIC_DRV_NAME, -1, NULL, 0);
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

MODULE_DESCRIPTION("Airoha AN7581 SLIC core");
MODULE_LICENSE("GPL");
