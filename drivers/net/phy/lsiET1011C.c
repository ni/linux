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

#define LSI_PHY_ID_ET1011C 0x0282f010
#define LSI_PHY_ID_MASK    0xfffffff0

#define LSI_PHY_ID_REV_MASK 0x0000000f
#define LSI_PHY_ID_REV_3    0x00000003

#define LSI_PHY_BIAS_LEVEL_ADJUST_STEP1 16

#define LSI_PHY_BIAS_LEVEL_ADJUST_STEP1_Internal 0x8817
#define LSI_PHY_BIAS_LEVEL_ADJUST_STEP1_1000Mb   0x8805
#define LSI_PHY_BIAS_LEVEL_ADJUST_STEP1_100Mb    0x8806
#define LSI_PHY_BIAS_LEVEL_ADJUST_STEP1_10Mb     0x8807

#define LSI_PHY_BIAS_LEVEL_ADJUST_STEP2 17

#define LSI_PHY_BIAS_LEVEL_ADJUST_STEP2_Internal 0x0001
#define LSI_PHY_BIAS_LEVEL_ADJUST_STEP2_1000Mb   0x503E
#define LSI_PHY_BIAS_LEVEL_ADJUST_STEP2_100Mb    0x303E
#define LSI_PHY_BIAS_LEVEL_ADJUST_STEP2_10Mb     0x6F20

#define LSI_PHY_PHYControl2 18

#define LSI_PHY_PHYControl2_EnableDiagnostics 0x0004

#define LSI_PHY_InterruptMask   24
#define LSI_PHY_InterruptStatus 25

#define LSI_PHY_Interrupt_AutonegotiationStatusChange 0x0100
#define LSI_PHY_Interrupt_LinkStatusChange            0x0004
#define LSI_PHY_Interrupt_MDINT_N                     0x0001

#define LSI_PHY_Interrupt_DefaultMask ( \
		LSI_PHY_Interrupt_AutonegotiationStatusChange \
	|	LSI_PHY_Interrupt_LinkStatusChange \
	|	LSI_PHY_Interrupt_MDINT_N \
	)

#define LSI_PHY_PHYStatus 26

#define LSI_PHY_PHYStatus_Speed_Mask    0x0300
#define LSI_PHY_PHYStatus_Speed_Unknown 0x0300
#define LSI_PHY_PHYStatus_Speed_1000    0x0200
#define LSI_PHY_PHYStatus_Speed_100     0x0100
#define LSI_PHY_PHYStatus_Speed_10      0x0000
#define LSI_PHY_PHYStatus_FullDuplex    0x0080
#define LSI_PHY_PHYStatus_Link          0x0040

#define LSI_PHY_LEDControl1 27
#define LSI_PHY_LEDControl2 28
#define LSI_PHY_LEDControl3 29

#define LSI_PHY_LEDControl1_Default 0x0001
#define LSI_PHY_LEDControl2_Default 0xF4F0
#define LSI_PHY_LEDControl3_Default 0x1F55

struct lsiET1011CCC_data {
	u32 led_control1_off;
	u32 led_control1_10Mb;
	u32 led_control1_100Mb;
	u32 led_control1_1Gb;
	u32 led_control2_off;
	u32 led_control2_10Mb;
	u32 led_control2_100Mb;
	u32 led_control2_1Gb;
	u32 led_control3;
};

static int lsiET1011C_probe(struct phy_device *phydev) {

	struct lsiET1011CCC_data *priv;

	dev_dbg (&phydev->dev, "probe\n");

	priv = kzalloc (sizeof(*priv), GFP_KERNEL);

	if (NULL == priv)
		return -ENOMEM;

	phydev->priv = priv;

	return 0;
}

static void lsiET1011C_remove(struct phy_device *phydev) {

	dev_dbg (&phydev->dev, "remove\n");

	kfree (phydev->priv);
	phydev->priv = NULL;
}

