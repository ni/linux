/*
 * Copyright (C) 2016 National Instruments Corp.
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

#include <linux/leds.h>
#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/mutex.h>

#define USER1_LED_MASK		0x3
#define USER1_GREEN_LED		BIT(0)
#define USER1_YELLOW_LED	BIT(1)

#define USER2_LED_MASK		0xC
#define USER2_GREEN_LED		BIT(2)
#define USER2_YELLOW_LED	BIT(3)

#define LOCK_REG_OFFSET		1
#define LOCK_VALUE		0xA5
#define UNLOCK_VALUE		0x5A

#define USER_LED_IO_SIZE	2

static u16 io_base;
static DEFINE_MUTEX(led_lock);

struct ni78bx_led {
	u8 bit;
	u8 mask;
	struct led_classdev cdev;
};

static inline struct ni78bx_led *to_ni78bx_led(struct led_classdev *cdev)
{
	return container_of(cdev, struct ni78bx_led, cdev);
}

static void ni78bx_brightness_set(struct led_classdev *cdev,
				  enum led_brightness brightness)
{
	struct ni78bx_led *nled = to_ni78bx_led(cdev);
	u8 value;

	mutex_lock(&led_lock);

	value = inb(io_base);

	if (brightness) {
		value &= ~nled->mask;
		value |= nled->bit;
	} else {
		value &= ~nled->bit;
	}

	outb(value, io_base);
	mutex_unlock(&led_lock);
}

static enum led_brightness ni78bx_brightness_get(struct led_classdev *cdev)
{
	struct ni78bx_led *nled = to_ni78bx_led(cdev);
	u8 value;

	mutex_lock(&led_lock);
	value = inb(io_base);
	mutex_unlock(&led_lock);

	return (value & nled->bit) ? 1 : LED_OFF;
}

static struct ni78bx_led ni78bx_leds[] = {
	{
		.bit = USER1_GREEN_LED,
		.mask = USER1_LED_MASK,
		.cdev = {
			.name = "nilrt:green:user1",
			.max_brightness = 1,
			.brightness_set = ni78bx_brightness_set,
			.brightness_get = ni78bx_brightness_get,
		}
	},
	{
		.bit = USER1_YELLOW_LED,
		.mask = USER1_LED_MASK,
		.cdev = {
			.name = "nilrt:yellow:user1",
			.max_brightness = 1,
			.brightness_set = ni78bx_brightness_set,
			.brightness_get = ni78bx_brightness_get,
		}
	},
	{
		.bit = USER2_GREEN_LED,
		.mask = USER2_LED_MASK,
		.cdev = {
			.name = "nilrt:green:user2",
			.max_brightness = 1,
			.brightness_set = ni78bx_brightness_set,
			.brightness_get = ni78bx_brightness_get,
		}
	},
	{
		.bit = USER2_YELLOW_LED,
		.mask = USER2_LED_MASK,
		.cdev = {
			.name = "nilrt:yellow:user2",
			.max_brightness = 1,
			.brightness_set = ni78bx_brightness_set,
			.brightness_get = ni78bx_brightness_get,
		}
	}
};

static acpi_status
acpi_resource_callback(struct acpi_resource *res, void *data)
{
	struct acpi_device *led = data;
	u16 io_size;

	switch (res->type) {
	case ACPI_RESOURCE_TYPE_IO:
		if (io_base != 0) {
			dev_err(&led->dev, "too many IO resources\n");
			return AE_ERROR;
		}

		io_base = res->data.io.minimum;
		io_size = res->data.io.address_length;

		if (io_size < USER_LED_IO_SIZE) {
			dev_err(&led->dev, "memory region too small\n");
			return AE_ERROR;
		}

		if (!devm_request_region(&led->dev, io_base, io_size,
					 KBUILD_MODNAME)) {
			dev_err(&led->dev, "failed to get memory region\n");
			return AE_ERROR;
		}

		return AE_OK;

	case ACPI_RESOURCE_TYPE_END_TAG:
	default:
		/* Ignore unsupported resources */
		return AE_OK;
	}
}

static int ni78bx_remove(struct acpi_device *pdev)
{
	/* Lock LED register */
	outb(LOCK_VALUE, io_base + LOCK_REG_OFFSET);

	return 0;
}

static int ni78bx_add(struct acpi_device *pdev)
{
	int ret, i;
	acpi_status status;

	status = acpi_walk_resources(pdev->handle, METHOD_NAME__CRS,
				     acpi_resource_callback, pdev);

	if (ACPI_FAILURE(status) || io_base == 0)
		return -ENODEV;

	/* Unlock LED register */
	outb(UNLOCK_VALUE, io_base + LOCK_REG_OFFSET);

	for (i = 0; i < ARRAY_SIZE(ni78bx_leds); i++) {
		ret = devm_led_classdev_register(&pdev->dev,
						 &ni78bx_leds[i].cdev);
		if (ret)
			return ret;
	}

	return ret;
}

static const struct acpi_device_id led_device_ids[] = {
	{"NIC78B3", 0},
	{"", 0},
};
MODULE_DEVICE_TABLE(acpi, led_device_ids);

static struct acpi_driver led_acpi_driver = {
	.name = KBUILD_MODNAME,
	.ids = led_device_ids,
	.ops = {
		.add = ni78bx_add,
		.remove = ni78bx_remove,
	},
};

module_acpi_driver(led_acpi_driver);

MODULE_DESCRIPTION("National Instruments PXI User LEDs driver");
MODULE_AUTHOR("Hui Chun Ong <hui.chun.ong@ni.com>");
MODULE_LICENSE("GPL v2");
