/*
 * Copyright (C) 2014 National Instruments Corp.
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

#ifndef _LINUX_DEVICE_POLL_H_
#define _LINUX_DEVICE_POLL_H_

#ifdef CONFIG_DEVICE_POLL

#include <linux/device.h>

struct device_poll;

struct device_poll_ops {
	/* Reinitialize, if the mode changes. */
	int (*reinit)(struct device_poll *device_poll);

	/* Lock and unlock, for consistency when changing settings. */
	void (*lock)(struct device_poll *device_poll);
	void (*unlock)(struct device_poll *device_poll);

	/* Polled interrupt handler. */
	void (*interrupt)(struct device_poll *device_poll);
};

struct device_poll {
	/* The following must be initialized by the driver before calling
	 * device_poll_init.
	 */

	/* The device for which we're polling. */
	struct device *device;

	/* Device operations. */
	struct device_poll_ops *ops;

	/* A capability can be specified to allow non-root users to modify
	 * the sysfs attributes.
	 */
	bool use_capability;
	int capability;

	/* Polling interval in milliseconds. A value of 0 or less means
	 * use interrupts.
	 */
	int interval;

	/* Polling task policy and priority, such as SCHED_FIFO 10. */
	int policy;
	int priority;

	/* The following are internal struct members and should not be touched
	 * by drivers.
	 */

	struct task_struct *task;
	int enabled;

	struct dev_ext_attribute interval_attr;
	struct dev_ext_attribute policy_attr;
	struct dev_ext_attribute priority_attr;
	struct attribute *attrs[4];
	struct attribute_group attr_group;
};

int device_poll_init(struct device_poll *device_poll);
void device_poll_exit(struct device_poll *device_poll);

int device_poll_request_irq(struct device_poll *device_poll);
void device_poll_free_irq(struct device_poll *device_poll);

static inline int device_poll_is_active(struct device_poll *device_poll)
{
	return likely(device_poll) && (device_poll->task != NULL);
}

static inline void device_poll_enable_irq(struct device_poll *device_poll)
{
	if (device_poll_is_active(device_poll)) {
		device_poll->enabled = 1;
		/* Ensure changes to device_poll->enabled are seen by the
		 * polling thread.
		 */
		smp_wmb();
	}
}

static inline void device_poll_disable_irq(struct device_poll *device_poll)
{
	if (device_poll_is_active(device_poll)) {
		device_poll->enabled = 0;
		/* Ensure changes to device_poll->enabled are seen by the
		 * polling thread.
		 */
		smp_wmb();
	}
}

#endif

#endif /* _LINUX_DEVICE_POLL_H_ */
