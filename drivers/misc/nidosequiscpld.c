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
#define NICPLD_PROCESSORRESET		0x02
#define NICPLD_PERIPHERALRESETCONTROL	0x03
#define NICPLD_PROCRESETSOURCE		0x04
#define NICPLD_LED			0x07
#define NICPLD_ETHERNETLED		0x08
#define NICPLD_DEBUGSWITCH		0x09
#define NICPLD_SCRATCHPADSR		0x1E
#define NICPLD_SCRATCHPADHR		0x1F

struct nidosequiscpld_led {
	u8 addr;
	u8 bit;
	unsigned on :1;
	struct led_classdev cdev;
	struct work_struct deferred_work;
};

#define to_nidosequiscpld_led(x) \
		container_of(x, struct nidosequiscpld_led, cdev)

enum nidosequiscpld_leds {
	USER1_LED_YELLOW,
	USER1_LED_GREEN,
	STATUS_LED_YELLOW,
	STATUS_LED_RED,
	/* POWER_LED is read-only */
	ETH0_SPEED_LED_YELLOW,
	ETH0_SPEED_LED_GREEN,
	ETH1_SPEED_LED_YELLOW,
	ETH1_SPEED_LED_GREEN,
	WIFI_SPEED_LED_YELLOW,
	WIFI_SPEED_LED_GREEN,
	MAX_NUM_LEDS
};

struct nidosequiscpld {
	struct i2c_client *client;
	struct mutex lock;
	struct nidosequiscpld_led leds[MAX_NUM_LEDS];
};

static struct nidosequiscpld nidosequiscpld;

static inline void nidosequiscpld_lock(void)
{
	mutex_lock(&nidosequiscpld.lock);
}
static inline void nidosequiscpld_unlock(void)
{
	mutex_unlock(&nidosequiscpld.lock);
}

static int nidosequiscpld_write(u8 reg, u8 data);
static int nidosequiscpld_read(u8 reg, u8 *data);

/* Can't issue i2c transfers in set_brightness, because
 * they can sleep */
static void nidosequiscpld_set_brightness_work(struct work_struct *work)
{
	struct nidosequiscpld_led *led = container_of(work, struct nidosequiscpld_led,
						  deferred_work);
	int err;
	u8 tmp;

	nidosequiscpld_lock();

	err = nidosequiscpld_read(led->addr, &tmp);
	if (err)
		goto unlock_out;

	tmp &= ~led->bit;
	if (led->on)
		tmp |= led->bit;

	nidosequiscpld_write(led->addr, tmp);
unlock_out:
	nidosequiscpld_unlock();
}

static int nidosequiscpld_led_init(struct nidosequiscpld_led *led)
{
	int err;
	u8 tmp;

	nidosequiscpld_lock();
	err = nidosequiscpld_read(led->addr, &tmp);
	nidosequiscpld_unlock();

	if (err)
		goto out;

	led->on = !!(tmp & led->bit);
	INIT_WORK(&led->deferred_work, nidosequiscpld_set_brightness_work);

out:
	return err;
}

static void nidosequiscpld_led_set_brightness(struct led_classdev *led_cdev,
					  enum led_brightness brightness)
{
	struct nidosequiscpld_led *led = to_nidosequiscpld_led(led_cdev);
	led->on = !!brightness;
	schedule_work(&led->deferred_work);
}

static enum led_brightness
nidosequiscpld_led_get_brightness(struct led_classdev *led_cdev)
{
	struct nidosequiscpld_led *led = to_nidosequiscpld_led(led_cdev);
	u8 tmp;

	nidosequiscpld_lock();
	/* can't handle an error here, so, roll with it. */
	nidosequiscpld_read(led->addr, &tmp);
	nidosequiscpld_unlock();

	return tmp & led->bit ? LED_FULL : 0;
}

