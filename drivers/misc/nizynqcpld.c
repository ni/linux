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
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/workqueue.h>


#define NIZYNQCPLD_VERSION		0x00
#define NIZYNQCPLD_PRODUCT		0x1D

#define PROTO_SWITCHANDLED		0x04
#define PROTO_ETHERNETLED		0x05
#define PROTO_SCRATCHPADSR		0xFE
#define PROTO_SCRATCHPADHR		0xFF

#define DOSX_STATUSLEDSHIFTBYTE1	0x05
#define DOSX_STATUSLEDSHIFTBYTE0	0x06
#define DOSX_LED			0x07
#define DOSX_ETHERNETLED		0x08
#define DOSX_SCRATCHPADSR		0x1E
#define DOSX_SCRATCHPADHR		0x1F

struct nizynqcpld_led_desc {
	const char *name;
	const char *default_trigger;
	u8 addr;
	u8 bit;
	u8 pattern_lo_addr;
	u8 pattern_hi_addr;
	int max_brightness;
};

struct nizynqcpld;

struct nizynqcpld_led {
	struct nizynqcpld *cpld;
	struct nizynqcpld_led_desc *desc;
	unsigned on:1;
	struct led_classdev cdev;
	struct work_struct deferred_work;
	u16 blink_pattern;
};

#define to_nizynqcpld_led(x) \
		container_of(x, struct nizynqcpld_led, cdev)

struct nizynqcpld_desc {
	const struct attribute **attrs;
	u8 supported_version;
	u8 supported_product;
	struct nizynqcpld_led_desc *led_descs;
	unsigned num_led_descs;
	u8 scratch_hr_addr;
	u8 scratch_sr_addr;
};

struct nizynqcpld {
	struct device *dev;
	struct nizynqcpld_desc *desc;
	struct nizynqcpld_led *leds;
	struct i2c_client *client;
	struct mutex lock;
};

static int nizynqcpld_write(struct nizynqcpld *cpld, u8 reg, u8 data);
static int nizynqcpld_read(struct nizynqcpld *cpld, u8 reg, u8 *data);

static inline void nizynqcpld_lock(struct nizynqcpld *cpld)
{
	mutex_lock(&cpld->lock);
}
static inline void nizynqcpld_unlock(struct nizynqcpld *cpld)
{
	mutex_unlock(&cpld->lock);
}

/* Can't issue i2c transfers in set_brightness, because
 * they can sleep */
static void nizynqcpld_set_brightness_work(struct work_struct *work)
{
	struct nizynqcpld_led *led = container_of(work, struct nizynqcpld_led,
						  deferred_work);
	struct nizynqcpld_led_desc *desc = led->desc;
	struct nizynqcpld *cpld = led->cpld;
	int err;
	u8 tmp;

	nizynqcpld_lock(cpld);

	err = nizynqcpld_read(cpld, desc->addr, &tmp);
	if (err)
		goto unlock_out;

	tmp &= ~desc->bit;
	if (led->on)
		tmp |= desc->bit;

	nizynqcpld_write(cpld, desc->addr, tmp);

	if (desc->pattern_lo_addr && desc->pattern_hi_addr) {
		/* spec says to write byte 1 first */
		nizynqcpld_write(cpld, desc->pattern_hi_addr,
				 led->blink_pattern >> 8);
		nizynqcpld_write(cpld, desc->pattern_lo_addr,
				 led->blink_pattern & 0xff);
	}

unlock_out:
	nizynqcpld_unlock(cpld);
}

static void nizynqcpld_led_set_brightness(struct led_classdev *led_cdev,
					  enum led_brightness brightness)
{
	struct nizynqcpld_led *led = to_nizynqcpld_led(led_cdev);
	led->on = !!brightness;
	/* some LED's support a blink pattern instead of a variable brightness,
	   and blink_set isn't flexible enough for the supported patterns */
	led->blink_pattern = brightness;
	schedule_work(&led->deferred_work);
}

