/*
 * Copyright (C) 2013 National Instruments Corp.
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

#include <linux/acpi.h>
#include <linux/module.h>
#include <linux/leds.h>
#include <linux/delay.h>

#ifdef CONFIG_NI_HW_REBOOT
#include <asm/emergency-restart.h>
#endif

#define MODULE_NAME "nirtfeatures"

/* Register addresses */

#define NIRTF_SIGNATURE		0x00
#define NIRTF_YEAR		0x01
#define NIRTF_MONTH		0x02
#define NIRTF_DAY		0x03
#define NIRTF_HOUR		0x04
#define NIRTF_MINUTE		0x05
#define NIRTF_SCRATCH		0x06
#define NIRTF_BPINFO		0x07
#define NIRTF_RAIL_STATUS1	0x08
#define NIRTF_RAIL_STATUS2	0x09
#define NIRTF_RESET		0x10
#define NIRTF_RESET_SOURCE	0x11
#define NIRTF_PROCESSOR_MODE	0x12
#define NIRTF_SYSTEM_LEDS	0x20
#define NIRTF_STATUS_LED_SHIFT1	0x21
#define NIRTF_STATUS_LED_SHIFT0	0x22
#define NIRTF_RT_LEDS		0x23

#define NIRTF_IO_SIZE	0x40

/* Register values */

#define NIRTF_BPINFO_ID_MASK	0x07

#define NIRTF_BPINFO_ID_MANHATTAN	0
#define NIRTF_BPINFO_ID_HAMMERHEAD	1

#define NIRTF_RESET_RESET_PROCESSOR	0x80

#define NIRTF_RESET_SOURCE_SOFT_OFF	0x20
#define NIRTF_RESET_SOURCE_SOFTWARE	0x10
#define NIRTF_RESET_SOURCE_WATCHDOG	0x08
#define NIRTF_RESET_SOURCE_FPGA		0x04
#define NIRTF_RESET_SOURCE_PROCESSOR	0x02
#define NIRTF_RESET_SOURCE_BUTTON	0x01

#define NIRTF_PROCESSOR_MODE_HARD_BOOT_N	0x20
#define NIRTF_PROCESSOR_MODE_NO_FPGA		0x10
#define NIRTF_PROCESSOR_MODE_RECOVERY		0x08
#define NIRTF_PROCESSOR_MODE_CONSOLE_OUT	0x04
#define NIRTF_PROCESSOR_MODE_IP_RESET		0x02
#define NIRTF_PROCESSOR_MODE_SAFE		0x01

#define NIRTF_SYSTEM_LEDS_STATUS_RED	0x08
#define NIRTF_SYSTEM_LEDS_STATUS_YELLOW	0x04
#define NIRTF_SYSTEM_LEDS_POWER_GREEN	0x02
#define NIRTF_SYSTEM_LEDS_POWER_YELLOW	0x01

#define NIRTF_RT_LEDS_USER2_GREEN	0x08
#define NIRTF_RT_LEDS_USER2_YELLOW	0x04
#define NIRTF_RT_LEDS_USER1_GREEN	0x02
#define NIRTF_RT_LEDS_USER1_YELLOW	0x01

/* Structures */

struct nirtfeatures {
	struct acpi_device *acpi_device;
	u16 io_base;
	u16 io_size;
	spinlock_t lock;
	u8 revision[5];
	const char *bpstring;
	struct nirtfeatures_led *extra_leds;
	unsigned num_extra_leds;
};

struct nirtfeatures_led {
	struct led_classdev cdev;
	struct nirtfeatures *nirtfeatures;
	u8 address;
	u8 mask;
	u8 pattern_hi_addr;
	u8 pattern_lo_addr;
};

/* sysfs files */

static ssize_t nirtfeatures_revision_get(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;

	return sprintf(buf, "20%02X/%02X/%02X %02X:%02X\n",
		       nirtfeatures->revision[0], nirtfeatures->revision[1],
		       nirtfeatures->revision[2], nirtfeatures->revision[3],
		       nirtfeatures->revision[4]);
}

