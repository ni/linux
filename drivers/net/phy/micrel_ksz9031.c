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

#include <linux/phy.h>
#include <linux/module.h>
#include <linux/netdevice.h>

#define MICREL_PHY_ID_KSZ9031	0x00221620
#define MICREL_PHY_ID_MASK	0xfffffff0

#define MICREL_KSZ9031_INT	0x1B
#define MICREL_KSZ9031_PHY_CTRL	0x1F

#define MICREL_KSZ9031_INT_ENABLE	0x0500
#define MICREL_KSZ9031_INT_DETECT	0x0005

#define MICREL_KSZ9031_PHY_CTRL_1000	0x0040
#define MICREL_KSZ9031_PHY_CTRL_100	0x0020
#define MICREL_KSZ9031_PHY_CTRL_10	0x0010
#define MICREL_KSZ9031_PHY_CTRL_LINKED	0x0070
#define MICREL_KSZ9031_PHY_CTRL_DUPLEX	0x0008

static void micrel_ksz9031_update_status(struct phy_device *phydev)
{
	u32 phy_control;

	phy_control = phy_read(phydev, MICREL_KSZ9031_PHY_CTRL);

	dev_dbg(&phydev->dev, "update_status: 0x%04X\n", phy_control);

	if (phy_control & MICREL_KSZ9031_PHY_CTRL_LINKED) {
		phydev->link = 1;
		phydev->speed = SPEED_1000;
		phydev->duplex = DUPLEX_FULL;

		if (!(phy_control & MICREL_KSZ9031_PHY_CTRL_DUPLEX))
			phydev->duplex = DUPLEX_HALF;

		if (phy_control & MICREL_KSZ9031_PHY_CTRL_100)
			phydev->speed = SPEED_100;
		else if (phy_control & MICREL_KSZ9031_PHY_CTRL_10)
			phydev->speed = SPEED_10;
	} else {
		phydev->link = 0;
	}
}

static int micrel_ksz9031_config_init(struct phy_device *phydev)
{
	dev_dbg(&phydev->dev, "config_init\n");

	mutex_lock(&phydev->lock);

	/* If interrupts are in use, the status won't be polled, so we need to
	 * get the initial status here.
	 */
	if (PHY_POLL != phydev->irq)
		micrel_ksz9031_update_status(phydev);

	mutex_unlock(&phydev->lock);

	return 0;
}

static int micrel_ksz9031_read_status(struct phy_device *phydev)
{
	dev_dbg(&phydev->dev, "read_status\n");

	if (PHY_POLL == phydev->irq)
		micrel_ksz9031_update_status(phydev);

	return 0;
}

static int micrel_ksz9031_config_intr(struct phy_device *phydev)
{
	dev_dbg(&phydev->dev, "config_intr: %s\n",
		(phydev->interrupts) ? "enabling" : "disabling");

	if (PHY_INTERRUPT_ENABLED == phydev->interrupts)
		phy_write(phydev, MICREL_KSZ9031_INT,
			MICREL_KSZ9031_INT_ENABLE);
	else
		phy_write(phydev, MICREL_KSZ9031_INT, 0);

	return 0;
}

static int micrel_ksz9031_did_interrupt(struct phy_device *phydev)
{
	int ret;
	u32 interrupt_status = phy_read(phydev, MICREL_KSZ9031_INT);

	ret = ((MICREL_KSZ9031_INT_DETECT & interrupt_status) ? 1 : 0);

	dev_dbg(&phydev->dev, "did_interrupt: %d\n", ret);

	if (ret) {
		mutex_lock(&phydev->lock);
		micrel_ksz9031_update_status(phydev);
		mutex_unlock(&phydev->lock);
	}

	return ret;
}

static struct phy_driver micrel_ksz9031_driver = {
	.phy_id        = MICREL_PHY_ID_KSZ9031,
	.name          = "Micrel KSZ9031",
	.phy_id_mask   = MICREL_PHY_ID_MASK,
	.features      = PHY_GBIT_FEATURES,
	.flags         = PHY_HAS_INTERRUPT,
	.config_init   = micrel_ksz9031_config_init,
	.config_aneg   = genphy_config_aneg,
	.read_status   = micrel_ksz9031_read_status,
	.config_intr   = micrel_ksz9031_config_intr,
	.did_interrupt = micrel_ksz9031_did_interrupt,
	.driver        = { .owner = THIS_MODULE,},
};

static int __init micrel_ksz9031_init(void)
{
	return phy_driver_register(&micrel_ksz9031_driver);
}

static void __exit micrel_ksz9031_exit(void)
{
	phy_driver_unregister(&micrel_ksz9031_driver);
}

module_init(micrel_ksz9031_init);
module_exit(micrel_ksz9031_exit);

static struct mdio_device_id __maybe_unused micrel_ksz9031_tbl[] = {
	{ MICREL_PHY_ID_KSZ9031, MICREL_PHY_ID_MASK },
	{ }
};

MODULE_DEVICE_TABLE(mdio, micrel_ksz9031_tbl);

MODULE_DESCRIPTION("Driver for Micrel KSZ9031 Ethernet PHY");
MODULE_AUTHOR("Jeff Westfahl <jeff.westfahl@ni.com>");
MODULE_LICENSE("GPL");