static enum led_brightness
nizynqcpld_led_get_brightness(struct led_classdev *led_cdev)
{
	struct nizynqcpld_led *led = to_nizynqcpld_led(led_cdev);
	struct nizynqcpld_led_desc *desc = led->desc;
	struct nizynqcpld *cpld = led->cpld;
	u8 tmp;

	nizynqcpld_lock(cpld);
	/* can't handle an error here, so, roll with it. */
	nizynqcpld_read(cpld, desc->addr, &tmp);
	nizynqcpld_unlock(cpld);

	/* for the status LED, the blink pattern used for brightness on write
	   is write-only, so we just return on/off for all LED's */
	return tmp & desc->bit ? LED_FULL : 0;
}

static int nizynqcpld_write(struct nizynqcpld *cpld, u8 reg, u8 data)
{
	int err;
	u8 tdata[2] = { reg, data };

	/* write reg byte, then data */
	struct i2c_msg msg = {
		.addr = cpld->client->addr,
		.len  = 2,
		.buf  = tdata,
	};

	err = i2c_transfer(cpld->client->adapter, &msg, 1);

	return err == 1 ? 0 : err;
}

static int nizynqcpld_led_register(struct nizynqcpld *cpld,
				   struct nizynqcpld_led_desc *desc,
				   struct nizynqcpld_led *led)
{
	int err;
	u8 tmp;

	nizynqcpld_lock(cpld);
	err = nizynqcpld_read(cpld, desc->addr, &tmp);
	nizynqcpld_unlock(cpld);

	if (err)
		goto err_out;

	led->cpld = cpld;
	led->desc = desc;
	led->on = !!(tmp & desc->bit);
	INIT_WORK(&led->deferred_work, nizynqcpld_set_brightness_work);

	led->cdev.name = desc->name;
	led->cdev.default_trigger = desc->default_trigger;
	led->cdev.max_brightness = desc->max_brightness ? desc->max_brightness
							: 1;
	led->cdev.brightness_set = nizynqcpld_led_set_brightness;
	led->cdev.brightness_get = nizynqcpld_led_get_brightness;

	err = led_classdev_register(cpld->dev, &led->cdev);
	if (err) {
		dev_err(cpld->dev, "error registering led.\n");
		goto err_led;
	}

err_led:
err_out:
	return err;
}

static void nizynqcpld_led_unregister(struct nizynqcpld_led *led)
{
	led_classdev_unregister(&led->cdev);
}


static int nizynqcpld_read(struct nizynqcpld *cpld, u8 reg, u8 *data)
{
	int err;

	/* First, write the CPLD register offset, then read the data. */
	struct i2c_msg msgs[] = {
		{
			.addr	= cpld->client->addr,
			.len	= 1,
			.buf	= &reg,
		},
		{
			.addr	= cpld->client->addr,
			.flags	= I2C_M_RD,
			.len	= 1,
			.buf	= data,
		},
	};

	err = i2c_transfer(cpld->client->adapter, msgs, ARRAY_SIZE(msgs));

	return err == ARRAY_SIZE(msgs) ? 0 : err;
}

static inline ssize_t nizynqcpld_scratch_show(struct nizynqcpld *cpld,
					      struct device_attribute *attr,
					      char *buf, u8 reg_addr)
{
	u8 data;
	int err;

	nizynqcpld_lock(cpld);
	err = nizynqcpld_read(cpld, reg_addr, &data);
	nizynqcpld_unlock(cpld);

	if (err) {
		dev_err(cpld->dev, "Error reading scratch register state.\n");
		return err;
	}

	return sprintf(buf, "%02x\n", data);
}

static ssize_t nizynqcpld_scratchsr_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct nizynqcpld *cpld = dev_get_drvdata(dev);
	struct nizynqcpld_desc *desc = cpld->desc;
	return nizynqcpld_scratch_show(cpld, attr, buf, desc->scratch_sr_addr);
}

static ssize_t nizynqcpld_scratchhr_show(
	struct device *dev, struct device_attribute *attr, char *buf)
{
	struct nizynqcpld *cpld = dev_get_drvdata(dev);
	struct nizynqcpld_desc *desc = cpld->desc;
	return nizynqcpld_scratch_show(cpld, attr, buf, desc->scratch_hr_addr);
}

