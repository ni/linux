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
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/leds.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/niwatchdog.h>
#include <linux/of.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include <mach/system.h>

#define NIZYNQCPLD_VERSION		0x00

#define PROTO_PROCESSORSTATE		0x01
#define PROTO_SWITCHANDLED		0x04
#define PROTO_ETHERNETLED		0x05
#define PROTO_SCRATCHPADSR		0xFE
#define PROTO_SCRATCHPADHR		0xFF

#define DOSX_PROCESSORRESET		0x02
#define DOSX_STATUSLEDSHIFTBYTE1	0x05
#define DOSX_STATUSLEDSHIFTBYTE0	0x06
#define DOSX_LED			0x07
#define DOSX_ETHERNETLED		0x08
#define DOSX_DEBUGSWITCH		0x09
#define DOSX_WATCHDOGCONTROL		0x13
#define DOSX_WATCHDOGCOUNTER2		0x14
#define DOSX_WATCHDOGCOUNTER1		0x15
#define DOSX_WATCHDOGCOUNTER0		0x16
#define DOSX_WATCHDOGSEED2		0x17
#define DOSX_WATCHDOGSEED1		0x18
#define DOSX_WATCHDOGSEED0		0x19
#define DOSX_SCRATCHPADSR		0x1E
#define DOSX_SCRATCHPADHR		0x1F

#define DOSX_WATCHDOGCONTROL_PROC_INTERRUPT	0x40
#define DOSX_WATCHDOGCONTROL_PROC_RESET		0x20
#define DOSX_WATCHDOGCONTROL_ENTER_USER_MODE	0x80
#define DOSX_WATCHDOGCONTROL_PET		0x10
#define DOSX_WATCHDOGCONTROL_RUNNING		0x08
#define DOSX_WATCHDOGCONTROL_CAPTURECOUNTER	0x04
#define DOSX_WATCHDOGCONTROL_RESET		0x02
#define DOSX_WATCHDOGCONTROL_ALARM		0x01

#define DOSX_WATCHDOG_MAX_COUNTER	0x00FFFFFF
#define DOSX_WATCHDOG_COUNTER_BYTES	3

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
	unsigned on :1;
	struct led_classdev cdev;
	struct work_struct deferred_work;
	u16 blink_pattern;
};

#define to_nizynqcpld_led(x) \
		container_of(x, struct nizynqcpld_led, cdev)

struct nizynqcpld_watchdog_desc {
	u32 watchdog_period_ns;
};

struct nizynqcpld_watchdog {
	struct miscdevice misc_dev;
	struct nizynqcpld_watchdog_desc *desc;
	atomic_t available;
	wait_queue_head_t irq_event;
	bool expired;
};

struct nizynqcpld_desc {
	const struct attribute **attrs;
	u8 supported_version;
	struct nizynqcpld_led_desc *led_descs;
	unsigned num_led_descs;
	struct nizynqcpld_watchdog_desc *watchdog_desc;
	u8 reboot_addr;
	u8 scratch_hr_addr;
	u8 scratch_sr_addr;
	u8 switch_addr;
	u8 watchdog_addr;
};

struct nizynqcpld {
	struct device *dev;
	struct nizynqcpld_desc *desc;
	struct nizynqcpld_led *leds;
	struct nizynqcpld_watchdog watchdog;
	struct i2c_client *client;
	struct mutex lock;
	struct zynq_board_reset reset;
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
		nizynqcpld_write(cpld, desc->pattern_hi_addr, led->blink_pattern >> 8);
		nizynqcpld_write(cpld, desc->pattern_lo_addr, led->blink_pattern & 0xff);
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
	led->cdev.max_brightness = desc->max_brightness ? desc->max_brightness : 1;
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

static void nizynqcpld_reset(struct zynq_board_reset *reset)
{
	struct nizynqcpld *cpld = container_of(reset, struct nizynqcpld, reset);
	struct nizynqcpld_desc *desc = cpld->desc;

	nizynqcpld_write(cpld, desc->reboot_addr, 0x80);
}

static inline ssize_t nizynqcpld_scratch_show(struct nizynqcpld *cpld, struct device_attribute *attr,
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

static ssize_t nizynqcpld_scratchsr_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct nizynqcpld *cpld = dev_get_drvdata(dev);
	struct nizynqcpld_desc *desc = cpld->desc;
	return nizynqcpld_scratch_show(cpld, attr, buf, desc->scratch_sr_addr);
}

static ssize_t nizynqcpld_scratchhr_show(struct device *dev, struct device_attribute *attr,
			char *buf)
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

