/*
 * Copyright (C) 2017 National Instruments Corp.
 *
 * Based on cfag12864b driver (v0.1.0)
 *     Copyright (C) 2006 by Miguel Ojeda Sandonis
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

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <linux/platform_device.h>

#include "sh1107.h"
#include "oel9m1027.h"

/*
 * OEL9M1027 Internal Commands
 */

static int oel9m1027_clear(struct oel9m1027 *oled)
{
	unsigned char i, j;
	int ret;

	mutex_lock(&oled->lock);

	for (i = 0; i < OEL9M1027_PAGES; i++) {
		ret = sh1107_page(oled->sh, i);
		if (ret)
			goto fail;

		ret = sh1107_address(oled->sh, OEL9M1027_ADDRESSES_OFFSET);
		if (ret)
			goto fail;

		for (j = 0; j < OEL9M1027_ADDRESSES; j++) {
			ret = sh1107_writedata(oled->sh, 0);
			if (ret)
				goto fail;
		}
	}

fail:
	mutex_unlock(&oled->lock);
	return ret;
}

static int oel9m1027_off(struct oel9m1027 *oled)
{
	int ret;

	mutex_lock(&oled->lock);
	ret = sh1107_displaystate(oled->sh, OEL9M1027_DISPLAYOFF);
	mutex_unlock(&oled->lock);

	return ret;
}

static int oel9m1027_on(struct oel9m1027 *oled)
{
	int ret;

	mutex_lock(&oled->lock);

	ret = sh1107_multiplexratio(oled->sh, OEL9M1027_DEF_MULTIRATIO);
	if (ret)
		goto fail;

	ret = sh1107_displayfreq(oled->sh, OEL9M1027_DEF_DISPFREQ);
	if (ret)
		goto fail;

	ret = sh1107_scandir(oled->sh, OEL9M1027_DEF_SCANDIR);
	if (ret)
		goto fail;

	ret = sh1107_offset(oled->sh, OEL9M1027_DEF_DISPOFFSET);
	if (ret)
		goto fail;

	ret = sh1107_startline(oled->sh, OEL9M1027_DEF_STARTLINE);
	if (ret)
		goto fail;

	ret = sh1107_addressingmode(oled->sh, OEL9M1027_DEF_ADDRMODE);
	if (ret)
		goto fail;

	ret = sh1107_displaycontrast(oled->sh, oled->contrast);
	if (ret)
		goto fail;

	ret = sh1107_segremap(oled->sh, OEL9M1027_DEF_SEGREMAP);
	if (ret)
		goto fail;

	ret = sh1107_entiredisplaystate(oled->sh, OEL9M1027_DISPLAYOFF);
	if (ret)
		goto fail;

	ret = sh1107_displayinvert(oled->sh, OEL9M1027_DISPNORMAL);
	if (ret)
		goto fail;

	ret = sh1107_dccontrol(oled->sh, OEL9M1027_DEF_DCCONTROL);
	if (ret)
		goto fail;

	ret = sh1107_phaseperiod(oled->sh, OEL9M1027_DEF_PHASEPERIOD);
	if (ret)
		goto fail;

	ret = sh1107_vcomcontrol(oled->sh, OEL9M1027_DEF_VCOMCONTROL);
	if (ret)
		goto fail;

	ret = sh1107_displaystate(oled->sh, OEL9M1027_DISPLAYON);
	if (ret)
		goto fail;

fail:
	mutex_unlock(&oled->lock);
	return ret;
}