static DEVICE_ATTR(revision, S_IRUGO, nirtfeatures_revision_get, NULL);

static ssize_t nirtfeatures_scratch_get(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	u8 data;

	data = inb(nirtfeatures->io_base + NIRTF_SCRATCH);

	return sprintf(buf, "%02x\n", data);
}

static ssize_t nirtfeatures_scratch_set(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	unsigned long tmp;
	u8 data;

	if (kstrtoul(buf, 0, &tmp) || (tmp > 0xFF))
		return -EINVAL;

	data = (u8)tmp;

	outb(data, nirtfeatures->io_base + NIRTF_SCRATCH);

	return count;
}

static DEVICE_ATTR(scratch, S_IRUGO|S_IWUSR, nirtfeatures_scratch_get,
	nirtfeatures_scratch_set);

static ssize_t nirtfeatures_backplane_id_get(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;

	return sprintf(buf, "%s\n", nirtfeatures->bpstring);
}

static DEVICE_ATTR(backplane_id, S_IRUGO, nirtfeatures_backplane_id_get, NULL);

static ssize_t nirtfeatures_railstatus1_get(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	u8 data;

	data = inb(nirtfeatures->io_base + NIRTF_RAIL_STATUS1);

	return sprintf(buf, "%02x\n", data);
}

static DEVICE_ATTR(railstatus1, S_IRUGO, nirtfeatures_railstatus1_get, NULL);

static ssize_t nirtfeatures_railstatus2_get(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	u8 data;

	data = inb(nirtfeatures->io_base + NIRTF_RAIL_STATUS2);

	return sprintf(buf, "%02x\n", data);
}

static DEVICE_ATTR(railstatus2, S_IRUGO, nirtfeatures_railstatus2_get, NULL);

static ssize_t nirtfeatures_reset_set(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;

	if (strcmp(buf, "1"))
		return -EINVAL;

	outb(NIRTF_RESET_RESET_PROCESSOR, nirtfeatures->io_base + NIRTF_RESET);

	return count;
}

static DEVICE_ATTR(reset, S_IWUSR, NULL, nirtfeatures_reset_set);

static ssize_t nirtfeatures_reset_source_get(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	u8 data;
	const char *reset_source;

	data = inb(nirtfeatures->io_base + NIRTF_PROCESSOR_MODE);

	data &= NIRTF_PROCESSOR_MODE_HARD_BOOT_N;

	/* Power-on reset status is in a different register from the other reset
	   sources, we must check it first. */
	if (!data) {
		reset_source = "power-on reset";
	} else {
		data = inb(nirtfeatures->io_base + NIRTF_RESET_SOURCE);

		switch (data) {
		case NIRTF_RESET_SOURCE_SOFT_OFF:
			reset_source = "soft off button";
			break;
		case NIRTF_RESET_SOURCE_SOFTWARE:
			reset_source = "software";
			break;
		case NIRTF_RESET_SOURCE_WATCHDOG:
			reset_source = "watchdog";
			break;
		case NIRTF_RESET_SOURCE_FPGA:
			reset_source = "FPGA";
			break;
		case NIRTF_RESET_SOURCE_PROCESSOR:
			reset_source = "processor";
			break;
		case NIRTF_RESET_SOURCE_BUTTON:
			reset_source = "reset button";
			break;
		default:
			dev_err(&nirtfeatures->acpi_device->dev,
				"Unrecognized reset source 0x%02X\n",
				data);
			reset_source = "unknown";
			break;
		}
	}

	return sprintf(buf, "%s\n", reset_source);
}

static DEVICE_ATTR(reset_source, S_IRUGO, nirtfeatures_reset_source_get, NULL);

static ssize_t nirtfeatures_hard_boot_get(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	u8 data;

	data = inb(nirtfeatures->io_base + NIRTF_PROCESSOR_MODE);

	data &= NIRTF_PROCESSOR_MODE_HARD_BOOT_N;

	return sprintf(buf, "%s\n", data ? "soft reset" : "power-on reset");
}

