// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2016 National Instruments Corp.
 */

#include <linux/acpi.h>
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>
#include <linux/watchdog.h>

#define LOCK			0xA5
#define UNLOCK			0x5A

#define WDT_CTRL_RESET_EN	BIT(7)
#define WDT_RELOAD_PORT_EN	BIT(7)
#define WDT_CTRL_TRIG_POL	BIT(4)
#define WDT_RELOAD_TRIG_POL	BIT(6)
#define WDT_CTRL_INTERRUPT_EN	BIT(5)

#define WDT_STATUS		0
#define WDT_CTRL		1
#define WDT_RELOAD_CTRL		2
#define WDT_PRESET_PRESCALE	4
#define WDT_REG_LOCK		5
#define WDT_COUNT		6
#define WDT_RELOAD_PORT		7

#define WDT_MIN_TIMEOUT		1
#define WDT_MAX_TIMEOUT		464
#define WDT_DEFAULT_TIMEOUT	80

#define WDT_MAX_COUNTER		15

static unsigned int timeout;
module_param(timeout, uint, 0);
MODULE_PARM_DESC(timeout,
		 "Watchdog timeout in seconds. (default="
		 __MODULE_STRING(WDT_DEFAULT_TIMEOUT) ")");

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout,
		 "Watchdog cannot be stopped once started. (default="
		 __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

struct nic7018_wdt {
	u16 io_base;
	u32 period;
	struct watchdog_device wdd;
	struct mutex lock;
};

struct nic7018_config {
	u32 period;
	u8 divider;
};

static const struct nic7018_config nic7018_configs[] = {
	{  2, 4 },
	{ 32, 5 },
};

static inline u32 nic7018_timeout(u32 period, u8 counter)
{
	return period * counter - period / 2;
}

static const struct nic7018_config *nic7018_get_config(u32 timeout,
						       u8 *counter)
{
	const struct nic7018_config *config;
	u8 count;

	if (timeout < 30 && timeout != 16) {
		config = &nic7018_configs[0];
		count = timeout / 2 + 1;
	} else {
		config = &nic7018_configs[1];
		count = DIV_ROUND_UP(timeout + 16, 32);

		if (count > WDT_MAX_COUNTER)
			count = WDT_MAX_COUNTER;
	}
	*counter = count;
	return config;
}

static int nic7018_set_timeout(struct watchdog_device *wdd,
			       unsigned int timeout)
{
	struct nic7018_wdt *wdt = watchdog_get_drvdata(wdd);
	const struct nic7018_config *config;
	u8 counter;

	config = nic7018_get_config(timeout, &counter);

	outb(counter << 4 | config->divider,
	     wdt->io_base + WDT_PRESET_PRESCALE);

	wdd->timeout = nic7018_timeout(config->period, counter);
	wdt->period = config->period;

	return 0;
}

static irqreturn_t nic7018_thread_isr(int irq, void *wdt_arg)
{
	struct nic7018_wdt *wdt = wdt_arg;
	struct watchdog_device *wdd = &wdt->wdd;
	u8 status, control;

	status = inb(wdt->io_base + WDT_STATUS);

	/* IRQ line asserted */
	if (status & 0x20) {

		mutex_lock(&wdt->lock);

		control = inb(wdt->io_base + WDT_CTRL);
		/* Disable IRQ line */
		outb(control & ~WDT_CTRL_INTERRUPT_EN,
		     wdt->io_base + WDT_CTRL);

		mutex_unlock(&wdt->lock);

		kobject_uevent(&wdd->parent->kobj, KOBJ_CHANGE);
	}
	return IRQ_HANDLED;
}

static int nic7018_start(struct watchdog_device *wdd)
{
	struct nic7018_wdt *wdt = watchdog_get_drvdata(wdd);
	u8 control;

	nic7018_set_timeout(wdd, wdd->timeout);

	control = inb(wdt->io_base + WDT_RELOAD_CTRL);
	outb(control | WDT_RELOAD_PORT_EN, wdt->io_base + WDT_RELOAD_CTRL);

	outb(1, wdt->io_base + WDT_RELOAD_PORT);

	control = inb(wdt->io_base + WDT_CTRL);
	outb(control | WDT_CTRL_RESET_EN, wdt->io_base + WDT_CTRL);

	return 0;
}

static int nic7018_stop(struct watchdog_device *wdd)
{
	struct nic7018_wdt *wdt = watchdog_get_drvdata(wdd);

	outb(0, wdt->io_base + WDT_CTRL);
	outb(0, wdt->io_base + WDT_RELOAD_CTRL);
	outb(0xF0, wdt->io_base + WDT_PRESET_PRESCALE);

	return 0;
}

static int nic7018_ping(struct watchdog_device *wdd)
{
	struct nic7018_wdt *wdt = watchdog_get_drvdata(wdd);

	outb(1, wdt->io_base + WDT_RELOAD_PORT);

	return 0;
}

