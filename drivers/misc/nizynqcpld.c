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
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/workqueue.h>

#define NICPLD_CPLDINFOREGS		0x00
#define NICPLD_PROCESSORSTATE		0x01
#define NICPLD_PROCRESETSOURCE		0x02
#define NICPLD_PERIPHERALRESETCONTROL	0x03
#define NICPLD_SWITCHANDLED		0x04
#define NICPLD_ETHERNETLED		0x05
#define NICPLD_SCRATCHPADSR		0xFE
#define NICPLD_SCRATCHPADHR		0xFF

struct nizynqcpld_led {
	u8 addr;
	u8 bit;
	unsigned on :1;
	struct led_classdev cdev;
	struct work_struct deferred_work;
};

#define to_nizynqcpld_led(x) \
		container_of(x, struct nizynqcpld_led, cdev)

enum nizynqcpld_leds {
	USER1_LED_YELLOW,
	USER1_LED_GREEN,
	STATUS_LED,
	/* POWER_LED is read-only */
	ETH1_SPEED_LED_YELLOW,
	ETH1_SPEED_LED_GREEN,
	MAX_NUM_LEDS
};

struct nizynqcpld {
	struct i2c_client *client;
	struct mutex lock;
	struct nizynqcpld_led leds[MAX_NUM_LEDS];
};

static struct nizynqcpld nizynqcpld;

static inline void nizynqcpld_lock(void)
{
	mutex_lock(&nizynqcpld.lock);
}
static inline void nizynqcpld_unlock(void)
{
	mutex_unlock(&nizynqcpld.lock);
}

static int nizynqcpld_write(u8 reg, u8 data);
static int nizynqcpld_read(u8 reg, u8 *data);

/* Can't issue i2c transfers in set_brightness, because
 * they can sleep */
static void nizynqcpld_set_brightness_work(struct work_struct *work)
{
	struct nizynqcpld_led *led = container_of(work, struct nizynqcpld_led,
						  deferred_work);
	int err;
	u8 tmp;

	nizynqcpld_lock();

	err = nizynqcpld_read(led->addr, &tmp);
	if (err)
		return;

	tmp &= ~led->bit;
	if (led->on)
		tmp |= led->bit;

	nizynqcpld_write(led->addr, tmp);

	nizynqcpld_unlock();
}

static int nizynqcpld_led_init(struct nizynqcpld_led *led)
{
	int err;
	u8 tmp;

	nizynqcpld_lock();
	err = nizynqcpld_read(led->addr, &tmp);
	nizynqcpld_unlock();

	if (err)
		goto out;

	led->on = !!(tmp & led->bit);
	INIT_WORK(&led->deferred_work, nizynqcpld_set_brightness_work);

out:
	return err;
}

static void nizynqcpld_led_set_brightness(struct led_classdev *led_cdev,
					  enum led_brightness brightness)
{
	struct nizynqcpld_led *led = to_nizynqcpld_led(led_cdev);
	led->on = !!brightness;
	schedule_work(&led->deferred_work);
}

static enum led_brightness
nizynqcpld_led_get_brightness(struct led_classdev *led_cdev)
{
	struct nizynqcpld_led *led = to_nizynqcpld_led(led_cdev);
	u8 tmp;

	nizynqcpld_lock();
	/* can't handle an error here, so, roll with it. */
	nizynqcpld_read(led->addr, &tmp);
	nizynqcpld_unlock();

	return tmp & led->bit ? LED_FULL : 0;
}