static inline ssize_t nizynqcpld_scratch_store(struct nizynqcpld *cpld,
					       struct device_attribute *attr,
					       const char *buf, size_t count,
					       u8 reg_addr)
{
	unsigned long tmp;
	u8 data;
	int err;

	err = kstrtoul(buf, 0, &tmp);
	if (err)
		return err;

	data = (u8) tmp;

	nizynqcpld_lock(cpld);
	err = nizynqcpld_write(cpld, reg_addr, data);
	nizynqcpld_unlock(cpld);

	if (err) {
		dev_err(cpld->dev,
			"Error writing to  scratch register.\n");
		return err;
	}

	return count;
}

static ssize_t nizynqcpld_scratchsr_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct nizynqcpld *cpld = dev_get_drvdata(dev);
	struct nizynqcpld_desc *desc = cpld->desc;
	return nizynqcpld_scratch_store(cpld, attr, buf, count,
					desc->scratch_sr_addr);
}

static ssize_t nizynqcpld_scratchhr_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct nizynqcpld *cpld = dev_get_drvdata(dev);
	struct nizynqcpld_desc *desc = cpld->desc;
	return nizynqcpld_scratch_store(cpld, attr, buf, count,
					desc->scratch_hr_addr);
}

static DEVICE_ATTR(scratch_softreset, S_IRUSR|S_IWUSR,
		nizynqcpld_scratchsr_show, nizynqcpld_scratchsr_store);
static DEVICE_ATTR(scratch_hardreset, S_IRUSR|S_IWUSR,
		nizynqcpld_scratchhr_show, nizynqcpld_scratchhr_store);

static const char * const bootmode_strings[] = {
	"runtime", "safemode", "recovery",
};

static ssize_t nizynqcpld_bootmode_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct nizynqcpld *cpld = dev_get_drvdata(dev);
	struct nizynqcpld_desc *desc = cpld->desc;
	int err;
	u8 tmp;

	nizynqcpld_lock(cpld);
	err = nizynqcpld_read(cpld, desc->scratch_hr_addr, &tmp);
	nizynqcpld_unlock(cpld);

	if (err)
		return err;

	tmp &= 0x3;
	if (tmp >= ARRAY_SIZE(bootmode_strings))
		return -EINVAL;

	return sprintf(buf, "%s\n", bootmode_strings[tmp]);
}

static int nizynqcpld_set_bootmode(struct nizynqcpld *cpld, u8 mode)
{
	struct nizynqcpld_desc *desc = cpld->desc;
	int err;
	u8 tmp;

	nizynqcpld_lock(cpld);

	err = nizynqcpld_read(cpld, desc->scratch_hr_addr, &tmp);
	if (err)
		goto unlock_out;

	tmp &= ~0x3;
	tmp |= mode;

	err = nizynqcpld_write(cpld, desc->scratch_hr_addr, tmp);

unlock_out:
	nizynqcpld_unlock(cpld);
	return err;
}

static ssize_t nizynqcpld_bootmode_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct nizynqcpld *cpld = dev_get_drvdata(dev);
	u8 i;

	for (i = 0; i < ARRAY_SIZE(bootmode_strings); i++)
		if (!strcmp(buf, bootmode_strings[i]))
			return nizynqcpld_set_bootmode(cpld, i) ?: count;

	return -EINVAL;
}

static DEVICE_ATTR(bootmode, S_IRUSR|S_IWUSR, nizynqcpld_bootmode_show,
		   nizynqcpld_bootmode_store);

static const struct attribute *nizynqcpld_attrs[] = {
	&dev_attr_bootmode.attr,
	&dev_attr_scratch_softreset.attr,
	&dev_attr_scratch_hardreset.attr,
	NULL
};

static const struct attribute *dosequis6_attrs[] = {
	&dev_attr_bootmode.attr,
	&dev_attr_scratch_softreset.attr,
	&dev_attr_scratch_hardreset.attr,
	NULL
};

static struct nizynqcpld_led_desc proto_leds[] = {
	{
		.name			= "nizynqcpld:user1:green",
		.addr			= PROTO_SWITCHANDLED,
		.bit			= 1 << 4,
	},
	{
		.name			= "nizynqcpld:user1:yellow",
		.addr			= PROTO_SWITCHANDLED,
		.bit			= 1 << 3,
	},
	{
		.name			= "nizynqcpld:status:yellow",
		.addr			= PROTO_SWITCHANDLED,
		.bit			= 1 << 2,
	},
	{
		.name			= "nizynqcpld:eth1:green",
		.addr			= PROTO_ETHERNETLED,
		.bit			= 1 << 1,
		.default_trigger	= "e000b000:00:100Mb",
	},
	{
		.name			= "nizynqcpld:eth1:yellow",
		.addr			= PROTO_ETHERNETLED,
		.bit			= 1 << 0,
		.default_trigger	= "e000b000:00:Gb",
	},
};

