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

#ifndef _LINUX_NIWATCHDOG_H_
#define _LINUX_NIWATCHDOG_H_

#include <linux/ioctl.h>
#include <linux/types.h>

#define NIWATCHDOG_ACTION_INTERRUPT	0
#define NIWATCHDOG_ACTION_RESET		1

#define NIWATCHDOG_STATE_RUNNING	0
#define NIWATCHDOG_STATE_EXPIRED	1

#define NIWATCHDOG_IOCTL_PERIOD_NS		_IOR('W', 0, __u32)
#define NIWATCHDOG_IOCTL_MAX_COUNTER		_IOR('W', 1, __u32)
#define NIWATCHDOG_IOCTL_COUNTER_SET		_IOW('W', 2, __u32)
#define NIWATCHDOG_IOCTL_CHECK_ACTION		_IOW('W', 3, __u32)
#define NIWATCHDOG_IOCTL_ADD_ACTION		_IOW('W', 4, __u32)
#define NIWATCHDOG_IOCTL_START			_IO('W', 5)
#define NIWATCHDOG_IOCTL_PET			_IOR('W', 6, __u32)
#define NIWATCHDOG_IOCTL_RESET			_IO('W', 7)
#define NIWATCHDOG_IOCTL_COUNTER_GET		_IOR('W', 8, __u32)

#define NIWATCHDOG_NAME	"niwatchdog"

#endif /* _LINUX_NIWATCHDOG_H_ */
