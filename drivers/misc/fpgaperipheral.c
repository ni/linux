/*
 * Copyright (C) 2012 National Instruments Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/atomic.h>
#include <linux/io.h>
#include <misc/fpgaperipheral.h>

#define FPGAPERIPHERAL_NAME "fpgaperipheral"
#define DEVCFG_INT_STS_OFFSET  0x0C
#define INT_STS_FPGA_DONE_MASK 0x04

BLOCKING_NOTIFIER_HEAD(fpgaperipheral_notifier_list);
EXPORT_SYMBOL_GPL(fpgaperipheral_notifier_list);

/**
 * struct fpgaperipheral_drvdata - fpgaperipheral driver structure
 * @devcfg_addr: Pointer (from ioremap) to the devcfg memory region
 */
struct fpgaperipheral_drvdata {
	void __iomem *devcfg_addr;
};

/*
 * drvdata given filewide scope instead of dynamically
 * allocated and passed around so it's easier to share between the
 * platform driver (probe/remove) and misc device driver (open/release).
 * This is okay since there is only ever 1 device.
 */
static struct fpgaperipheral_drvdata drvdata;

/* Access control: Only allow one open at a time */
static atomic_t fpgaperipheral_available = ATOMIC_INIT(1);

/**
 * is_fpga_programmed() - Checks if the FPGA is currently programmed
 * Returns non-zero iff FPGA is programmed
 */
static int is_fpga_programmed(void)
{
	return ioread32(drvdata.devcfg_addr + DEVCFG_INT_STS_OFFSET) &
		INT_STS_FPGA_DONE_MASK;
}

/**
 * notify_clients() - Depending on up_or_down, notifies clients FPGA is about
 * to be unprogrammed, or is now programmed.
 * @up_or_down: Whether FPGA is about to go down, or is now up.
 */
static void notify_clients(unsigned long up_or_down)
{
	blocking_notifier_call_chain(&fpgaperipheral_notifier_list,
						up_or_down, NULL);
}

/**
 * fpgaperipheral_misc_open() - Notifies clients that FPGA is about to go down.
 * Only allows 1 open() at a time.
 * @inode:	Pointer to the inode structure of this device.
 * @file:	Pointer to the file structure.
 * Returns 0 on success, negative error otherwise
 */
static int fpgaperipheral_misc_open(struct inode *inode, struct file *file)
{
	if (!atomic_dec_and_test(&fpgaperipheral_available)) {
		atomic_inc(&fpgaperipheral_available);
		return -EBUSY;
	}

	notify_clients(FPGA_PERIPHERAL_DOWN);
	return 0;
}

/**
 * fpgaperipheral_misc_release() - Notifies clients that FPGA is back up.
 * @inode:	Pointer to the inode structure of this device.
 * @file:	Pointer to the file structure.
 * Returns 0 on success, negative error otherwise
 */
static int fpgaperipheral_misc_release(struct inode *inode, struct file *file)
{
	if (is_fpga_programmed())
		notify_clients(FPGA_PERIPHERAL_UP);
	else
		notify_clients(FPGA_PERIPHERAL_FAILED);

	atomic_inc(&fpgaperipheral_available);
	return 0;
}

static const struct file_operations fpgaperipheral_misc_fops = {
	.owner		= THIS_MODULE,
	.open		= fpgaperipheral_misc_open,
	.release	= fpgaperipheral_misc_release
};

static struct miscdevice fpgaperipheral_misc_dev = {
	.minor		= MISC_DYNAMIC_MINOR,
	.name		= FPGAPERIPHERAL_NAME,
	.fops		= &fpgaperipheral_misc_fops,
};

/**
 * fpgaperipheral_probe - Platform driver probe
 * @pdev: Pointer to the platform device structure
 * Returns 0 on success, negative error otherwise
 */
static int fpgaperipheral_probe(struct platform_device *pdev)
{
	int retval;
	void __iomem *devcfg_addr;
	struct resource *devcfg_res;

	/* Get devcfg resource */
	devcfg_res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!devcfg_res) {
		dev_err(&pdev->dev, "Couldn't get io resource\n");
		return -ENODEV;
	}

	/* Get the mem region for devcfg */
	if (!request_mem_region(devcfg_res->start,
				resource_size(devcfg_res),
				FPGAPERIPHERAL_NAME)) {
		dev_err(&pdev->dev, "Couldn't lock memory region at %Lx\n",
			(unsigned long long) devcfg_res->start);
		return -EBUSY;
	}

	/* Map the devcfg mem region */
	devcfg_addr = ioremap(devcfg_res->start, resource_size(devcfg_res));
	if (!devcfg_addr) {
		dev_err(&pdev->dev, "ioremap failed\n");
		retval = -EIO;
		goto failed_after_request_mem_region;
	}

	/* Set the drvdata where the misc device can find it */
	drvdata.devcfg_addr = devcfg_addr;

	/* Now register the misc device */
	retval = misc_register(&fpgaperipheral_misc_dev);
	if (retval) {
		dev_err(&pdev->dev, "Couldn't register misc device\n");
		goto failed_after_ioremap;
	}

	return 0;

failed_after_ioremap:
	iounmap(drvdata.devcfg_addr);

failed_after_request_mem_region:
	release_mem_region(devcfg_res->start, resource_size(devcfg_res));

	drvdata.devcfg_addr = NULL;
	return retval;
}

/**
 * fpgaperipheral_remove - called when the platform driver is unregistered
 * @pdev: Pointer to the platform device structure
 * Returns 0 on success, negative error otherwise
 */
static int __exit fpgaperipheral_remove(struct platform_device *pdev)
{
	struct resource *devcfg_res;

	devcfg_res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	misc_deregister(&fpgaperipheral_misc_dev);
	iounmap(drvdata.devcfg_addr);
	release_mem_region(devcfg_res->start, resource_size(devcfg_res));

	drvdata.devcfg_addr = NULL;
	return 0;
}

/* Match table for of_platform binding */
#ifdef CONFIG_OF
static const struct of_device_id fpgaperipheral_of_match[] = {
	{ .compatible = FPGAPERIPHERAL_NAME, },
	{}
};
MODULE_DEVICE_TABLE(of, fpgaperipheral_of_match);
#else
#define fpgaperipheral_of_match NULL
#endif

static struct platform_driver fpgaperipheral_platform_driver = {
	.probe   = fpgaperipheral_probe,		/* Probe method */
	.remove  = __exit_p(fpgaperipheral_remove),	/* Detach method */
	.driver	 = {
		.owner = THIS_MODULE,
		.name = FPGAPERIPHERAL_NAME,		/* Driver name */
		.of_match_table = fpgaperipheral_of_match,
	},
};

/**
 * fpgaperipheral_init - Initial driver registration call
 * Returns 0 on success, negative error otherwise
 */
static int __init fpgaperipheral_init(void)
{
	return platform_driver_register(&fpgaperipheral_platform_driver);
}
module_init(fpgaperipheral_init);

/**
 * fpgaperipheral_exit - Driver unregistration call
 * Returns 0 on success, negative error otherwise
 */
static void __exit fpgaperipheral_exit(void)
{
	platform_driver_unregister(&fpgaperipheral_platform_driver);
}
module_exit(fpgaperipheral_exit);

MODULE_DESCRIPTION("Bus driver for FPGA peripherals on NI's Zynq-based controllers");
MODULE_AUTHOR("Kyle Teske <kyle.teske@ni.com>");
MODULE_LICENSE("GPL");
