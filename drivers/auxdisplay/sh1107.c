/*
 * Copyright (C) 2017 National Instruments Corp.
 *
 * Based on ks0108 driver (v0.1.0)
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
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/uaccess.h>
#include <linux/of_platform.h>

#include "sh1107.h"

#define SH1107_CMDBYTE	0x00
#define SH1107_DATABYTE	0x40

#define SH1107_SET_DC_HI		0xAD
#define SH1107_SET_DC_LO		0x80
#define SH1107_SET_DC_MASK		0x0F
#define SH1107_SET_VCOM			0xDB
#define SH1107_SET_PHASE_PERIOD		0xD9
#define SH1107_SET_ENTIRE_DISP		0xA4
#define SH1107_SET_DISP			0xAE
#define SH1107_SET_INVERT_DISP		0xA6
#define SH1107_SET_DISP_CONTRAST	0x81
#define SH1107_SET_SCANDIR		0xC0
#define SH1107_SET_DISP_FREQ		0xD5
#define SH1107_SET_MULTIPLEX_RATIO	0xA8
#define SH1107_SET_MULTIPLEX_RATIO_MASK	0x7F
#define SH1107_SET_ADDR_MODE		0x20
#define SH1107_SET_REMAP_SEG		0xA0
#define SH1107_SET_STARTLINE		0xDC
#define SH1107_SET_STARTLINE_MASK	0x7F
#define SH1107_SET_COL_ADDR_LO		0x00
#define SH1107_SET_COL_ADDR_HI		0x10
#define SH1107_SET_COL_ADDR_LO_MASK	0x0F
#define SH1107_SET_COL_ADDR_HI_MASK	0x70
#define SH1107_SET_PAGE			0xB0
#define SH1107_SET_PAGE_MASK		0x0F
#define SH1107_SET_OFFSET		0xD3
#define SH1107_SET_OFFSET_MASK		0x7F

struct sh1107 {
	struct device *dev; /* sh1107 device */
	struct i2c_client *client;
};

int sh1107_writedata(struct sh1107 *sh, unsigned char byte)
{
	unsigned char tdata[2] = { SH1107_DATABYTE, byte };
	int ret;

	/* write control byte, then data */
	struct i2c_msg msg = {
		.addr = sh->client->addr,
		.len  = 2,
		.buf  = tdata,
	};

	ret = i2c_transfer(sh->client->adapter, &msg, 1);

	return ret == 1 ? 0 : ret;
}
EXPORT_SYMBOL_GPL(sh1107_writedata);

int sh1107_writecontrol(struct sh1107 *sh, unsigned char byte)
{
	unsigned char tdata[2] = { SH1107_CMDBYTE, byte };
	int ret;

	/* write control byte, then data */
	struct i2c_msg msg = {
		.addr = sh->client->addr,
		.len  = 2,
		.buf  = tdata,
	};

	ret = i2c_transfer(sh->client->adapter, &msg, 1);

	return ret == 1 ? 0 : ret;
}
EXPORT_SYMBOL_GPL(sh1107_writecontrol);

int sh1107_dccontrol(struct sh1107 *sh, unsigned char value)
{
	int ret;

	ret = sh1107_writecontrol(sh, SH1107_SET_DC_HI);
	if (ret)
		return ret;

	return sh1107_writecontrol(sh, SH1107_SET_DC_LO |
				   (value & SH1107_SET_DC_MASK));
}
EXPORT_SYMBOL_GPL(sh1107_dccontrol);

int sh1107_vcomcontrol(struct sh1107 *sh, unsigned char vcom)
{
	int ret;

	ret = sh1107_writecontrol(sh, SH1107_SET_VCOM);
	if (ret)
		return ret;

	return sh1107_writecontrol(sh, vcom);
}
EXPORT_SYMBOL_GPL(sh1107_vcomcontrol);

int sh1107_phaseperiod(struct sh1107 *sh, unsigned char period)
{
	int ret;

	ret = sh1107_writecontrol(sh, SH1107_SET_PHASE_PERIOD);
	if (ret)
		return ret;

	return sh1107_writecontrol(sh, period);
}
EXPORT_SYMBOL_GPL(sh1107_phaseperiod);

int sh1107_entiredisplaystate(struct sh1107 *sh, unsigned char state)
{
	return sh1107_writecontrol(sh, SH1107_SET_ENTIRE_DISP |
				   (state ? BIT(0) : 0));
}
EXPORT_SYMBOL_GPL(sh1107_entiredisplaystate);

int sh1107_displaystate(struct sh1107 *sh, unsigned char state)
{
	return sh1107_writecontrol(sh, SH1107_SET_DISP |
				   (state ? BIT(0) : 0));
}
EXPORT_SYMBOL_GPL(sh1107_displaystate);

int sh1107_displayinvert(struct sh1107 *sh, unsigned char invert)
{
	return sh1107_writecontrol(sh, SH1107_SET_INVERT_DISP |
				   (invert ? BIT(0) : 0));
}
EXPORT_SYMBOL_GPL(sh1107_displayinvert);