static ssize_t nirtfeatures_hard_boot_set(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	u8 data;

	if (strcmp(buf, "1"))
		return -EINVAL;

	spin_lock(&nirtfeatures->lock);

	data = inb(nirtfeatures->io_base + NIRTF_PROCESSOR_MODE);

	data |= NIRTF_PROCESSOR_MODE_HARD_BOOT_N;

	outb(data, nirtfeatures->io_base + NIRTF_PROCESSOR_MODE);

	spin_unlock(&nirtfeatures->lock);

	return count;
}

static DEVICE_ATTR(hard_boot, S_IRUGO|S_IWUSR, nirtfeatures_hard_boot_get,
	nirtfeatures_hard_boot_set);

static ssize_t nirtfeatures_no_fpga_get(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	u8 data;

	data = inb(nirtfeatures->io_base + NIRTF_PROCESSOR_MODE);

	data &= NIRTF_PROCESSOR_MODE_NO_FPGA;

	return sprintf(buf, "%u\n", !!data);
}

static ssize_t nirtfeatures_no_fpga_set(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	unsigned long tmp;
	u8 data;

	if (kstrtoul(buf, 0, &tmp) || (tmp > 1))
		return -EINVAL;

	spin_lock(&nirtfeatures->lock);

	data = inb(nirtfeatures->io_base + NIRTF_PROCESSOR_MODE);

	if (tmp)
		data |= NIRTF_PROCESSOR_MODE_NO_FPGA;
	else
		data &= ~NIRTF_PROCESSOR_MODE_NO_FPGA;

	outb(data, nirtfeatures->io_base + NIRTF_PROCESSOR_MODE);

	spin_unlock(&nirtfeatures->lock);

	return count;
}

static DEVICE_ATTR(no_fpga, S_IRUGO|S_IWUSR, nirtfeatures_no_fpga_get,
	nirtfeatures_no_fpga_set);

static ssize_t nirtfeatures_recovery_mode_get(struct device *dev,
					      struct device_attribute *attr,
					      char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	u8 data;

	data = inb(nirtfeatures->io_base + NIRTF_PROCESSOR_MODE);

	data &= NIRTF_PROCESSOR_MODE_RECOVERY;

	return sprintf(buf, "%u\n", !!data);
}

static ssize_t nirtfeatures_recovery_mode_set(struct device *dev,
					      struct device_attribute *attr,
					      const char *buf, size_t count)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	unsigned long tmp;
	u8 data;

	if (kstrtoul(buf, 0, &tmp) || (tmp > 1))
		return -EINVAL;

	spin_lock(&nirtfeatures->lock);

	data = inb(nirtfeatures->io_base + NIRTF_PROCESSOR_MODE);

	if (tmp)
		data |= NIRTF_PROCESSOR_MODE_RECOVERY;
	else
		data &= ~NIRTF_PROCESSOR_MODE_RECOVERY;

	outb(data, nirtfeatures->io_base + NIRTF_PROCESSOR_MODE);

	spin_unlock(&nirtfeatures->lock);

	return count;
}

static DEVICE_ATTR(recovery_mode, S_IRUGO|S_IWUSR,
	nirtfeatures_recovery_mode_get, nirtfeatures_recovery_mode_set);

static ssize_t nirtfeatures_console_out_get(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	u8 data;

	data = inb(nirtfeatures->io_base + NIRTF_PROCESSOR_MODE);

	data &= NIRTF_PROCESSOR_MODE_CONSOLE_OUT;

	return sprintf(buf, "%u\n", !!data);
}

static DEVICE_ATTR(console_out, S_IRUGO, nirtfeatures_console_out_get, NULL);

static ssize_t nirtfeatures_ip_reset_get(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	u8 data;

	data = inb(nirtfeatures->io_base + NIRTF_PROCESSOR_MODE);

	data &= NIRTF_PROCESSOR_MODE_IP_RESET;

	return sprintf(buf, "%u\n", !!data);
}

static DEVICE_ATTR(ip_reset, S_IRUGO, nirtfeatures_ip_reset_get, NULL);

