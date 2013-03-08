//UIO driver for LCI.  Allows mmap-ing just the FPGA registers amd DMA space,
//and handling interrupts from the FPGA

#include <linux/device.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/uio_driver.h>
#include <linux/slab.h>

#include <asm/cacheflush.h>

struct lci_dev {
	struct uio_info info;
	unsigned long flags;
	spinlock_t lock;
};

#define IRQ_ENABLE_FLAG 0

static irqreturn_t uio_lci_handler(int irq, struct uio_info *dev_info)
{
	struct lci_dev *priv = dev_info->priv;

	if (!test_and_set_bit(IRQ_ENABLE_FLAG, &priv->flags))
		disable_irq_nosync(irq);

	return IRQ_HANDLED;
}

static int uio_lci_irqcontrol(struct uio_info *dev_info, s32 irq_on)
{
	struct lci_dev *priv = dev_info->priv;
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	if(irq_on) {
		if (test_and_clear_bit(IRQ_ENABLE_FLAG, &priv->flags))
			enable_irq(dev_info->irq);
	} else {
		if (!test_and_set_bit(IRQ_ENABLE_FLAG, &priv->flags))
			disable_irq(dev_info->irq);
	}
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int __devinit lci_probe(struct platform_device *dev)
{
	struct lci_dev* pdev;
	int irq, err;

	printk(KERN_INFO "Probed the lci device!\n");
	//register with uio
	pdev = kzalloc(sizeof(struct lci_dev), GFP_KERNEL);
	if(!pdev) {
		err = -ENOMEM;
		goto err_alloc;
	}

	pdev->info.name = "lci_dev";
	pdev->info.version = "1.00a";
	pdev->info.handler = uio_lci_handler;
	pdev->info.irqcontrol = uio_lci_irqcontrol;
	pdev->info.priv = pdev;
	//Mem region 0 is for the FPGA registers
	pdev->info.mem[0].name = "registers";
	pdev->info.mem[0].addr = dev->resource[0].start;
	pdev->info.mem[0].size = dev->resource[0].end - dev->resource[0].start + 1;
	pdev->info.mem[0].memtype = UIO_MEM_PHYS;
	//Mem region 1 is for accessing the DMA memory
	//(the top half of physical RAM)
	pdev->info.mem[1].name = "DMA_mem";
	pdev->info.mem[1].addr = dev->resource[1].start;
	pdev->info.mem[1].size = dev->resource[1].end - dev->resource[1].start + 1;
	pdev->info.mem[1].memtype = UIO_MEM_PHYS;
	//Set flags to 0, since our IRQ is enabled initially
	pdev->flags = 0;

	irq = platform_get_irq(dev, 0);
	if (irq == -ENXIO)
		pdev->info.irq = UIO_IRQ_NONE;
	else
		pdev->info.irq = irq;

	spin_lock_init(&pdev->lock);

	if(uio_register_device(&dev->dev, &pdev->info))
		goto err_register;

	platform_set_drvdata(dev, pdev);
	return 0;

err_register:
	kfree(pdev);
err_alloc:
	return err;
}

static int __devexit lci_remove(struct platform_device *dev)
{
	struct lci_dev* pdev = platform_get_drvdata(dev);
	uio_unregister_device(&pdev->info);
	kfree(pdev);
	platform_set_drvdata(dev, NULL);
	printk(KERN_INFO "cleaned up the lci device!\n");
        return 0;
}

static struct of_device_id lci_of_match[] __devinitdata = {
	        { .compatible = "ni,lci-1.00.a", },
		        { /* end of table */}
};
MODULE_DEVICE_TABLE(of, lci_of_match);

static struct platform_driver lci_driver = {
        .probe = lci_probe,
        .remove = __devexit_p(lci_remove),
        .driver = {
		.name = "lci_uio",
		.owner = THIS_MODULE,
		.of_match_table = lci_of_match,
		},
};

static int __init lci_init_module(void)
{
        return platform_driver_register(&lci_driver);
}

module_init(lci_init_module);

static void __exit lci_exit_module(void)
{
        platform_driver_unregister(&lci_driver);
}

module_exit(lci_exit_module);

MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Nathan Sullivan <nathan.sullivan@ni.com");