static void oel9m1027_update(struct work_struct *work)
{
	struct oel9m1027 *oled = container_of(to_delayed_work(work),
					      struct oel9m1027,
					      dwork);
	unsigned short page, addr, bit;
	unsigned char c_buff;
	unsigned int pos;
	int ret;

	mutex_lock(&oled->lock);

	for (page = 0; page < OEL9M1027_PAGES; page++) {
		ret = sh1107_page(oled->sh, page);
		if (ret)
			goto fail;

		ret = sh1107_address(oled->sh,
				     OEL9M1027_ADDRESSES_OFFSET);
		if (ret)
			goto fail;

		for (addr = 0; addr < OEL9M1027_ADDRESSES; addr++) {
			for (c_buff = 0, bit = 0; bit < 8; bit++) {
				pos = addr / 8 + (page * 8 + bit) *
				      OEL9M1027_WIDTH / 8;

				if (oled->framebuffer[pos] & BIT(addr % 8))
					c_buff |= BIT(bit);
			}

			ret = sh1107_writedata(oled->sh, c_buff);
			if (ret)
				goto fail;
		}
	}

	mutex_unlock(&oled->lock);
	return;

fail:
	mutex_unlock(&oled->lock);

	/* Error occur, retry after 100ms */
	dev_warn(oled->dev, "ERROR: OLED update fail.\n");
	queue_delayed_work(system_wq, &oled->dwork, HZ / 10);
}

static ssize_t oel9m1027_contrast_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct oel9m1027 *oled = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", oled->contrast);
}

static ssize_t oel9m1027_contrast_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct oel9m1027 *oled = dev_get_drvdata(dev);
	int ret;

	ret = kstrtou8(buf, 10, &oled->contrast);
	if (ret)
		return -EINVAL;

	/* Restart for new contrast setting to take effect */
	ret = oel9m1027_off(oled);
	if (ret)
		return ret;

	ret = oel9m1027_on(oled);

	return ret ? ret : count;
}

static DEVICE_ATTR(contrast, 0644, oel9m1027_contrast_show,
		   oel9m1027_contrast_store);

static int oel9m1027_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct oel9m1027 *oled;
	int ret;

	oled = devm_kzalloc(dev, sizeof(*oled), GFP_KERNEL);
	if (!oled) {
		ret = -ENOMEM;
		goto none;
	}

	platform_set_drvdata(pdev, oled);
	oled->dev = dev;
	oled->sh = dev_get_drvdata(dev->parent);
	mutex_init(&oled->lock);
	oled->contrast = OEL9M1027_DEF_CONTRAST;

	ret = device_create_file(dev, &dev_attr_contrast);
	if (ret) {
		dev_warn(dev, "ERROR: OLED sysfs create fail.\n");
		goto mutexcreated;
	}

	ret = oel9m1027_off(oled);
	if (ret) {
		dev_warn(dev, "ERROR: OLED switch off fail.\n");
		goto mutexcreated;
	}

	ret = oel9m1027_clear(oled);
	if (ret) {
		dev_warn(dev, "ERROR: OLED buffer clear fail.\n");
		goto mutexcreated;
	}

	ret = oel9m1027_on(oled);
	if (ret) {
		dev_err(dev, "ERROR: OLED switch on fail.\n");
		goto mutexcreated;
	}

	INIT_DELAYED_WORK(&oled->dwork, oel9m1027_update);

	ret = oel9m1027fb_init(oled);
	if (ret) {
		dev_err(dev, "ERROR: can't initialize framebuffer\n");
		goto oledturnedon;
	}

	return 0;

oledturnedon:
	oel9m1027_off(oled);

mutexcreated:
	mutex_destroy(&oled->lock);

none:
	return ret;
}

static int oel9m1027_remove(struct platform_device *pdev)
{
	struct oel9m1027 *oled = platform_get_drvdata(pdev);

	oel9m1027fb_exit(oled);
	flush_delayed_work(&oled->dwork);
	cancel_delayed_work_sync(&oled->dwork);
	oel9m1027_off(oled);
	mutex_destroy(&oled->lock);

	return 0;
}

static const struct of_device_id oel9m1027_of_match[] = {
	{ .compatible = "truly,oel9m1027" },
	{ },
};
MODULE_DEVICE_TABLE(of, oel9m1027_of_match);

static struct platform_driver oel9m1027_driver = {
	.probe	= oel9m1027_probe,
	.remove = oel9m1027_remove,
	.driver = {
		.name	= KBUILD_MODNAME,
		.of_match_table = oel9m1027_of_match,
	},
};

module_platform_driver(oel9m1027_driver);

MODULE_DESCRIPTION("OEL9M1027 OLED driver");
MODULE_AUTHOR("Wilson Lee <wilson.lee@ni.com>");
MODULE_LICENSE("GPL");