static struct nidosequiscpld nidosequiscpld = {
	.lock	= __MUTEX_INITIALIZER(nidosequiscpld.lock),
	.leds	= {
		[USER1_LED_GREEN]	= {
			.addr	= NICPLD_LED,
			.bit	= 1 << 5,
			.cdev	= {
				.name		= "nizynqcpld:user1:green",
				.max_brightness	= 1,
				.brightness_set	= nidosequiscpld_led_set_brightness,
				.brightness_get	= nidosequiscpld_led_get_brightness,
			},
		},
		[USER1_LED_YELLOW]	= {
			.addr	= NICPLD_LED,
			.bit	= 1 << 4,
			.cdev	= {
				.name		= "nizynqcpld:user1:yellow",
				.max_brightness	= 1,
				.brightness_set	= nidosequiscpld_led_set_brightness,
				.brightness_get	= nidosequiscpld_led_get_brightness,
			},
		},
		[STATUS_LED_RED]	= {
			.addr	= NICPLD_LED,
			.bit	= 1 << 3,
			.cdev	= {
				.name		= "nizynqcpld:status:red",
				.max_brightness	= 1,
				.brightness_set	= nidosequiscpld_led_set_brightness,
				.brightness_get	= nidosequiscpld_led_get_brightness,
			},
		},
		[STATUS_LED_YELLOW]	= {
			.addr	= NICPLD_LED,
			.bit	= 1 << 2,
			.cdev	= {
				.name		= "nizynqcpld:status:yellow",
				.max_brightness	= 1,
				.brightness_set	= nidosequiscpld_led_set_brightness,
				.brightness_get	= nidosequiscpld_led_get_brightness,
			},
		},
		[WIFI_SPEED_LED_GREEN]	= {
			.addr	= NICPLD_ETHERNETLED,
			.bit	= 1 << 5,
			.cdev	= {
				.name			= "nizynqcpld:wifi:green",
				.max_brightness		= 1,
				.brightness_set		= nidosequiscpld_led_set_brightness,
				.brightness_get		= nidosequiscpld_led_get_brightness,
			},
		},
		[WIFI_SPEED_LED_YELLOW]	= {
			.addr	= NICPLD_ETHERNETLED,
			.bit	= 1 << 4,
			.cdev	= {
				.name			= "nizynqcpld:wifi:yellow",
				.max_brightness		= 1,
				.brightness_set		= nidosequiscpld_led_set_brightness,
				.brightness_get		= nidosequiscpld_led_get_brightness,
			},
		},
		[ETH1_SPEED_LED_GREEN]	= {
			.addr	= NICPLD_ETHERNETLED,
			.bit	= 1 << 3,
			.cdev	= {
				.name			= "nizynqcpld:eth1:green",
				.max_brightness		= 1,
				.brightness_set		= nidosequiscpld_led_set_brightness,
				.brightness_get		= nidosequiscpld_led_get_brightness,
				.default_trigger	= "e000b000:01:100Mb",
			},
		},
		[ETH1_SPEED_LED_YELLOW]	= {
			.addr	= NICPLD_ETHERNETLED,
			.bit	= 1 << 2,
			.cdev	= {
				.name			= "nizynqcpld:eth1:yellow",
				.max_brightness		= 1,
				.brightness_set		= nidosequiscpld_led_set_brightness,
				.brightness_get		= nidosequiscpld_led_get_brightness,
				.default_trigger	= "e000b000:01:Gb",
			},
		},
		[ETH0_SPEED_LED_GREEN]	= {
			.addr	= NICPLD_ETHERNETLED,
			.bit	= 1 << 1,
			.cdev	= {
				.name			= "nizynqcpld:eth0:green",
				.max_brightness		= 1,
				.brightness_set		= nidosequiscpld_led_set_brightness,
				.brightness_get		= nidosequiscpld_led_get_brightness,
				.default_trigger	= "e000b000:00:100Mb",
			},
		},
		[ETH0_SPEED_LED_YELLOW]	= {
			.addr	= NICPLD_ETHERNETLED,
			.bit	= 1 << 0,
			.cdev	= {
				.name			= "nizynqcpld:eth0:yellow",
				.max_brightness		= 1,
				.brightness_set		= nidosequiscpld_led_set_brightness,
				.brightness_get		= nidosequiscpld_led_get_brightness,
				.default_trigger	= "e000b000:00:Gb",
			},
		},
	},
};

static int nidosequiscpld_write(u8 reg, u8 data)
{
	int err;
	u8 tdata[2] = { reg, data };

	/* write reg byte, then data */
	struct i2c_msg msg = {
		.addr = nidosequiscpld.client->addr,
		.len  = 2,
		.buf  = tdata,
	};

	err = i2c_transfer(nidosequiscpld.client->adapter, &msg, 1);

	return err == 1 ? 0 : err;
}