static struct nizynqcpld_led_desc dosx_leds[] = {
	{
		.name			= "nizynqcpld:user1:green",
		.addr			= DOSX_LED,
		.bit			= 1 << 5,
	},
	{
		.name			= "nizynqcpld:user1:yellow",
		.addr			= DOSX_LED,
		.bit			= 1 << 4,
	},
	{
		.name			= "nizynqcpld:status:red",
		.addr			= DOSX_LED,
		.bit			= 1 << 3,
	},
	{
		.name			= "nizynqcpld:status:yellow",
		.addr			= DOSX_LED,
		.bit			= 1 << 2,
		.pattern_lo_addr	= DOSX_STATUSLEDSHIFTBYTE0,
		.pattern_hi_addr	= DOSX_STATUSLEDSHIFTBYTE1,
		.max_brightness		= 0xffff,
	},
	{
		.name			= "nizynqcpld:wifi:green",
		.addr			= DOSX_ETHERNETLED,
		.bit			= 1 << 5,
	},
	{
		.name			= "nizynqcpld:wifi:yellow",
		.addr			= DOSX_ETHERNETLED,
		.bit			= 1 << 4,
	},
	{
		.name			= "nizynqcpld:eth1:green",
		.addr			= DOSX_ETHERNETLED,
		.bit			= 1 << 3,
		.default_trigger	= "e000b000:01:100Mb",
	},
	{
		.name			= "nizynqcpld:eth1:yellow",
		.addr			= DOSX_ETHERNETLED,
		.bit			= 1 << 2,
		.default_trigger	= "e000b000:01:Gb",
	},
	{
		.name			= "nizynqcpld:eth0:green",
		.addr			= DOSX_ETHERNETLED,
		.bit			= 1 << 1,
		.default_trigger	= "e000b000:00:100Mb",
	},
	{
		.name			= "nizynqcpld:eth0:yellow",
		.addr			= DOSX_ETHERNETLED,
		.bit			= 1 << 0,
		.default_trigger	= "e000b000:00:Gb",
	},
};

static struct nizynqcpld_desc nizynqcpld_descs[] = {
	/* DosEquis and myRIO development CPLD */
	{
		.attrs			= nizynqcpld_attrs,
		.supported_version	= 3,
		.supported_product	= 0,
		.led_descs		= proto_leds,
		.num_led_descs		= ARRAY_SIZE(proto_leds),
		.scratch_hr_addr	= PROTO_SCRATCHPADHR,
		.scratch_sr_addr	= PROTO_SCRATCHPADSR,
	},
	/* DosEquis and myRIO development CPLD */
	{
		.attrs			= nizynqcpld_attrs,
		.supported_version	= 4,
		.supported_product	= 0,
		.led_descs		= dosx_leds,
		.num_led_descs		= ARRAY_SIZE(dosx_leds),
		.scratch_hr_addr	= DOSX_SCRATCHPADHR,
		.scratch_sr_addr	= DOSX_SCRATCHPADSR,
	},
	/* DosEquis and myRIO development CPLD */
	{
		.attrs			= nizynqcpld_attrs,
		.supported_version	= 5,
		.supported_product	= 0,
		.led_descs		= dosx_leds,
		.num_led_descs		= ARRAY_SIZE(dosx_leds),
		.scratch_hr_addr	= DOSX_SCRATCHPADHR,
		.scratch_sr_addr	= DOSX_SCRATCHPADSR,
	},
	/* DosEquis CPLD */
	{
		.attrs			= dosequis6_attrs,
		.supported_version	= 6,
		.supported_product	= 0,
		.led_descs		= dosx_leds,
		.num_led_descs		= ARRAY_SIZE(dosx_leds),
		.scratch_hr_addr	= DOSX_SCRATCHPADHR,
		.scratch_sr_addr	= DOSX_SCRATCHPADSR,
	},
	/* myRIO CPLD */
	{
		.attrs			= dosequis6_attrs,
		.supported_version	= 6,
		.supported_product	= 1,
		.led_descs		= dosx_leds,
		.num_led_descs		= ARRAY_SIZE(dosx_leds),
		.scratch_hr_addr	= DOSX_SCRATCHPADHR,
		.scratch_sr_addr	= DOSX_SCRATCHPADSR,
	},
};