static unsigned int nic7018_get_timeleft(struct watchdog_device *wdd)
{
	struct nic7018_wdt *wdt = watchdog_get_drvdata(wdd);
	u8 count;

	count = inb(wdt->io_base + WDT_COUNT) & 0xF;
	if (!count)
		return 0;

	return nic7018_timeout(wdt->period, count);
}

static const struct watchdog_info nic7018_wdd_info = {
	.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE,
	.identity = "NIC7018 Watchdog",
};

static const struct watchdog_ops nic7018_wdd_ops = {
	.owner = THIS_MODULE,
	.start = nic7018_start,
	.stop = nic7018_stop,
	.ping = nic7018_ping,
	.set_timeout = nic7018_set_timeout,
	.get_timeleft = nic7018_get_timeleft,
};

struct nic7018_attr {
	struct device_attribute dev_attr;
	u8 offset;
	u8 bit;
};
#define to_nic7018_attr(_attr) \
	container_of((_attr), struct nic7018_attr, dev_attr)

static ssize_t wdt_attr_show(struct device *dev,
			     struct device_attribute *da,
			     char *buf)
{
	struct watchdog_device *wdd = dev_get_drvdata(dev);
	struct nic7018_wdt *wdt = watchdog_get_drvdata(wdd);
	struct nic7018_attr *attr = to_nic7018_attr(da);
	u8 control;

	mutex_lock(&wdt->lock);

	control = inb(wdt->io_base + attr->offset);

	mutex_unlock(&wdt->lock);
	return sprintf(buf, "%u\n", !!(control & attr->bit));
}

static ssize_t wdt_attr_store(struct device *dev,
			      struct device_attribute *da,
			      const char *buf,
			      size_t size)
{
	struct watchdog_device *wdd = dev_get_drvdata(dev);
	struct nic7018_wdt *wdt = watchdog_get_drvdata(wdd);
	struct nic7018_attr *attr = to_nic7018_attr(da);
	unsigned long val;
	u8 control;

	int ret = kstrtoul(buf, 10, &val);

	if (ret)
		return ret;

	if (val > 1)
		return -EINVAL;

	mutex_lock(&wdt->lock);

	control = inb(wdt->io_base + attr->offset);
	if (val)
		outb(control | attr->bit, wdt->io_base + attr->offset);
	else
		outb(control & ~attr->bit, wdt->io_base + attr->offset);

	mutex_unlock(&wdt->lock);
	return size;
}

#define WDT_ATTR(_name, _offset, _bit) \
	struct nic7018_attr dev_attr_##_name = { \
		.offset = _offset, \
		.bit = _bit, \
		.dev_attr = \
			__ATTR(_name, S_IWUSR | S_IRUGO, \
			       wdt_attr_show, wdt_attr_store), \
	}

static WDT_ATTR(enable_reset, WDT_CTRL, WDT_CTRL_RESET_EN);
static WDT_ATTR(enable_soft_ping, WDT_RELOAD_CTRL, WDT_RELOAD_PORT_EN);
static WDT_ATTR(trigger_polarity, WDT_CTRL, WDT_CTRL_TRIG_POL);
static WDT_ATTR(keepalive_trigger_polarity, WDT_RELOAD_CTRL,
		WDT_RELOAD_TRIG_POL);
static WDT_ATTR(enable_interrupt, WDT_CTRL, WDT_CTRL_INTERRUPT_EN);

static ssize_t wdt_trig_show(struct device *dev,
			     struct device_attribute *da,
			     char *buf)
{
	struct watchdog_device *wdd = dev_get_drvdata(dev);
	struct nic7018_wdt *wdt = watchdog_get_drvdata(wdd);
	struct nic7018_attr *attr = to_nic7018_attr(da);
	u8 control;

	mutex_lock(&wdt->lock);

	control = inb(wdt->io_base + attr->offset);

	mutex_unlock(&wdt->lock);

	if (control & 0x0F)
		return sprintf(buf, "trig%u\n", (control & 0x0F) - 1);
	else
		return sprintf(buf, "none\n");
}

static ssize_t wdt_trig_store(struct device *dev,
			      struct device_attribute *da,
			      const char *buf,
			      size_t size)
{
	struct watchdog_device *wdd = dev_get_drvdata(dev);
	struct nic7018_wdt *wdt = watchdog_get_drvdata(wdd);
	struct nic7018_attr *attr = to_nic7018_attr(da);
	u8 control;

	char *p = memchr(buf, '\n', size);
	size_t count = p ? p - buf : size;

	if (count == 5 && !strncmp(buf, "trig", 4)) {
		unsigned long val;
		int ret;

		ret = kstrtoul(buf + 4, 10, &val);
		if (ret)
			return ret;

		if (val > 8)
			return -EINVAL;

		mutex_lock(&wdt->lock);

		control = inb(wdt->io_base + attr->offset);
		outb((control & 0xF0) | (val + 1),
		     wdt->io_base + attr->offset);

		mutex_unlock(&wdt->lock);

	} else if (count == 4 && !strncmp(buf, "none", 4)) {
		mutex_lock(&wdt->lock);

		control = inb(wdt->io_base + attr->offset);
		outb(control & 0xF0, wdt->io_base + attr->offset);

		mutex_unlock(&wdt->lock);
	} else {
		return -EINVAL;
	}

	return size;
}