static ssize_t nirtfeatures_safe_mode_get(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	u8 data;

	data = inb(nirtfeatures->io_base + NIRTF_PROCESSOR_MODE);

	data &= NIRTF_PROCESSOR_MODE_SAFE;

	return sprintf(buf, "%u\n", !!data);
}

static DEVICE_ATTR(safe_mode, S_IRUGO, nirtfeatures_safe_mode_get, NULL);

static ssize_t nirtfeatures_register_dump_get(struct device *dev,
					      struct device_attribute *attr,
					      char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct nirtfeatures *nirtfeatures = acpi_device->driver_data;
	u8 signature, year, month, day, hour, minute, scratch, bpinfo;
	u8 railstatus1, railstatus2, reset, reset_source, processor_mode;
	u8 system_leds, status_led_shift1, status_led_shift0, rt_leds;

	signature = inb(nirtfeatures->io_base + NIRTF_SIGNATURE);
	year = inb(nirtfeatures->io_base + NIRTF_YEAR);
	month = inb(nirtfeatures->io_base + NIRTF_MONTH);
	day = inb(nirtfeatures->io_base + NIRTF_DAY);
	hour = inb(nirtfeatures->io_base + NIRTF_HOUR);
	minute = inb(nirtfeatures->io_base + NIRTF_MINUTE);
	scratch = inb(nirtfeatures->io_base + NIRTF_SCRATCH);
	bpinfo = inb(nirtfeatures->io_base + NIRTF_BPINFO);
	railstatus1 = inb(nirtfeatures->io_base + NIRTF_RAIL_STATUS1);
	railstatus2 = inb(nirtfeatures->io_base + NIRTF_RAIL_STATUS2);
	reset = inb(nirtfeatures->io_base + NIRTF_RESET);
	reset_source = inb(nirtfeatures->io_base + NIRTF_RESET_SOURCE);
	processor_mode = inb(nirtfeatures->io_base + NIRTF_PROCESSOR_MODE);
	system_leds = inb(nirtfeatures->io_base + NIRTF_SYSTEM_LEDS);
	status_led_shift1 =
		inb(nirtfeatures->io_base + NIRTF_STATUS_LED_SHIFT1);
	status_led_shift0 =
		inb(nirtfeatures->io_base + NIRTF_STATUS_LED_SHIFT0);
	rt_leds = inb(nirtfeatures->io_base + NIRTF_RT_LEDS);

	return sprintf(buf,
		       "Signature:          0x%02X\n"
		       "Year:               0x%02X\n"
		       "Month:              0x%02X\n"
		       "Day:                0x%02X\n"
		       "Hour:               0x%02X\n"
		       "Minute:             0x%02X\n"
		       "Scratch:            0x%02X\n"
		       "BPInfo:             0x%02X\n"
		       "Rail status 1:      0x%02X\n"
		       "Rail status 2:      0x%02X\n"
		       "Reset:              0x%02X\n"
		       "Reset source:       0x%02X\n"
		       "Processor mode:     0x%02X\n"
		       "System LEDs:        0x%02X\n"
		       "Status LED shift 1: 0x%02X\n"
		       "Status LED shift 0: 0x%02X\n"
		       "RT LEDs:            0x%02X\n",
		       signature, year, month, day, hour, minute, scratch,
		       bpinfo, railstatus1, railstatus2, reset, reset_source,
		       processor_mode, system_leds, status_led_shift1,
		       status_led_shift0, rt_leds);
}

static DEVICE_ATTR(register_dump, S_IRUGO, nirtfeatures_register_dump_get,
		   NULL);

static const struct attribute *nirtfeatures_attrs[] = {
	&dev_attr_revision.attr,
	&dev_attr_scratch.attr,
	&dev_attr_backplane_id.attr,
	&dev_attr_railstatus1.attr,
	&dev_attr_railstatus2.attr,
	&dev_attr_reset.attr,
	&dev_attr_reset_source.attr,
	&dev_attr_hard_boot.attr,
	&dev_attr_no_fpga.attr,
	&dev_attr_recovery_mode.attr,
	&dev_attr_console_out.attr,
	&dev_attr_ip_reset.attr,
	&dev_attr_safe_mode.attr,
	&dev_attr_register_dump.attr,
	NULL
};

