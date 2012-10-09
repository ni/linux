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
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/poll.h>
#include <linux/niwatchdog.h>

#define NICPLD_CPLDINFOREGS		0x00
#define NICPLD_PROCESSORSTATE		0x01
#define NICPLD_PROCESSORRESET		0x02
#define NICPLD_PERIPHERALRESETCONTROL	0x03
#define NICPLD_PROCRESETSOURCE		0x04
#define NICPLD_LED			0x07
#define NICPLD_ETHERNETLED		0x08
#define NICPLD_DEBUGSWITCH		0x09
#define NICPLD_WATCHDOGCONTROL		0x13
#define NICPLD_WATCHDOGCOUNTER2		0x14
#define NICPLD_WATCHDOGCOUNTER1		0x15
#define NICPLD_WATCHDOGCOUNTER0		0x16
#define NICPLD_WATCHDOGSEED2		0x17
#define NICPLD_WATCHDOGSEED1		0x18
#define NICPLD_WATCHDOGSEED0		0x19
#define NICPLD_SCRATCHPADSR		0x1E
#define NICPLD_SCRATCHPADHR		0x1F

#define NICPLD_WATCHDOGCONTROL_PROC_INTERRUPT	0x40
#define NICPLD_WATCHDOGCONTROL_PROC_RESET	0x20
#define NICPLD_WATCHDOGCONTROL_PET		0x10
#define NICPLD_WATCHDOGCONTROL_RUNNING		0x08
#define NICPLD_WATCHDOGCONTROL_CAPTURECOUNTER	0x04
#define NICPLD_WATCHDOGCONTROL_RESET		0x02
#define NICPLD_WATCHDOGCONTROL_ALARM		0x01

#define NICPLD_WATCHDOG_MIN_VERSION	4

/* !!!Version 5 of the CPLD will have a different, as yet undetermined
   watchdog clock period. The max counter value may also be different. */
#define NICPLD_WATCHDOG_V4_PERIOD_NS	24000
#define NICPLD_WATCHDOG_MAX_COUNTER	0x00FFFFFF
#define NICPLD_WATCHDOG_COUNTER_BYTES	3

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

struct nidosequiscpld_watchdog {
	u8 version;
	atomic_t available;
	wait_queue_head_t irq_event;
	bool expired;
};