static struct nizynqcpld nizynqcpld = {
	.lock	= __MUTEX_INITIALIZER(nizynqcpld.lock),
	.leds	= {
		[USER1_LED_GREEN]	= {
			.addr	= NICPLD_SWITCHANDLED,
			.bit	= 1 << 4,
			.cdev	= {
				.name		= "nizynqcpld:user1:green",
				.max_brightness	= 1,
				.brightness_set	= nizynqcpld_led_set_brightness,
				.brightness_get	= nizynqcpld_led_get_brightness,
			},
		},
		[USER1_LED_YELLOW]	= {
			.addr	= NICPLD_SWITCHANDLED,
			.bit	= 1 << 3,
			.cdev	= {
				.name		= "nizynqcpld:user1:yellow",
				.max_brightness	= 1,
				.brightness_set	= nizynqcpld_led_set_brightness,
				.brightness_get	= nizynqcpld_led_get_brightness,
			},
		},
		[STATUS_LED]	= {
			.addr	= NICPLD_SWITCHANDLED,
			.bit	= 1 << 2,
			.cdev	= {
				.name		= "nizynqcpld:status:yellow",
				.max_brightness	= 1,
				.brightness_set	= nizynqcpld_led_set_brightness,
				.brightness_get	= nizynqcpld_led_get_brightness,
			},
		},
		[ETH1_SPEED_LED_GREEN]	= {
			.addr	= NICPLD_ETHERNETLED,
			.bit	= 1 << 1,
			.cdev	= {
				.name			= "nizynqcpld:eth1:green",
				.max_brightness		= 1,
				.brightness_set		= nizynqcpld_led_set_brightness,
				.brightness_get		= nizynqcpld_led_get_brightness,
				.default_trigger	= "e000b000:00:100Mb",
			},
		},
		[ETH1_SPEED_LED_YELLOW]	= {
			.addr	= NICPLD_ETHERNETLED,
			.bit	= 1 << 0,
			.cdev	= {
				.name			= "nizynqcpld:eth1:yellow",
				.max_brightness		= 1,
				.brightness_set		= nizynqcpld_led_set_brightness,
				.brightness_get		= nizynqcpld_led_get_brightness,
				.default_trigger	= "e000b000:00:Gb",
			},
		},
	},
};

static int nizynqcpld_write(u8 reg, u8 data)
{
	int err;
	u8 tdata[2] = { reg, data };

	/* write reg byte, then data */
	struct i2c_msg msg = {
		.addr = nizynqcpld.client->addr,
		.len  = 2,
		.buf  = tdata,
	};

	err = i2c_transfer(nizynqcpld.client->adapter, &msg, 1);

	return err == 1 ? 0 : err;
}

static int nizynqcpld_read(u8 reg, u8 *data)
{
	int err;

	/* First, write the CPLD register offset, then read the data. */
	struct i2c_msg msgs[] = {
		{
			.addr	= nizynqcpld.client->addr,
			.len	= 1,
			.buf	= &reg,
		},
		{
			.addr	= nizynqcpld.client->addr,
			.flags	= I2C_M_RD,
			.len	= 1,
			.buf	= data,
		},
	};

	err = i2c_transfer(nizynqcpld.client->adapter, msgs, ARRAY_SIZE(msgs));

	return err == ARRAY_SIZE(msgs) ? 0 : err;
}

/* Called by architecture reboot() handling code */
int nizynqcpld_reboot(void)
{
	if (!nizynqcpld.client)
		return -EINVAL;

	return nizynqcpld_write(NICPLD_PROCESSORSTATE, 0x80);
}

static inline ssize_t nizynqcpld_scratch_show(struct device *dev, struct device_attribute *attr,
			char *buf, u8 reg_addr)
{
	u8 data;
	int err;

	nizynqcpld_lock();
	err = nizynqcpld_read(reg_addr, &data);
	nizynqcpld_unlock();

	if (err) {
		dev_err(dev, "Error reading scratch register state.\n");
		return err;
	}

	return sprintf(buf, "%02x\n", data);
}

static ssize_t nizynqcpld_scratchsr_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	return nizynqcpld_scratch_show(dev, attr, buf, NICPLD_SCRATCHPADSR);
}

static ssize_t nizynqcpld_scratchhr_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	return nizynqcpld_scratch_show(dev, attr, buf, NICPLD_SCRATCHPADHR);
}

