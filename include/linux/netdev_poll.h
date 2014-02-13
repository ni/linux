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

#ifndef _LINUX_NETDEV_POLL_H_
#define _LINUX_NETDEV_POLL_H_

#ifdef CONFIG_NETDEV_POLL

#include <linux/device_poll.h>

int netdev_poll_init(struct device_poll *device_poll);

#endif

#endif /* _LINUX_NETDEV_POLL_H_ */
