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

#include <linux/of.h>
#include <linux/phy.h>
#include <linux/module.h>
#include <linux/netdevice.h>

#define MARVELL_PHY_ID_88E1512 0x01410dd0
#define MARVELL_PHY_ID_MASK    0xfffffff0

#define MARVELL_88E1512_Status 17

#define MARVELL_88E1512_Status_SpeedMask 0xC000
#define MARVELL_88E1512_Status_Speed1000 0x8000
#define MARVELL_88E1512_Status_Speed100  0x4000
#define MARVELL_88E1512_Status_Speed10   0x0000
#define MARVELL_88E1512_Status_Duplex    0x2000
#define MARVELL_88E1512_Status_Link      0x0C00

#define MARVELL_88E1512_InterruptEnable 18
#define MARVELL_88E1512_InterruptStatus 19

#define MARVELL_88E1512_Interrupt_LinkStatusChange 0x0400

#define MARVELL_88E1512_Interrupt_DefaultMask MARVELL_88E1512_Interrupt_LinkStatusChange

#define MARVELL_88E1512_PageAddress 22

#define MARVELL_88E1512_PageAddress_LED 3

#define MARVELL_88E1512_GlobalInterruptStatus 23

#define MARVELL_88E1512_GlobalInterruptStatus_Interrupt 0x0001

#define MARVELL_88E1512_LEDFunctionControl 16

#define MARVELL_88E1512_LEDTimerControl 18

#define MARVELL_88E1512_LEDTimerControl_InterruptEnable 0x0080

static void marvell88E1512_update_status(struct phy_device *phydev);

static int marvell88E1512_probe(struct phy_device *phydev) {

	u32 led_timer_control;

	dev_dbg (&phydev->dev, "probe\n");

	/* Enable the interrupt output. This is just a pin configuration,
	   we're not actually enabling any interrupts here. But the default
	   configuration causes our interrupt pin to be asserted. We need
	   to stop that as early as possible. */
	phy_write (phydev, MARVELL_88E1512_PageAddress, MARVELL_88E1512_PageAddress_LED);
	led_timer_control = phy_read (phydev, MARVELL_88E1512_LEDTimerControl);
	led_timer_control |= MARVELL_88E1512_LEDTimerControl_InterruptEnable;
	phy_write (phydev, MARVELL_88E1512_LEDTimerControl, led_timer_control);
	phy_write (phydev, MARVELL_88E1512_PageAddress, 0);

	return 0;
}

static int marvell88E1512_config_init(struct phy_device *phydev) {

#ifdef CONFIG_OF
	const u32 * led_prop;
	int len;
#endif

	dev_dbg (&phydev->dev, "config_init\n");

	mutex_lock(&phydev->lock);

#ifdef CONFIG_OF

	/* Look for LED configuration. */
	led_prop = of_get_property(phydev->dev.of_node, "leds", &len);

	if (led_prop && (1 == (len / sizeof (u32)))) {
		phy_write (phydev, MARVELL_88E1512_PageAddress, MARVELL_88E1512_PageAddress_LED);
		phy_write (phydev, MARVELL_88E1512_LEDFunctionControl, be32_to_cpu (*led_prop));
		phy_write (phydev, MARVELL_88E1512_PageAddress, 0);
	}

#endif

	/* If interrupts are in use, the status won't be polled, so we need to
	   get the initial status here. */
	if (PHY_POLL != phydev->irq) {
		marvell88E1512_update_status(phydev);
	}

	mutex_unlock(&phydev->lock);

	return 0;
}

