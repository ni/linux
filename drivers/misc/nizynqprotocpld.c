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

struct nizynqprotocpld_led {
	u8 addr;
	u8 bit;
	unsigned on :1;
	struct led_classdev cdev;
	struct work_struct deferred_work;
};

#define to_nizynqprotocpld_led(x) \
		container_of(x, struct nizynqprotocpld_led, cdev)

enum nizynqprotocpld_leds {
	USER1_LED_YELLOW,
	USER1_LED_GREEN,
	STATUS_LED,
	/* POWER_LED is read-only */
	ETH1_SPEED_LED_YELLOW,
	ETH1_SPEED_LED_GREEN,
	MAX_NUM_LEDS
};

struct nizynqprotocpld {
	struct i2c_client *client;
	struct mutex lock;
	struct nizynqprotocpld_led leds[MAX_NUM_LEDS];
};

static struct nizynqprotocpld nizynqprotocpld;

static inline void nizynqprotocpld_lock(void)
{
	mutex_lock(&nizynqprotocpld.lock);
}
static inline void nizynqprotocpld_unlock(void)
{
	mutex_unlock(&nizynqprotocpld.lock);
}

static int nizynqprotocpld_write(u8 reg, u8 data);
static int nizynqprotocpld_read(u8 reg, u8 *data);

/* Can't issue i2c transfers in set_brightness, because
 * they can sleep */
static void nizynqprotocpld_set_brightness_work(struct work_struct *work)
{
	struct nizynqprotocpld_led *led = container_of(work, struct nizynqprotocpld_led,
						  deferred_work);
	int err;
	u8 tmp;

	nizynqprotocpld_lock();

	err = nizynqprotocpld_read(led->addr, &tmp);
	if (err)
		goto unlock_out;

	tmp &= ~led->bit;
	if (led->on)
		tmp |= led->bit;

	nizynqprotocpld_write(led->addr, tmp);
unlock_out:
	nizynqprotocpld_unlock();
}

static int nizynqprotocpld_led_init(struct nizynqprotocpld_led *led)
{
	int err;
	u8 tmp;

	nizynqprotocpld_lock();
	err = nizynqprotocpld_read(led->addr, &tmp);
	nizynqprotocpld_unlock();

	if (err)
		goto out;

	led->on = !!(tmp & led->bit);
	INIT_WORK(&led->deferred_work, nizynqprotocpld_set_brightness_work);

out:
	return err;
}

static void nizynqprotocpld_led_set_brightness(struct led_classdev *led_cdev,
					  enum led_brightness brightness)
{
	struct nizynqprotocpld_led *led = to_nizynqprotocpld_led(led_cdev);
	led->on = !!brightness;
	schedule_work(&led->deferred_work);
}

static enum led_brightness
nizynqprotocpld_led_get_brightness(struct led_classdev *led_cdev)
{
	struct nizynqprotocpld_led *led = to_nizynqprotocpld_led(led_cdev);
	u8 tmp;

	nizynqprotocpld_lock();
	/* can't handle an error here, so, roll with it. */
	nizynqprotocpld_read(led->addr, &tmp);
	nizynqprotocpld_unlock();

	return tmp & led->bit ? LED_FULL : 0;
}

static struct nizynqprotocpld nizynqprotocpld = {
	.lock	= __MUTEX_INITIALIZER(nizynqprotocpld.lock),
	.leds	= {
		[USER1_LED_GREEN]	= {
			.addr	= NICPLD_SWITCHANDLED,
			.bit	= 1 << 4,
			.cdev	= {
				.name		= "nizynqcpld:user1:green",
				.max_brightness	= 1,
				.brightness_set	= nizynqprotocpld_led_set_brightness,
				.brightness_get	= nizynqprotocpld_led_get_brightness,
			},
		},
		[USER1_LED_YELLOW]	= {
			.addr	= NICPLD_SWITCHANDLED,
			.bit	= 1 << 3,
			.cdev	= {
				.name		= "nizynqcpld:user1:yellow",
				.max_brightness	= 1,
				.brightness_set	= nizynqprotocpld_led_set_brightness,
				.brightness_get	= nizynqprotocpld_led_get_brightness,
			},
		},
		[STATUS_LED]	= {
			.addr	= NICPLD_SWITCHANDLED,
			.bit	= 1 << 2,
			.cdev	= {
				.name		= "nizynqcpld:status:yellow",
				.max_brightness	= 1,
				.brightness_set	= nizynqprotocpld_led_set_brightness,
				.brightness_get	= nizynqprotocpld_led_get_brightness,
			},
		},
		[ETH1_SPEED_LED_GREEN]	= {
			.addr	= NICPLD_ETHERNETLED,
			.bit	= 1 << 1,
			.cdev	= {
				.name			= "nizynqcpld:eth1:green",
				.max_brightness		= 1,
				.brightness_set		= nizynqprotocpld_led_set_brightness,
				.brightness_get		= nizynqprotocpld_led_get_brightness,
				.default_trigger	= "e000b000:00:100Mb",
			},
		},
		[ETH1_SPEED_LED_YELLOW]	= {
			.addr	= NICPLD_ETHERNETLED,
			.bit	= 1 << 0,
			.cdev	= {
				.name			= "nizynqcpld:eth1:yellow",
				.max_brightness		= 1,
				.brightness_set		= nizynqprotocpld_led_set_brightness,
				.brightness_get		= nizynqprotocpld_led_get_brightness,
				.default_trigger	= "e000b000:00:Gb",
			},
		},
	},
};

