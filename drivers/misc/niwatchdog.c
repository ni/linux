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
#include <linux/niwatchdog.h>
#include <linux/miscdevice.h>
#include <linux/interrupt.h>
#include <linux/poll.h>

#define MODULE_NAME "niwatchdog"

#define NIWD_CONTROL	0x01
#define NIWD_COUNTER2	0x02
#define NIWD_COUNTER1	0x03
#define NIWD_COUNTER0	0x04
#define NIWD_SEED2	0x05
#define NIWD_SEED1	0x06
#define NIWD_SEED0	0x07

#define NIWD_IO_SIZE	0x08

#define NIWD_CONTROL_MODE		0x80
#define NIWD_CONTROL_PROC_INTERRUPT	0x40
#define NIWD_CONTROL_PROC_RESET		0x20
#define NIWD_CONTROL_PET		0x10
#define NIWD_CONTROL_RUNNING		0x08
#define NIWD_CONTROL_CAPTURECOUNTER	0x04
#define NIWD_CONTROL_RESET		0x02
#define NIWD_CONTROL_ALARM		0x01

#define NIWD_PERIOD_NS		30720
#define NIWD_MAX_COUNTER	0x00FFFFFF


struct niwatchdog {
	struct acpi_device *acpi_device;
	u16 io_base;
	u32 irq;
	spinlock_t lock;
	struct miscdevice misc_dev;
	atomic_t available;
	wait_queue_head_t irq_event;
	u32 running:1;
	u32 expired:1;
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

	data = inb(niwatchdog->io_base + NIWD_CONTROL);

	/* Check if we're already in user mode. */
	if (!(data & NIWD_CONTROL_MODE))
		return count;

	data = NIWD_CONTROL_MODE | NIWD_CONTROL_RESET;

	outb(data, niwatchdog->io_base + NIWD_CONTROL);

	return count;
}

static DEVICE_ATTR(watchdog_mode, S_IRUSR|S_IWUSR, niwatchdog_wdmode_get,
	niwatchdog_wdmode_set);

static const struct attribute *niwatchdog_attrs[] = {
	&dev_attr_watchdog_mode.attr,
	NULL
};

static int niwatchdog_counter_set(struct niwatchdog *niwatchdog, u32 counter)
{
	int ret;
	unsigned long flags;

	spin_lock_irqsave(&niwatchdog->lock, flags);

	/* Prevent changing the counter while the watchdog is running. */
	if (niwatchdog->running) {
		ret = -EBUSY;
		goto out_unlock;
	}

	outb(((0x00FF0000 & counter) >> 16), niwatchdog->io_base + NIWD_SEED2);
	outb(((0x0000FF00 & counter) >> 8), niwatchdog->io_base + NIWD_SEED1);
	outb((0x000000FF & counter), niwatchdog->io_base + NIWD_SEED0);

	ret = 0;

out_unlock:
	spin_unlock_irqrestore(&niwatchdog->lock, flags);

	return ret;
}

static int niwatchdog_check_action(u32 action)
{
	int err = 0;

	switch (action) {
	case NIWD_CONTROL_PROC_INTERRUPT:
	case NIWD_CONTROL_PROC_RESET:
		break;
	default:
		err = -ENOTSUPP;
	}

	return err;
}

static int niwatchdog_add_action(struct niwatchdog *niwatchdog, u32 action)
{
	u8 action_mask;
	u8 control;
	unsigned long flags;

	if (action == NIWATCHDOG_ACTION_INTERRUPT)
		action_mask = NIWD_CONTROL_PROC_INTERRUPT;
	else if (action == NIWATCHDOG_ACTION_RESET)
		action_mask = NIWD_CONTROL_PROC_RESET;
	else
		return -ENOTSUPP;

	spin_lock_irqsave(&niwatchdog->lock, flags);

	control = inb(niwatchdog->io_base + NIWD_CONTROL);
	control |= action_mask;
	outb(control, niwatchdog->io_base + NIWD_CONTROL);

	spin_unlock_irqrestore(&niwatchdog->lock, flags);

	return 0;
}