struct nidosequiscpld {
	struct i2c_client *client;
	struct mutex lock;
	struct nidosequiscpld_led leds[MAX_NUM_LEDS];
	struct nidosequiscpld_watchdog watchdog;
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


/*
 * CPLD Watchdog
 */

static int nidosequiscpld_watchdog_counter_set(u32 counter)
{
	int err;
	u8 data[NICPLD_WATCHDOG_COUNTER_BYTES];

	data [0] = ((0x00FF0000 & counter) >> 16);
	data [1] = ((0x0000FF00 & counter) >> 8);
	data [2] =  (0x000000FF & counter);

	nidosequiscpld_lock();

	err = i2c_smbus_write_i2c_block_data(nidosequiscpld.client,
					     NICPLD_WATCHDOGSEED2,
					     NICPLD_WATCHDOG_COUNTER_BYTES,
					     data);
	nidosequiscpld_unlock();
	if (err)
		dev_err(&nidosequiscpld.client->dev,
			"Error %d writing watchdog counter.\n", err);
	return err;
}

static int nidosequiscpld_watchdog_check_action(u32 action)
{
	int err = 0;

	switch (action) {
		case NICPLD_WATCHDOGCONTROL_PROC_INTERRUPT:
		case NICPLD_WATCHDOGCONTROL_PROC_RESET:
			break;
		default:
			err = -ENOTSUPP;
	}

	return err;
}

static int nidosequiscpld_watchdog_add_action(u32 action)
{
	int err;
	u8 action_mask;
	u8 control;

	if (NIWATCHDOG_ACTION_INTERRUPT == action)
		action_mask = NICPLD_WATCHDOGCONTROL_PROC_INTERRUPT;
	else if (NIWATCHDOG_ACTION_RESET == action)
		action_mask = NICPLD_WATCHDOGCONTROL_PROC_RESET;
	else
		return -ENOTSUPP;

	nidosequiscpld_lock();

	err = nidosequiscpld_read(NICPLD_WATCHDOGCONTROL, &control);

	if (err) {
		dev_err(&nidosequiscpld.client->dev,
			"Error %d reading watchdog control.\n", err);
		goto out_unlock;
	}
	control |= action_mask;

	err = nidosequiscpld_write(NICPLD_WATCHDOGCONTROL, control);

	if (err) {
		dev_err(&nidosequiscpld.client->dev,
			"Error %d writing watchdog control.\n", err);
		goto out_unlock;
	}
out_unlock:
	nidosequiscpld_unlock();
	return err;
}

static int nidosequiscpld_watchdog_start(void)
{
	int err;
	u8 control;

	nidosequiscpld_lock();

	nidosequiscpld.watchdog.expired = false;

	err = nidosequiscpld_read(NICPLD_WATCHDOGCONTROL, &control);

	if (err) {
		dev_err(&nidosequiscpld.client->dev,
			"Error %d reading watchdog control.\n", err);
		goto out_unlock;
	}
	err = nidosequiscpld_write(NICPLD_WATCHDOGCONTROL,
				   control | NICPLD_WATCHDOGCONTROL_RESET);
	if (err) {
		dev_err(&nidosequiscpld.client->dev,
			"Error %d writing watchdog control.\n", err);
		goto out_unlock;
	}
	err = nidosequiscpld_write(NICPLD_WATCHDOGCONTROL,
				   control | NICPLD_WATCHDOGCONTROL_PET);
	if (err) {
		dev_err(&nidosequiscpld.client->dev,
			"Error %d writing watchdog control.\n", err);
		goto out_unlock;
	}
out_unlock:
	nidosequiscpld_unlock();
	return err;
}

static int nidosequiscpld_watchdog_pet(u32 *state)
{
	int err;
	u8 control;

	nidosequiscpld_lock();

	if (nidosequiscpld.watchdog.expired) {
		err = 0;
		*state = NIWATCHDOG_STATE_EXPIRED;
	} else {
		err = nidosequiscpld_read(NICPLD_WATCHDOGCONTROL, &control);

		if (err) {
			dev_err(&nidosequiscpld.client->dev,
				"Error %d reading watchdog control.\n", err);
			goto out_unlock;
		}
		control |= NICPLD_WATCHDOGCONTROL_PET;

		err = nidosequiscpld_write(NICPLD_WATCHDOGCONTROL, control);

		if (err) {
			dev_err(&nidosequiscpld.client->dev,
				"Error %d writing watchdog control.\n", err);
			goto out_unlock;
		}

		*state = NIWATCHDOG_STATE_RUNNING;
	}

out_unlock:
	nidosequiscpld_unlock();
	return err;
}

static int nidosequiscpld_watchdog_reset(void)
{
	int err;

	nidosequiscpld_lock();

	nidosequiscpld.watchdog.expired = false;

	err = nidosequiscpld_write(NICPLD_WATCHDOGCONTROL,
				   NICPLD_WATCHDOGCONTROL_RESET);
	nidosequiscpld_unlock();
	if (err)
		dev_err(&nidosequiscpld.client->dev,
			"Error %d writing watchdog control.\n", err);
	return err;
}

static int nidosequiscpld_watchdog_counter_get(u32 *counter)
{
	int err;
	u8 control;
	u8 data[NICPLD_WATCHDOG_COUNTER_BYTES];

	nidosequiscpld_lock();

	err = nidosequiscpld_read(NICPLD_WATCHDOGCONTROL, &control);

	if (err) {
		dev_err(&nidosequiscpld.client->dev,
			"Error %d reading watchdog control.\n", err);
		goto out_unlock;
	}

	err = nidosequiscpld_write(NICPLD_WATCHDOGCONTROL,
			control | NICPLD_WATCHDOGCONTROL_CAPTURECOUNTER);
	if (err) {
		dev_err(&nidosequiscpld.client->dev,
			"Error %d capturing watchdog counter.\n", err);
		goto out_unlock;
	}

	/* Returns the number of read bytes */
	err = i2c_smbus_read_i2c_block_data(nidosequiscpld.client,
					    NICPLD_WATCHDOGCOUNTER2,
					    NICPLD_WATCHDOG_COUNTER_BYTES,
					    data);
	if (NICPLD_WATCHDOG_COUNTER_BYTES == err)
		err = 0;
	else {
		dev_err(&nidosequiscpld.client->dev,
			"Error %d reading watchdog counter.\n", err);
		goto out_unlock;
	}

	*counter = (data[0] << 16) | (data[1] << 8) | data[2];

out_unlock:
	nidosequiscpld_unlock();
	return err;
}

static irqreturn_t nidosequiscpld_watchdog_irq(int irq, void *unused)
{
	irqreturn_t ret = IRQ_NONE;
	u8 control;
	int err;

	nidosequiscpld_lock();

	err = nidosequiscpld_read(NICPLD_WATCHDOGCONTROL, &control);

	if (err) {
		dev_err(&nidosequiscpld.client->dev,
			"Error %d reading watchdog control.\n", err);
		goto out_unlock;
	} else if (!(NICPLD_WATCHDOGCONTROL_ALARM & control)) {
		dev_err(&nidosequiscpld.client->dev,
			"Spurious watchdog interrupt, 0x%02X\n", control);
		goto out_unlock;
	}

	nidosequiscpld.watchdog.expired = true;

	/* Acknowledge the interrupt. */
	control |= NICPLD_WATCHDOGCONTROL_RESET;
	err = nidosequiscpld_write(NICPLD_WATCHDOGCONTROL, control);

	/* Signal the watchdog event. */
	wake_up_all(&nidosequiscpld.watchdog.irq_event);

	ret = IRQ_HANDLED;

out_unlock:
	nidosequiscpld_unlock();
	return ret;
}

static int nidosequiscpld_watchdog_misc_open(struct inode *inode,
					     struct file *file)
{
	if (!atomic_dec_and_test(&nidosequiscpld.watchdog.available)) {
		atomic_inc(&nidosequiscpld.watchdog.available);
		return -EBUSY;
	}

