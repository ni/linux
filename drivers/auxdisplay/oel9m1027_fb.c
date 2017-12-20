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
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/mm.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include "oel9m1027.h"

static ssize_t oel9m1027fb_write(struct fb_info *fb,
				 const char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct oel9m1027 *oled = dev_get_drvdata(fb->device);
	ssize_t ret;

	ret = fb_sys_write(fb, buf, count, ppos);
	if (ret < 0)
		return ret;

	queue_delayed_work(system_wq, &oled->dwork, 0);
	return ret;
}

static void oel9m1027fb_fillrect(struct fb_info *fb,
				 const struct fb_fillrect *rect)
{
	struct oel9m1027 *oled = dev_get_drvdata(fb->device);

	sys_fillrect(fb, rect);
	queue_delayed_work(system_wq, &oled->dwork, 0);
}

static void oel9m1027fb_copyarea(struct fb_info *fb,
				 const struct fb_copyarea *area)
{
	struct oel9m1027 *oled = dev_get_drvdata(fb->device);

	sys_copyarea(fb, area);
	queue_delayed_work(system_wq, &oled->dwork, 0);
}

static void oel9m1027fb_imageblit(struct fb_info *fb,
				  const struct fb_image *image)
{
	struct oel9m1027 *oled = dev_get_drvdata(fb->device);

	sys_imageblit(fb, image);
	queue_delayed_work(system_wq, &oled->dwork, 0);
}

static const struct fb_fix_screeninfo oel9m1027fb_fix = {
	.id = "oel9m1027",
	.type = FB_TYPE_PACKED_PIXELS,
	.visual = FB_VISUAL_MONO10,
	.xpanstep = 0,
	.ypanstep = 0,
	.ywrapstep = 0,
	.line_length = OEL9M1027_WIDTH / 8,
	.accel = FB_ACCEL_NONE,
};

static const struct fb_var_screeninfo oel9m1027fb_var = {
	.xres = OEL9M1027_WIDTH,
	.yres = OEL9M1027_HEIGHT,
	.xres_virtual = OEL9M1027_WIDTH,
	.yres_virtual = OEL9M1027_HEIGHT,
	.bits_per_pixel = 1,
	.red = { 0, 1, 0 },
	.green = { 0, 1, 0 },
	.blue = { 0, 1, 0 },
	.left_margin = 0,
	.right_margin = 0,
	.upper_margin = 0,
	.lower_margin = 0,
	.vmode = FB_VMODE_NONINTERLACED,
};

static struct fb_ops oel9m1027fb_ops = {
	.owner = THIS_MODULE,
	.fb_read = fb_sys_read,
	.fb_write = oel9m1027fb_write,
	.fb_fillrect = oel9m1027fb_fillrect,
	.fb_copyarea = oel9m1027fb_copyarea,
	.fb_imageblit = oel9m1027fb_imageblit,
};

int oel9m1027fb_init(struct oel9m1027 *oled)
{
	int ret;
	struct fb_info *fb = framebuffer_alloc(0, oled->dev);

	if (!fb) {
		ret = -EINVAL;
		goto none;
	}

	oled->framebuffer = vzalloc(sizeof(unsigned char) *
				    OEL9M1027_SIZE);
	if (!oled->framebuffer) {
		ret = -ENOMEM;
		goto fbinfoalloced;
	}

	fb->screen_base = (char __iomem *)oled->framebuffer;
	fb->screen_size = OEL9M1027_SIZE;
	fb->fbops = &oel9m1027fb_ops;
	fb->fix = oel9m1027fb_fix;
	fb->fix.smem_start = page_to_phys(vmalloc_to_page(oled->framebuffer));
	fb->fix.smem_len   = sizeof(unsigned char) * OEL9M1027_SIZE;
	fb->var = oel9m1027fb_var;
	fb->pseudo_palette = NULL;
	fb->par = NULL;
	fb->flags = FBINFO_FLAG_DEFAULT;
	oled->fb = fb;

	if (register_framebuffer(fb) < 0) {
		ret = -EINVAL;
		goto fballoced;
	}

	fb_info(fb, "%s framebuffer device\n", fb->fix.id);

	return 0;

fballoced:
	vfree(oled->framebuffer);

fbinfoalloced:
	framebuffer_release(fb);

none:
	return ret;
}

void oel9m1027fb_exit(struct oel9m1027 *oled)
{
	unregister_framebuffer(oled->fb);
	vfree(oled->framebuffer);
	framebuffer_release(oled->fb);
}

MODULE_DESCRIPTION("OEL9M1027 OLED frame buffer driver");
MODULE_AUTHOR("Wilson Lee <wilson.lee@ni.com>");
MODULE_LICENSE("GPL");