static int nidosequiscpld_read(u8 reg, u8 *data)
{
	int err;

	/* First, write the CPLD register offset, then read the data. */
	struct i2c_msg msgs[] = {
		{
			.addr	= nidosequiscpld.client->addr,
			.len	= 1,
			.buf	= &reg,
		},
		{
			.addr	= nidosequiscpld.client->addr,
			.flags	= I2C_M_RD,
			.len	= 1,
			.buf	= data,
		},
	};

	err = i2c_transfer(nidosequiscpld.client->adapter, msgs, ARRAY_SIZE(msgs));

	return err == ARRAY_SIZE(msgs) ? 0 : err;
}

/* Called by architecture reboot() handling code */
int nidosequiscpld_reboot(void)
{
	if (!nidosequiscpld.client)
		return -EINVAL;

	return nidosequiscpld_write(NICPLD_PROCESSORRESET, 0x80);
}

static inline ssize_t nidosequiscpld_scratch_show(struct device *dev, struct device_attribute *attr,
			char *buf, u8 reg_addr)
{
	u8 data;
	int err;

	nidosequiscpld_lock();
	err = nidosequiscpld_read(reg_addr, &data);
	nidosequiscpld_unlock();

	if (err) {
		dev_err(dev, "Error reading scratch register state.\n");
		return err;
	}

	return sprintf(buf, "%02x\n", data);
}

static ssize_t nidosequiscpld_scratchsr_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	return nidosequiscpld_scratch_show(dev, attr, buf, NICPLD_SCRATCHPADSR);
}

static ssize_t nidosequiscpld_scratchhr_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	return nidosequiscpld_scratch_show(dev, attr, buf, NICPLD_SCRATCHPADHR);
}

static inline ssize_t nidosequiscpld_scratch_store(struct device *dev, struct device_attribute *attr,
		 const char *buf, size_t count, u8 reg_addr)
{
	unsigned long tmp;
	u8 data;
	int err;

	tmp = simple_strtoul(buf, NULL, 0);
	if (tmp > 0xFF)
		return -EINVAL;

	data = (u8) tmp;

	nidosequiscpld_lock();
	err = nidosequiscpld_write(reg_addr, data);
	nidosequiscpld_unlock();

	if (err) {
		dev_err(dev, "Error writing to  scratch register.\n");
		return err;
	}

	return count;
}

static ssize_t nidosequiscpld_scratchsr_store(struct device *dev, struct device_attribute *attr,
		 const char *buf, size_t count)
{
	return nidosequiscpld_scratch_store(dev, attr, buf, count, NICPLD_SCRATCHPADSR);
}

static ssize_t nidosequiscpld_scratchhr_store(struct device *dev, struct device_attribute *attr,
		 const char *buf, size_t count)
{
	return nidosequiscpld_scratch_store(dev, attr, buf, count, NICPLD_SCRATCHPADHR);
}

static DEVICE_ATTR(scratch_softreset, S_IRUSR|S_IWUSR,
		nidosequiscpld_scratchsr_show, nidosequiscpld_scratchsr_store);
static DEVICE_ATTR(scratch_hardreset, S_IRUSR|S_IWUSR,
		nidosequiscpld_scratchhr_show, nidosequiscpld_scratchhr_store);


struct switch_attribute {
	u8 reg;
	u8 bit;
	struct device_attribute dev_attr;
};

static inline ssize_t nidosequiscpld_switch_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct switch_attribute *sa = container_of(attr, struct switch_attribute, dev_attr);
	int err;
	u8 data;

	nidosequiscpld_lock();
	err = nidosequiscpld_read(sa->reg, &data);
	nidosequiscpld_unlock();

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
		.dev_attr = __ATTR(_name, 0444, nidosequiscpld_switch_show, NULL),	\
	}

static SWITCH_ATTR(console_out, NICPLD_DEBUGSWITCH, 1 << 7);
static SWITCH_ATTR(ip_reset,    NICPLD_DEBUGSWITCH, 1 << 6);
static SWITCH_ATTR(safe_mode,   NICPLD_DEBUGSWITCH, 1 << 5);

static const char * const bootmode_strings[] = {
	"runtime", "safemode", "recovery",
};

static ssize_t nidosequiscpld_bootmode_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	int err;
	u8 tmp;

	nidosequiscpld_lock();
	err = nidosequiscpld_read(NICPLD_SCRATCHPADHR, &tmp);
	nidosequiscpld_unlock();

	if (err)
		return err;

	tmp &= 0x3;
	if (tmp >= ARRAY_SIZE(bootmode_strings))
		return -EINVAL;

	return sprintf(buf, "%s\n", bootmode_strings[tmp]);
}

