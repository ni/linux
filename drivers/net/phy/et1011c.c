/*
 * drivers/net/phy/et1011c.c
 *
 * Driver for LSI ET1011C PHYs
 *
 * Author: Chaithrika U S
 *
 * Copyright (c) 2008 Texas Instruments
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/unistd.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mii.h>
#include <linux/ethtool.h>
#include <linux/phy.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <asm/irq.h>

#define ET1011C_STATUS_REG	(0x1A)
#define ET1011C_CONFIG_REG	(0x16)
#define ET1011C_SPEED_MASK		(0x0300)
#define ET1011C_GIGABIT_SPEED		(0x0200)
#define ET1011C_TX_FIFO_MASK		(0x3000)
#define ET1011C_TX_FIFO_DEPTH_8		(0x0000)
#define ET1011C_TX_FIFO_DEPTH_16	(0x1000)
#define ET1011C_INTERFACE_MASK		(0x0007)
#define ET1011C_GMII_INTERFACE		(0x0002)
#define ET1011C_SYS_CLK_EN		(0x01 << 4)

/* LSI ET1011 PHY LED Control 1 Register */
#define MIIM_ET1011_PHY_LED1                       0x1B
#define MIIM_ET1011_PHY_LED1_PULSE_STRETCH_0       0x0001
#define MIIM_ET1011_PHY_LED1_STRETCH_EVENT_28MS    0x0000
#define MIIM_ET1011_PHY_LED1_STRETCH_EVENT_60MS    0x0004
#define MIIM_ET1011_PHY_LED1_STRETCH_EVENT_100MS   0x0008
#define MIIM_ET1011_PHY_LED1_TWO_COLOR_100_1000    0x8000

/* LSI ET1011 PHY LED Control 2 Register */
#define MIIM_ET1011_PHY_LED2                       0x1C
#define MIIM_ET1011_PHY_LED2_TXRX_LED_SHIFT        12
#define MIIM_ET1011_PHY_LED2_LINK_LED_SHIFT        8
#define MIIM_ET1011_PHY_LED2_100_LED_SHIFT         4
#define MIIM_ET1011_PHY_LED2_1000_LED_SHIFT        0
#define MIIM_ET1011_PHY_LED2_1000                  0x0
#define MIIM_ET1011_PHY_LED2_100                   0x1
#define MIIM_ET1011_PHY_LED2_10                    0x2
#define MIIM_ET1011_PHY_LED2_1000_ON_100_BLINK     0x3
#define MIIM_ET1011_PHY_LED2_LINK                  0x4
#define MIIM_ET1011_PHY_LED2_TRANSMIT              0x5
#define MIIM_ET1011_PHY_LED2_RECEIVE               0x6
#define MIIM_ET1011_PHY_LED2_ACTIVITY              0x7
#define MIIM_ET1011_PHY_LED2_FULL_DUPLEX           0x8
#define MIIM_ET1011_PHY_LED2_COLLISION             0x9
#define MIIM_ET1011_PHY_LED2_LINK_ON_ACTIVITY_BLINK   0xA
#define MIIM_ET1011_PHY_LED2_LINK_ON_RECEIVE_BLINK 0xB
#define MIIM_ET1011_PHY_LED2_FULL_DUPLEX_ON_COLLISION_BLINK 0xC
#define MIIM_ET1011_PHY_LED2_BLINK                 0xD
#define MIIM_ET1011_PHY_LED2_ON                    0xE
#define MIIM_ET1011_PHY_LED2_OFF                   0xF

MODULE_DESCRIPTION("LSI ET1011C PHY driver");
MODULE_AUTHOR("Chaithrika U S");
MODULE_LICENSE("GPL");

static int et1011c_config_aneg(struct phy_device *phydev)
{
	int ctl = 0;
	ctl = phy_read(phydev, MII_BMCR);
	if (ctl < 0)
		return ctl;
	ctl &= ~(BMCR_FULLDPLX | BMCR_SPEED100 | BMCR_SPEED1000 |
		 BMCR_ANENABLE);
	/* First clear the PHY */
	phy_write(phydev, MII_BMCR, ctl | BMCR_RESET);

	return genphy_config_aneg(phydev);
}

static int et1011c_read_status(struct phy_device *phydev)
{
	int ret;
	u32 val;
	static int speed;
	ret = genphy_read_status(phydev);

	if (speed != phydev->speed) {
		speed = phydev->speed;
		val = phy_read(phydev, ET1011C_STATUS_REG);
		if ((val & ET1011C_SPEED_MASK) ==
					ET1011C_GIGABIT_SPEED) {
			val = phy_read(phydev, ET1011C_CONFIG_REG);
			val &= ~ET1011C_TX_FIFO_MASK;
			phy_write(phydev, ET1011C_CONFIG_REG, val\
					| ET1011C_GMII_INTERFACE\
					| ET1011C_SYS_CLK_EN\
					| ET1011C_TX_FIFO_DEPTH_16);

		}
	}
	return ret;
}

static struct phy_driver et1011c_driver = {
	.phy_id		= 0x0282f014,
	.name		= "ET1011C",
	.phy_id_mask	= 0xfffffff0,
	.features	= (PHY_BASIC_FEATURES | SUPPORTED_1000baseT_Full),
	.flags		= PHY_POLL,
	.config_aneg	= et1011c_config_aneg,
	.read_status	= et1011c_read_status,
	.driver 	= { .owner = THIS_MODULE,},
};

static int __init et1011c_init(void)
{
	return phy_driver_register(&et1011c_driver);
}

static void __exit et1011c_exit(void)
{
	phy_driver_unregister(&et1011c_driver);
}

module_init(et1011c_init);
module_exit(et1011c_exit);