#define WDT_TRIG_ATTR(_name, _offset) \
	struct nic7018_attr dev_attr_##_name = { \
		.offset = _offset, \
		.dev_attr = \
			__ATTR(_name, S_IWUSR | S_IRUGO, \
			       wdt_trig_show, wdt_trig_store), \
	}

static WDT_TRIG_ATTR(trigger, WDT_CTRL);
static WDT_TRIG_ATTR(keepalive_trigger, WDT_RELOAD_CTRL);

static struct attribute *nic7018_wdt_attrs[] = {
	&dev_attr_enable_reset.dev_attr.attr,
	&dev_attr_enable_soft_ping.dev_attr.attr,
	&dev_attr_trigger_polarity.dev_attr.attr,
	&dev_attr_keepalive_trigger_polarity.dev_attr.attr,
	&dev_attr_enable_interrupt.dev_attr.attr,
	&dev_attr_trigger.dev_attr.attr,
	&dev_attr_keepalive_trigger.dev_attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(nic7018_wdt);

static int nic7018_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct watchdog_device *wdd;
	struct nic7018_wdt *wdt;
	struct resource *io_rc;
	int ret, irq;

	wdt = devm_kzalloc(dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	platform_set_drvdata(pdev, wdt);

	io_rc = platform_get_resource(pdev, IORESOURCE_IO, 0);
	if (!io_rc) {
		dev_err(dev, "missing IO resources\n");
		return -EINVAL;
	}

	if (!devm_request_region(dev, io_rc->start, resource_size(io_rc),
				 KBUILD_MODNAME)) {
		dev_err(dev, "failed to get IO region\n");
		return -EBUSY;
	}

	irq = platform_get_irq(pdev, 0);
	if (!irq)
		return -EINVAL;

	mutex_init(&wdt->lock);

	wdt->io_base = io_rc->start;
	wdd = &wdt->wdd;
	wdd->info = &nic7018_wdd_info;
	wdd->ops = &nic7018_wdd_ops;
	wdd->min_timeout = WDT_MIN_TIMEOUT;
	wdd->max_timeout = WDT_MAX_TIMEOUT;
	wdd->timeout = WDT_DEFAULT_TIMEOUT;
	wdd->parent = dev;
	wdd->groups = nic7018_wdt_groups;

	watchdog_set_drvdata(wdd, wdt);
	watchdog_set_nowayout(wdd, nowayout);
	watchdog_init_timeout(wdd, timeout, dev);

	ret = devm_request_threaded_irq(dev, irq, NULL,
					nic7018_thread_isr,
					IRQF_ONESHOT,
					KBUILD_MODNAME, wdt);
	if (ret) {
		dev_err(dev, "failed to register interrupt handler\n");
		return ret;
	}

	/* Unlock WDT register */
	outb(UNLOCK, wdt->io_base + WDT_REG_LOCK);

	ret = watchdog_register_device(wdd);
	if (ret) {
		outb(LOCK, wdt->io_base + WDT_REG_LOCK);
		return ret;
	}

	dev_dbg(dev, "io_base=0x%04X, timeout=%d, nowayout=%d\n",
		wdt->io_base, timeout, nowayout);
	return 0;
}

static int nic7018_remove(struct platform_device *pdev)
{
	struct nic7018_wdt *wdt = platform_get_drvdata(pdev);

	watchdog_unregister_device(&wdt->wdd);

	/* Lock WDT register */
	outb(LOCK, wdt->io_base + WDT_REG_LOCK);

	return 0;
}

static const struct acpi_device_id nic7018_device_ids[] = {
	{"NIC7018", 0},
	{"", 0},
};
MODULE_DEVICE_TABLE(acpi, nic7018_device_ids);

static struct platform_driver watchdog_driver = {
	.probe = nic7018_probe,
	.remove = nic7018_remove,
	.driver = {
		.name = KBUILD_MODNAME,
		.acpi_match_table = ACPI_PTR(nic7018_device_ids),
	},
};

module_platform_driver(watchdog_driver);

MODULE_DESCRIPTION("National Instruments NIC7018 Watchdog driver");
MODULE_AUTHOR("Hui Chun Ong <hui.chun.ong@ni.com>");
MODULE_LICENSE("GPL");