/* LEDs */

static void nirtfeatures_led_brightness_set(struct led_classdev *led_cdev,
					    enum led_brightness brightness)
{
	struct nirtfeatures_led *led = (struct nirtfeatures_led *)led_cdev;
	u8 data;
	bool on;
	u16 pattern;

	on = !!brightness;
	pattern = brightness;

	spin_lock(&led->nirtfeatures->lock);

	data = inb(led->nirtfeatures->io_base + led->address);

	data &= ~led->mask;

	if (on)
		data |= led->mask;

	outb(data, led->nirtfeatures->io_base + led->address);

	if (led->pattern_hi_addr && led->pattern_lo_addr) {
		/* Write the high byte first. */
		outb(pattern >> 8,
		     led->nirtfeatures->io_base + led->pattern_hi_addr);
		outb(pattern & 0xFF,
		     led->nirtfeatures->io_base + led->pattern_lo_addr);
	}

	spin_unlock(&led->nirtfeatures->lock);
}

static enum led_brightness
nirtfeatures_led_brightness_get(struct led_classdev *led_cdev)
{
	struct nirtfeatures_led *led = (struct nirtfeatures_led *)led_cdev;
	u8 data;

	data = inb(led->nirtfeatures->io_base + led->address);

	/* For the yellow status LED, the blink pattern used for brightness
	   on write is write-only, so we just return on/off for all LEDs. */
	return (data & led->mask) ? LED_FULL : LED_OFF;
}

static struct nirtfeatures_led nirtfeatures_leds_common[] = {
	{
		{
			.name = CONFIG_NI_LED_PREFIX ":user1:green",
		},
		.address = NIRTF_RT_LEDS,
		.mask = NIRTF_RT_LEDS_USER1_GREEN,
	},
	{
		{
			.name = CONFIG_NI_LED_PREFIX ":user1:yellow",
		},
		.address = NIRTF_RT_LEDS,
		.mask = NIRTF_RT_LEDS_USER1_YELLOW,
	},
	{
		{
			.name = CONFIG_NI_LED_PREFIX ":status:red",
		},
		.address = NIRTF_SYSTEM_LEDS,
		.mask = NIRTF_SYSTEM_LEDS_STATUS_RED,
	},
	{
		{
			.name = CONFIG_NI_LED_PREFIX ":status:yellow",
			.max_brightness = 0xFFFF,
		},
		.address = NIRTF_SYSTEM_LEDS,
		.mask = NIRTF_SYSTEM_LEDS_STATUS_YELLOW,
		.pattern_hi_addr = NIRTF_STATUS_LED_SHIFT1,
		.pattern_lo_addr = NIRTF_STATUS_LED_SHIFT0,
	},
	{
		{
			.name = CONFIG_NI_LED_PREFIX ":power:green",
		},
		.address = NIRTF_SYSTEM_LEDS,
		.mask = NIRTF_SYSTEM_LEDS_POWER_GREEN,
	},
	{
		{
			.name = CONFIG_NI_LED_PREFIX ":power:yellow",
		},
		.address = NIRTF_SYSTEM_LEDS,
		.mask = NIRTF_SYSTEM_LEDS_POWER_YELLOW,
	},
};

static struct nirtfeatures_led nirtfeatures_leds_cdaq[] = {
	{
		{
			.name = CONFIG_NI_LED_PREFIX ":user2:green",
		},
		.address = NIRTF_RT_LEDS,
		.mask = NIRTF_RT_LEDS_USER2_GREEN,
	},
	{
		{
			.name = CONFIG_NI_LED_PREFIX ":user2:yellow",
		},
		.address = NIRTF_RT_LEDS,
		.mask = NIRTF_RT_LEDS_USER2_YELLOW,
	},
};