int sh1107_displaycontrast(struct sh1107 *sh, unsigned char contrast)
{
	int ret;

	ret = sh1107_writecontrol(sh, SH1107_SET_DISP_CONTRAST);
	if (ret)
		return ret;

	return sh1107_writecontrol(sh, contrast);
}
EXPORT_SYMBOL_GPL(sh1107_displaycontrast);

int sh1107_scandir(struct sh1107 *sh, unsigned char direction)
{
	return sh1107_writecontrol(sh, SH1107_SET_SCANDIR |
				   (direction ? BIT(3) : 0));
}
EXPORT_SYMBOL_GPL(sh1107_scandir);

int sh1107_displayfreq(struct sh1107 *sh, unsigned char frequency)
{
	int ret;

	ret = sh1107_writecontrol(sh, SH1107_SET_DISP_FREQ);
	if (ret)
		return ret;

	return sh1107_writecontrol(sh, frequency);
}
EXPORT_SYMBOL_GPL(sh1107_displayfreq);

int sh1107_multiplexratio(struct sh1107 *sh, unsigned char ratio)
{
	int ret;

	ret = sh1107_writecontrol(sh, SH1107_SET_MULTIPLEX_RATIO);
	if (ret)
		return ret;

	return sh1107_writecontrol(sh, ratio &
				   SH1107_SET_MULTIPLEX_RATIO_MASK);
}
EXPORT_SYMBOL_GPL(sh1107_multiplexratio);

int sh1107_addressingmode(struct sh1107 *sh, unsigned char memmode)
{
	return sh1107_writecontrol(sh, SH1107_SET_ADDR_MODE |
				   (memmode ? BIT(0) : 0));
}
EXPORT_SYMBOL_GPL(sh1107_addressingmode);

int sh1107_segremap(struct sh1107 *sh, unsigned char uprotation)
{
	return sh1107_writecontrol(sh, SH1107_SET_REMAP_SEG |
				   (uprotation ? BIT(0) : 0));
}
EXPORT_SYMBOL_GPL(sh1107_segremap);

int sh1107_startline(struct sh1107 *sh, unsigned char startline)
{
	int ret;

	ret = sh1107_writecontrol(sh, SH1107_SET_STARTLINE);
	if (ret)
		return ret;

	return sh1107_writecontrol(sh, startline & SH1107_SET_STARTLINE_MASK);
}
EXPORT_SYMBOL_GPL(sh1107_startline);

int sh1107_address(struct sh1107 *sh, unsigned char address)
{
	int ret;

	ret = sh1107_writecontrol(sh, SH1107_SET_COL_ADDR_LO |
				  (address & SH1107_SET_COL_ADDR_LO_MASK));
	if (ret)
		return ret;

	return sh1107_writecontrol(sh, SH1107_SET_COL_ADDR_HI |
				   ((address & SH1107_SET_COL_ADDR_HI_MASK)
				    >> 4));
}
EXPORT_SYMBOL_GPL(sh1107_address);

int sh1107_page(struct sh1107 *sh, unsigned char page)
{
	return sh1107_writecontrol(sh, SH1107_SET_PAGE |
				   (page & SH1107_SET_PAGE_MASK));
}
EXPORT_SYMBOL_GPL(sh1107_page);

int sh1107_offset(struct sh1107 *sh, unsigned char offset)
{
	int ret;

	ret = sh1107_writecontrol(sh, SH1107_SET_OFFSET);
	if (ret)
		return ret;

	return sh1107_writecontrol(sh, offset & SH1107_SET_OFFSET_MASK);
}
EXPORT_SYMBOL_GPL(sh1107_offset);

static int sh1107_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct sh1107 *sh;

	sh = devm_kzalloc(dev, sizeof(*sh), GFP_KERNEL);
	if (!sh)
		return -ENOMEM;

	sh->dev = dev;
	sh->client = client;

	/* Test write data into OLED module (RAM) */
	if (sh1107_writedata(sh, 0))
		return -ENODEV;

	dev_set_drvdata(dev, sh);
	dev_info(dev, "%s OLED module found.\n", client->name);

	return devm_of_platform_populate(dev);
}

static int sh1107_remove(struct i2c_client *client)
{
	return 0;
}

static const struct of_device_id sh1107_dt_ids[] = {
	{ .compatible = "sinowealth,sh1107" },
	{ },
};
MODULE_DEVICE_TABLE(of, sh1107_dt_ids);

static const struct i2c_device_id sh1107_ids[] = {
	{ "sh1107", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, sh1107_ids);

static struct i2c_driver sh1107_driver = {
	.driver = {
		.name		= KBUILD_MODNAME,
		.of_match_table	= sh1107_dt_ids,
	},
	.probe		= sh1107_probe,
	.remove		= sh1107_remove,
	.id_table	= sh1107_ids,
};
module_i2c_driver(sh1107_driver);

MODULE_DESCRIPTION("Driver for SH1107 LCD Driver");
MODULE_AUTHOR("Wilson Lee <wilson.lee@ni.com>");
MODULE_LICENSE("GPL");