static int nidosequiscpld_set_bootmode(u8 mode)
{
	int err;
	u8 tmp;

	nidosequiscpld_lock();

	err = nidosequiscpld_read(NICPLD_SCRATCHPADHR, &tmp);
	if (err)
		goto unlock_out;

	tmp &= ~0x3;
	tmp |= mode;

	err = nidosequiscpld_write(NICPLD_SCRATCHPADHR, tmp);

unlock_out:
	nidosequiscpld_unlock();
	return err;
}

static ssize_t nidosequiscpld_bootmode_store(struct device *dev, struct device_attribute *attr,
		 const char *buf, size_t count)
{
	u8 i;

	for (i = 0; i < ARRAY_SIZE(bootmode_strings); i++)
		if (!strcmp(buf, bootmode_strings[i]))
			return nidosequiscpld_set_bootmode(i) ?: count;

	return -EINVAL;
}

static DEVICE_ATTR(bootmode, S_IRUSR|S_IWUSR, nidosequiscpld_bootmode_show,
		   nidosequiscpld_bootmode_store);

static struct attribute *nidosequiscpld_attrs[] = {
	&dev_attr_bootmode.attr,
	&dev_attr_scratch_softreset.attr,
	&dev_attr_scratch_hardreset.attr,
	&dev_attr_console_out.dev_attr.attr,
	&dev_attr_ip_reset.dev_attr.attr,
	&dev_attr_safe_mode.dev_attr.attr,
	NULL
};

static const struct attribute_group nidosequiscpld_attr_group = {
	.name = "nidosequiscpld",
	.attrs = nidosequiscpld_attrs,
};

static int nidosequiscpld_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int err;
	int i;

	nidosequiscpld.client = client;

	err = sysfs_create_group(&client->dev.kobj, &nidosequiscpld_attr_group);
	if (err) {
		dev_err(&client->dev, "could not register attr group for device.\n");
		return err;
	}

	for (i = 0; i < ARRAY_SIZE(nidosequiscpld.leds); i++) {
		err = nidosequiscpld_led_init(&nidosequiscpld.leds[i]);
		if (err)
			goto err_led;

		err = led_classdev_register(&client->dev, &nidosequiscpld.leds[i].cdev);
		if (err)
			goto err_led;
	}

	dev_info(&client->dev, "%s National Instruments Dos Equis CPLD found.\n",
							client->name);

	return 0;

err_led:
	while (i--)
		led_classdev_unregister(&nidosequiscpld.leds[i].cdev);
	sysfs_remove_group(&client->dev.kobj, &nidosequiscpld_attr_group);
	nidosequiscpld.client = NULL;
	return err;
}

static int __devexit nidosequiscpld_remove(struct i2c_client *client)
{
	int i;
	for (i = ARRAY_SIZE(nidosequiscpld.leds) - 1; i; --i)
		led_classdev_unregister(&nidosequiscpld.leds[i].cdev);
	sysfs_remove_group(&client->dev.kobj, &nidosequiscpld_attr_group);
	nidosequiscpld.client = NULL;
	return 0;
}

static const struct i2c_device_id nidosequiscpld_ids[] = {
	{ "nidosequiscpld", 0 },
};
MODULE_DEVICE_TABLE(i2c, nidosequiscpld_ids);

static struct i2c_driver nidosequiscpld_driver = {
	.driver = {
		.name = "nidosequiscpld",
		.owner = THIS_MODULE,
	},
	.probe = nidosequiscpld_probe,
	.remove = __devexit_p(nidosequiscpld_remove),
	.id_table = nidosequiscpld_ids,
};

static int __init nidosequiscpld_init(void)
{
	return i2c_add_driver(&nidosequiscpld_driver);
}
module_init(nidosequiscpld_init);

static void __exit nidosequiscpld_exit(void)
{
	i2c_del_driver(&nidosequiscpld_driver);
}
module_exit(nidosequiscpld_exit);

MODULE_DESCRIPTION("Driver for CPLD on NI's Dos Equis controllers");
MODULE_AUTHOR("Josh Cartwright <josh.cartwright@ni.com>");
MODULE_LICENSE("GPL");