	tmp = simple_strtoul(buf, NULL, 0);
	if (tmp > 0xFF)
		return -EINVAL;

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
	return nizynqcpld_scratch_store(cpld, attr, buf, count, desc->scratch_sr_addr);
}

static ssize_t nizynqcpld_scratchhr_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct nizynqcpld *cpld = dev_get_drvdata(dev);
	struct nizynqcpld_desc *desc = cpld->desc;
	return nizynqcpld_scratch_store(cpld, attr, buf, count, desc->scratch_hr_addr);
}

static DEVICE_ATTR(scratch_softreset, S_IRUSR|S_IWUSR,
		nizynqcpld_scratchsr_show, nizynqcpld_scratchsr_store);
static DEVICE_ATTR(scratch_hardreset, S_IRUSR|S_IWUSR,
		nizynqcpld_scratchhr_show, nizynqcpld_scratchhr_store);

struct switch_attribute {
	struct device_attribute dev_attr;
	u8 bit;
};

static inline ssize_t nizynqcpld_switch_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	struct nizynqcpld *cpld = dev_get_drvdata(dev);
	struct nizynqcpld_desc *desc = cpld->desc;
	struct switch_attribute *sa =
		container_of(attr, struct switch_attribute, dev_attr);
	int err;
	u8 data;

	nizynqcpld_lock(cpld);
	err = nizynqcpld_read(cpld, desc->switch_addr, &data);
	nizynqcpld_unlock(cpld);

	if (err) {
		dev_err(dev, "Error reading switch state.\n");
		return err;
	}

	return sprintf(buf, "%u\n", !!(data & sa->bit));
}

#define SWITCH_ATTR(_name,_bit)							\
	struct switch_attribute dev_attr_##_name = {				\
		.bit = _bit,							\
		.dev_attr =							\
			__ATTR(_name, 0444, nizynqcpld_switch_show, NULL),	\
	}

static SWITCH_ATTR(console_out, 1 << 7);
static SWITCH_ATTR(ip_reset,    1 << 6);
static SWITCH_ATTR(safe_mode,   1 << 5);

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
	&dev_attr_console_out.dev_attr.attr,
	&dev_attr_ip_reset.dev_attr.attr,
	&dev_attr_safe_mode.dev_attr.attr,
	NULL
};

static ssize_t dosequiscpld_wdmode_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct nizynqcpld *cpld = dev_get_drvdata(dev);
	struct nizynqcpld_desc *desc = cpld->desc;
	int err;
	u8 tmp;

	nizynqcpld_lock(cpld);
	err = nizynqcpld_read(cpld, desc->watchdog_addr, &tmp);
	nizynqcpld_unlock(cpld);

	if (err)
		return err;

	/* you write a 1 to the bit to enter user mode, but it reads as a
	   0 in user mode for backwards compatibility */
	tmp &= DOSX_WATCHDOGCONTROL_ENTER_USER_MODE;
	return sprintf(buf, "%s\n", tmp ? "boot" : "user");
}

static ssize_t dosequiscpld_wdmode_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct nizynqcpld *cpld = dev_get_drvdata(dev);
	struct nizynqcpld_desc *desc = cpld->desc;
	int err;

	/* you can only switch boot->user */
	if (strcmp(buf, "user"))
		return -EINVAL;

	nizynqcpld_lock(cpld);
	err = nizynqcpld_write(cpld, desc->watchdog_addr,
		DOSX_WATCHDOGCONTROL_ENTER_USER_MODE);
	nizynqcpld_unlock(cpld);

	return err ?: count;
}

static DEVICE_ATTR(watchdog_mode, S_IRUSR|S_IWUSR, dosequiscpld_wdmode_show,
	dosequiscpld_wdmode_store);

static const struct attribute *dosequis6_attrs[] = {
	&dev_attr_bootmode.attr,
	&dev_attr_scratch_softreset.attr,
	&dev_attr_scratch_hardreset.attr,
	&dev_attr_console_out.dev_attr.attr,
	&dev_attr_ip_reset.dev_attr.attr,
	&dev_attr_safe_mode.dev_attr.attr,
	&dev_attr_watchdog_mode.attr,
	NULL
};

