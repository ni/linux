/*
 * Copyright (C) 2015 National Instruments Corp.
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
#include <linux/device.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/kernel.h>
#include <linux/io.h>

#define USER1_STATUS_BIT		BIT(0)
#define USER1_OFF_CMD			0x1
#define USER1_ON_CMD			0x2

struct ni793x_led {
	void __iomem		*user1;
	struct led_classdev	cdev;
};

static inline struct ni793x_led *to_ni793x_led(struct led_classdev *cdev)
{
	return container_of(cdev, struct ni793x_led, cdev);
}

static void ni793x_led_set_brightness(struct led_classdev *cdev,
				      enum led_brightness brightness)
{
	struct ni793x_led *led = to_ni793x_led(cdev);

	if (brightness == LED_OFF)
		writel_relaxed(USER1_OFF_CMD, led->user1);
	else
		writel_relaxed(USER1_ON_CMD, led->user1);
}

static enum led_brightness
ni793x_led_get_brightness(struct led_classdev *cdev)
{
	struct ni793x_led *led = to_ni793x_led(cdev);

	return readl_relaxed(led->user1) & USER1_STATUS_BIT;
}

static int ni793x_leds_probe(struct platform_device *pdev)
{
	struct device_node *node;
	struct ni793x_led *led;
	struct resource *res;

	led = devm_kzalloc(&pdev->dev, sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;

	platform_set_drvdata(pdev, led);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	led->user1 = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(led->user1))
		return PTR_ERR(led->user1);

	node = of_get_child_by_name(pdev->dev.of_node, "user1");
	if (!node) {
		dev_err(&pdev->dev, "user1 LED description not found\n");
		return -ENOENT;
	}

	led->cdev.name = of_get_property(node, "label", NULL) ? : node->name;
	led->cdev.default_trigger = of_get_property(node,
						    "linux,default-trigger",
						    NULL);
	led->cdev.max_brightness = 1;
	led->cdev.brightness_set = ni793x_led_set_brightness;
	led->cdev.brightness_get = ni793x_led_get_brightness;

	return led_classdev_register(&pdev->dev, &led->cdev);
}

static int ni793x_leds_remove(struct platform_device *pdev)
{
	struct ni793x_led *led = platform_get_drvdata(pdev);

	led_classdev_unregister(&led->cdev);
	return 0;
}

static const struct of_device_id ni793x_led_ids[] = {
	{ .compatible = "ni,led-793x" },
	{ },
};
MODULE_DEVICE_TABLE(of, ni793x_leds_ids);

static struct platform_driver ni793x_leds_driver = {
	.driver = {
		.name		= "leds-ni793x",
		.of_match_table	= of_match_ptr(ni793x_led_ids),
	},
	.probe		= ni793x_leds_probe,
	.remove		= ni793x_leds_remove,
};
module_platform_driver(ni793x_leds_driver);

MODULE_DESCRIPTION("Driver for RT User1 LED on FlexRIO NI-793xR Products");
MODULE_AUTHOR("National Instruments");
MODULE_LICENSE("GPL");