static inline ssize_t nizynqcpld_scratch_store(struct device *dev, struct device_attribute *attr,
		 const char *buf, size_t count, u8 reg_addr)
{
	unsigned long tmp;
	u8 data;
	int err;

	tmp = simple_strtoul(buf, NULL, 0);
	if (tmp > 0xFF)
		return -EINVAL;

	data = (u8) tmp;

	nizynqcpld_lock();
	err = nizynqcpld_write(reg_addr, data);
	nizynqcpld_unlock();

	if (err) {
		dev_err(dev, "Error writing to  scratch register.\n");
		return err;
	}

	return count;
}

static ssize_t nizynqcpld_scratchsr_store(struct device *dev, struct device_attribute *attr,
		 const char *buf, size_t count)
{
	return nizynqcpld_scratch_store(dev, attr, buf, count, NICPLD_SCRATCHPADSR);
}

static ssize_t nizynqcpld_scratchhr_store(struct device *dev, struct device_attribute *attr,
		 const char *buf, size_t count)
{
	return nizynqcpld_scratch_store(dev, attr, buf, count, NICPLD_SCRATCHPADHR);
}

static DEVICE_ATTR(scratch_softreset, S_IRUSR|S_IWUSR,
		nizynqcpld_scratchsr_show, nizynqcpld_scratchsr_store);
static DEVICE_ATTR(scratch_hardreset, S_IRUSR|S_IWUSR,
		nizynqcpld_scratchhr_show, nizynqcpld_scratchhr_store);

static struct attribute *nizynqcpld_attrs[] = {
	&dev_attr_scratch_softreset.attr,
	&dev_attr_scratch_hardreset.attr,
	NULL
};

static const struct attribute_group nizynqcpld_attr_group = {
	.name = "nizynqcpld",
	.attrs = nizynqcpld_attrs,
};

static int nizynqcpld_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int err;
	int i;

	nizynqcpld.client = client;

	err = sysfs_create_group(&client->dev.kobj, &nizynqcpld_attr_group);
	if (err) {
		dev_err(&client->dev, "could not register attr group for device.\n");
		return err;
	}

	for (i = 0; i < ARRAY_SIZE(nizynqcpld.leds); i++) {
		err = nizynqcpld_led_init(&nizynqcpld.leds[i]);
		if (err)
			goto err_led;

		err = led_classdev_register(&client->dev, &nizynqcpld.leds[i].cdev);
		if (err)
			goto err_led;
	}

	dev_info(&client->dev, "%s National Instruments Zynq CPLD found.\n",
							client->name);

	return 0;

err_led:
	while (i--)
		led_classdev_unregister(&nizynqcpld.leds[i].cdev);
	sysfs_remove_group(&client->dev.kobj, &nizynqcpld_attr_group);
	nizynqcpld.client = NULL;
	return err;
}

static int __devexit nizynqcpld_remove(struct i2c_client *client)
{
	int i;
	for (i = ARRAY_SIZE(nizynqcpld.leds) - 1; i; --i)
		led_classdev_unregister(&nizynqcpld.leds[i].cdev);
	sysfs_remove_group(&client->dev.kobj, &nizynqcpld_attr_group);
	nizynqcpld.client = NULL;
	return 0;
}

static const struct i2c_device_id nizynqcpld_ids[] = {
	{ "nizynqcpld", 0 },
};
MODULE_DEVICE_TABLE(i2c, nizynqcpld_ids);

static struct i2c_driver nizynqcpld_driver = {
	.driver = {
		.name = "nizynqcpld",
		.owner = THIS_MODULE,
	},
	.probe = nizynqcpld_probe,
	.remove = __devexit_p(nizynqcpld_remove),
	.id_table = nizynqcpld_ids,
};

static int __init nizynqcpld_init(void)
{
	return i2c_add_driver(&nizynqcpld_driver);
}
module_init(nizynqcpld_init);

static void __exit nizynqcpld_exit(void)
{
	i2c_del_driver(&nizynqcpld_driver);
}
module_exit(nizynqcpld_exit);

MODULE_DESCRIPTION("Driver for CPLD on NI's Zynq-based controllers");
MODULE_AUTHOR("Josh Cartwright <josh.cartwright@ni.com>");
MODULE_LICENSE("GPL");
