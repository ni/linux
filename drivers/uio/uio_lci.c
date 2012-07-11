//UIO driver for LCI.  Allows mmap-ing just the FPGA registers, handling interrupts from the FPGA,
//and getting page lists for userspace DMA buffers through sysfs

#include <linux/device.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/uio_driver.h>
#include <linux/slab.h>

#include <asm/cacheflush.h>

struct lci_dev {
	struct uio_info info;
};

static int __devinit lci_probe(struct platform_device *dev)
{
	struct lci_dev* pdev;
	int err;

	printk(KERN_INFO "Probed the lci device!\n");
	//register with uio
	pdev = kzalloc(sizeof(struct lci_dev), GFP_KERNEL);
	if(!pdev) {
		err = -ENOMEM;
		goto err_alloc;
	}

	pdev->info.name = "lci_dev";
	pdev->info.version = "1.00a";
	pdev->info.mem[0].name = "registers";
	pdev->info.mem[0].addr = dev->resource[0].start;
	pdev->info.mem[0].size = dev->resource[0].end - dev->resource[0].start + 1;
	pdev->info.mem[0].memtype = UIO_MEM_PHYS;
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
