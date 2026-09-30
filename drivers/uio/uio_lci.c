// SPDX-License-Identifier: GPL-2.0
/*
 * UIO driver for the NI LCI FPGA. Allows mmap-ing just the FPGA registers
 * and the DMA space, and handling interrupts from the FPGA.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/uio_driver.h>

struct lci_dev {
	struct uio_info info;
	unsigned long flags;
	spinlock_t lock;
};

#define IRQ_DISABLED_FLAG 0

static irqreturn_t uio_lci_handler(int irq, struct uio_info *dev_info)
{
	struct lci_dev *priv = dev_info->priv;

	if (!test_and_set_bit(IRQ_DISABLED_FLAG, &priv->flags))
		disable_irq_nosync(irq);

	return IRQ_HANDLED;
}

static int uio_lci_irqcontrol(struct uio_info *dev_info, s32 irq_on)
{
	struct lci_dev *priv = dev_info->priv;
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	if (irq_on) {
		if (test_and_clear_bit(IRQ_DISABLED_FLAG, &priv->flags))
			enable_irq(dev_info->irq);
	} else {
		if (!test_and_set_bit(IRQ_DISABLED_FLAG, &priv->flags))
			disable_irq_nosync(dev_info->irq);
	}
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int lci_probe(struct platform_device *pdev)
{
	struct lci_dev *priv;
	struct resource *res;
	int i, irq;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->info.name = "lci_dev";
	priv->info.version = "1.00a";
	priv->info.handler = uio_lci_handler;
	priv->info.irqcontrol = uio_lci_irqcontrol;
	priv->info.priv = priv;

	/* Map 0 is the FPGA registers, map 1 is the DMA memory. */
	for (i = 0; i < 2; i++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!res) {
			dev_err(&pdev->dev, "missing memory resource %d\n", i);
			return -EINVAL;
		}
		priv->info.mem[i].name = i ? "DMA_mem" : "registers";
		priv->info.mem[i].addr = res->start;
		priv->info.mem[i].size = resource_size(res);
		priv->info.mem[i].memtype = UIO_MEM_PHYS;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq == -EPROBE_DEFER)
		return irq;
	priv->info.irq = irq < 0 ? UIO_IRQ_NONE : irq;

	spin_lock_init(&priv->lock);
	platform_set_drvdata(pdev, priv);

	return uio_register_device(&pdev->dev, &priv->info);
}

static int lci_remove(struct platform_device *pdev)
{
	struct lci_dev *priv = platform_get_drvdata(pdev);

	uio_unregister_device(&priv->info);
	return 0;
}

static const struct of_device_id lci_of_match[] = {
	{ .compatible = "ni,lci-1.00.a", },
	{ /* end of table */ }
};
MODULE_DEVICE_TABLE(of, lci_of_match);

static struct platform_driver lci_driver = {
	.probe = lci_probe,
	.remove = lci_remove,
	.driver = {
		.name = "lci_uio",
		.of_match_table = lci_of_match,
	},
};
module_platform_driver(lci_driver);

MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Nathan Sullivan <nathan.sullivan@ni.com>");
MODULE_DESCRIPTION("UIO driver for the NI LCI FPGA");