static int nizynqprotocpld_write(u8 reg, u8 data)
{
	int err;
	u8 tdata[2] = { reg, data };

	/* write reg byte, then data */
	struct i2c_msg msg = {
		.addr = nizynqprotocpld.client->addr,
		.len  = 2,
		.buf  = tdata,
	};

	err = i2c_transfer(nizynqprotocpld.client->adapter, &msg, 1);

	return err == 1 ? 0 : err;
}

static int nizynqprotocpld_read(u8 reg, u8 *data)
{
	int err;

	/* First, write the CPLD register offset, then read the data. */
	struct i2c_msg msgs[] = {
		{
			.addr	= nizynqprotocpld.client->addr,
			.len	= 1,
			.buf	= &reg,
		},
		{
			.addr	= nizynqprotocpld.client->addr,
			.flags	= I2C_M_RD,
			.len	= 1,
			.buf	= data,
		},
	};

	err = i2c_transfer(nizynqprotocpld.client->adapter, msgs, ARRAY_SIZE(msgs));

	return err == ARRAY_SIZE(msgs) ? 0 : err;
}

/* Called by architecture reboot() handling code */
int nizynqprotocpld_reboot(void)
{
	if (!nizynqprotocpld.client)
		return -EINVAL;

	return nizynqprotocpld_write(NICPLD_PROCESSORSTATE, 0x80);
}

static inline ssize_t nizynqprotocpld_scratch_show(struct device *dev, struct device_attribute *attr,
			char *buf, u8 reg_addr)
{
	u8 data;
	int err;

	nizynqprotocpld_lock();
	err = nizynqprotocpld_read(reg_addr, &data);
	nizynqprotocpld_unlock();

	if (err) {
		dev_err(dev, "Error reading scratch register state.\n");
		return err;
	}

	return sprintf(buf, "%02x\n", data);
}

static ssize_t nizynqprotocpld_scratchsr_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	return nizynqprotocpld_scratch_show(dev, attr, buf, NICPLD_SCRATCHPADSR);
}

static ssize_t nizynqprotocpld_scratchhr_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	return nizynqprotocpld_scratch_show(dev, attr, buf, NICPLD_SCRATCHPADHR);
}

static inline ssize_t nizynqprotocpld_scratch_store(struct device *dev, struct device_attribute *attr,
		 const char *buf, size_t count, u8 reg_addr)
{
	unsigned long tmp;
	u8 data;
	int err;

	tmp = simple_strtoul(buf, NULL, 0);
	if (tmp > 0xFF)
		return -EINVAL;

	data = (u8) tmp;

	nizynqprotocpld_lock();
	err = nizynqprotocpld_write(reg_addr, data);
	nizynqprotocpld_unlock();

	if (err) {
		dev_err(dev, "Error writing to  scratch register.\n");
		return err;
	}

	return count;
}

static ssize_t nizynqprotocpld_scratchsr_store(struct device *dev, struct device_attribute *attr,
		 const char *buf, size_t count)
{
	return nizynqprotocpld_scratch_store(dev, attr, buf, count, NICPLD_SCRATCHPADSR);
}

static ssize_t nizynqprotocpld_scratchhr_store(struct device *dev, struct device_attribute *attr,
		 const char *buf, size_t count)
{
	return nizynqprotocpld_scratch_store(dev, attr, buf, count, NICPLD_SCRATCHPADHR);
}

static DEVICE_ATTR(scratch_softreset, S_IRUSR|S_IWUSR,
		nizynqprotocpld_scratchsr_show, nizynqprotocpld_scratchsr_store);
static DEVICE_ATTR(scratch_hardreset, S_IRUSR|S_IWUSR,
		nizynqprotocpld_scratchhr_show, nizynqprotocpld_scratchhr_store);


struct switch_attribute {
	u8 reg;
	u8 bit;
	struct device_attribute dev_attr;
};

static inline ssize_t nizynqprotocpld_switch_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct switch_attribute *sa = container_of(attr, struct switch_attribute, dev_attr);
	int err;
	u8 data;

	nizynqprotocpld_lock();
	err = nizynqprotocpld_read(sa->reg, &data);
	nizynqprotocpld_unlock();

	if (err) {
		dev_err(dev, "Error reading switch state.\n");
		return err;
	}

	return sprintf(buf, "%u\n", !!(data & sa->bit));
}

#define SWITCH_ATTR(_name,_reg,_bit) \
	struct switch_attribute dev_attr_##_name = {				\
		.reg = _reg,							\
		.bit = _bit,							\
		.dev_attr = __ATTR(_name, 0444, nizynqprotocpld_switch_show, NULL),	\
	}