/*
 * CPLD Watchdog (only for dosequis)
 */
static int nizynqcpld_watchdog_counter_set(struct nizynqcpld *cpld, u32 counter)
{
	int err;
	u8 data[DOSX_WATCHDOG_COUNTER_BYTES];

	data [0] = ((0x00FF0000 & counter) >> 16);
	data [1] = ((0x0000FF00 & counter) >> 8);
	data [2] =  (0x000000FF & counter);

	nizynqcpld_lock(cpld);

	err = i2c_smbus_write_i2c_block_data(cpld->client,
					     DOSX_WATCHDOGSEED2,
					     DOSX_WATCHDOG_COUNTER_BYTES,
					     data);
	nizynqcpld_unlock(cpld);
	if (err)
		dev_err(cpld->dev,
			"Error %d writing watchdog counter.\n", err);
	return err;
}

static int nizynqcpld_watchdog_check_action(u32 action)
{
	int err = 0;

	switch (action) {
	case DOSX_WATCHDOGCONTROL_PROC_INTERRUPT:
	case DOSX_WATCHDOGCONTROL_PROC_RESET:
		break;
	default:
		err = -ENOTSUPP;
	}

	return err;
}

static int nizynqcpld_watchdog_add_action(struct nizynqcpld *cpld, u32 action)
{
	int err;
	u8 action_mask;
	u8 control;

	if (NIWATCHDOG_ACTION_INTERRUPT == action)
		action_mask = DOSX_WATCHDOGCONTROL_PROC_INTERRUPT;
	else if (NIWATCHDOG_ACTION_RESET == action)
		action_mask = DOSX_WATCHDOGCONTROL_PROC_RESET;
	else
		return -ENOTSUPP;

	nizynqcpld_lock(cpld);

	err = nizynqcpld_read(cpld, DOSX_WATCHDOGCONTROL, &control);

	if (err) {
		dev_err(cpld->dev,
			"Error %d reading watchdog control.\n", err);
		goto out_unlock;
	}
	control |= action_mask;

	err = nizynqcpld_write(cpld, DOSX_WATCHDOGCONTROL, control);

	if (err) {
		dev_err(cpld->dev,
			"Error %d writing watchdog control.\n", err);
		goto out_unlock;
	}
out_unlock:
	nizynqcpld_unlock(cpld);
	return err;
}

static int nizynqcpld_watchdog_start(struct nizynqcpld *cpld)
{
	int err;
	u8 control;

	nizynqcpld_lock(cpld);

	cpld->watchdog.expired = false;

	err = nizynqcpld_read(cpld, DOSX_WATCHDOGCONTROL, &control);
	if (err) {
		dev_err(cpld->dev,
			"Error %d reading watchdog control.\n", err);
		goto out_unlock;
	}

	err = nizynqcpld_write(cpld, DOSX_WATCHDOGCONTROL,
			       control | DOSX_WATCHDOGCONTROL_RESET);
	if (err) {
		dev_err(cpld->dev,
			"Error %d writing watchdog control.\n", err);
		goto out_unlock;
	}

	err = nizynqcpld_write(cpld, DOSX_WATCHDOGCONTROL,
			       control | DOSX_WATCHDOGCONTROL_PET);
	if (err) {
		dev_err(cpld->dev,
			"Error %d writing watchdog control.\n", err);
		goto out_unlock;
	}
out_unlock:
	nizynqcpld_unlock(cpld);
	return err;
}

static int nizynqcpld_watchdog_pet(struct nizynqcpld *cpld, u32 *state)
{
	int err;
	u8 control;

	nizynqcpld_lock(cpld);

	if (cpld->watchdog.expired) {
		err = 0;
		*state = NIWATCHDOG_STATE_EXPIRED;
	} else {
		err = nizynqcpld_read(cpld, DOSX_WATCHDOGCONTROL, &control);
		if (err) {
			dev_err(cpld->dev,
				"Error %d reading watchdog control.\n", err);
			goto out_unlock;
		}

		control |= DOSX_WATCHDOGCONTROL_PET;

		err = nizynqcpld_write(cpld, DOSX_WATCHDOGCONTROL, control);
		if (err) {
			dev_err(cpld->dev,
				"Error %d writing watchdog control.\n", err);
			goto out_unlock;
		}

		*state = NIWATCHDOG_STATE_RUNNING;
	}

out_unlock:
	nizynqcpld_unlock(cpld);
	return err;
}