static int niwatchdog_start(struct niwatchdog *niwatchdog)
{
	u8 control;
	unsigned long flags;

	spin_lock_irqsave(&niwatchdog->lock, flags);

	niwatchdog->running = true;
	niwatchdog->expired = false;

	control = inb(niwatchdog->io_base + NIWD_CONTROL);
	outb(control | NIWD_CONTROL_RESET, niwatchdog->io_base + NIWD_CONTROL);
	outb(control | NIWD_CONTROL_PET, niwatchdog->io_base + NIWD_CONTROL);

	spin_unlock_irqrestore(&niwatchdog->lock, flags);

	return 0;
}

static int niwatchdog_pet(struct niwatchdog *niwatchdog, u32 *state)
{
	u8 control;
	unsigned long flags;

	spin_lock_irqsave(&niwatchdog->lock, flags);

	if (niwatchdog->expired) {
		*state = NIWATCHDOG_STATE_EXPIRED;
	} else if (!niwatchdog->running) {
		*state = NIWATCHDOG_STATE_DISABLED;
	} else {
		control = inb(niwatchdog->io_base + NIWD_CONTROL);
		control |= NIWD_CONTROL_PET;
		outb(control, niwatchdog->io_base + NIWD_CONTROL);

		*state = NIWATCHDOG_STATE_RUNNING;
	}

	spin_unlock_irqrestore(&niwatchdog->lock, flags);

	return 0;
}

static int niwatchdog_reset(struct niwatchdog *niwatchdog)
{
	unsigned long flags;

	spin_lock_irqsave(&niwatchdog->lock, flags);

	niwatchdog->running = false;
	niwatchdog->expired = false;

	outb(NIWD_CONTROL_RESET, niwatchdog->io_base + NIWD_CONTROL);

	spin_unlock_irqrestore(&niwatchdog->lock, flags);

	return 0;
}

static int niwatchdog_counter_get(struct niwatchdog *niwatchdog,
				  u32 *counter)
{
	u8 control;
	u8 counter2, counter1, counter0;
	unsigned long flags;

	spin_lock_irqsave(&niwatchdog->lock, flags);

	control = inb(niwatchdog->io_base + NIWD_CONTROL);
	control |= NIWD_CONTROL_CAPTURECOUNTER;
	outb(control, niwatchdog->io_base + NIWD_CONTROL);

	counter2 = inb(niwatchdog->io_base + NIWD_COUNTER2);
	counter1 = inb(niwatchdog->io_base + NIWD_COUNTER1);
	counter0 = inb(niwatchdog->io_base + NIWD_COUNTER0);

	*counter = (counter2 << 16) | (counter1 << 8) | counter0;

	spin_unlock_irqrestore(&niwatchdog->lock, flags);

	return 0;
}

static irqreturn_t niwatchdog_irq(int irq, void *data)
{
	struct niwatchdog *niwatchdog = data;
	irqreturn_t ret = IRQ_NONE;
	u8 control;
	unsigned long flags;

	spin_lock_irqsave(&niwatchdog->lock, flags);

	control = inb(niwatchdog->io_base + NIWD_CONTROL);

	if (!(NIWD_CONTROL_ALARM & control)) {
		dev_err(&niwatchdog->acpi_device->dev,
			"Spurious watchdog interrupt, 0x%02X\n", control);
		goto out_unlock;
	}

	niwatchdog->running = false;
	niwatchdog->expired = true;

	/* Acknowledge the interrupt. */
	control |= NIWD_CONTROL_RESET;
	outb(control, niwatchdog->io_base + NIWD_CONTROL);

	/* Signal the watchdog event. */
	wake_up_all(&niwatchdog->irq_event);

	ret = IRQ_HANDLED;

out_unlock:
	spin_unlock_irqrestore(&niwatchdog->lock, flags);

	return ret;
}