static int lsiET1011C_config_init(struct phy_device *phydev) {

	struct lsiET1011CCC_data *priv;
#ifdef CONFIG_OF
	const u32 * led_prop;
	int len;
#endif
	dev_dbg (&phydev->dev, "config_init\n");

	mutex_lock(&phydev->lock);

	priv = phydev->priv;

	/* Rev 3 has an errata to be taken care of. */
	if (LSI_PHY_ID_REV_3 == (LSI_PHY_ID_REV_MASK & phydev->phy_id)) {

		u32 bmcr;
		u32 phy_control2;

		/* This methodology comes from LSI ET1011C Product Advisory,
		   June 8, 2007, Revision 2.0.  According to this advisory,
		   we need to update the output amplitudes on the MDI interface
		   to be in compliance with IEEE specifications.  This needs
		   to be done after device power-up, a pin reset, or a software
		   reset. */

		/* Power down the PHY. */
		bmcr = phy_read(phydev, MII_BMCR);
		phy_write(phydev, MII_BMCR, BMCR_PDOWN | bmcr);

		/* Enable diagnostics. */
		phy_control2 = phy_read(phydev, LSI_PHY_PHYControl2);
		phy_write(phydev, LSI_PHY_PHYControl2, LSI_PHY_PHYControl2_EnableDiagnostics | phy_control2);

		/* Adjust internal bias level. */
		phy_write(phydev, LSI_PHY_BIAS_LEVEL_ADJUST_STEP1, LSI_PHY_BIAS_LEVEL_ADJUST_STEP1_Internal);
		phy_write(phydev, LSI_PHY_BIAS_LEVEL_ADJUST_STEP2, LSI_PHY_BIAS_LEVEL_ADJUST_STEP2_Internal);

		/* Adjust 1000Mb bias level. */
		phy_write(phydev, LSI_PHY_BIAS_LEVEL_ADJUST_STEP1, LSI_PHY_BIAS_LEVEL_ADJUST_STEP1_1000Mb);
		phy_write(phydev, LSI_PHY_BIAS_LEVEL_ADJUST_STEP2, LSI_PHY_BIAS_LEVEL_ADJUST_STEP2_1000Mb);

		/* Adjust 100Mb bias level. */
		phy_write(phydev, LSI_PHY_BIAS_LEVEL_ADJUST_STEP1, LSI_PHY_BIAS_LEVEL_ADJUST_STEP1_100Mb);
		phy_write(phydev, LSI_PHY_BIAS_LEVEL_ADJUST_STEP2, LSI_PHY_BIAS_LEVEL_ADJUST_STEP2_100Mb);

		/* Adjust 10Mb bias level. */
		phy_write(phydev, LSI_PHY_BIAS_LEVEL_ADJUST_STEP1, LSI_PHY_BIAS_LEVEL_ADJUST_STEP1_10Mb);
		phy_write(phydev, LSI_PHY_BIAS_LEVEL_ADJUST_STEP2, LSI_PHY_BIAS_LEVEL_ADJUST_STEP2_10Mb);

		/* Disable diagnostics. */
		phy_write(phydev, LSI_PHY_PHYControl2, phy_control2);

		/* Power up the PHY. */
		phy_write(phydev, MII_BMCR, bmcr);
	}

	/* Register default values. */
	priv->led_control1_off   = LSI_PHY_LEDControl1_Default;
	priv->led_control1_10Mb  = LSI_PHY_LEDControl1_Default;
	priv->led_control1_100Mb = LSI_PHY_LEDControl1_Default;
	priv->led_control1_1Gb   = LSI_PHY_LEDControl1_Default;
	priv->led_control2_off   = LSI_PHY_LEDControl2_Default;
	priv->led_control2_10Mb  = LSI_PHY_LEDControl2_Default;
	priv->led_control2_100Mb = LSI_PHY_LEDControl2_Default;
	priv->led_control2_1Gb   = LSI_PHY_LEDControl2_Default;
	priv->led_control3       = LSI_PHY_LEDControl3_Default;

#ifdef CONFIG_OF

	/* Look for LED configuration. */
	led_prop = of_get_property(phydev->dev.of_node, "leds", &len);

	if (led_prop && (9 == (len / sizeof (u32)))) {

		priv->led_control1_off   = be32_to_cpu (led_prop [0]);
		priv->led_control1_10Mb  = be32_to_cpu (led_prop [1]);
		priv->led_control1_100Mb = be32_to_cpu (led_prop [2]);
		priv->led_control1_1Gb   = be32_to_cpu (led_prop [3]);
		priv->led_control2_off   = be32_to_cpu (led_prop [4]);
		priv->led_control2_10Mb  = be32_to_cpu (led_prop [5]);
		priv->led_control2_100Mb = be32_to_cpu (led_prop [6]);
		priv->led_control2_1Gb   = be32_to_cpu (led_prop [7]);
		priv->led_control3       = be32_to_cpu (led_prop [8]);
	}

#endif

	/* Configure the PHY activity LED blink rate. */
	phy_write(phydev, LSI_PHY_LEDControl3, priv->led_control3);

	mutex_unlock(&phydev->lock);

	return 0;
}