	return request_threaded_irq(nidosequiscpld.client->irq,
				    NULL, nidosequiscpld_watchdog_irq,
				    0, NIWATCHDOG_NAME, NULL);
}

static int nidosequiscpld_watchdog_misc_release(struct inode *inode,
						struct file *file)
{
	free_irq(nidosequiscpld.client->irq, NULL);
	atomic_inc(&nidosequiscpld.watchdog.available);
	return 0;
}

long nidosequiscpld_watchdog_misc_ioctl(struct file *file, unsigned int cmd,
					unsigned long arg)
{
	int err;

	switch(cmd) {
		case NIWATCHDOG_IOCTL_PERIOD_NS: {
			/* !!!Support v5 and newer when available. */
			__u32 period = NICPLD_WATCHDOG_V4_PERIOD_NS;
			err = copy_to_user((__u32 *)arg, &period,
					   sizeof(__u32));
			break;
		}
		case NIWATCHDOG_IOCTL_MAX_COUNTER: {
			__u32 counter = NICPLD_WATCHDOG_MAX_COUNTER;
			err = copy_to_user((__u32 *)arg, &counter,
					   sizeof(__u32));
			break;
		}
		case NIWATCHDOG_IOCTL_COUNTER_SET: {
			__u32 counter;
			err = copy_from_user(&counter, (__u32 *)arg,
					     sizeof(__u32));
			if (!err)
				err = nidosequiscpld_watchdog_counter_set(
					counter);
			break;
		}
		case NIWATCHDOG_IOCTL_CHECK_ACTION: {
			__u32 action;
			err = copy_from_user(&action, (__u32 *)arg,
					     sizeof(__u32));
			if (!err)
				err = nidosequiscpld_watchdog_check_action (
					action);
			break;
		}
		case NIWATCHDOG_IOCTL_ADD_ACTION: {
			__u32 action;
			err = copy_from_user(&action, (__u32 *)arg,
					     sizeof(__u32));
			if (!err)
				err = nidosequiscpld_watchdog_add_action (
					action);
			break;
		}
		case NIWATCHDOG_IOCTL_START: {
			err = nidosequiscpld_watchdog_start();
			break;
		}
		case NIWATCHDOG_IOCTL_PET: {
			__u32 state;
			err = nidosequiscpld_watchdog_pet(&state);
			if (!err)
				err = copy_to_user((__u32 *)arg, &state,
						   sizeof(__u32));
			break;
		}
		case NIWATCHDOG_IOCTL_RESET: {
			err = nidosequiscpld_watchdog_reset();
			break;
		}
		case NIWATCHDOG_IOCTL_COUNTER_GET: {
			__u32 counter;
			err = nidosequiscpld_watchdog_counter_get(&counter);
			if (!err) {
				err = copy_to_user((__u32 *)arg, &counter,
						   sizeof(__u32));
			}
			break;
		}
		default:
			err = -EINVAL;
			break;
	};

	return err;
}

unsigned int nidosequiscpld_watchdog_misc_poll (struct file *file,
						struct poll_table_struct *wait)
{
	unsigned int mask = 0;