static int niwatchdog_misc_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc_dev = file->private_data;
	struct niwatchdog *niwatchdog = container_of(
		misc_dev, struct niwatchdog, misc_dev);

	file->private_data = niwatchdog;

	if (!atomic_dec_and_test(&niwatchdog->available)) {
		atomic_inc(&niwatchdog->available);
		return -EBUSY;
	}

	return 0;
}

static int niwatchdog_misc_release(struct inode *inode, struct file *file)
{
	struct niwatchdog *niwatchdog = file->private_data;

	atomic_inc(&niwatchdog->available);
	return 0;
}

static long niwatchdog_misc_ioctl(struct file *file, unsigned int cmd,
				  unsigned long arg)
{
	struct niwatchdog *niwatchdog = file->private_data;
	int err;

	switch (cmd) {
	case NIWATCHDOG_IOCTL_PERIOD_NS: {
		__u32 period = NIWD_PERIOD_NS;

		err = copy_to_user((__u32 *)arg, &period,
				   sizeof(__u32));
		break;
	}
	case NIWATCHDOG_IOCTL_MAX_COUNTER: {
		__u32 counter = NIWD_MAX_COUNTER;

		err = copy_to_user((__u32 *)arg, &counter,
				   sizeof(__u32));
		break;
	}
	case NIWATCHDOG_IOCTL_COUNTER_SET: {
		__u32 counter;

		err = copy_from_user(&counter, (__u32 *)arg,
				     sizeof(__u32));
		if (!err)
			err = niwatchdog_counter_set(niwatchdog, counter);
		break;
	}
	case NIWATCHDOG_IOCTL_CHECK_ACTION: {
		__u32 action;

		err = copy_from_user(&action, (__u32 *)arg,
				     sizeof(__u32));
		if (!err)
			err = niwatchdog_check_action(action);
		break;
	}
	case NIWATCHDOG_IOCTL_ADD_ACTION: {
		__u32 action;
		err = copy_from_user(&action, (__u32 *)arg,
				     sizeof(__u32));
		if (!err)
			err = niwatchdog_add_action(niwatchdog, action);
		break;
	}
	case NIWATCHDOG_IOCTL_START: {
		err = niwatchdog_start(niwatchdog);
		break;
	}
	case NIWATCHDOG_IOCTL_PET: {
		__u32 state;

		err = niwatchdog_pet(niwatchdog, &state);
		if (!err)
			err = copy_to_user((__u32 *)arg, &state,
					   sizeof(__u32));
		break;
	}
	case NIWATCHDOG_IOCTL_RESET: {
		err = niwatchdog_reset(niwatchdog);
		break;
	}
	case NIWATCHDOG_IOCTL_COUNTER_GET: {
		__u32 counter;

		err = niwatchdog_counter_get(niwatchdog, &counter);
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

unsigned int niwatchdog_misc_poll(struct file *file,
				  struct poll_table_struct *wait)
{
	struct niwatchdog *niwatchdog = file->private_data;
	unsigned int mask = 0;
	unsigned long flags;

	poll_wait(file, &niwatchdog->irq_event, wait);

	spin_lock_irqsave(&niwatchdog->lock, flags);

	if (niwatchdog->expired)
		mask = POLLIN;

	spin_unlock_irqrestore(&niwatchdog->lock, flags);

	return mask;
}

static const struct file_operations niwatchdog_misc_fops = {
	.owner		= THIS_MODULE,
	.open		= niwatchdog_misc_open,
	.release	= niwatchdog_misc_release,
	.unlocked_ioctl	= niwatchdog_misc_ioctl,
	.poll		= niwatchdog_misc_poll,
};

static acpi_status niwatchdog_resources(struct acpi_resource *res, void *data)
{
	struct niwatchdog *niwatchdog = data;
	struct device *dev = &niwatchdog->acpi_device->dev;
	u16 io_size;

	switch (res->type) {
	case ACPI_RESOURCE_TYPE_IO:
		if (niwatchdog->io_base != 0) {
			dev_err(dev, "too many IO resources\n");
			return AE_ERROR;
		}

		niwatchdog->io_base = res->data.io.minimum;
		io_size = res->data.io.address_length;

		if (io_size < NIWD_IO_SIZE) {
			dev_err(dev, "memory region too small\n");
			return AE_ERROR;
		}

		if (!devm_request_region(dev, niwatchdog->io_base, io_size,
					 MODULE_NAME)) {
			dev_err(dev, "failed to get memory region\n");
			return AE_ERROR;
		}

		return AE_OK;

	case ACPI_RESOURCE_TYPE_IRQ:
		if (niwatchdog->irq != 0) {
			dev_err(dev, "too many IRQ resources\n");
			return AE_ERROR;
		}

		niwatchdog->irq = res->data.irq.interrupts[0];

		if (devm_request_threaded_irq(dev, niwatchdog->irq, NULL,
					      niwatchdog_irq, IRQF_ONESHOT,
					      NIWATCHDOG_NAME, niwatchdog)) {
			dev_err(dev, "failed to get interrupt\n");
			return AE_ERROR;
		}

		return AE_OK;

	case ACPI_RESOURCE_TYPE_END_TAG:
		return AE_OK;

	default:
		dev_err(dev, "unsupported resource type %d\n", res->type);
		return AE_ERROR;
	}
}

static int niwatchdog_acpi_remove(struct acpi_device *device)
{
	struct niwatchdog *niwatchdog = device->driver_data;

	misc_deregister(&niwatchdog->misc_dev);

	sysfs_remove_files(&niwatchdog->acpi_device->dev.kobj,
			   niwatchdog_attrs);

	return 0;
}

static int niwatchdog_acpi_add(struct acpi_device *device)
{
	struct device *dev = &device->dev;
	struct niwatchdog *niwatchdog;
	acpi_status acpi_ret;
	int err;

	niwatchdog = devm_kzalloc(dev, sizeof(*niwatchdog), GFP_KERNEL);
	if (!niwatchdog)
		return -ENOMEM;

	device->driver_data = niwatchdog;
	niwatchdog->acpi_device = device;

	acpi_ret = acpi_walk_resources(device->handle, METHOD_NAME__CRS,
				       niwatchdog_resources, niwatchdog);
	if (ACPI_FAILURE(acpi_ret) || niwatchdog->io_base == 0 ||
	    niwatchdog->irq == 0) {
		dev_err(dev, "failed to get resources\n");
		return -ENODEV;
	}

	spin_lock_init(&niwatchdog->lock);

	atomic_set(&niwatchdog->available, 1);
	init_waitqueue_head(&niwatchdog->irq_event);
	niwatchdog->expired = false;

	err = sysfs_create_files(&dev->kobj, niwatchdog_attrs);
	if (err) {
		dev_err(dev, "failed to create sysfs attributes\n");
		return err;
	}

	niwatchdog->misc_dev.minor = MISC_DYNAMIC_MINOR;
	niwatchdog->misc_dev.name  = NIWATCHDOG_NAME;
	niwatchdog->misc_dev.fops  = &niwatchdog_misc_fops;

	err = misc_register(&niwatchdog->misc_dev);
	if (err) {
		dev_err(dev, "failed to register misc device\n");
		sysfs_remove_files(&dev->kobj, niwatchdog_attrs);
		return err;
	}

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

module_acpi_driver(niwatchdog_acpi_driver);

MODULE_DEVICE_TABLE(acpi, niwatchdog_device_ids);
MODULE_DESCRIPTION("NI Watchdog");
MODULE_AUTHOR("Jeff Westfahl <jeff.westfahl@ni.com>");
MODULE_LICENSE("GPL");
