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

#define MODULE_NAME "niwatchdog"

#define NIWD_CONTROL	0x01
#define NIWD_COUNTER2	0x02
#define NIWD_COUNTER1	0x03
#define NIWD_COUNTER0	0x04
#define NIWD_SEED2	0x05
#define NIWD_SEED1	0x06
#define NIWD_SEED0	0x07

#define NIWD_IO_SIZE	0x08

#define NIWD_CONTROL_MODE	0x80
#define NIWD_CONTROL_RESET	0x02

struct niwatchdog {
	struct acpi_device *acpi_device;
	u16 io_base;
	u16 io_size;
	u32 irq;
	spinlock_t lock;
};

static ssize_t niwatchdog_wdmode_get(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct niwatchdog *niwatchdog = acpi_device->driver_data;
	u8 data;

	data = inb(niwatchdog->io_base + NIWD_CONTROL);

	data &= NIWD_CONTROL_MODE;

	return sprintf(buf, "%s\n", data ? "boot" : "user");
}

static ssize_t niwatchdog_wdmode_set(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct niwatchdog *niwatchdog = acpi_device->driver_data;
	u8 data;

	/* you can only switch boot->user */
	if (strcmp(buf, "user"))
		return -EINVAL;

	data = NIWD_CONTROL_MODE | NIWD_CONTROL_RESET;

	outb(data, niwatchdog->io_base + NIWD_CONTROL);

	return count;
}

static DEVICE_ATTR(watchdog_mode, S_IRUSR|S_IWUSR, niwatchdog_wdmode_get,
	niwatchdog_wdmode_set);

static ssize_t niwatchdog_register_dump_get(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct acpi_device *acpi_device = to_acpi_device(dev);
	struct niwatchdog *niwatchdog = acpi_device->driver_data;
	u8 control, counter2, counter1, counter0;
	u8 seed2, seed1, seed0;

	control = inb(niwatchdog->io_base + NIWD_CONTROL);
	counter2 = inb(niwatchdog->io_base + NIWD_COUNTER2);
	counter1 = inb(niwatchdog->io_base + NIWD_COUNTER1);
	counter0 = inb(niwatchdog->io_base + NIWD_COUNTER0);
	seed2 = inb(niwatchdog->io_base + NIWD_SEED2);
	seed1 = inb(niwatchdog->io_base + NIWD_SEED1);
	seed0 = inb(niwatchdog->io_base + NIWD_SEED0);

	return sprintf(buf,
		       "Control:   0x%02X\n"
		       "Counter 2: 0x%02X\n"
		       "Counter 1: 0x%02X\n"
		       "Counter 0: 0x%02X\n"
		       "Seed 2:    0x%02X\n"
		       "Seed 1:    0x%02X\n"
		       "Seed 0:    0x%02X\n",
		       control, counter2, counter1, counter0,
		       seed2, seed1, seed0);
}

static DEVICE_ATTR(register_dump, S_IRUGO, niwatchdog_register_dump_get, NULL);

static const struct attribute *niwatchdog_attrs[] = {
	&dev_attr_watchdog_mode.attr,
	&dev_attr_register_dump.attr,
	NULL
};

static acpi_status niwatchdog_resources(struct acpi_resource *res, void *data)
{
	struct niwatchdog *niwatchdog = data;

	switch (res->type) {
	case ACPI_RESOURCE_TYPE_IO:
		if ((niwatchdog->io_base != 0) ||
		    (niwatchdog->io_size != 0)) {
			dev_err(&niwatchdog->acpi_device->dev,
				"too many IO resources\n");
			return AE_ERROR;
		}

		niwatchdog->io_base = res->data.io.minimum;
		niwatchdog->io_size = res->data.io.address_length;

		return AE_OK;

	case ACPI_RESOURCE_TYPE_IRQ:
		if (niwatchdog->irq != 0) {
			dev_err(&niwatchdog->acpi_device->dev,
				"too many IRQ resources\n");
			return AE_ERROR;
		}

		niwatchdog->irq = res->data.irq.interrupts[0];

		return AE_OK;

	case ACPI_RESOURCE_TYPE_END_TAG:
		return AE_OK;

	default:
		dev_err(&niwatchdog->acpi_device->dev,
			"unsupported resource type %d\n",
			res->type);
		return AE_ERROR;
	}

	return AE_OK;
}

static int niwatchdog_acpi_remove(struct acpi_device *device)
{
	struct niwatchdog *niwatchdog = device->driver_data;

	sysfs_remove_files(&niwatchdog->acpi_device->dev.kobj,
			   niwatchdog_attrs);

	if ((niwatchdog->io_base != 0) &&
	    (niwatchdog->io_size == NIWD_IO_SIZE))
		release_region(niwatchdog->io_base, niwatchdog->io_size);

	device->driver_data = NULL;

	kfree(niwatchdog);

	return 0;
}

static int niwatchdog_acpi_add(struct acpi_device *device)
{
	struct niwatchdog *niwatchdog;
	acpi_status acpi_ret;
	int err;

	niwatchdog = kzalloc(sizeof(*niwatchdog), GFP_KERNEL);

	if (!niwatchdog)
		return -ENOMEM;

	device->driver_data = niwatchdog;

	niwatchdog->acpi_device = device;

	acpi_ret = acpi_walk_resources(device->handle, METHOD_NAME__CRS,
				       niwatchdog_resources, niwatchdog);

	if (ACPI_FAILURE(acpi_ret) ||
	    (niwatchdog->io_base == 0) ||
	    (niwatchdog->io_size != NIWD_IO_SIZE) ||
	    (niwatchdog->irq == 0)) {
		niwatchdog_acpi_remove(device);
		return -ENODEV;
	}

	if (!request_region(niwatchdog->io_base, niwatchdog->io_size,
	    MODULE_NAME)) {
		niwatchdog_acpi_remove(device);
		return -EBUSY;
	}

	err = sysfs_create_files(&niwatchdog->acpi_device->dev.kobj,
				 niwatchdog_attrs);
	if (err) {
		niwatchdog_acpi_remove(device);
		return err;
	}

	spin_lock_init(&niwatchdog->lock);

	dev_info(&niwatchdog->acpi_device->dev,
		"IO range 0x%04X-0x%04X, IRQ %d\n",
		niwatchdog->io_base,
		niwatchdog->io_base + niwatchdog->io_size - 1, niwatchdog->irq);

	return 0;
}

static const struct acpi_device_id niwatchdog_device_ids[] = {
	{"NIC775C", 0},
	{"", 0},
};

static struct acpi_driver niwatchdog_acpi_driver = {
	.name = MODULE_NAME,
	.ids = niwatchdog_device_ids,
	.ops = {
		.add = niwatchdog_acpi_add,
		.remove = niwatchdog_acpi_remove,
		},
};

static int __init niwatchdog_init(void)
{
	return acpi_bus_register_driver(&niwatchdog_acpi_driver);
}

static void __exit niwatchdog_exit(void)
{
	acpi_bus_unregister_driver(&niwatchdog_acpi_driver);
}

module_init(niwatchdog_init);
module_exit(niwatchdog_exit);

MODULE_DEVICE_TABLE(acpi, niwatchdog_device_ids);
MODULE_DESCRIPTION("NI Watchdog");
MODULE_AUTHOR("Jeff Westfahl <jeff.westfahl@ni.com>");
MODULE_LICENSE("GPL");