	poll_wait(file, &nidosequiscpld.watchdog.irq_event, wait);
	nidosequiscpld_lock();
	if (nidosequiscpld.watchdog.expired)
		mask = POLLIN;
	nidosequiscpld_unlock();
	return mask;
}

static const struct file_operations nidosequiscpld_watchdog_misc_fops = {
	.owner		= THIS_MODULE,
	.open		= nidosequiscpld_watchdog_misc_open,
	.release	= nidosequiscpld_watchdog_misc_release,
	.unlocked_ioctl	= nidosequiscpld_watchdog_misc_ioctl,
	.poll		= nidosequiscpld_watchdog_misc_poll,
};

static struct miscdevice nidosequiscpld_watchdog_misc_dev = {
	.minor		= MISC_DYNAMIC_MINOR,
	.name		= NIWATCHDOG_NAME,
	.fops		= &nidosequiscpld_watchdog_misc_fops,
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
	err = nidosequiscpld_read(NICPLD_CPLDINFOREGS,
				  &nidosequiscpld.watchdog.version);
	if (err) {
		dev_err(&nidosequiscpld.client->dev,
			"Error %d reading watchdog version.\n", err);
		goto err_led;
	}

	/* !!!Don't support version 5 or newer until we know how to do so. */
	if (NICPLD_WATCHDOG_MIN_VERSION == nidosequiscpld.watchdog.version) {
		atomic_set(&nidosequiscpld.watchdog.available, 1);
		init_waitqueue_head(&nidosequiscpld.watchdog.irq_event);
		nidosequiscpld.watchdog.expired = false;

		err = misc_register(&nidosequiscpld_watchdog_misc_dev);
		if (err) {
			dev_err(&client->dev,
				"Couldn't register misc device\n");
			goto err_led;
		}
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
	/* !!!Don't support version 5 or newer until we know how to do so. */
	if (NICPLD_WATCHDOG_MIN_VERSION == nidosequiscpld.watchdog.version)
		misc_deregister(&nidosequiscpld_watchdog_misc_dev);
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