static int nizynqcpld_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	struct nizynqcpld_desc *desc;
	struct nizynqcpld *cpld;
	u8 version;
	u8 product;
	int err;
	int i;

	cpld = kzalloc(sizeof(*cpld), GFP_KERNEL);
	if (!cpld) {
		err = -ENOMEM;
		dev_err(&client->dev, "could not allocate private data.\n");
		goto err_cpld_alloc;
	}

	cpld->dev = &client->dev;
	cpld->client = client;
	mutex_init(&cpld->lock);

	err = nizynqcpld_read(cpld, NIZYNQCPLD_VERSION,
			      &version);
	if (err) {
		dev_err(cpld->dev, "could not read version cpld version.\n");
		goto err_read_info;
	}

	err = nizynqcpld_read(cpld, NIZYNQCPLD_PRODUCT, &product);
	if (err) {
		dev_err(cpld->dev, "could not read cpld product number.\n");
		goto err_read_info;
	}

	for (i = 0, desc = NULL; i < ARRAY_SIZE(nizynqcpld_descs); i++) {
		if (nizynqcpld_descs[i].supported_version == version &&
		    nizynqcpld_descs[i].supported_product == product) {
			desc = &nizynqcpld_descs[i];
			break;
		}
	}

	if (!desc) {
		err = -ENODEV;
		dev_err(cpld->dev,
			"this driver does not support cpld with version %d and"
			" product %d.\n", version, product);
		goto err_no_version;
	}

	cpld->desc = desc;

	cpld->leds = kzalloc(sizeof(*cpld->leds) * desc->num_led_descs,
			     GFP_KERNEL);
	if (!cpld->leds) {
		err = -ENOMEM;
		dev_err(cpld->dev, "could not allocate led state data\n");
		goto err_led_alloc;
	}

	for (i = 0; i < desc->num_led_descs; i++) {
		err = nizynqcpld_led_register(cpld, &desc->led_descs[i],
					      &cpld->leds[i]);
		if (err)
			goto err_led;
	}

	err = sysfs_create_files(&cpld->dev->kobj, desc->attrs);
	if (err) {
		dev_err(cpld->dev, "could not register attrs for device.\n");
		goto err_sysfs_create_files;
	}

	i2c_set_clientdata(client, cpld);

	dev_info(&client->dev,
		 "%s NI Zynq-based target CPLD found.\n",
		 client->name);

	return 0;

	sysfs_remove_files(&client->dev.kobj, desc->attrs);
err_sysfs_create_files:
err_led:
	while (i--)
		nizynqcpld_led_unregister(&cpld->leds[i]);
	kfree(cpld->leds);
err_led_alloc:
err_no_version:
err_read_info:
	kfree(cpld);
err_cpld_alloc:
	return err;
}

static int nizynqcpld_remove(struct i2c_client *client)
{
	struct nizynqcpld *cpld = i2c_get_clientdata(client);
	struct nizynqcpld_desc *desc = cpld->desc;
	int i;

	sysfs_remove_files(&cpld->dev->kobj, desc->attrs);
	for (i = desc->num_led_descs - 1; i; --i)
		nizynqcpld_led_unregister(&cpld->leds[i]);
	kfree(cpld->leds);
	kfree(cpld);
	return 0;
}

static const struct i2c_device_id nizynqcpld_ids[] = {
	{ .name = "nizynqcpld" },
	{ },
};
MODULE_DEVICE_TABLE(i2c, nizynqcpld_ids);

static struct i2c_driver nizynqcpld_driver = {
	.driver = {
		.name		= "nizynqcpld",
		.owner		= THIS_MODULE,
	},
	.probe		= nizynqcpld_probe,
	.remove		= nizynqcpld_remove,
	.id_table	= nizynqcpld_ids,
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

MODULE_DESCRIPTION("Driver for CPLD on NI's Zynq RIO products");
MODULE_AUTHOR("National Instruments");
MODULE_LICENSE("GPL");
