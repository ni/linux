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

#include <linux/netdev_poll.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>

/* netdev_poll internal functions */

static int netdev_poll_reinit(struct device_poll *device_poll)
{
	int ret = 0;
	struct net_device *netdev = to_net_dev(device_poll->device);

	if (netif_running(netdev)) {
		netdev->netdev_ops->ndo_stop(netdev);
		ret = netdev->netdev_ops->ndo_open(netdev);
	}

	return ret;
}

static void netdev_poll_lock(struct device_poll *device_poll)
{
	rtnl_lock();
}

static void netdev_poll_unlock(struct device_poll *device_poll)
{
	rtnl_unlock();
}

/* netdev_poll external functions */

int netdev_poll_init(struct device_poll *device_poll)
{
	if (!device_poll || !device_poll->device || !device_poll->ops)
		return -EINVAL;

	if (!device_poll->ops->reinit)
		device_poll->ops->reinit = netdev_poll_reinit;
	if (!device_poll->ops->lock)
		device_poll->ops->lock = netdev_poll_lock;
	if (!device_poll->ops->unlock)
		device_poll->ops->unlock = netdev_poll_unlock;

	/* Allow changes from any process with CAP_NET_ADMIN. */
	device_poll->use_capability = 1;
	device_poll->capability = CAP_NET_ADMIN;

	return device_poll_init(device_poll);
}
EXPORT_SYMBOL(netdev_poll_init);