static void marvell88E1512_update_status(struct phy_device *phydev) {
	u32 phy_status;

	phy_status = phy_read(phydev, MARVELL_88E1512_Status);

	dev_dbg (&phydev->dev, "update_status: 0x%04X\n", phy_status);

	if (MARVELL_88E1512_Status_Link & phy_status) {
		phydev->speed = SPEED_10;
		phydev->link = 1;
		phydev->duplex = DUPLEX_HALF;

		if (MARVELL_88E1512_Status_Duplex & phy_status) {
			phydev->duplex = DUPLEX_FULL;
		}

		if ((MARVELL_88E1512_Status_SpeedMask & phy_status) == MARVELL_88E1512_Status_Speed1000) {
			phydev->speed = SPEED_1000;
		} else if ((MARVELL_88E1512_Status_SpeedMask & phy_status) == MARVELL_88E1512_Status_Speed100) {
			phydev->speed = SPEED_100;
		}
	} else {
		phydev->link = 0;
	}
}

static int marvell88E1512_read_status(struct phy_device *phydev) {

	dev_dbg (&phydev->dev, "read_status\n");

	/* If interrupts are in use, the status will have been updated in the
	   deferred interrupt handler below. */
	if (PHY_POLL == phydev->irq) {
		marvell88E1512_update_status(phydev);
	}

	return 0;
}

static int marvell88E1512_config_intr(struct phy_device *phydev) {

	dev_dbg (&phydev->dev, "config_intr\n");

	if (PHY_INTERRUPT_ENABLED == phydev->interrupts) {
		phy_write(phydev, MARVELL_88E1512_InterruptEnable, MARVELL_88E1512_Interrupt_DefaultMask);
	}
	else {
		phy_write(phydev, MARVELL_88E1512_InterruptEnable, 0);
	}

	return 0;
}

static int marvell88E1512_did_interrupt(struct phy_device *phydev) {

	int ret;
	u32 global_interrupt_status = phy_read(phydev, MARVELL_88E1512_GlobalInterruptStatus);

	ret = ((MARVELL_88E1512_GlobalInterruptStatus_Interrupt & global_interrupt_status) ? 1 : 0);

	dev_dbg (&phydev->dev, "did_interrupt: %d\n", ret);

	if (ret) {
		mutex_lock(&phydev->lock);
		marvell88E1512_update_status(phydev);
		mutex_unlock(&phydev->lock);
	}

	return ret;
}

static int marvell88E1512_ack_interrupt(struct phy_device *phydev) {

	u32 interrupt_status = phy_read(phydev, MARVELL_88E1512_InterruptStatus);

	dev_dbg (&phydev->dev, "ack_interrupt: 0x%04X\n", interrupt_status);

	return 0;
}

static struct phy_driver marvell88E1512_driver = {
	.phy_id		= MARVELL_PHY_ID_88E1512,
	.name		= "Marvell 88E1512",
	.phy_id_mask	= MARVELL_PHY_ID_MASK,
	.features	= PHY_GBIT_FEATURES,
	.flags		= PHY_HAS_INTERRUPT,
	.probe		= marvell88E1512_probe,
	.config_init	= marvell88E1512_config_init,
	.config_aneg	= genphy_config_aneg,
	.read_status	= marvell88E1512_read_status,
	.config_intr	= marvell88E1512_config_intr,
	.did_interrupt	= marvell88E1512_did_interrupt,
	.ack_interrupt	= marvell88E1512_ack_interrupt,
	.driver		= { .owner = THIS_MODULE,},
};

static int __init marvell88E1512_init(void)
{
	return phy_driver_register(&marvell88E1512_driver);
}

static void __exit marvell88E1512_exit(void)
{
	phy_driver_unregister(&marvell88E1512_driver);
}

module_init(marvell88E1512_init);
module_exit(marvell88E1512_exit);

static struct mdio_device_id __maybe_unused marvell88E1512_tbl[] = {
	{ MARVELL_PHY_ID_88E1512, MARVELL_PHY_ID_MASK },
	{ }
};

MODULE_DEVICE_TABLE(mdio, marvell88E1512_tbl);

MODULE_DESCRIPTION("Driver for Marvell 88E1512 Ethernet PHY");
MODULE_AUTHOR("Jeff Westfahl <jeff.westfahl@ni.com>");
MODULE_LICENSE("GPL");