static int nizynqcpld_watchdog_reset(struct nizynqcpld *cpld)
{
	int err;

	nizynqcpld_lock(cpld);

	cpld->watchdog.expired = false;

	err = nizynqcpld_write(cpld, DOSX_WATCHDOGCONTROL,
			       DOSX_WATCHDOGCONTROL_RESET);

	nizynqcpld_unlock(cpld);

	if (err)
		dev_err(cpld->dev,
			"Error %d writing watchdog control.\n", err);
	return err;
}

static int nizynqcpld_watchdog_counter_get(struct nizynqcpld *cpld,
					   u32 *counter)
{
	int err;
	u8 control;
	u8 data[DOSX_WATCHDOG_COUNTER_BYTES];

	nizynqcpld_lock(cpld);

	err = nizynqcpld_read(cpld, DOSX_WATCHDOGCONTROL, &control);

	if (err) {
		dev_err(cpld->dev,
			"Error %d reading watchdog control.\n", err);
		goto out_unlock;
	}

	err = nizynqcpld_write(cpld, DOSX_WATCHDOGCONTROL,
			       control | DOSX_WATCHDOGCONTROL_CAPTURECOUNTER);
	if (err) {
		dev_err(cpld->dev,
			"Error %d capturing watchdog counter.\n", err);
		goto out_unlock;
	}

	/* Returns the number of read bytes */
	err = i2c_smbus_read_i2c_block_data(cpld->client,
					    DOSX_WATCHDOGCOUNTER2,
					    DOSX_WATCHDOG_COUNTER_BYTES,
					    data);
	if (DOSX_WATCHDOG_COUNTER_BYTES == err)
		err = 0;
	else {
		dev_err(cpld->dev,
			"Error %d reading watchdog counter.\n", err);
		goto out_unlock;
	}

	*counter = (data[0] << 16) | (data[1] << 8) | data[2];

out_unlock:
	nizynqcpld_unlock(cpld);
	return err;
}

static irqreturn_t nizynqcpld_watchdog_irq(int irq, void *data)
{
	struct nizynqcpld *cpld = data;
	irqreturn_t ret = IRQ_NONE;
	u8 control;
	int err;

	nizynqcpld_lock(cpld);

	err = nizynqcpld_read(cpld, DOSX_WATCHDOGCONTROL, &control);

	if (err) {
		dev_err(cpld->dev,
			"Error %d reading watchdog control.\n", err);
		goto out_unlock;
	} else if (!(DOSX_WATCHDOGCONTROL_ALARM & control)) {
		dev_err(cpld->dev,
			"Spurious watchdog interrupt, 0x%02X\n", control);
		goto out_unlock;
	}

	cpld->watchdog.expired = true;

	/* Acknowledge the interrupt. */
	control |= DOSX_WATCHDOGCONTROL_RESET;
	err = nizynqcpld_write(cpld, DOSX_WATCHDOGCONTROL, control);

	/* Signal the watchdog event. */
	wake_up_all(&cpld->watchdog.irq_event);

	ret = IRQ_HANDLED;

out_unlock:
	nizynqcpld_unlock(cpld);
	return ret;
}

static int nizynqcpld_watchdog_misc_open(struct inode *inode,
					 struct file *file)
{
	struct miscdevice *misc_dev = file->private_data;
	struct nizynqcpld *cpld = container_of(misc_dev, struct nizynqcpld,
					       watchdog.misc_dev);
	file->private_data = cpld;

	if (!atomic_dec_and_test(&cpld->watchdog.available)) {
		atomic_inc(&cpld->watchdog.available);
		return -EBUSY;
	}

	return request_threaded_irq(cpld->client->irq,
				    NULL, nizynqcpld_watchdog_irq,
				    0, NIWATCHDOG_NAME, cpld);
}

static int nizynqcpld_watchdog_misc_release(struct inode *inode,
					    struct file *file)
{
	struct nizynqcpld *cpld = file->private_data;
	free_irq(cpld->client->irq, cpld);
	atomic_inc(&cpld->watchdog.available);
	return 0;
}