static void lsiET1011C_update_status(struct phy_device *phydev) {
	struct lsiET1011CCC_data *priv;
	u32 phy_status;
	u32 led_control1;
	u32 led_control2;

	priv = phydev->priv;

	phy_status = phy_read(phydev, LSI_PHY_PHYStatus);

	dev_dbg (&phydev->dev, "update_status: 0x%04X\n", phy_status);

	if (LSI_PHY_PHYStatus_Link & phy_status) {
		phydev->speed = SPEED_10;
		phydev->link = 1;
		phydev->duplex = DUPLEX_HALF;

		led_control1 = priv->led_control1_10Mb;
		led_control2 = priv->led_control2_10Mb;

		if (LSI_PHY_PHYStatus_FullDuplex & phy_status) {
			phydev->duplex = DUPLEX_FULL;
		}

		if ((LSI_PHY_PHYStatus_Speed_Mask & phy_status) == LSI_PHY_PHYStatus_Speed_1000) {
			phydev->speed = SPEED_1000;

			led_control1 = priv->led_control1_1Gb;
			led_control2 = priv->led_control2_1Gb;
		} else if ((LSI_PHY_PHYStatus_Speed_Mask & phy_status) == LSI_PHY_PHYStatus_Speed_100) {
			phydev->speed = SPEED_100;

			led_control1 = priv->led_control1_100Mb;
			led_control2 = priv->led_control2_100Mb;
		}
	} else {
		phydev->link = 0;
		led_control1 = priv->led_control1_off;
		led_control2 = priv->led_control2_off;
	}

	phy_write(phydev, LSI_PHY_LEDControl1, led_control1);
	phy_write(phydev, LSI_PHY_LEDControl2, led_control2);
}

static int lsiET1011C_read_status(struct phy_device *phydev) {

	dev_dbg (&phydev->dev, "read_status\n");

	if (PHY_POLL == phydev->irq) {
		lsiET1011C_update_status(phydev);
	}

	return 0;
}

static int lsiET1011C_config_intr(struct phy_device *phydev) {

	dev_dbg (&phydev->dev, "config_intr: %s\n", (phydev->interrupts) ? "enabling" : "disabling");

	if (PHY_INTERRUPT_ENABLED == phydev->interrupts) {
		phy_write(phydev, LSI_PHY_InterruptMask, LSI_PHY_Interrupt_DefaultMask);
	}
	else {
		phy_write(phydev, LSI_PHY_InterruptMask, 0);
	}

	return 0;
}

static int lsiET1011C_did_interrupt(struct phy_device *phydev) {

	int ret;
	u32 interrupt_status = phy_read(phydev, LSI_PHY_InterruptStatus);

	ret = ((LSI_PHY_Interrupt_MDINT_N & interrupt_status) ? 1 : 0);

	dev_dbg (&phydev->dev, "did_interrupt: %d\n", ret);

	if (ret) {
		mutex_lock(&phydev->lock);
		lsiET1011C_update_status(phydev);
		mutex_unlock(&phydev->lock);
	}

	return ret;
}

static struct phy_driver lsiET1011C_driver = {
	.phy_id        = LSI_PHY_ID_ET1011C,
	.name          = "LSI ET1011C",
	.phy_id_mask   = LSI_PHY_ID_MASK,
	.features      = PHY_GBIT_FEATURES,
	.flags         = PHY_HAS_INTERRUPT,
	.probe         = lsiET1011C_probe,
	.remove        = lsiET1011C_remove,
	.config_init   = lsiET1011C_config_init,
	.config_aneg   = genphy_config_aneg,
	.read_status   = lsiET1011C_read_status,
	.config_intr   = lsiET1011C_config_intr,
	.did_interrupt = lsiET1011C_did_interrupt,
	.driver        = { .owner = THIS_MODULE,},
};

static int __init lsiET1011C_init(void)
{
	return phy_driver_register(&lsiET1011C_driver);
}

static void __exit lsiET1011C_exit(void)
{
	phy_driver_unregister(&lsiET1011C_driver);
}

module_init(lsiET1011C_init);
module_exit(lsiET1011C_exit);

static struct mdio_device_id __maybe_unused lsiET1011C_tbl[] = {
	{ LSI_PHY_ID_ET1011C, LSI_PHY_ID_MASK },
	{ }
};

MODULE_DEVICE_TABLE(mdio, lsiET1011C_tbl);

MODULE_DESCRIPTION("Driver for LSI ET1011C Ethernet PHY");
MODULE_AUTHOR("Jeff Westfahl <jeff.westfahl@ni.com>");
MODULE_LICENSE("GPL");