static int nirtfeatures_create_leds(struct nirtfeatures *nirtfeatures)
{
	int i;
	int err;

	for (i = 0; i < ARRAY_SIZE(nirtfeatures_leds_common); ++i) {

		nirtfeatures_leds_common[i].nirtfeatures = nirtfeatures;

		if (0 == nirtfeatures_leds_common[i].cdev.max_brightness)
			nirtfeatures_leds_common[i].cdev.max_brightness = 1;

		nirtfeatures_leds_common[i].cdev.brightness_set =
			nirtfeatures_led_brightness_set;

		nirtfeatures_leds_common[i].cdev.brightness_get =
			nirtfeatures_led_brightness_get;

		err = led_classdev_register(&nirtfeatures->acpi_device->dev,
					    &nirtfeatures_leds_common[i].cdev);
		if (err)
			return err;
	}

	for (i = 0; i < nirtfeatures->num_extra_leds; ++i) {

		nirtfeatures->extra_leds[i].nirtfeatures = nirtfeatures;

		if (0 == nirtfeatures->extra_leds[i].cdev.max_brightness)
			nirtfeatures->extra_leds[i].cdev.max_brightness = 1;

		nirtfeatures->extra_leds[i].cdev.brightness_set =
			nirtfeatures_led_brightness_set;

		nirtfeatures->extra_leds[i].cdev.brightness_get =
			nirtfeatures_led_brightness_get;

		err = led_classdev_register(&nirtfeatures->acpi_device->dev,
					    &nirtfeatures->extra_leds[i].cdev);
		if (err)
			return err;
	}

	return 0;
}

static void nirtfeatures_remove_leds(struct nirtfeatures *nirtfeatures)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(nirtfeatures_leds_common); ++i)
		led_classdev_unregister(&nirtfeatures_leds_common[i].cdev);

	for (i = 0; i < nirtfeatures->num_extra_leds; ++i)
		led_classdev_unregister(&nirtfeatures->extra_leds[i].cdev);
}

/* Board specific reboot fixup */

#ifdef CONFIG_NI_HW_REBOOT

static u16 mach_reboot_fixup_io_base;

void mach_reboot_fixups(void)
{
	int i;

	if (mach_reboot_fixup_io_base)
		for (i = 0; i < 10; ++i) {
			outb(NIRTF_RESET_RESET_PROCESSOR,
			     mach_reboot_fixup_io_base + NIRTF_RESET);
			udelay(100);
		}
}

#endif

/* ACPI driver */

static acpi_status nirtfeatures_resources(struct acpi_resource *res, void *data)
{
	struct nirtfeatures *nirtfeatures = data;

	switch (res->type) {
	case ACPI_RESOURCE_TYPE_IO:
		if ((nirtfeatures->io_base != 0) ||
		    (nirtfeatures->io_size != 0)) {
			dev_err(&nirtfeatures->acpi_device->dev,
				"too many IO resources\n");
			return AE_ERROR;
		}

		nirtfeatures->io_base = res->data.io.minimum;
		nirtfeatures->io_size = res->data.io.address_length;

		return AE_OK;

	case ACPI_RESOURCE_TYPE_END_TAG:
		return AE_OK;

	default:
		dev_err(&nirtfeatures->acpi_device->dev,
			"unsupported resource type %u\n",
			res->type);
		return AE_ERROR;
	}

	return AE_OK;
}

static int nirtfeatures_acpi_remove(struct acpi_device *device)
{
	struct nirtfeatures *nirtfeatures = device->driver_data;

	nirtfeatures_remove_leds(nirtfeatures);

	sysfs_remove_files(&nirtfeatures->acpi_device->dev.kobj,
			   nirtfeatures_attrs);

	if ((nirtfeatures->io_base != 0) &&
	    (nirtfeatures->io_size == NIRTF_IO_SIZE))
		release_region(nirtfeatures->io_base, nirtfeatures->io_size);

	device->driver_data = NULL;

	kfree(nirtfeatures);

	return 0;
}