static SWITCH_ATTR(console_out, NICPLD_SWITCHANDLED, 1 << 7);
static SWITCH_ATTR(ip_reset,    NICPLD_SWITCHANDLED, 1 << 6);
static SWITCH_ATTR(safe_mode,   NICPLD_SWITCHANDLED, 1 << 5);

static const char * const bootmode_strings[] = {
	"runtime", "safemode", "recovery",
};

static ssize_t nizynqprotocpld_bootmode_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	int err;
	u8 tmp;

	nizynqprotocpld_lock();
	err = nizynqprotocpld_read(NICPLD_SCRATCHPADHR, &tmp);
	nizynqprotocpld_unlock();

	if (err)
		return err;

	tmp &= 0x3;
	if (tmp >= ARRAY_SIZE(bootmode_strings))
		return -EINVAL;

	return sprintf(buf, "%s\n", bootmode_strings[tmp]);
}

static int nizynqprotocpld_set_bootmode(u8 mode)
{
	int err;
	u8 tmp;

	nizynqprotocpld_lock();

	err = nizynqprotocpld_read(NICPLD_SCRATCHPADHR, &tmp);
	if (err)
		goto unlock_out;

	tmp &= ~0x3;
	tmp |= mode;

	err = nizynqprotocpld_write(NICPLD_SCRATCHPADHR, tmp);

unlock_out:
	nizynqprotocpld_unlock();
	return err;
}

static ssize_t nizynqprotocpld_bootmode_store(struct device *dev, struct device_attribute *attr,
		 const char *buf, size_t count)
{
	u8 i;

	for (i = 0; i < ARRAY_SIZE(bootmode_strings); i++)
		if (!strcmp(buf, bootmode_strings[i]))
			return nizynqprotocpld_set_bootmode(i) ?: count;

	return -EINVAL;
}

static DEVICE_ATTR(bootmode, S_IRUSR|S_IWUSR, nizynqprotocpld_bootmode_show,
		   nizynqprotocpld_bootmode_store);

static struct attribute *nizynqprotocpld_attrs[] = {
	&dev_attr_bootmode.attr,
	&dev_attr_scratch_softreset.attr,
	&dev_attr_scratch_hardreset.attr,
	&dev_attr_console_out.dev_attr.attr,
	&dev_attr_ip_reset.dev_attr.attr,
	&dev_attr_safe_mode.dev_attr.attr,
	NULL
};

static const struct attribute_group nizynqprotocpld_attr_group = {
	.name = "nizynqprotocpld",
	.attrs = nizynqprotocpld_attrs,
};

static int nizynqprotocpld_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int err;
	int i;

	nizynqprotocpld.client = client;

	err = sysfs_create_group(&client->dev.kobj, &nizynqprotocpld_attr_group);
	if (err) {
		dev_err(&client->dev, "could not register attr group for device.\n");
		return err;
	}

	for (i = 0; i < ARRAY_SIZE(nizynqprotocpld.leds); i++) {
		err = nizynqprotocpld_led_init(&nizynqprotocpld.leds[i]);
		if (err)
			goto err_led;

		err = led_classdev_register(&client->dev, &nizynqprotocpld.leds[i].cdev);
		if (err)
			goto err_led;
	}

	dev_info(&client->dev, "%s National Instruments Zynq CPLD found.\n",
							client->name);

	return 0;

err_led:
	while (i--)
		led_classdev_unregister(&nizynqprotocpld.leds[i].cdev);
	sysfs_remove_group(&client->dev.kobj, &nizynqprotocpld_attr_group);
	nizynqprotocpld.client = NULL;
	return err;
}

static int __devexit nizynqprotocpld_remove(struct i2c_client *client)
{
	int i;
	for (i = ARRAY_SIZE(nizynqprotocpld.leds) - 1; i; --i)
		led_classdev_unregister(&nizynqprotocpld.leds[i].cdev);
	sysfs_remove_group(&client->dev.kobj, &nizynqprotocpld_attr_group);
	nizynqprotocpld.client = NULL;
	return 0;
}

static const struct i2c_device_id nizynqprotocpld_ids[] = {
	{ "nizynqprotocpld", 0 },
};
MODULE_DEVICE_TABLE(i2c, nizynqprotocpld_ids);

static struct i2c_driver nizynqprotocpld_driver = {
	.driver = {
		.name = "nizynqprotocpld",
		.owner = THIS_MODULE,
	},
	.probe = nizynqprotocpld_probe,
	.remove = __devexit_p(nizynqprotocpld_remove),
	.id_table = nizynqprotocpld_ids,
};

static int __init nizynqprotocpld_init(void)
{
	return i2c_add_driver(&nizynqprotocpld_driver);
}
module_init(nizynqprotocpld_init);

static void __exit nizynqprotocpld_exit(void)
{
	i2c_del_driver(&nizynqprotocpld_driver);
}
module_exit(nizynqprotocpld_exit);

MODULE_DESCRIPTION("Driver for CPLD on NI's Zynq Prototype controllers");
MODULE_AUTHOR("Josh Cartwright <josh.cartwright@ni.com>");
MODULE_LICENSE("GPL");
