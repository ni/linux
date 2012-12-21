/*
 * Copyright (c) 2011 Xilinx Inc.
 * Copyright (c) 2012 National Instruments Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the Free
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA
 * 02139, USA.
 */

#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/clocksource.h>

#include <mach/zynq_soc.h>

#include <asm/sched_clock.h>

struct xscugtimer {
	u32 counter0;
	u32 counter1;
	u32 control;
};

static struct xscugtimer __iomem *scug_timer;
static u32 mult, shift;

#define xscugtimer_writel(r,v)		__raw_writel(v,r)
#define xscugtimer_readl(r)		__raw_readl(r)

unsigned long long notrace sched_clock(void)
{
	u32 upper, upper2, lower;

	if (!scug_timer)
		return 0ULL;

	do {
		upper  = xscugtimer_readl(&scug_timer->counter1);
		lower  = xscugtimer_readl(&scug_timer->counter0);
		upper2 = xscugtimer_readl(&scug_timer->counter1);
	} while (upper != upper2);

	return cyc_to_ns(((u64) upper << 32) + lower, mult, shift);
}

static cycle_t scug_cs_read(struct clocksource *cs)
{
	u32 upper, upper2, lower;

	if (!scug_timer)
		return 0ULL;

	do {
		upper  = xscugtimer_readl(&scug_timer->counter1);
		lower  = xscugtimer_readl(&scug_timer->counter0);
		upper2 = xscugtimer_readl(&scug_timer->counter1);
	} while (upper != upper2);

	return ((u64) upper << 32) + lower;
}

struct clocksource scug_clocksource = {
	.name		= "scu_gtimer",
	.rating		= 300,
	.read		= scug_cs_read,
	.mask		= CLOCKSOURCE_MASK(64),
	.flags		= CLOCK_SOURCE_IS_CONTINUOUS,
};

/**
 * xscugtimer_drv_probe -  Probe call for the device.
 *
 * @pdev:	handle to the platform device structure.
 *
 * It does all the memory allocation and registration for the device.
 * Returns 0 on success, negative error otherwise.
 **/
static int __devinit xscugtimer_drv_probe(struct platform_device *pdev)
{
	struct xscugtimer __iomem *tmp_timer;
	struct resource *res;
#if CONFIG_OF
	const u32 *freqp;
#endif
	u32 freq;
	int err = 0;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "Could not get resource for device.\n");
		err = -ENODEV;
		goto out;
	}

#if CONFIG_OF
	freqp = of_get_property(pdev->dev.of_node, "clock-frequency", NULL);
	if (!freqp) {
		dev_err(&pdev->dev, "Clock frequency unspecified.\n");
		err = -EINVAL;
		goto out;
	}

	freq = be32_to_cpup(freqp);
#else
	freq = 333333000UL;
#endif

	/* Maximize 'maxsec' argument... see clocksource.c */
	clocks_calc_mult_shift(&mult, &shift, freq, NSEC_PER_SEC, ~0);

	tmp_timer = ioremap(res->start, resource_size(res));
	if (!tmp_timer) {
		dev_err(&pdev->dev, "ioremap() failed\n");
		err = -ENOMEM;
		goto out;
	}

	/* Reset counter */
	xscugtimer_writel(&tmp_timer->control, 0x0);
	xscugtimer_writel(&tmp_timer->counter0, 0x0);
	xscugtimer_writel(&tmp_timer->counter1, 0x0);

	/* Enabled. Autoincrement. */
	xscugtimer_writel(&tmp_timer->control, 0x9);

	smp_wmb();
	scug_timer = tmp_timer;

	clocksource_register_hz(&scug_clocksource, freq);

out:
	return err;
}

static int __devexit xscugtimer_drv_remove(struct platform_device *pdev)
{
	scug_timer = NULL;
	smp_wmb();

	clocksource_unregister(&scug_clocksource);

	/* disable counter */
	xscugtimer_writel(&scug_timer->control, 0x0);
	iounmap(scug_timer);

	return 0;
}

#ifdef CONFIG_OF
static struct of_device_id xscug_of_match[] __devinitdata = {
	{ .compatible = "xlnx,xscugtimer-1.00.a", },
	{ }
};
MODULE_DEVICE_TABLE(of, xscug_of_match);
#endif

static struct platform_driver xscugtimer_platform_driver = {
	.probe = xscugtimer_drv_probe,
	.remove = __devexit_p(xscugtimer_drv_remove),
	.driver = {
		.name = "xscugtimer",
		.owner = THIS_MODULE,
#ifdef CONFIG_OF
		.of_match_table = xscug_of_match,
#endif
	},
};

static int __init xscugtimer_init(void)
{
	return platform_driver_register(&xscugtimer_platform_driver);
}

static void __exit xscugtimer_exit(void)
{
	platform_driver_unregister(&xscugtimer_platform_driver);

}

module_init(xscugtimer_init);
module_exit(xscugtimer_exit);

MODULE_AUTHOR("Xilinx, Inc");
MODULE_DESCRIPTION("Xilinx SCU Global Timer Driver");
MODULE_LICENSE("GPL");