static int nirtfeatures_acpi_add(struct acpi_device *device)
{
	struct nirtfeatures *nirtfeatures;
	acpi_status acpi_ret;
	u8 bpinfo;
	int err;

	nirtfeatures = kzalloc(sizeof(*nirtfeatures), GFP_KERNEL);

	if (!nirtfeatures)
		return -ENOMEM;

	device->driver_data = nirtfeatures;

	nirtfeatures->acpi_device = device;

	acpi_ret = acpi_walk_resources(device->handle, METHOD_NAME__CRS,
				       nirtfeatures_resources, nirtfeatures);

	if (ACPI_FAILURE(acpi_ret) ||
	    (nirtfeatures->io_base == 0) ||
	    (nirtfeatures->io_size != NIRTF_IO_SIZE)) {
		nirtfeatures_acpi_remove(device);
		return -ENODEV;
	}

	if (!request_region(nirtfeatures->io_base, nirtfeatures->io_size,
	    MODULE_NAME)) {
		nirtfeatures_acpi_remove(device);
		return -EBUSY;
	}

#ifdef CONFIG_NI_HW_REBOOT
	mach_reboot_fixup_io_base = nirtfeatures->io_base;
	reboot_type = BOOT_KBD;
#endif

	bpinfo = inb(nirtfeatures->io_base + NIRTF_BPINFO);

	bpinfo &= NIRTF_BPINFO_ID_MASK;

	switch (bpinfo) {
	case NIRTF_BPINFO_ID_MANHATTAN:
		nirtfeatures->bpstring = "Manhattan";
		break;
	case NIRTF_BPINFO_ID_HAMMERHEAD:
		nirtfeatures->bpstring = "Hammerhead";
		nirtfeatures->extra_leds = nirtfeatures_leds_cdaq;
		nirtfeatures->num_extra_leds =
			ARRAY_SIZE(nirtfeatures_leds_cdaq);
		break;
	default:
		dev_err(&nirtfeatures->acpi_device->dev,
			"Unrecognized backplane type %u\n",
			bpinfo);
		nirtfeatures_acpi_remove(device);
		return -ENODEV;
	}

	err = sysfs_create_files(&nirtfeatures->acpi_device->dev.kobj,
				 nirtfeatures_attrs);
	if (0 != err) {
		nirtfeatures_acpi_remove(device);
		return err;
	}

	err = nirtfeatures_create_leds(nirtfeatures);
	if (0 != err) {
		nirtfeatures_acpi_remove(device);
		return err;
	}

	spin_lock_init(&nirtfeatures->lock);

	nirtfeatures->revision[0] = inb(nirtfeatures->io_base + NIRTF_YEAR);
	nirtfeatures->revision[1] = inb(nirtfeatures->io_base + NIRTF_MONTH);
	nirtfeatures->revision[2] = inb(nirtfeatures->io_base + NIRTF_DAY);
	nirtfeatures->revision[3] = inb(nirtfeatures->io_base + NIRTF_HOUR);
	nirtfeatures->revision[4] = inb(nirtfeatures->io_base + NIRTF_MINUTE);

	dev_info(&nirtfeatures->acpi_device->dev,
		 "IO range 0x%04X-0x%04X\n",
		 nirtfeatures->io_base,
		 nirtfeatures->io_base + nirtfeatures->io_size - 1);

	return 0;
}

static const struct acpi_device_id nirtfeatures_device_ids[] = {
	{"NIC775D", 0},
	{"", 0},
};

static struct acpi_driver nirtfeatures_acpi_driver = {
	.name = MODULE_NAME,
	.ids = nirtfeatures_device_ids,
	.ops = {
		.add = nirtfeatures_acpi_add,
		.remove = nirtfeatures_acpi_remove,
		},
};

static int __init nirtfeatures_init(void)
{
	return acpi_bus_register_driver(&nirtfeatures_acpi_driver);
}

static void __exit nirtfeatures_exit(void)
{
	acpi_bus_unregister_driver(&nirtfeatures_acpi_driver);
}

module_init(nirtfeatures_init);
module_exit(nirtfeatures_exit);

MODULE_DEVICE_TABLE(acpi, nirtfeatures_device_ids);
MODULE_DESCRIPTION("NI RT Features");
MODULE_AUTHOR("Jeff Westfahl <jeff.westfahl@ni.com>");
MODULE_LICENSE("GPL");