long nizynqcpld_watchdog_misc_ioctl(struct file *file, unsigned int cmd,
				    unsigned long arg)
{
	struct nizynqcpld *cpld = file->private_data;
	struct nizynqcpld_watchdog_desc *desc;
	int err;

	desc = cpld->watchdog.desc;

	switch(cmd) {
	case NIWATCHDOG_IOCTL_PERIOD_NS: {
		__u32 period = desc->watchdog_period_ns;
		err = copy_to_user((__u32 *)arg, &period,
				   sizeof(__u32));
		break;
	}
	case NIWATCHDOG_IOCTL_MAX_COUNTER: {
		__u32 counter = DOSX_WATCHDOG_MAX_COUNTER;
		err = copy_to_user((__u32 *)arg, &counter,
				   sizeof(__u32));
		break;
	}
	case NIWATCHDOG_IOCTL_COUNTER_SET: {
		__u32 counter;
		err = copy_from_user(&counter, (__u32 *)arg,
				     sizeof(__u32));
		if (!err)
			err = nizynqcpld_watchdog_counter_set(cpld, counter);
		break;
	}
	case NIWATCHDOG_IOCTL_CHECK_ACTION: {
		__u32 action;
		err = copy_from_user(&action, (__u32 *)arg,
				     sizeof(__u32));
		if (!err)
			err = nizynqcpld_watchdog_check_action(action);
		break;
	}
	case NIWATCHDOG_IOCTL_ADD_ACTION: {
		__u32 action;
		err = copy_from_user(&action, (__u32 *)arg,
				     sizeof(__u32));
		if (!err)
			err = nizynqcpld_watchdog_add_action(cpld, action);
		break;
	}
	case NIWATCHDOG_IOCTL_START: {
		err = nizynqcpld_watchdog_start(cpld);
		break;
	}
	case NIWATCHDOG_IOCTL_PET: {
		__u32 state;
		err = nizynqcpld_watchdog_pet(cpld, &state);
		if (!err)
			err = copy_to_user((__u32 *)arg, &state,
					   sizeof(__u32));
		break;
	}
	case NIWATCHDOG_IOCTL_RESET: {
		err = nizynqcpld_watchdog_reset(cpld);
		break;
	}
	case NIWATCHDOG_IOCTL_COUNTER_GET: {
		__u32 counter;
		err = nizynqcpld_watchdog_counter_get(cpld, &counter);
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

unsigned int nizynqcpld_watchdog_misc_poll(struct file *file,
					   struct poll_table_struct *wait)
{
	struct nizynqcpld *cpld = file->private_data;
	unsigned int mask = 0;

	poll_wait(file, &cpld->watchdog.irq_event, wait);
	nizynqcpld_lock(cpld);
	if (cpld->watchdog.expired)
		mask = POLLIN;
	nizynqcpld_unlock(cpld);
	return mask;
}

static const struct file_operations nizynqcpld_watchdog_misc_fops = {
	.owner		= THIS_MODULE,
	.open		= nizynqcpld_watchdog_misc_open,
	.release	= nizynqcpld_watchdog_misc_release,
	.unlocked_ioctl	= nizynqcpld_watchdog_misc_ioctl,
	.poll		= nizynqcpld_watchdog_misc_poll,
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
		.pattern_hi_addr	= DOSX_STATUSLEDSHIFTBYTE1, /* write byte 1 first */
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

static struct nizynqcpld_watchdog_desc dosxv4_watchdog_desc = {
	.watchdog_period_ns	= 24000,
};

static struct nizynqcpld_watchdog_desc dosxv5_watchdog_desc = {
	.watchdog_period_ns	= 30720,
};

static struct nizynqcpld_desc nizynqcpld_descs[] = {
	{
		.attrs			= nizynqcpld_attrs,
		.supported_version	= 3,
		.led_descs		= proto_leds,
		.num_led_descs		= ARRAY_SIZE(proto_leds),
		.reboot_addr		= PROTO_PROCESSORSTATE,
		.scratch_hr_addr	= PROTO_SCRATCHPADHR,
		.scratch_sr_addr	= PROTO_SCRATCHPADSR,
		.switch_addr		= PROTO_SWITCHANDLED,
	},
	{
		.attrs			= nizynqcpld_attrs,
		.supported_version	= 4,
		.watchdog_desc		= &dosxv4_watchdog_desc,
		.led_descs		= dosx_leds,
		.num_led_descs		= ARRAY_SIZE(dosx_leds),
		.reboot_addr		= DOSX_PROCESSORRESET,
		.scratch_hr_addr	= DOSX_SCRATCHPADHR,
		.scratch_sr_addr	= DOSX_SCRATCHPADSR,
		.switch_addr		= DOSX_DEBUGSWITCH,
	},
	{
		.attrs			= nizynqcpld_attrs,
		.supported_version	= 5,
		.watchdog_desc		= &dosxv5_watchdog_desc,
		.led_descs		= dosx_leds,
		.num_led_descs		= ARRAY_SIZE(dosx_leds),
		.reboot_addr		= DOSX_PROCESSORRESET,
		.scratch_hr_addr	= DOSX_SCRATCHPADHR,
		.scratch_sr_addr	= DOSX_SCRATCHPADSR,
		.switch_addr		= DOSX_DEBUGSWITCH,
	},
	{
		.attrs			= dosequis6_attrs,
		.supported_version	= 6,
		.watchdog_desc		= &dosxv5_watchdog_desc,
		.led_descs		= dosx_leds,
		.num_led_descs		= ARRAY_SIZE(dosx_leds),
		.reboot_addr		= DOSX_PROCESSORRESET,
		.scratch_hr_addr	= DOSX_SCRATCHPADHR,
		.scratch_sr_addr	= DOSX_SCRATCHPADSR,
		.switch_addr		= DOSX_DEBUGSWITCH,
		.watchdog_addr		= DOSX_WATCHDOGCONTROL,
	},
};

static int nizynqcpld_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	struct nizynqcpld_desc *desc;
	struct nizynqcpld *cpld;
	u8 version;
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
		goto err_read_version;
	}

	for (i = 0, desc = NULL; i < ARRAY_SIZE(nizynqcpld_descs); i++) {
		if (nizynqcpld_descs[i].supported_version == version) {
			desc = &nizynqcpld_descs[i];
			break;
		}
	}

	if (!desc) {
		err = -ENODEV;
		dev_err(cpld->dev,
			"this driver does not support cpld with version %d.\n", version);
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

	if (desc->watchdog_desc) {
		struct nizynqcpld_watchdog *watchdog = &cpld->watchdog;

		watchdog->desc = desc->watchdog_desc;
		atomic_set(&watchdog->available, 1);
		init_waitqueue_head(&watchdog->irq_event);
		watchdog->expired = false;

		watchdog->misc_dev.minor = MISC_DYNAMIC_MINOR;
		watchdog->misc_dev.name  = NIWATCHDOG_NAME;
		watchdog->misc_dev.fops  = &nizynqcpld_watchdog_misc_fops;
		err = misc_register(&watchdog->misc_dev);
		if (err) {
			dev_err(cpld->dev,
				"Couldn't register misc device\n");
			goto err_watchdog_register;
		}
	}

	cpld->reset.reset = nizynqcpld_reset;
	zynq_board_reset = &cpld->reset;

	i2c_set_clientdata(client, cpld);

	dev_info(&client->dev, "%s National Instruments Dos Equis CPLD found.\n",
							client->name);

	return 0;

err_watchdog_register:
	sysfs_remove_files(&client->dev.kobj, desc->attrs);
err_sysfs_create_files:
err_led:
	while (i--)
		nizynqcpld_led_unregister(&cpld->leds[i]);
	kfree(cpld->leds);
err_led_alloc:
err_no_version:
err_read_version:
	kfree(cpld);
err_cpld_alloc:
	return err;
}

static int __devexit nizynqcpld_remove(struct i2c_client *client)
{
	struct nizynqcpld *cpld = i2c_get_clientdata(client);
	struct nizynqcpld_desc *desc = cpld->desc;
	int i;

	zynq_board_reset = NULL;

	if (desc->watchdog_desc)
		misc_deregister(&cpld->watchdog.misc_dev);

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
	.remove		= __devexit_p(nizynqcpld_remove),
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
MODULE_AUTHOR("Josh Cartwright <josh.cartwright@ni.com>");
MODULE_LICENSE("GPL");
