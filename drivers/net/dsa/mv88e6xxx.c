/*
 * net/dsa/mv88e6xxx.c - Marvell 88e6xxx switch chip support
 * Copyright (c) 2008 Marvell Semiconductor
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/if_bridge.h>
#include <linux/if_vlan.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/net_tstamp.h>
#include <linux/netdevice.h>
#include <linux/phy.h>
#include <linux/ptp_classify.h>
#include <linux/ptp_clock_kernel.h>
#include <net/dsa.h>
#include "mv88e6xxx.h"

/* If the switch's ADDR[4:0] strap pins are strapped to zero, it will
 * use all 32 SMI bus addresses on its SMI bus, and all switch registers
 * will be directly accessible on some {device address,register address}
 * pair.  If the ADDR[4:0] pins are not strapped to zero, the switch
 * will only respond to SMI transactions to that specific address, and
 * an indirect addressing mechanism needs to be used to access its
 * registers.
 */
static int mv88e6xxx_reg_wait_ready(struct mii_bus *bus, int sw_addr)
{
	int ret;
	int i;

	for (i = 0; i < 16; i++) {
		ret = mdiobus_read(bus, sw_addr, SMI_CMD);
		if (ret < 0)
			return ret;

		if ((ret & SMI_CMD_BUSY) == 0)
			return 0;
	}

	return -ETIMEDOUT;
}

int __mv88e6xxx_reg_read(struct mii_bus *bus, int sw_addr, int addr, int reg)
{
	int ret;

	if (sw_addr == 0)
		return mdiobus_read(bus, addr, reg);

	/* Wait for the bus to become free. */
	ret = mv88e6xxx_reg_wait_ready(bus, sw_addr);
	if (ret < 0)
		return ret;

	/* Transmit the read command. */
	ret = mdiobus_write(bus, sw_addr, SMI_CMD,
			    SMI_CMD_OP_22_READ | (addr << 5) | reg);
	if (ret < 0)
		return ret;

	/* Wait for the read command to complete. */
	ret = mv88e6xxx_reg_wait_ready(bus, sw_addr);
	if (ret < 0)
		return ret;

	/* Read the data. */
	ret = mdiobus_read(bus, sw_addr, SMI_DATA);
	if (ret < 0)
		return ret;

	return ret & 0xffff;
}

/* Must be called with SMI mutex held */
static int _mv88e6xxx_reg_read(struct dsa_switch *ds, int addr, int reg)
{
	struct mii_bus *bus = dsa_host_dev_to_mii_bus(ds->master_dev);
	int ret;

	if (bus == NULL)
		return -EINVAL;

	ret = __mv88e6xxx_reg_read(bus, ds->pd->sw_addr, addr, reg);
	if (ret < 0)
		return ret;

	dev_dbg(ds->master_dev, "<- addr: 0x%.2x reg: 0x%.2x val: 0x%.4x\n",
		addr, reg, ret);

	return ret;
}

int mv88e6xxx_reg_read(struct dsa_switch *ds, int addr, int reg)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int ret;

	mutex_lock(&ps->smi_mutex);
	ret = _mv88e6xxx_reg_read(ds, addr, reg);
	mutex_unlock(&ps->smi_mutex);

	return ret;
}

int __mv88e6xxx_reg_write(struct mii_bus *bus, int sw_addr, int addr,
			  int reg, u16 val)
{
	int ret;

	if (sw_addr == 0)
		return mdiobus_write(bus, addr, reg, val);

	/* Wait for the bus to become free. */
	ret = mv88e6xxx_reg_wait_ready(bus, sw_addr);
	if (ret < 0)
		return ret;

	/* Transmit the data to write. */
	ret = mdiobus_write(bus, sw_addr, SMI_DATA, val);
	if (ret < 0)
		return ret;

	/* Transmit the write command. */
	ret = mdiobus_write(bus, sw_addr, SMI_CMD,
			    SMI_CMD_OP_22_WRITE | (addr << 5) | reg);
	if (ret < 0)
		return ret;

	/* Wait for the write command to complete. */
	ret = mv88e6xxx_reg_wait_ready(bus, sw_addr);
	if (ret < 0)
		return ret;

	return 0;
}

/* Must be called with SMI mutex held */
static int _mv88e6xxx_reg_write(struct dsa_switch *ds, int addr, int reg,
				u16 val)
{
	struct mii_bus *bus = dsa_host_dev_to_mii_bus(ds->master_dev);

	if (bus == NULL)
		return -EINVAL;

	dev_dbg(ds->master_dev, "-> addr: 0x%.2x reg: 0x%.2x val: 0x%.4x\n",
		addr, reg, val);

	return __mv88e6xxx_reg_write(bus, ds->pd->sw_addr, addr, reg, val);
}

int mv88e6xxx_reg_write(struct dsa_switch *ds, int addr, int reg, u16 val)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int ret;

	mutex_lock(&ps->smi_mutex);
	ret = _mv88e6xxx_reg_write(ds, addr, reg, val);
	mutex_unlock(&ps->smi_mutex);

	return ret;
}

int mv88e6xxx_config_prio(struct dsa_switch *ds)
{
	/* Configure the IP ToS mapping registers. */
	REG_WRITE(REG_GLOBAL, GLOBAL_IP_PRI_0, 0x0000);
	REG_WRITE(REG_GLOBAL, GLOBAL_IP_PRI_1, 0x0000);
	REG_WRITE(REG_GLOBAL, GLOBAL_IP_PRI_2, 0x5555);
	REG_WRITE(REG_GLOBAL, GLOBAL_IP_PRI_3, 0x5555);
	REG_WRITE(REG_GLOBAL, GLOBAL_IP_PRI_4, 0xaaaa);
	REG_WRITE(REG_GLOBAL, GLOBAL_IP_PRI_5, 0xaaaa);
	REG_WRITE(REG_GLOBAL, GLOBAL_IP_PRI_6, 0xffff);
	REG_WRITE(REG_GLOBAL, GLOBAL_IP_PRI_7, 0xffff);

	/* Configure the IEEE 802.1p priority mapping register. */
	REG_WRITE(REG_GLOBAL, GLOBAL_IEEE_PRI, 0xfa41);

	return 0;
}

int mv88e6xxx_set_addr_direct(struct dsa_switch *ds, u8 *addr)
{
	REG_WRITE(REG_GLOBAL, GLOBAL_MAC_01, (addr[0] << 8) | addr[1]);
	REG_WRITE(REG_GLOBAL, GLOBAL_MAC_23, (addr[2] << 8) | addr[3]);
	REG_WRITE(REG_GLOBAL, GLOBAL_MAC_45, (addr[4] << 8) | addr[5]);

	return 0;
}

int mv88e6xxx_set_addr_indirect(struct dsa_switch *ds, u8 *addr)
{
	int i;
	int ret;

	for (i = 0; i < 6; i++) {
		int j;

		/* Write the MAC address byte. */
		REG_WRITE(REG_GLOBAL2, GLOBAL2_SWITCH_MAC,
			  GLOBAL2_SWITCH_MAC_BUSY | (i << 8) | addr[i]);

		/* Wait for the write to complete. */
		for (j = 0; j < 16; j++) {
			ret = REG_READ(REG_GLOBAL2, GLOBAL2_SWITCH_MAC);
			if ((ret & GLOBAL2_SWITCH_MAC_BUSY) == 0)
				break;
		}
		if (j == 16)
			return -ETIMEDOUT;
	}

	return 0;
}

/* Must be called with phy mutex held */
static int _mv88e6xxx_phy_read(struct dsa_switch *ds, int addr, int regnum)
{
	if (addr >= 0)
		return mv88e6xxx_reg_read(ds, addr, regnum);
	return 0xffff;
}

/* Must be called with phy mutex held */
static int _mv88e6xxx_phy_write(struct dsa_switch *ds, int addr, int regnum,
				u16 val)
{
	if (addr >= 0)
		return mv88e6xxx_reg_write(ds, addr, regnum, val);
	return 0;
}

#ifdef CONFIG_NET_DSA_MV88E6XXX_NEED_PPU
static int mv88e6xxx_ppu_disable(struct dsa_switch *ds)
{
	int ret;
	int i;

	ret = REG_READ(REG_GLOBAL, GLOBAL_CONTROL);
	REG_WRITE(REG_GLOBAL, GLOBAL_CONTROL,
		  ret & ~GLOBAL_CONTROL_PPU_ENABLE);

	for (i = 0; i < 16; i++) {
		ret = REG_READ(REG_GLOBAL, GLOBAL_STATUS);
		usleep_range(1000, 2000);
		if ((ret & GLOBAL_STATUS_PPU_MASK) !=
		    GLOBAL_STATUS_PPU_POLLING)
			return 0;
	}

	return -ETIMEDOUT;
}

static int mv88e6xxx_ppu_enable(struct dsa_switch *ds)
{
	int ret;
	unsigned long timeout;

	ret = REG_READ(REG_GLOBAL, GLOBAL_CONTROL);
	REG_WRITE(REG_GLOBAL, GLOBAL_CONTROL, ret | GLOBAL_CONTROL_PPU_ENABLE);

	timeout = jiffies + 1 * HZ;
	while (time_before(jiffies, timeout)) {
		ret = REG_READ(REG_GLOBAL, GLOBAL_STATUS);
		usleep_range(1000, 2000);
		if ((ret & GLOBAL_STATUS_PPU_MASK) ==
		    GLOBAL_STATUS_PPU_POLLING)
			return 0;
	}

	return -ETIMEDOUT;
}

static void mv88e6xxx_ppu_reenable_work(struct work_struct *ugly)
{
	struct mv88e6xxx_priv_state *ps;

	ps = container_of(ugly, struct mv88e6xxx_priv_state, ppu_work);
	if (mutex_trylock(&ps->ppu_mutex)) {
		struct dsa_switch *ds = ((struct dsa_switch *)ps) - 1;

		if (mv88e6xxx_ppu_enable(ds) == 0)
			ps->ppu_disabled = 0;
		mutex_unlock(&ps->ppu_mutex);
	}
}

static void mv88e6xxx_ppu_reenable_timer(unsigned long _ps)
{
	struct mv88e6xxx_priv_state *ps = (void *)_ps;

	schedule_work(&ps->ppu_work);
}

static int mv88e6xxx_ppu_access_get(struct dsa_switch *ds)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int ret;

	mutex_lock(&ps->ppu_mutex);

	/* If the PHY polling unit is enabled, disable it so that
	 * we can access the PHY registers.  If it was already
	 * disabled, cancel the timer that is going to re-enable
	 * it.
	 */
	if (!ps->ppu_disabled) {
		ret = mv88e6xxx_ppu_disable(ds);
		if (ret < 0) {
			mutex_unlock(&ps->ppu_mutex);
			return ret;
		}
		ps->ppu_disabled = 1;
	} else {
		del_timer(&ps->ppu_timer);
		ret = 0;
	}

	return ret;
}

static void mv88e6xxx_ppu_access_put(struct dsa_switch *ds)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);

	/* Schedule a timer to re-enable the PHY polling unit. */
	mod_timer(&ps->ppu_timer, jiffies + msecs_to_jiffies(10));
	mutex_unlock(&ps->ppu_mutex);
}

void mv88e6xxx_ppu_state_init(struct dsa_switch *ds)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);

	mutex_init(&ps->ppu_mutex);
	INIT_WORK(&ps->ppu_work, mv88e6xxx_ppu_reenable_work);
	init_timer(&ps->ppu_timer);
	ps->ppu_timer.data = (unsigned long)ps;
	ps->ppu_timer.function = mv88e6xxx_ppu_reenable_timer;
}

int mv88e6xxx_phy_read_ppu(struct dsa_switch *ds, int addr, int regnum)
{
	int ret;

	ret = mv88e6xxx_ppu_access_get(ds);
	if (ret >= 0) {
		ret = mv88e6xxx_reg_read(ds, addr, regnum);
		mv88e6xxx_ppu_access_put(ds);
	}

	return ret;
}

int mv88e6xxx_phy_write_ppu(struct dsa_switch *ds, int addr,
			    int regnum, u16 val)
{
	int ret;

	ret = mv88e6xxx_ppu_access_get(ds);
	if (ret >= 0) {
		ret = mv88e6xxx_reg_write(ds, addr, regnum, val);
		mv88e6xxx_ppu_access_put(ds);
	}

	return ret;
}
#endif

void mv88e6xxx_poll_link(struct dsa_switch *ds)
{
	int i;

	for (i = 0; i < DSA_MAX_PORTS; i++) {
		struct net_device *dev;
		int uninitialized_var(port_status);
		int link;
		int speed;
		int duplex;
		int fc;

		dev = ds->ports[i];
		if (dev == NULL)
			continue;

		link = 0;
		if (dev->flags & IFF_UP) {
			port_status = mv88e6xxx_reg_read(ds, REG_PORT(i),
							 PORT_STATUS);
			if (port_status < 0)
				continue;

			link = !!(port_status & PORT_STATUS_LINK);
		}

		if (!link) {
			if (netif_carrier_ok(dev)) {
				netdev_info(dev, "link down\n");
				netif_carrier_off(dev);
			}
			continue;
		}

		switch (port_status & PORT_STATUS_SPEED_MASK) {
		case PORT_STATUS_SPEED_10:
			speed = 10;
			break;
		case PORT_STATUS_SPEED_100:
			speed = 100;
			break;
		case PORT_STATUS_SPEED_1000:
			speed = 1000;
			break;
		default:
			speed = -1;
			break;
		}
		duplex = (port_status & PORT_STATUS_DUPLEX) ? 1 : 0;
		fc = (port_status & PORT_STATUS_PAUSE_EN) ? 1 : 0;

		if (!netif_carrier_ok(dev)) {
			netdev_info(dev,
				    "link up, %d Mb/s, %s duplex, flow control %sabled\n",
				    speed,
				    duplex ? "full" : "half",
				    fc ? "en" : "dis");
			netif_carrier_on(dev);
		}
	}
}

static bool mv88e6xxx_6352_family(struct dsa_switch *ds)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);

	switch (ps->id) {
	case PORT_SWITCH_ID_6352:
	case PORT_SWITCH_ID_6172:
	case PORT_SWITCH_ID_6176:
		return true;
	}
	return false;
}

static bool mv88e6xxx_6320_family(struct dsa_switch *ds)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);

	switch (ps->id) {
	case PORT_SWITCH_ID_6341:
		return true;
	}
	return false;
}

static int _mv88e6xxx_stats_wait(struct dsa_switch *ds)
{
	int ret;
	int i;

	for (i = 0; i < 10; i++) {
		ret = _mv88e6xxx_reg_read(ds, REG_GLOBAL, GLOBAL_STATS_OP);
		if ((ret & GLOBAL_STATS_OP_BUSY) == 0)
			return 0;
	}

	return -ETIMEDOUT;
}

static int _mv88e6xxx_stats_snapshot(struct dsa_switch *ds, int port)
{
	int ret;
	int cmd;

	if (mv88e6xxx_6320_family(ds) || mv88e6xxx_6352_family(ds))
		port = (port + 1) << 5;

	cmd = GLOBAL_STATS_OP_CAPTURE_PORT | port;

	if (mv88e6xxx_6320_family(ds)) {
		ret = _mv88e6xxx_reg_write(ds, REG_GLOBAL, GLOBAL_CONTROL_2,
					   GLOBAL_CONTROL_2_RMU_DISABLED |
					   GLOBAL_CONTROL_2_HIST_RX_TX | port);
		if (ret < 0)
			return ret;
	} else {
		cmd |= GLOBAL_STATS_OP_HIST_RX_TX;
	}

	/* Snapshot the hardware statistics counters for this port. */
	ret = _mv88e6xxx_reg_write(ds, REG_GLOBAL, GLOBAL_STATS_OP, cmd);
	if (ret < 0)
		return ret;

	/* Wait for the snapshotting to complete. */
	ret = _mv88e6xxx_stats_wait(ds);
	if (ret < 0)
		return ret;

	return 0;
}

static void _mv88e6xxx_stats_read(struct dsa_switch *ds, int stat, u32 *val)
{
	u32 _val;
	int ret;
	int cmd;

	*val = 0;

	cmd = GLOBAL_STATS_OP_READ_CAPTURED | stat;

	/* 6352 family has the histogram configuration in Global Control 2 */
	if (!mv88e6xxx_6320_family(ds))
		cmd |= GLOBAL_STATS_OP_HIST_RX_TX;

	ret = _mv88e6xxx_reg_write(ds, REG_GLOBAL, GLOBAL_STATS_OP, cmd);
	if (ret < 0)
		return;

	ret = _mv88e6xxx_stats_wait(ds);
	if (ret < 0)
		return;

	ret = _mv88e6xxx_reg_read(ds, REG_GLOBAL, GLOBAL_STATS_COUNTER_32);
	if (ret < 0)
		return;

	_val = ret << 16;

	ret = _mv88e6xxx_reg_read(ds, REG_GLOBAL, GLOBAL_STATS_COUNTER_01);
	if (ret < 0)
		return;

	*val = _val | ret;
}

static struct mv88e6xxx_hw_stat mv88e6xxx_hw_stats[] = {
	{ "in_good_octets",	8, 0x00, BANK0, },
	{ "in_bad_octets",	4, 0x02, BANK0, },
	{ "in_unicast",		4, 0x04, BANK0, },
	{ "in_broadcasts",	4, 0x06, BANK0, },
	{ "in_multicasts",	4, 0x07, BANK0, },
	{ "in_pause",		4, 0x16, BANK0, },
	{ "in_undersize",	4, 0x18, BANK0, },
	{ "in_fragments",	4, 0x19, BANK0, },
	{ "in_oversize",	4, 0x1a, BANK0, },
	{ "in_jabber",		4, 0x1b, BANK0, },
	{ "in_rx_error",	4, 0x1c, BANK0, },
	{ "in_fcs_error",	4, 0x1d, BANK0, },
	{ "out_octets",		8, 0x0e, BANK0, },
	{ "out_unicast",	4, 0x10, BANK0, },
	{ "out_broadcasts",	4, 0x13, BANK0, },
	{ "out_multicasts",	4, 0x12, BANK0, },
	{ "out_pause",		4, 0x15, BANK0, },
	{ "excessive",		4, 0x11, BANK0, },
	{ "collisions",		4, 0x1e, BANK0, },
	{ "deferred",		4, 0x05, BANK0, },
	{ "single",		4, 0x14, BANK0, },
	{ "multiple",		4, 0x17, BANK0, },
	{ "out_fcs_error",	4, 0x03, BANK0, },
	{ "late",		4, 0x1f, BANK0, },
	{ "hist_64bytes",	4, 0x08, BANK0, },
	{ "hist_65_127bytes",	4, 0x09, BANK0, },
	{ "hist_128_255bytes",	4, 0x0a, BANK0, },
	{ "hist_256_511bytes",	4, 0x0b, BANK0, },
	{ "hist_512_1023bytes", 4, 0x0c, BANK0, },
	{ "hist_1024_max_bytes", 4, 0x0d, BANK0, },
	{ "sw_in_discards",	4, 0x10, PORT, },
	{ "sw_in_filtered",	2, 0x12, PORT, },
	{ "sw_out_filtered",	2, 0x13, PORT, },
	{ "in_discards",	4, 0x00 | GLOBAL_STATS_OP_BANK_1, BANK1, },
	{ "in_filtered",	4, 0x01 | GLOBAL_STATS_OP_BANK_1, BANK1, },
	{ "in_accepted",	4, 0x02 | GLOBAL_STATS_OP_BANK_1, BANK1, },
	{ "in_bad_accepted",	4, 0x03 | GLOBAL_STATS_OP_BANK_1, BANK1, },
	{ "in_good_avb_class_a", 4, 0x04 | GLOBAL_STATS_OP_BANK_1, BANK1, },
	{ "in_good_avb_class_b", 4, 0x05 | GLOBAL_STATS_OP_BANK_1, BANK1, },
	{ "in_bad_avb_class_a", 4, 0x06 | GLOBAL_STATS_OP_BANK_1, BANK1, },
	{ "in_bad_avb_class_b", 4, 0x07 | GLOBAL_STATS_OP_BANK_1, BANK1, },
	{ "tcam_counter_0",	4, 0x08 | GLOBAL_STATS_OP_BANK_1, BANK1, },
	{ "tcam_counter_1",	4, 0x09 | GLOBAL_STATS_OP_BANK_1, BANK1, },
	{ "tcam_counter_2",	4, 0x0a | GLOBAL_STATS_OP_BANK_1, BANK1, },
	{ "tcam_counter_3",	4, 0x0b | GLOBAL_STATS_OP_BANK_1, BANK1, },
	{ "in_da_unknown",	4, 0x0e | GLOBAL_STATS_OP_BANK_1, BANK1, },
	{ "in_management",	4, 0x0f | GLOBAL_STATS_OP_BANK_1, BANK1, },
	{ "out_queue_0",	4, 0x10 | GLOBAL_STATS_OP_BANK_1, BANK1, },
	{ "out_queue_1",	4, 0x11 | GLOBAL_STATS_OP_BANK_1, BANK1, },
	{ "out_queue_2",	4, 0x12 | GLOBAL_STATS_OP_BANK_1, BANK1, },
	{ "out_queue_3",	4, 0x13 | GLOBAL_STATS_OP_BANK_1, BANK1, },
	{ "out_queue_4",	4, 0x14 | GLOBAL_STATS_OP_BANK_1, BANK1, },
	{ "out_queue_5",	4, 0x15 | GLOBAL_STATS_OP_BANK_1, BANK1, },
	{ "out_queue_6",	4, 0x16 | GLOBAL_STATS_OP_BANK_1, BANK1, },
	{ "out_queue_7",	4, 0x17 | GLOBAL_STATS_OP_BANK_1, BANK1, },
	{ "out_cut_through",	4, 0x18 | GLOBAL_STATS_OP_BANK_1, BANK1, },
	{ "out_octets_a",	4, 0x1a | GLOBAL_STATS_OP_BANK_1, BANK1, },
	{ "out_octets_b",	4, 0x1b | GLOBAL_STATS_OP_BANK_1, BANK1, },
	{ "out_management",	4, 0x1f | GLOBAL_STATS_OP_BANK_1, BANK1, },
};

static bool mv88e6xxx_has_stat(struct dsa_switch *ds,
			       struct mv88e6xxx_hw_stat *stat)
{
	switch (stat->type) {
	case BANK0:
		return true;
	case BANK1:
		return mv88e6xxx_6320_family(ds);
	case PORT:
		return mv88e6xxx_6352_family(ds);
	}
	return false;
}

static uint64_t _mv88e6xxx_get_ethtool_stat(struct dsa_switch *ds,
					    struct mv88e6xxx_hw_stat *s,
					    int port)
{
	u32 low;
	u32 high = 0;
	int ret;
	u64 value;

	switch (s->type) {
	case PORT:
		ret = _mv88e6xxx_reg_read(ds, REG_PORT(port), s->reg);
		if (ret < 0)
			return UINT64_MAX;

		low = ret;
		if (s->sizeof_stat == 4) {
			ret = _mv88e6xxx_reg_read(ds, REG_PORT(port),
						  s->reg + 1);
			if (ret < 0)
				return UINT64_MAX;
			high = ret;
		}
		break;
	case BANK0:
	case BANK1:
		_mv88e6xxx_stats_read(ds, s->reg, &low);
		if (s->sizeof_stat == 8)
			_mv88e6xxx_stats_read(ds, s->reg + 1, &high);
	}
	value = (((u64)high) << 16) | low;
	return value;
}

void mv88e6xxx_get_strings(struct dsa_switch *ds, int port,
			   uint8_t *data)
{
	struct mv88e6xxx_hw_stat *stat;
	int i, j;

	for (i = 0, j = 0; i < ARRAY_SIZE(mv88e6xxx_hw_stats); i++) {
		stat = &mv88e6xxx_hw_stats[i];
		if (mv88e6xxx_has_stat(ds, stat)) {
			memcpy(data + j * ETH_GSTRING_LEN, stat->string,
			       ETH_GSTRING_LEN);
			j++;
		}
	}
}

int mv88e6xxx_get_sset_count(struct dsa_switch *ds)
{
	struct mv88e6xxx_hw_stat *stat;
	int i, j;

	for (i = 0, j = 0; i < ARRAY_SIZE(mv88e6xxx_hw_stats); i++) {
		stat = &mv88e6xxx_hw_stats[i];
		if (mv88e6xxx_has_stat(ds, stat))
			j++;
	}
	return j;
}

void mv88e6xxx_get_ethtool_stats(struct dsa_switch *ds, int port,
				 uint64_t *data)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	struct mv88e6xxx_hw_stat *stat;
	int ret;
	int i, j;

	mutex_lock(&ps->smi_mutex);

	ret = _mv88e6xxx_stats_snapshot(ds, port);
	if (ret < 0) {
		mutex_unlock(&ps->smi_mutex);
		return;
	}
	for (i = 0, j = 0; i < ARRAY_SIZE(mv88e6xxx_hw_stats); i++) {
		stat = &mv88e6xxx_hw_stats[i];
		if (mv88e6xxx_has_stat(ds, stat)) {
			data[j] = _mv88e6xxx_get_ethtool_stat(ds, stat, port);
			j++;
		}
	}

	mutex_unlock(&ps->smi_mutex);
}

int mv88e6xxx_get_regs_len(struct dsa_switch *ds, int port)
{
	return 32 * sizeof(u16);
}

void mv88e6xxx_get_regs(struct dsa_switch *ds, int port,
			struct ethtool_regs *regs, void *_p)
{
	u16 *p = _p;
	int i;

	regs->version = 0;

	memset(p, 0xff, 32 * sizeof(u16));

	for (i = 0; i < 32; i++) {
		int ret;

		ret = mv88e6xxx_reg_read(ds, REG_PORT(port), i);
		if (ret >= 0)
			p[i] = ret;
	}
}

#ifdef CONFIG_NET_DSA_HWMON

int  mv88e6xxx_get_temp(struct dsa_switch *ds, int *temp)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int ret;
	int val;

	*temp = 0;

	mutex_lock(&ps->phy_mutex);

	ret = _mv88e6xxx_phy_write(ds, 0x0, 0x16, 0x6);
	if (ret < 0)
		goto error;

	/* Enable temperature sensor */
	ret = _mv88e6xxx_phy_read(ds, 0x0, 0x1a);
	if (ret < 0)
		goto error;

	ret = _mv88e6xxx_phy_write(ds, 0x0, 0x1a, ret | (1 << 5));
	if (ret < 0)
		goto error;

	/* Wait for temperature to stabilize */
	usleep_range(10000, 12000);

	val = _mv88e6xxx_phy_read(ds, 0x0, 0x1a);
	if (val < 0) {
		ret = val;
		goto error;
	}

	/* Disable temperature sensor */
	ret = _mv88e6xxx_phy_write(ds, 0x0, 0x1a, ret & ~(1 << 5));
	if (ret < 0)
		goto error;

	*temp = ((val & 0x1f) - 5) * 5;

error:
	_mv88e6xxx_phy_write(ds, 0x0, 0x16, 0x0);
	mutex_unlock(&ps->phy_mutex);
	return ret;
}
#endif /* CONFIG_NET_DSA_HWMON */

static int mv88e6xxx_wait(struct dsa_switch *ds, int reg, int offset, u16 mask)
{
	int i;

	for (i = 0; i < 16; i++) {
		int ret;

		ret = REG_READ(reg, offset);
		if (!(ret & mask))
			return 0;

		usleep_range(1000, 2000);
	}

	dev_err(ds->master_dev, "Timeout while waiting for switch\n");
	return -ETIMEDOUT;
}

int mv88e6xxx_phy_wait(struct dsa_switch *ds)
{
	return mv88e6xxx_wait(ds, REG_GLOBAL2, GLOBAL2_SMI_OP,
			      GLOBAL2_SMI_OP_BUSY);
}

int mv88e6xxx_eeprom_load_wait(struct dsa_switch *ds)
{
	return mv88e6xxx_wait(ds, REG_GLOBAL2, GLOBAL2_EEPROM_OP,
			      GLOBAL2_EEPROM_OP_LOAD);
}

int mv88e6xxx_eeprom_busy_wait(struct dsa_switch *ds)
{
	return mv88e6xxx_wait(ds, REG_GLOBAL2, GLOBAL2_EEPROM_OP,
			      GLOBAL2_EEPROM_OP_BUSY);
}

int mv88e6xxx_ptp_busy_wait(struct dsa_switch *ds)
{
	return mv88e6xxx_wait(ds, REG_GLOBAL2, GLOBAL2_PTP_AVB_OP,
			      GLOBAL2_PTP_AVB_OP_BUSY);
}

/* Read a single 16-bit word in the PTP space starting at addr */
int mv88e6xxx_read_ptp_word(struct dsa_switch *ds, int port, int block,
			    int addr)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int ret;

	mutex_lock(&ps->ptp_mutex);

	ret = mv88e6xxx_reg_write(ds, REG_GLOBAL2, GLOBAL2_PTP_AVB_OP,
				  GLOBAL2_PTP_AVB_OP_READ |
				  GLOBAL2_PTP_AVB_OP_PORT(port) |
				  GLOBAL2_PTP_AVB_OP_BLOCK(block) |
				  GLOBAL2_PTP_AVB_OP_ADDR(addr));
	if (ret < 0)
		goto error;

	ret = mv88e6xxx_ptp_busy_wait(ds);
	if (ret < 0)
		goto error;

	ret = mv88e6xxx_reg_read(ds, REG_GLOBAL2, GLOBAL2_PTP_AVB_DATA);

	dev_dbg(ds->master_dev, "<-PTP- port: 0x%.2x block: 0x%.2x addr: 0x%.2x val: 0x%.4x\n",
		port, block, addr, ret);

error:
	mutex_unlock(&ps->ptp_mutex);
	return ret;
}

/* Read four coherent u16s in the PTP space starting at addr */
int mv88e6xxx_read_ptp_block(struct dsa_switch *ds, int port, int block,
			     int addr, u16 *data)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int ret;
	int i;

	mutex_lock(&ps->ptp_mutex);

	ret = mv88e6xxx_reg_write(ds, REG_GLOBAL2, GLOBAL2_PTP_AVB_OP,
				  GLOBAL2_PTP_AVB_OP_READ_INCR |
				  GLOBAL2_PTP_AVB_OP_PORT(port) |
				  GLOBAL2_PTP_AVB_OP_BLOCK(block) |
				  GLOBAL2_PTP_AVB_OP_ADDR(addr));
	if (ret < 0)
		goto error;

	ret = mv88e6xxx_ptp_busy_wait(ds);
	if (ret < 0)
		goto error;

	for (i = 0; i < 4; i++) {
		ret = mv88e6xxx_reg_read(ds, REG_GLOBAL2, GLOBAL2_PTP_AVB_DATA);
		if (ret < 0)
			goto error;
		data[i] = (u16)ret;
	}

	mutex_unlock(&ps->ptp_mutex);
	return 0;

error:
	mutex_unlock(&ps->ptp_mutex);
	return ret;
}

int mv88e6xxx_write_ptp_word(struct dsa_switch *ds, int port, int block,
			     int addr, u16 data)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int ret;

	mutex_lock(&ps->ptp_mutex);

	ret = mv88e6xxx_reg_write(ds, REG_GLOBAL2, GLOBAL2_PTP_AVB_DATA, data);
	if (ret < 0)
		goto error;

	ret = mv88e6xxx_reg_write(ds, REG_GLOBAL2, GLOBAL2_PTP_AVB_OP,
				  GLOBAL2_PTP_AVB_OP_WRITE |
				  GLOBAL2_PTP_AVB_OP_PORT(port) |
				  GLOBAL2_PTP_AVB_OP_BLOCK(block) |
				  GLOBAL2_PTP_AVB_OP_ADDR(addr));
	if (ret < 0)
		goto error;

	ret = mv88e6xxx_ptp_busy_wait(ds);

	dev_dbg(ds->master_dev, "-PTP-> port: 0x%.2x block: 0x%.2x addr: 0x%.2x val: 0x%.4x\n",
		port, block, addr, data);
error:
	mutex_unlock(&ps->ptp_mutex);
	return ret;
}

/* Set global and per-port timestamping
 *
 * The time stamping configuration used here relies on hardware
 * capability to insert timestamps into received PTP packets. Only a
 * subset of Marvell switches support this, but this code doesn't
 * check yet.
 */
int mv88e6xxx_set_timestamp_mode(struct dsa_switch *ds, int port,
				 struct hwtstamp_config *config)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	struct mv88e6xxx_port_priv_state *pps = &ps->port_priv[port];
	bool port_ts_enable;
	bool port_check_trans_spec;
	int port_check_trans_spec_val;
	u16 port_ts_msg_types;

	u16 val;
	int ret;

	mutex_lock(&pps->ptp_mutex);

	/* Prevent the TX/RX paths from trying to interact with
	 * timestamp hardware while we reconfigure it
	 */
	spin_lock_bh(&pps->ptp_lock);
	pps->ts_enable = false;
	spin_unlock_bh(&pps->ptp_lock);

	port_ts_enable = true;
	port_check_trans_spec = false;
	port_check_trans_spec_val = PTP_PORT_CONFIG_0_TRANS_1588;
	port_ts_msg_types = 0;

	/* In default hardware configuration, 1588 SYNC frames are
	 * forwarded through the switch and thus condidates for
	 * timestamping on egress. Boundary clock implementations
	 * must configure the ATU to capture/discard these frames.
	 */

#ifdef CONFIG_NET_DSA_MV88E6XXX_ONLY_8021AS
	/* Config override to enable transport specific check for
	 * 802.1AS frames as default. The rx_filter can override
	 * this.
	 */
	port_check_trans_spec = true;
	port_check_trans_spec_val = PTP_PORT_CONFIG_0_TRANS_8021AS;
#endif

	/* reserved for future extensions */
	if (config->flags) {
		ret = -EINVAL;
		goto out;
	}

	switch (config->tx_type) {
	case HWTSTAMP_TX_OFF:
		port_ts_enable = false;
		break;
	case HWTSTAMP_TX_ON:
		break;
	default:
		ret = -ERANGE;
		goto out;
	}

	/* The switch supports timestamping both L2 and L4; one cannot be
	 * disabled independently of the other
	 */
	switch (config->rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		port_ts_enable = false;
		break;
	case HWTSTAMP_FILTER_PTP_V1_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_SYNC:
		port_ts_msg_types = (1 << 0);
		break;
	case HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
		port_ts_msg_types = (1 << 1);
		break;
	case HWTSTAMP_FILTER_PTP_V1_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
		port_ts_msg_types = 0xf;
		break;
	case HWTSTAMP_FILTER_ALL:
		config->rx_filter = HWTSTAMP_FILTER_PTP_V2_EVENT;
		port_check_trans_spec = false;
		port_ts_msg_types = 0xf;
		break;
	default:
		config->rx_filter = HWTSTAMP_FILTER_NONE;
		ret = -ERANGE;
		goto out;
	}

	/* Disable timestamping during configuration */
	ret = mv88e6xxx_write_ptp_word(ds, port,
				       GLOBAL2_PTP_AVB_OP_BLOCK_PTP,
				       PTP_PORT_CONFIG_0,
				       PTP_PORT_CONFIG_0_DISABLE_TS);
	if (ret < 0)
		goto out;

	if (!port_ts_enable) {
		/* We're done here */
		ret = 0;
		goto out;
	}

	/* Configure PTP message types to be timestamped */
	ret = mv88e6xxx_write_ptp_word(ds, GLOBAL2_PTP_AVB_OP_PORT_PTP_GLOBAL,
				       GLOBAL2_PTP_AVB_OP_BLOCK_PTP,
				       PTP_GLOBAL_MSG_TYPE, port_ts_msg_types);
	if (ret < 0)
		goto out;

	/* Capture all arrival timestamps in ARRIVAL0 */
	ret = mv88e6xxx_write_ptp_word(ds, GLOBAL2_PTP_AVB_OP_PORT_PTP_GLOBAL,
				       GLOBAL2_PTP_AVB_OP_BLOCK_PTP,
				       PTP_GLOBAL_TS_ARRIVAL_PTR, 0);
	if (ret < 0)
		goto out;

	/* Embed arrival timestamp in packet and disable
	 * interrupts. Below, overwrites are enabled for the hardware
	 * timestamp registers. The combination lets us handle
	 * back-to-back RX packets easily, but requires the TX source
	 * to send timestamp-able frames one at a time (per port)
	 */
	val = PTP_PORT_CONFIG_2_EMBED_ARRIVAL_0;
	ret = mv88e6xxx_write_ptp_word(ds, port,
				       GLOBAL2_PTP_AVB_OP_BLOCK_PTP,
				       PTP_PORT_CONFIG_2, val);
	if (ret < 0)
		goto out;

	/* Set PTP timestamping mode to timestamp at the MAC on 6341 */
	switch (ps->id) {
	case PORT_SWITCH_ID_6341:
		ret = mv88e6xxx_write_ptp_word(ds,
					       GLOBAL2_PTP_AVB_OP_PORT_PTP_GLOBAL,
					       GLOBAL2_PTP_AVB_OP_BLOCK_PTP,
					       PTP_GLOBAL_CONFIG,
					       PTP_GLOBAL_CONFIG_UPD |
					       	PTP_GLOBAL_CONFIG_MODE_IDX |
						PTP_GLOBAL_CONFIG_MODE_TS_AT_MAC);
		if (ret < 0)
			goto out;
	}

	/* Final port configuration and enable timestamping */
	val = PTP_PORT_CONFIG_0_ENABLE_OVERWRITE;
	val |= port_check_trans_spec ?
			PTP_PORT_CONFIG_0_ENABLE_TRANS_CHECK :
			PTP_PORT_CONFIG_0_DISABLE_TRANS_CHECK;
	val |= port_check_trans_spec_val;
	val |= port_ts_enable ?
			PTP_PORT_CONFIG_0_ENABLE_TS :
			PTP_PORT_CONFIG_0_DISABLE_TS;
	ret = mv88e6xxx_write_ptp_word(ds, port,
				       GLOBAL2_PTP_AVB_OP_BLOCK_PTP,
				       PTP_PORT_CONFIG_0, val);
	if (ret < 0)
		goto out;

	/* Once hardware configuration is settled, enable timestamp
	 *	checking in the RX/TX paths
	 */

	spin_lock_bh(&pps->ptp_lock);
	pps->ts_enable = port_ts_enable;
	pps->check_trans_spec = port_check_trans_spec;
	pps->check_trans_spec_val = port_check_trans_spec_val;
	pps->ts_msg_types = port_ts_msg_types;
	spin_unlock_bh(&pps->ptp_lock);

	netdev_dbg(ds->ports[port], "HWTStamp %s msg types %x transcheck %s val %d\n",
		   pps->ts_enable ? "enabled" : "disabled",
		   pps->ts_msg_types,
		   pps->check_trans_spec ? "ON" : "OFF",
		   PTP_PORT_CONFIG_0_TRANS_TO_VAL(pps->check_trans_spec_val));

out:
	mutex_unlock(&pps->ptp_mutex);

	return ret;
}

int mv88e6xxx_port_set_ts_config(struct dsa_switch *ds, int port,
				 struct ifreq *ifr)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);

	struct hwtstamp_config config;
	int err;

	if (copy_from_user(&config, ifr->ifr_data, sizeof(config)))
		return -EFAULT;

	err = mv88e6xxx_set_timestamp_mode(ds, port, &config);
	if (err)
		return err;

	/* save these settings for future reference */
	memcpy(&ps->port_priv[port].tstamp_config, &config,
	       sizeof(config));

	return copy_to_user(ifr->ifr_data, &config, sizeof(config)) ?
		-EFAULT : 0;
}

int mv88e6xxx_port_get_ts_config(struct dsa_switch *ds, int port,
				 struct ifreq *ifr)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	struct hwtstamp_config *config = &ps->port_priv[port].tstamp_config;

	return copy_to_user(ifr->ifr_data, config, sizeof(*config)) ?
		-EFAULT : 0;
}

static u8 *_get_ptp_header(struct sk_buff *skb, unsigned int type)
{
	unsigned int offset = 0;
	u8 *data = skb_mac_header(skb);

	if (type & PTP_CLASS_VLAN)
		offset += VLAN_HLEN;

	switch (type & PTP_CLASS_PMASK) {
	case PTP_CLASS_IPV4:
		offset += ETH_HLEN + IPV4_HLEN(data + offset) + UDP_HLEN;
		break;
	case PTP_CLASS_IPV6:
		offset += ETH_HLEN + IP6_HLEN + UDP_HLEN;
		break;
	case PTP_CLASS_L2:
		offset += ETH_HLEN;
		break;
	default:
		return ERR_PTR(-EINVAL);
	}

	/* Ensure that entire header is present in this packet */
	if (skb->len + ETH_HLEN < offset + 34)
		return ERR_PTR(-EINVAL);

	return data + offset;
}

static u64 mv88e6xxx_raw_to_ns(struct dsa_switch *ds, u64 raw)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	u64 ns;
	unsigned long flags;

	spin_lock_irqsave(&ps->phc_lock, flags);
	/* Raw timestamps are in units of 8-ns clock periods. */
	ns = (raw * 8) + ps->phc_offset_ns;
	spin_unlock_irqrestore(&ps->phc_lock, flags);

	return ns;
}

/* Detect and track rollovers in the PHC clock.
 *
 * Because PHC times come from both direct TAI/PHC event reads and
 * packet timestamps, we can't assume they are in order. This means a
 * new value can be from the past, potentially from a previous
 * rollover.
 *
 * This function must be called at least every rollover period (~34s
 * at 125MHz) for the rollover count to remain in sync with the switch
 * time. This is guaranteed to happen in any functioning PTP scenario
 * since a GM will send peer delays >1/s, and a non-GM must be
 * receiving sync frames >1/s
 */
static u32 mv88e6xxx_update_phc_rollover(struct dsa_switch *ds, u32 phc_counter)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	u32 rollovers_this_ts;
	unsigned long flags;

	spin_lock_irqsave(&ps->phc_lock, flags);

	rollovers_this_ts = ps->phc_rollovers;

	if (phc_counter > ps->latest_phc_counter) {
		if (phc_counter - ps->latest_phc_counter < 0x7ffffff) {
			/* phc_counter is newer than latest seen, no rollover */
			ps->latest_phc_counter = phc_counter;
		} else {
			/* phc_counter is older than latest seen, and
			 * latest rolled over
			 */
			rollovers_this_ts--;
		}
	} else {
		if (ps->latest_phc_counter - phc_counter < 0x7ffffff) {
			/* phc_counter is older than latest seen, no rollover */
		} else {
			/* phc_counter is newer than latest seen, and
			 * rolled over
			 */
			ps->latest_phc_counter = phc_counter;
			rollovers_this_ts++;
			ps->phc_rollovers = rollovers_this_ts;
		}
	}

	spin_unlock_irqrestore(&ps->phc_lock, flags);

	return rollovers_this_ts;
}

/* Augment a 32-bit PHC count with the current rollover count to get a
 * valid u64 count. Also convert to a ktime_t timestamp. This function
 * must be called within one rollover period from when the timestamp
 * was taken to work correctly.
 */
static u64 mv88e6xxx_augment_phc_count(struct dsa_switch *ds, u32 phc_count,
				       ktime_t *kt)
{
	u64 raw = (((u64)mv88e6xxx_update_phc_rollover(ds, phc_count)) << 32)
		| phc_count;

	if (kt)
		*kt = ns_to_ktime(mv88e6xxx_raw_to_ns(ds, raw));

	return raw;
}

/* Retrieve the current global time from the switch.
 */
static int mv88e6xxx_get_raw_phc_time(struct dsa_switch *ds, u64 *raw)
{
	int ret;
	u16 phc_block[4];
	u32 phc_counter;

	ret = mv88e6xxx_read_ptp_block(ds, GLOBAL2_PTP_AVB_OP_PORT_TAI_GLOBAL,
				       GLOBAL2_PTP_AVB_OP_BLOCK_PTP,
				       TAI_GLOBAL_TIME_LO, phc_block);
	if (ret < 0)
		goto out;

	phc_counter = ((u32)phc_block[1] << 16) | phc_block[0];

	*raw = mv88e6xxx_augment_phc_count(ds, phc_counter, NULL);

out:
	return ret;
}

static bool mv88e6xxx_should_timestamp(struct dsa_switch *ds, int port,
				       struct sk_buff *skb, unsigned int type)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	struct mv88e6xxx_port_priv_state *pps = &ps->port_priv[port];

	u8 *msgtype, *ptp_hdr;
	u16 msg_mask;
	bool ret;
	int trans_spec;

	if (port < 0 || port >= ps->num_ports)
		return false;

	ptp_hdr = _get_ptp_header(skb, type);
	if (IS_ERR(ptp_hdr))
		return false;

	if (unlikely(type & PTP_CLASS_V1))
		msgtype = ptp_hdr + OFF_PTP_CONTROL;
	else
		msgtype = ptp_hdr;

	msg_mask = 1 << (*msgtype & 0xf);
	trans_spec = (*msgtype >> 4);

	spin_lock_bh(&pps->ptp_lock);
	ret = !pps->check_trans_spec ||
		(PTP_PORT_CONFIG_0_TRANS_TO_VAL(pps->check_trans_spec_val) == trans_spec);
	ret &= pps->ts_enable && ((pps->ts_msg_types & msg_mask) != 0);
	spin_unlock_bh(&pps->ptp_lock);

	netdev_dbg(ds->ports[port],
		   "PTP message classification 0x%x type 0x%x should ts %d",
		   type, *msgtype, (int)ret);

	return ret;
}

/* rxtstamp will be called in interrupt context so we can't do
 * anything like read PTP registers
 */
bool mv88e6xxx_port_rxtstamp(struct dsa_switch *ds, int port,
			     struct sk_buff *skb, unsigned int type)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	struct mv88e6xxx_port_priv_state *pps = &ps->port_priv[port];
	__be32 *ptp_rx_ts;
	u32 raw_ts;
	struct skb_shared_hwtstamps *shhwtstamps;

	if (port < 0 || port >= ps->num_ports)
		return false;

	if (!mv88e6xxx_should_timestamp(ds, port, skb, type))
		return false;

	shhwtstamps = skb_hwtstamps(skb);
	memset(shhwtstamps, 0, sizeof(*shhwtstamps));
	/* RX timestamps are written into the PTP header itself */
	ptp_rx_ts = (__be32 *)(_get_ptp_header(skb, type) + 16);
	raw_ts = __be32_to_cpu(*ptp_rx_ts);
	mv88e6xxx_augment_phc_count(ds, raw_ts, &shhwtstamps->hwtstamp);

	netdev_dbg(ds->ports[pps->port_id], "rxtstamp %llx\n",
		   ktime_to_ns(shhwtstamps->hwtstamp));

	return false;
}

static void mv88e6xxx_tx_tstamp_work(struct work_struct *ugly)
{
	struct mv88e6xxx_port_priv_state *pps = container_of(
		ugly, struct mv88e6xxx_port_priv_state, tx_tstamp_work);
	struct mv88e6xxx_priv_state *ps = container_of(
		pps, struct mv88e6xxx_priv_state, port_priv[pps->port_id]);
	struct dsa_switch *ds = ((struct dsa_switch *)ps) - 1;

	struct sk_buff *tmp_skb;
	u16 tmp_seq_id;
	unsigned long tmp_tstamp_start;

	u16 departure_block[4];
	int ret;

	spin_lock_bh(&pps->tx_tstamp_lock);

	tmp_skb = pps->tx_skb;
	tmp_seq_id = pps->tx_seq_id;
	tmp_tstamp_start = pps->tx_tstamp_start;

	spin_unlock_bh(&pps->tx_tstamp_lock);

	if (!tmp_skb)
		return;

	ret = mv88e6xxx_read_ptp_block(ds, pps->port_id,
				       GLOBAL2_PTP_AVB_OP_BLOCK_PTP,
				       PTP_PORT_DEPARTURE_STATUS,
				       departure_block);

	if (ret < 0)
		goto free_and_clear_skb;

	if (departure_block[0] & PTP_PORT_DEPARTURE_STATUS_VALID) {
		u16 status;
		struct skb_shared_hwtstamps shhwtstamps;
		u32 tx_low_word;

		/* We have the timestamp; go ahead and clear valid now */
		mv88e6xxx_write_ptp_word(ds, pps->port_id,
					 GLOBAL2_PTP_AVB_OP_BLOCK_PTP,
					 PTP_PORT_DEPARTURE_STATUS, 0);

		status = departure_block[0] &
				PTP_PORT_DEPARTURE_STATUS_STATUS_MASK;
		if (status != PTP_PORT_DEPARTURE_STATUS_STATUS_NORMAL) {
			netdev_warn(ds->ports[pps->port_id], "tx timestamp overrun\n");
			goto free_and_clear_skb;
		}

		if (departure_block[3] != tmp_seq_id) {
			netdev_warn(ds->ports[pps->port_id], "unexpected sequence id\n");
			goto free_and_clear_skb;
		}

		memset(&shhwtstamps, 0, sizeof(shhwtstamps));
		tx_low_word = ((u32)departure_block[2] << 16) |
			      departure_block[1];
		mv88e6xxx_augment_phc_count(ds, tx_low_word,
					    &shhwtstamps.hwtstamp);

		netdev_dbg(ds->ports[pps->port_id],
			   "txtstamp %llx status 0x%04x skb ID 0x%04x hw ID 0x%04x\n",
			   ktime_to_ns(shhwtstamps.hwtstamp),
			   departure_block[0], tmp_seq_id,
			   departure_block[3]);

		/* skb_complete_tx_timestamp() will free up the client to make
		 * another timestamp-able transmit. We have to be ready for it
		 * -- by clearing the pps->tx_skb "flag" -- beforehand.
		 */

		spin_lock_bh(&pps->tx_tstamp_lock);

		pps->tx_skb = NULL;

		spin_unlock_bh(&pps->tx_tstamp_lock);

		skb_complete_tx_timestamp(tmp_skb, &shhwtstamps);

	} else {
		if (time_is_before_jiffies(
			    tmp_tstamp_start + TX_TSTAMP_TIMEOUT)) {
			netdev_warn(ds->ports[pps->port_id],
				    "clearing tx timestamp hang\n");
			goto free_and_clear_skb;
		}

		/* The timestamp should be available quickly, while getting it
		 * is high priority and time bounded to only 10ms. A poll is
		 * warranted and this is the nicest way to realize it in a work
		 * item.
		 */

		queue_work(system_highpri_wq, &pps->tx_tstamp_work);
	}

	return;

free_and_clear_skb:
	spin_lock_bh(&pps->tx_tstamp_lock);
	pps->tx_skb = NULL;
	spin_unlock_bh(&pps->tx_tstamp_lock);

	dev_kfree_skb_any(tmp_skb);
}

void mv88e6xxx_port_txtstamp(struct dsa_switch *ds, int port,
			     struct sk_buff *clone, unsigned int type)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);

	if (port < 0 || port >= ps->num_ports)
		goto out;

	if (unlikely(skb_shinfo(clone)->tx_flags & SKBTX_HW_TSTAMP) &&
	    mv88e6xxx_should_timestamp(ds, port, clone, type)) {
		struct mv88e6xxx_port_priv_state *pps = &ps->port_priv[port];
		bool collision = false;

		__be16 *seq_ptr = (__be16 *)(_get_ptp_header(clone, type) +
					     OFF_PTP_SEQUENCE_ID);

		spin_lock(&pps->tx_tstamp_lock);

		if (pps->tx_skb) {
			collision = true;
		} else {
			pps->tx_skb = clone;
			pps->tx_tstamp_start = jiffies;
			pps->tx_seq_id = be16_to_cpup(seq_ptr);
		}

		spin_unlock(&pps->tx_tstamp_lock);

		if (collision) {
			netdev_dbg(ds->ports[port], "Tx timestamp already in progress, discarding");
			kfree_skb(clone);
		} else {
			/* Fetching the timestamp is high-priority work because
			 * 802.1AS bounds the time foir a response.
			 */

			/* No need to check result of queue_work(). pps->tx_skb
			 * check ensures work item is not pending (it may be
			 * waiting to exit).
			 */

			queue_work(system_highpri_wq, &pps->tx_tstamp_work);
		}

		return;
	}

out:
	/* We don't need it after all */
	kfree_skb(clone);
}

/* Must be called with SMI lock held */
static int _mv88e6xxx_wait(struct dsa_switch *ds, int reg, int offset, u16 mask)
{
	int i;

	for (i = 0; i < 16; i++) {
		int ret;

		ret = _mv88e6xxx_reg_read(ds, reg, offset);
		if (ret < 0)
			return ret;
		if (!(ret & mask))
			return 0;

		usleep_range(1000, 2000);
	}

	dev_err(ds->master_dev, "Timeout while waiting for switch\n");
	return -ETIMEDOUT;
}

/* Must be called with SMI lock held */
static int _mv88e6xxx_atu_wait(struct dsa_switch *ds)
{
	return _mv88e6xxx_wait(ds, REG_GLOBAL, GLOBAL_ATU_OP,
			       GLOBAL_ATU_OP_BUSY);
}

/* Must be called with phy mutex held */
static int _mv88e6xxx_phy_read_indirect(struct dsa_switch *ds, int addr,
					int regnum)
{
	int ret;

	REG_WRITE(REG_GLOBAL2, GLOBAL2_SMI_OP,
		  GLOBAL2_SMI_OP_22_READ | (addr << 5) | regnum);

	ret = mv88e6xxx_phy_wait(ds);
	if (ret < 0)
		return ret;

	return REG_READ(REG_GLOBAL2, GLOBAL2_SMI_DATA);
}

/* Must be called with phy mutex held */
static int _mv88e6xxx_phy_write_indirect(struct dsa_switch *ds, int addr,
					 int regnum, u16 val)
{
	REG_WRITE(REG_GLOBAL2, GLOBAL2_SMI_DATA, val);
	REG_WRITE(REG_GLOBAL2, GLOBAL2_SMI_OP,
		  GLOBAL2_SMI_OP_22_WRITE | (addr << 5) | regnum);

	return mv88e6xxx_phy_wait(ds);
}

int mv88e6xxx_get_eee(struct dsa_switch *ds, int port, struct ethtool_eee *e)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int reg;

	mutex_lock(&ps->phy_mutex);

	reg = _mv88e6xxx_phy_read_indirect(ds, port, 16);
	if (reg < 0)
		goto out;

	e->eee_enabled = !!(reg & 0x0200);
	e->tx_lpi_enabled = !!(reg & 0x0100);

	reg = mv88e6xxx_reg_read(ds, REG_PORT(port), PORT_STATUS);
	if (reg < 0)
		goto out;

	e->eee_active = !!(reg & PORT_STATUS_EEE);
	reg = 0;

out:
	mutex_unlock(&ps->phy_mutex);
	return reg;
}

int mv88e6xxx_set_eee(struct dsa_switch *ds, int port,
		      struct phy_device *phydev, struct ethtool_eee *e)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int reg;
	int ret;

	mutex_lock(&ps->phy_mutex);

	ret = _mv88e6xxx_phy_read_indirect(ds, port, 16);
	if (ret < 0)
		goto out;

	reg = ret & ~0x0300;
	if (e->eee_enabled)
		reg |= 0x0200;
	if (e->tx_lpi_enabled)
		reg |= 0x0100;

	ret = _mv88e6xxx_phy_write_indirect(ds, port, 16, reg);
out:
	mutex_unlock(&ps->phy_mutex);

	return ret;
}

static int _mv88e6xxx_atu_cmd(struct dsa_switch *ds, int fid, u16 cmd)
{
	int ret;

	ret = _mv88e6xxx_reg_write(ds, REG_GLOBAL, 0x01, fid);
	if (ret < 0)
		return ret;

	ret = _mv88e6xxx_reg_write(ds, REG_GLOBAL, GLOBAL_ATU_OP, cmd);
	if (ret < 0)
		return ret;

	return _mv88e6xxx_atu_wait(ds);
}

static int _mv88e6xxx_flush_fid(struct dsa_switch *ds, int fid)
{
	int ret;

	ret = _mv88e6xxx_atu_wait(ds);
	if (ret < 0)
		return ret;

	return _mv88e6xxx_atu_cmd(ds, fid, GLOBAL_ATU_OP_FLUSH_NON_STATIC_DB);
}

static int mv88e6xxx_set_port_state(struct dsa_switch *ds, int port, u8 state)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int reg, ret = 0;
	u8 oldstate;

	mutex_lock(&ps->smi_mutex);

	reg = _mv88e6xxx_reg_read(ds, REG_PORT(port), PORT_CONTROL);
	if (reg < 0) {
		ret = reg;
		goto abort;
	}

	oldstate = reg & PORT_CONTROL_STATE_MASK;
	if (oldstate != state) {
		/* Flush forwarding database if we're moving a port
		 * from Learning or Forwarding state to Disabled or
		 * Blocking or Listening state.
		 */
		if (oldstate >= PORT_CONTROL_STATE_LEARNING &&
		    state <= PORT_CONTROL_STATE_BLOCKING) {
			ret = _mv88e6xxx_flush_fid(ds, ps->port_priv[port].fid);
			if (ret)
				goto abort;
		}
		reg = (reg & ~PORT_CONTROL_STATE_MASK) | state;
		ret = _mv88e6xxx_reg_write(ds, REG_PORT(port), PORT_CONTROL,
					   reg);
	}

abort:
	mutex_unlock(&ps->smi_mutex);
	return ret;
}

/* Must be called with smi lock held */
static int _mv88e6xxx_update_port_config(struct dsa_switch *ds, int port)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	u8 fid = ps->port_priv[port].fid;
	u16 reg = fid << 12;

	if (dsa_is_cpu_port(ds, port))
		reg |= ds->phys_port_mask;
	else
		reg |= (ps->bridge_mask[fid] |
		       (1 << dsa_upstream_port(ds))) & ~(1 << port);

	return _mv88e6xxx_reg_write(ds, REG_PORT(port), PORT_BASE_VLAN, reg);
}

/* Must be called with smi lock held */
static int _mv88e6xxx_update_bridge_config(struct dsa_switch *ds, int fid)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int port;
	u32 mask;
	int ret;

	mask = ds->phys_port_mask;
	while (mask) {
		port = __ffs(mask);
		mask &= ~(1 << port);
		if (ps->port_priv[port].fid != fid)
			continue;

		ret = _mv88e6xxx_update_port_config(ds, port);
		if (ret)
			return ret;
	}

	return _mv88e6xxx_flush_fid(ds, fid);
}

/* Bridge handling functions */

int mv88e6xxx_join_bridge(struct dsa_switch *ds, int port, u32 br_port_mask)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int ret = 0;
	u32 nmask;
	int fid;

	/* If the bridge group is not empty, join that group.
	 * Otherwise create a new group.
	 */
	fid = ps->port_priv[port].fid;
	nmask = br_port_mask & ~(1 << port);
	if (nmask)
		fid = ps->port_priv[__ffs(nmask)].fid;

	nmask = ps->bridge_mask[fid] | (1 << port);
	if (nmask != br_port_mask) {
		netdev_err(ds->ports[port],
			   "join: Bridge port mask mismatch fid=%d mask=0x%x expected 0x%x\n",
			   fid, br_port_mask, nmask);
		return -EINVAL;
	}

	mutex_lock(&ps->smi_mutex);

	ps->bridge_mask[fid] = br_port_mask;

	if (fid != ps->port_priv[port].fid) {
		ps->fid_mask |= 1 << ps->port_priv[port].fid;
		ps->port_priv[port].fid = fid;
		ret = _mv88e6xxx_update_bridge_config(ds, fid);
	}

	mutex_unlock(&ps->smi_mutex);

	return ret;
}

int mv88e6xxx_leave_bridge(struct dsa_switch *ds, int port, u32 br_port_mask)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	u8 fid, newfid;
	int ret;

	fid = ps->port_priv[port].fid;

	if (ps->bridge_mask[fid] != br_port_mask) {
		netdev_err(ds->ports[port],
			   "leave: Bridge port mask mismatch fid=%d mask=0x%x expected 0x%x\n",
			   fid, br_port_mask, ps->bridge_mask[fid]);
		return -EINVAL;
	}

	/* If the port was the last port of a bridge, we are done.
	 * Otherwise assign a new fid to the port, and fix up
	 * the bridge configuration.
	 */
	if (br_port_mask == (1 << port))
		return 0;

	mutex_lock(&ps->smi_mutex);

	newfid = __ffs(ps->fid_mask);
	ps->port_priv[port].fid = newfid;
	ps->fid_mask &= (1 << newfid);
	ps->bridge_mask[fid] &= ~(1 << port);
	ps->bridge_mask[newfid] = 1 << port;

	ret = _mv88e6xxx_update_bridge_config(ds, fid);
	if (!ret)
		ret = _mv88e6xxx_update_bridge_config(ds, newfid);

	mutex_unlock(&ps->smi_mutex);

	return ret;
}

int mv88e6xxx_port_stp_update(struct dsa_switch *ds, int port, u8 state)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int stp_state;

	switch (state) {
	case BR_STATE_DISABLED:
		stp_state = PORT_CONTROL_STATE_DISABLED;
		break;
	case BR_STATE_BLOCKING:
	case BR_STATE_LISTENING:
		stp_state = PORT_CONTROL_STATE_BLOCKING;
		break;
	case BR_STATE_LEARNING:
		stp_state = PORT_CONTROL_STATE_LEARNING;
		break;
	case BR_STATE_FORWARDING:
	default:
		stp_state = PORT_CONTROL_STATE_FORWARDING;
		break;
	}

	netdev_dbg(ds->ports[port], "port state %d [%d]\n", state, stp_state);

	/* mv88e6xxx_port_stp_update may be called with softirqs disabled,
	 * so we can not update the port state directly but need to schedule it.
	 */
	ps->port_priv[port].stp_state = stp_state;
	set_bit(port, &ps->port_state_update_mask);
	schedule_work(&ps->bridge_work);

	return 0;
}

static int __mv88e6xxx_write_addr(struct dsa_switch *ds,
				  const unsigned char *addr)
{
	int i, ret;

	for (i = 0; i < 3; i++) {
		ret = _mv88e6xxx_reg_write(
			ds, REG_GLOBAL, GLOBAL_ATU_MAC_01 + i,
			(addr[i * 2] << 8) | addr[i * 2 + 1]);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int __mv88e6xxx_read_addr(struct dsa_switch *ds, unsigned char *addr)
{
	int i, ret;

	for (i = 0; i < 3; i++) {
		ret = _mv88e6xxx_reg_read(ds, REG_GLOBAL,
					  GLOBAL_ATU_MAC_01 + i);
		if (ret < 0)
			return ret;
		addr[i * 2] = ret >> 8;
		addr[i * 2 + 1] = ret & 0xff;
	}

	return 0;
}

static int __mv88e6xxx_port_fdb_cmd(struct dsa_switch *ds, int port,
				    const unsigned char *addr, int state)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	u8 fid = ps->port_priv[port].fid;
	int ret;

	ret = _mv88e6xxx_atu_wait(ds);
	if (ret < 0)
		return ret;

	ret = __mv88e6xxx_write_addr(ds, addr);
	if (ret < 0)
		return ret;

	ret = _mv88e6xxx_reg_write(ds, REG_GLOBAL, GLOBAL_ATU_DATA,
				   (0x10 << port) | state);
	if (ret)
		return ret;

	ret = _mv88e6xxx_atu_cmd(ds, fid, GLOBAL_ATU_OP_LOAD_DB);

	return ret;
}

int mv88e6xxx_port_fdb_add(struct dsa_switch *ds, int port,
			   const unsigned char *addr, u16 vid)
{
	int state = is_multicast_ether_addr(addr) ?
		GLOBAL_ATU_DATA_STATE_MC_STATIC :
		GLOBAL_ATU_DATA_STATE_UC_STATIC;
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int ret;

	mutex_lock(&ps->smi_mutex);
	ret = __mv88e6xxx_port_fdb_cmd(ds, port, addr, state);
	mutex_unlock(&ps->smi_mutex);

	return ret;
}

int mv88e6xxx_port_fdb_del(struct dsa_switch *ds, int port,
			   const unsigned char *addr, u16 vid)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int ret;

	mutex_lock(&ps->smi_mutex);
	ret = __mv88e6xxx_port_fdb_cmd(ds, port, addr,
				       GLOBAL_ATU_DATA_STATE_UNUSED);
	mutex_unlock(&ps->smi_mutex);

	return ret;
}

static int __mv88e6xxx_port_getnext(struct dsa_switch *ds, int port,
				    unsigned char *addr, bool *is_static)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	u8 fid = ps->port_priv[port].fid;
	int ret, state;

	ret = _mv88e6xxx_atu_wait(ds);
	if (ret < 0)
		return ret;

	ret = __mv88e6xxx_write_addr(ds, addr);
	if (ret < 0)
		return ret;

	do {
		ret = _mv88e6xxx_atu_cmd(ds, fid,  GLOBAL_ATU_OP_GET_NEXT_DB);
		if (ret < 0)
			return ret;

		ret = _mv88e6xxx_reg_read(ds, REG_GLOBAL, GLOBAL_ATU_DATA);
		if (ret < 0)
			return ret;
		state = ret & GLOBAL_ATU_DATA_STATE_MASK;
		if (state == GLOBAL_ATU_DATA_STATE_UNUSED)
			return -ENOENT;
	} while (!(((ret >> 4) & 0xff) & (1 << port)));

	ret = __mv88e6xxx_read_addr(ds, addr);
	if (ret < 0)
		return ret;

	*is_static = state == (is_multicast_ether_addr(addr) ?
			       GLOBAL_ATU_DATA_STATE_MC_STATIC :
			       GLOBAL_ATU_DATA_STATE_UC_STATIC);

	return 0;
}

/* get next entry for port */
int mv88e6xxx_port_fdb_getnext(struct dsa_switch *ds, int port,
			       unsigned char *addr, bool *is_static)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int ret;

	mutex_lock(&ps->smi_mutex);
	ret = __mv88e6xxx_port_getnext(ds, port, addr, is_static);
	mutex_unlock(&ps->smi_mutex);

	return ret;
}

static void mv88e6xxx_bridge_work(struct work_struct *work)
{
	struct mv88e6xxx_priv_state *ps;
	struct dsa_switch *ds;
	int port;

	ps = container_of(work, struct mv88e6xxx_priv_state, bridge_work);
	ds = ((struct dsa_switch *)ps) - 1;

	while (ps->port_state_update_mask) {
		port = __ffs(ps->port_state_update_mask);
		clear_bit(port, &ps->port_state_update_mask);
		mv88e6xxx_set_port_state(ds, port,
					 ps->port_priv[port].stp_state);
	}
}

int mv88e6xxx_setup_port_common(struct dsa_switch *ds, int port)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	struct mv88e6xxx_port_priv_state *pps = &ps->port_priv[port];
	int ret, fid;

	pps->port_id = port;

	pps->ts_enable = false;

	mutex_init(&pps->ptp_mutex);
	spin_lock_init(&pps->ptp_lock);
	spin_lock_init(&pps->tx_tstamp_lock);

	INIT_WORK(&pps->tx_tstamp_work, mv88e6xxx_tx_tstamp_work);

	mutex_lock(&ps->smi_mutex);

	/* Port Control 1: disable trunking, disable sending
	 * learning messages to this port.
	 */
	ret = _mv88e6xxx_reg_write(ds, REG_PORT(port), PORT_CONTROL_1, 0x0000);
	if (ret)
		goto abort;

	/* Port based VLAN map: give each port its own address
	 * database, allow the CPU port to talk to each of the 'real'
	 * ports, and allow each of the 'real' ports to only talk to
	 * the upstream port.
	 */
	fid = __ffs(ps->fid_mask);
	pps->fid = fid;
	ps->fid_mask &= ~(1 << fid);

	if (!dsa_is_cpu_port(ds, port))
		ps->bridge_mask[fid] = 1 << port;

	ret = _mv88e6xxx_update_port_config(ds, port);
	if (ret)
		goto abort;

	/* Default VLAN ID and priority: don't set a default VLAN
	 * ID, and set the default packet priority to zero.
	 */
	ret = _mv88e6xxx_reg_write(ds, REG_PORT(port), PORT_DEFAULT_VLAN,
				   0x0000);
abort:
	mutex_unlock(&ps->smi_mutex);
	return ret;
}

static int mv88e6xxx_phc_adjfreq(struct ptp_clock_info *ptp, s32 ppb)
{
	struct mv88e6xxx_priv_state *ps =
		container_of(ptp, struct mv88e6xxx_priv_state, ptp_clock_caps);
	struct dsa_switch *ds = ((struct dsa_switch *)ps) - 1;

	if (ppb == 0)
		return 0;

	/* Only support steering of the primary XTAL_IN clock for now */
	if (ps->xtal_in) {
		u32 old_freq, new_freq;
		u64 adjust;

		old_freq = clk_get_rate(ps->xtal_in);
		adjust = (u64)old_freq * abs(ppb);
		do_div(adjust, 1000000000U);
		if (ppb > 0)
			new_freq = old_freq + (u32)adjust;
		else
			new_freq = old_freq - (u32)adjust;

		dev_dbg(ds->master_dev, "adjusted clock from %d by %d ppb to %d",
			old_freq, ppb, new_freq);

		return clk_set_rate(ps->xtal_in, new_freq);
	}

	return -EOPNOTSUPP;
}

static int mv88e6xxx_phc_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct mv88e6xxx_priv_state *ps =
		container_of(ptp, struct mv88e6xxx_priv_state, ptp_clock_caps);
	unsigned long flags;

	spin_lock_irqsave(&ps->phc_lock, flags);
	ps->phc_offset_ns += delta;
	spin_unlock_irqrestore(&ps->phc_lock, flags);

	return 0;
}

static int mv88e6xxx_phc_gettime(struct ptp_clock_info *ptp,
				 struct timespec64 *ts)
{
	struct mv88e6xxx_priv_state *ps =
		container_of(ptp, struct mv88e6xxx_priv_state, ptp_clock_caps);
	struct dsa_switch *ds = ((struct dsa_switch *)ps) - 1;

	u64 raw_count;
	int ret;

	ret = mv88e6xxx_get_raw_phc_time(ds, &raw_count);
	if (ret < 0)
		return ret;

	*ts = ns_to_timespec64(mv88e6xxx_raw_to_ns(ds, raw_count));

	return 0;
}

static int mv88e6xxx_phc_settime(struct ptp_clock_info *ptp,
				 const struct timespec64 *ts)
{
	struct mv88e6xxx_priv_state *ps =
		container_of(ptp, struct mv88e6xxx_priv_state, ptp_clock_caps);
	struct dsa_switch *ds = ((struct dsa_switch *)ps) - 1;

	u64 raw_count, new_now;
	unsigned long flags;
	int ret;

	ret = mv88e6xxx_get_raw_phc_time(ds, &raw_count);
	if (ret < 0)
		return ret;

	new_now = timespec64_to_ns(ts);

	spin_lock_irqsave(&ps->phc_lock, flags);
	/* Raw timestamps are in units of 8-ns clock periods. */
	ps->phc_offset_ns = new_now - (raw_count * 8);
	spin_unlock_irqrestore(&ps->phc_lock, flags);

	return 0;
}

static int mv88e6xxx_misc_reg_read(struct dsa_switch *ds, int reg)
{
	int ret;

	ret = mv88e6xxx_reg_write(ds, REG_GLOBAL2, GLOBAL2_SCRATCH_MISC,
				  reg << GLOBAL2_SCRATCH_MISC_REG_OFFSET);
	if (ret < 0)
		return ret;

	ret = mv88e6xxx_reg_read(ds, REG_GLOBAL2, GLOBAL2_SCRATCH_MISC);
	if (ret < 0)
		return ret;

	return ret & GLOBAL2_SCRATCH_MISC_DATA_MASK;
}

static int mv88e6xxx_misc_reg_write(struct dsa_switch *ds, int reg, u8 data)
{
	int ret;

	ret = mv88e6xxx_reg_write(ds, REG_GLOBAL2, GLOBAL2_SCRATCH_MISC,
				  GLOBAL2_SCRATCH_MISC_UPDATE |
				  reg << GLOBAL2_SCRATCH_MISC_REG_OFFSET |
				  data);

	return ret;
}

/* Configures the specified pin for the specified function. This function does
 * not unset other pins configured for the same function. If multiple pins
 * are configured for the same function, the lower-index pin gets that function
 * and the higher-index pin goes back to being GPIO.
 */
static int mv88e6xxx_config_gpio(struct dsa_switch *ds, int pin, int func,
				 int dir)
{
	int reg_data;
	int ret;

	dev_dbg(ds->master_dev, "config pin %d func %d dir %d\n",
		pin, func, dir);

	/* Set function first */
	ret = mv88e6xxx_misc_reg_read(ds, MISC_REG_GPIO_MODE(pin));
	if (ret < 0)
		return ret;

	/* Zero bits in the field for this GPIO and OR in the new config */
	reg_data = ret & ~MISC_REG_GPIO_MODE_MASK(pin);
	reg_data |= func << MISC_REG_GPIO_MODE_OFFSET(pin);

	ret = mv88e6xxx_misc_reg_write(ds, MISC_REG_GPIO_MODE(pin), reg_data);
	if (ret < 0)
		return ret;

	/* Set direction */
	ret = mv88e6xxx_misc_reg_read(ds, MISC_REG_GPIO_DIR(pin));
	if (ret < 0)
		return ret;

	/* Zero bits in the field for this GPIO and OR in the new config */
	reg_data = ret & ~MISC_REG_GPIO_DIR_MASK(pin);
	reg_data |= dir << MISC_REG_GPIO_DIR_OFFSET(pin);

	ret = mv88e6xxx_misc_reg_write(ds, MISC_REG_GPIO_DIR(pin), reg_data);

	return ret;
}

static int mv88e6xxx_disable_trig(struct dsa_switch *ds)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int ret;

	mutex_lock(&ps->phc_mutex);

	ps->trig_config = TAI_GLOBAL_CONFIG_TRIG_DISABLE;

	ret = mv88e6xxx_write_ptp_word(ds,
				       GLOBAL2_PTP_AVB_OP_PORT_TAI_GLOBAL,
				       GLOBAL2_PTP_AVB_OP_BLOCK_PTP,
				       TAI_GLOBAL_CONFIG,
				       ps->evcap_config | ps->trig_config);

	mutex_unlock(&ps->phc_mutex);

	return ret;
}

static int mv88e6xxx_config_periodic_trig(struct dsa_switch *ds, u32 ns,
					  u16 picos)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int ret;

	if (picos >= 1000)
		return -ERANGE;

	/* TRIG generation is in units of 8 ns clock periods. Convert ns and
	 * ps into 8 ns clock periods and up to 8000 additional ps.
	 */
	picos += (ns & 0x7) * 1000;
	ns = ns >> 3;

	mutex_lock(&ps->phc_mutex);

	ret = mv88e6xxx_write_ptp_word(ds, GLOBAL2_PTP_AVB_OP_PORT_TAI_GLOBAL,
				       GLOBAL2_PTP_AVB_OP_BLOCK_PTP,
				       TAI_GLOBAL_TRIG_GEN_AMOUNT_LO,
				       ns & 0xffff);
	if (ret < 0)
		goto out;

	ret = mv88e6xxx_write_ptp_word(ds, GLOBAL2_PTP_AVB_OP_PORT_TAI_GLOBAL,
				       GLOBAL2_PTP_AVB_OP_BLOCK_PTP,
				       TAI_GLOBAL_TRIG_GEN_AMOUNT_HI,
				       ns >> 16);
	if (ret < 0)
		goto out;

	ret = mv88e6xxx_write_ptp_word(ds, GLOBAL2_PTP_AVB_OP_PORT_TAI_GLOBAL,
				       GLOBAL2_PTP_AVB_OP_BLOCK_PTP,
				       TAI_GLOBAL_TRIG_CLOCK_COMP,
				       picos);
	if (ret < 0)
		goto out;

	ps->trig_config = TAI_GLOBAL_CONFIG_TRIG_ACTIVE_HI |
		TAI_GLOBAL_CONFIG_TRIG_MODE_CLOCK |
		TAI_GLOBAL_CONFIG_TRIG_ENABLE;

	ret = mv88e6xxx_write_ptp_word(ds, GLOBAL2_PTP_AVB_OP_PORT_TAI_GLOBAL,
				       GLOBAL2_PTP_AVB_OP_BLOCK_PTP,
				       TAI_GLOBAL_CONFIG,
				       ps->evcap_config | ps->trig_config);

out:
	mutex_unlock(&ps->phc_mutex);

	return ret;
}

/* Configures the TAI event capture circuitry. Pass in
 * TAI_GLOBAL_EVENT_STATUS_CAPTURE_TRIG for internal trigger
 * TAI_GLOBAL_EVENT_STATUS_CAPTURE_EVREQ for external trigger
 * This will also reset the capture sequence counter.
 */
static int mv88e6xxx_config_eventcap(struct dsa_switch *ds, u16 type,
				     int rising)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int ret;

	mutex_lock(&ps->phc_mutex);

	ps->evcap_config = TAI_GLOBAL_CONFIG_DISABLE_OVERWRITE |
		TAI_GLOBAL_CONFIG_ENABLE_CAPTURE_COUNTER |
		rising ? TAI_GLOBAL_CONFIG_EVREQ_RISING :
			 TAI_GLOBAL_CONFIG_EVREQ_FALLING;

	ret = mv88e6xxx_write_ptp_word(ds,
				       GLOBAL2_PTP_AVB_OP_PORT_TAI_GLOBAL,
				       GLOBAL2_PTP_AVB_OP_BLOCK_PTP,
				       TAI_GLOBAL_CONFIG,
				       ps->evcap_config | ps->trig_config);
	if (ret < 0)
		goto out;

	ret = mv88e6xxx_write_ptp_word(ds,
				       GLOBAL2_PTP_AVB_OP_PORT_TAI_GLOBAL,
				       GLOBAL2_PTP_AVB_OP_BLOCK_PTP,
				       TAI_GLOBAL_EVENT_STATUS, type);

out:
	mutex_unlock(&ps->phc_mutex);

	return ret;
}

static void mv88e6xxx_tai_work(struct work_struct *ugly)
{
	struct delayed_work *dw = to_delayed_work(ugly);
	struct mv88e6xxx_priv_state *ps =
		container_of(dw, struct mv88e6xxx_priv_state, tai_work);
	struct dsa_switch *ds = ((struct dsa_switch *)ps) - 1;
	u16 event_block[4];
	int ret;

	ret = mv88e6xxx_read_ptp_block(ds,
				       GLOBAL2_PTP_AVB_OP_PORT_TAI_GLOBAL,
				       GLOBAL2_PTP_AVB_OP_BLOCK_PTP,
				       TAI_GLOBAL_EVENT_STATUS, event_block);
	if (ret < 0)
		return;

	if (event_block[0] & TAI_GLOBAL_EVENT_STATUS_ERROR)
		dev_warn(ds->master_dev, "missed event capture\n");

	if (event_block[0] & TAI_GLOBAL_EVENT_STATUS_VALID) {
		struct ptp_clock_event ev;
		ktime_t ev_time;
		u32 raw_ts = ((u32)event_block[2] << 16) | event_block[1];

		mv88e6xxx_augment_phc_count(ds, raw_ts, &ev_time);

		/* Clear the valid bit so the next timestamp can come in */
		event_block[0] &= ~TAI_GLOBAL_EVENT_STATUS_VALID;
		ret = mv88e6xxx_write_ptp_word(ds,
					       GLOBAL2_PTP_AVB_OP_PORT_TAI_GLOBAL,
					       GLOBAL2_PTP_AVB_OP_BLOCK_PTP,
					       TAI_GLOBAL_EVENT_STATUS,
					       event_block[0]);

		if (event_block[0] & TAI_GLOBAL_EVENT_STATUS_CAPTURE_TRIG) {
			/* TAI is configured to timestamp internal events.
			 * This will be a PPS event.
			 */
			ev.type = PTP_CLOCK_PPS;
		} else {
			/* Otherwise this is an external timestamp */
			ev.type = PTP_CLOCK_EXTTS;
		}
		/* We only have the one TAI timestamping channel */
		ev.index = 0;
		ev.timestamp = ktime_to_ns(ev_time);

		ptp_clock_event(ps->ptp_clock, &ev);
	}

	schedule_delayed_work(&ps->tai_work, TAI_WORK_INTERVAL);
}

/* ptp_find_pin locks the pincfg mutex, so we cannot use it to locate the
 * appropriate pin from inside phc_enable below
 */
static int mv88e6xxx_find_pin(struct ptp_clock_info *ptp,
			      enum ptp_pin_function func, unsigned int chan)
{
	int i;

	for (i = 0; i < ptp->n_pins; i++) {
		if (ptp->pin_config[i].func == func &&
		    ptp->pin_config[i].chan == chan) {
			return i;
		}
	}

	return -1;
}

static int mv88e6xxx_phc_enable(struct ptp_clock_info *ptp,
				struct ptp_clock_request *rq, int on)
{
	struct mv88e6xxx_priv_state *ps =
		container_of(ptp, struct mv88e6xxx_priv_state, ptp_clock_caps);
	struct dsa_switch *ds = ((struct dsa_switch *)ps) - 1;
	struct timespec ts;
	u64 ns;
	int ret;
	int pin;

	switch (rq->type) {
	case PTP_CLK_REQ_EXTTS:
		pin = mv88e6xxx_find_pin(ptp, PTP_PF_EXTTS, rq->extts.index);
		dev_dbg(ds->master_dev, "EXTTS req on=%d index %d pin %d\n",
			on, rq->extts.index, pin);

		if (pin < 0)
			return -EINVAL;

		if (on) {
			ret = mv88e6xxx_config_gpio(ds, pin,
						    MISC_REG_GPIO_MODE_EVREQ,
						    MISC_REG_GPIO_DIR_IN);
			schedule_delayed_work(&ps->tai_work, TAI_WORK_INTERVAL);
			mv88e6xxx_config_eventcap(ds,
						  TAI_GLOBAL_EVENT_STATUS_CAPTURE_EVREQ,
						  rq->extts.flags & PTP_RISING_EDGE);
		} else {
			ret = mv88e6xxx_config_gpio(ds, pin,
						    MISC_REG_GPIO_MODE_GPIO,
						    MISC_REG_GPIO_DIR_IN);
			cancel_delayed_work_sync(&ps->tai_work);
		}
		return ret;

	case PTP_CLK_REQ_PEROUT:
		pin = mv88e6xxx_find_pin(ptp, PTP_PF_PEROUT, rq->extts.index);
		dev_dbg(ds->master_dev, "PEROUT req on=%d index %d pin %d\n",
			on, rq->perout.index, pin);

		if (pin < 0)
			return -EINVAL;

		ts.tv_sec = rq->perout.period.sec;
		ts.tv_nsec = rq->perout.period.nsec;
		ns = timespec_to_ns(&ts);

		if (ns > 0xffffffffULL)
			return -ERANGE;

		ret = mv88e6xxx_config_periodic_trig(ds, (u32)ns, 0);
		if (ret < 0)
			return ret;

		if (on) {
			ret = mv88e6xxx_config_gpio(ds, pin,
						    MISC_REG_GPIO_MODE_TRIG,
						    MISC_REG_GPIO_DIR_OUT);
		} else {
			ret = mv88e6xxx_config_gpio(ds, pin,
						    MISC_REG_GPIO_MODE_GPIO,
						    MISC_REG_GPIO_DIR_IN);
		}

		return ret;

	case PTP_CLK_REQ_PPS:
		pin = mv88e6xxx_find_pin(ptp, PTP_PF_PEROUT, 0);
		dev_dbg(ds->master_dev, "PPS req on=%d pin %d\n", on, pin);

		if (pin < 0)
			return -EINVAL;

		if (on) {
			ret = mv88e6xxx_config_gpio(ds, pin,
						    MISC_REG_GPIO_MODE_TRIG,
						    MISC_REG_GPIO_DIR_OUT);
			if (ret < 0)
				return ret;
			ret = mv88e6xxx_config_periodic_trig(ds, 1000000000UL,
							     0);
			schedule_delayed_work(&ps->tai_work, 0);
			mv88e6xxx_config_eventcap(ds,
						  TAI_GLOBAL_EVENT_STATUS_CAPTURE_TRIG,
						  1);
		} else {
			ret = mv88e6xxx_config_gpio(ds, pin,
						    MISC_REG_GPIO_MODE_GPIO,
						    MISC_REG_GPIO_DIR_IN);
			if (ret < 0)
				return ret;
			ret = mv88e6xxx_disable_trig(ds);
			cancel_delayed_work_sync(&ps->tai_work);
		}

		return ret;
	}

	return -EOPNOTSUPP;
}

static int mv88e6xxx_phc_verify(struct ptp_clock_info *ptp,
				unsigned int pin, enum ptp_pin_function func,
				unsigned int chan)
{
	switch (func) {
	case PTP_PF_NONE:
	case PTP_PF_EXTTS:
	case PTP_PF_PEROUT:
		break;
	case PTP_PF_PHYSYNC:
		return -EOPNOTSUPP;
	}
	return 0;
}

int mv88e6xxx_setup_phc(struct dsa_switch *ds)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int i;

	/* These are optional clock handles for steering the PTP time */
	ps->xtal_in = of_clk_get_by_name(ds->pd->of_node, "xtal_in");
	if (IS_ERR(ps->xtal_in))
		ps->xtal_in = NULL;

	ps->ptp_extclk = of_clk_get_by_name(ds->pd->of_node, "ptp_ext");
	if (IS_ERR(ps->ptp_extclk))
		ps->ptp_extclk = NULL;

	ps->ptp_clock_caps.owner = THIS_MODULE;
	for (i = 0; i < MV88E6XXX_NUM_GPIO; i++) {
		struct ptp_pin_desc *ppd = &ps->pin_config[i];

		snprintf(ppd->name, sizeof(ppd->name), "mv88e6xxx_gpio%d", i);
		ppd->index = i;
		ppd->func = PTP_PF_NONE;
	}
	snprintf(ps->ptp_clock_caps.name, 20, "dsa-%d:mv88e6xxx", ds->index);

	if (ps->xtal_in || ps->ptp_extclk)
		/* Default to 1000 ppm steering */
		ps->ptp_clock_caps.max_adj = 1000000;
	else
		ps->ptp_clock_caps.max_adj = 0;

	ps->ptp_clock_caps.n_ext_ts = MV88E6XXX_NUM_EXTTS;
	ps->ptp_clock_caps.n_per_out = MV88E6XXX_NUM_PEROUT;
	ps->ptp_clock_caps.n_pins = MV88E6XXX_NUM_GPIO;
	ps->ptp_clock_caps.pin_config = ps->pin_config;
	ps->ptp_clock_caps.adjfreq = mv88e6xxx_phc_adjfreq;
	ps->ptp_clock_caps.adjtime = mv88e6xxx_phc_adjtime;
	ps->ptp_clock_caps.gettime64 = mv88e6xxx_phc_gettime;
	ps->ptp_clock_caps.settime64 = mv88e6xxx_phc_settime;
	ps->ptp_clock_caps.enable = mv88e6xxx_phc_enable;
	ps->ptp_clock_caps.verify = mv88e6xxx_phc_verify;

	ps->ptp_clock = ptp_clock_register(&ps->ptp_clock_caps, ds->master_dev);
	if (IS_ERR(ps->ptp_clock))
		return PTR_ERR(ps->ptp_clock);

	return 0;
}

int mv88e6xxx_get_ts_info(struct dsa_switch *ds, int port,
			  struct ethtool_ts_info *info)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);

	info->so_timestamping =
		SOF_TIMESTAMPING_TX_HARDWARE |
		SOF_TIMESTAMPING_RX_HARDWARE |
		SOF_TIMESTAMPING_RAW_HARDWARE;
	info->phc_index = ptp_clock_index(ps->ptp_clock);
	info->tx_types =
		(1 << HWTSTAMP_TX_OFF) |
		(1 << HWTSTAMP_TX_ON);
	info->rx_filters =
		(1 << HWTSTAMP_FILTER_NONE) |
		(1 << HWTSTAMP_FILTER_PTP_V1_L4_EVENT) |
		(1 << HWTSTAMP_FILTER_PTP_V1_L4_SYNC) |
		(1 << HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L4_EVENT) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L4_SYNC) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L2_EVENT) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L2_SYNC) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ) |
		(1 << HWTSTAMP_FILTER_PTP_V2_EVENT) |
		(1 << HWTSTAMP_FILTER_PTP_V2_SYNC) |
		(1 << HWTSTAMP_FILTER_PTP_V2_DELAY_REQ);

	return 0;
}

int mv88e6xxx_setup_common(struct dsa_switch *ds)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);

	mutex_init(&ps->smi_mutex);
	mutex_init(&ps->stats_mutex);
	mutex_init(&ps->phy_mutex);
	mutex_init(&ps->ptp_mutex);
	mutex_init(&ps->phc_mutex);
	spin_lock_init(&ps->phc_lock);

	ps->id = REG_READ(REG_PORT(0), PORT_SWITCH_ID) & 0xfff0;

	ps->fid_mask = (1 << DSA_MAX_PORTS) - 1;

	/* The actual value of the rollovers doesn't matter, but starting at once
	 * avoids making it look like the 64-bit PHC clock rolls over right away
	 */

	ps->phc_rollovers = 1;
	ps->latest_phc_counter = 0;

	INIT_WORK(&ps->bridge_work, mv88e6xxx_bridge_work);
	INIT_DELAYED_WORK(&ps->tai_work, mv88e6xxx_tai_work);

	return 0;
}

int mv88e6xxx_switch_reset(struct dsa_switch *ds, bool ppu_active)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	u16 is_reset = (ppu_active ? 0x8800 : 0xc800);
	unsigned long timeout;
	int ret;
	int i;

	/* Set all ports to the disabled state. */
	for (i = 0; i < ps->num_ports; i++) {
		ret = REG_READ(REG_PORT(i), PORT_CONTROL);
		REG_WRITE(REG_PORT(i), PORT_CONTROL, ret & 0xfffc);
	}

	/* Wait for transmit queues to drain. */
	usleep_range(2000, 4000);

	/* Reset the switch. Keep the PPU active if requested. The PPU
	 * needs to be active to support indirect phy register access
	 * through global registers 0x18 and 0x19.
	 */
	if (ppu_active)
		REG_WRITE(REG_GLOBAL, 0x04, 0xc000);
	else
		REG_WRITE(REG_GLOBAL, 0x04, 0xc400);

	/* Wait up to one second for reset to complete. */
	timeout = jiffies + 1 * HZ;
	while (time_before(jiffies, timeout)) {
		ret = REG_READ(REG_GLOBAL, 0x00);
		if ((ret & is_reset) == is_reset)
			break;
		usleep_range(1000, 2000);
	}
	if (time_after(jiffies, timeout))
		return -ETIMEDOUT;

	return 0;
}

int mv88e6xxx_phy_page_read(struct dsa_switch *ds, int port, int page, int reg)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int ret;

	mutex_lock(&ps->phy_mutex);
	ret = _mv88e6xxx_phy_write_indirect(ds, port, 0x16, page);
	if (ret < 0)
		goto error;
	ret = _mv88e6xxx_phy_read_indirect(ds, port, reg);
error:
	_mv88e6xxx_phy_write_indirect(ds, port, 0x16, 0x0);
	mutex_unlock(&ps->phy_mutex);
	return ret;
}

int mv88e6xxx_phy_page_write(struct dsa_switch *ds, int port, int page,
			     int reg, int val)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int ret;

	mutex_lock(&ps->phy_mutex);
	ret = _mv88e6xxx_phy_write_indirect(ds, port, 0x16, page);
	if (ret < 0)
		goto error;

	ret = _mv88e6xxx_phy_write_indirect(ds, port, reg, val);
error:
	_mv88e6xxx_phy_write_indirect(ds, port, 0x16, 0x0);
	mutex_unlock(&ps->phy_mutex);
	return ret;
}

static int mv88e6xxx_port_to_phy_addr(struct dsa_switch *ds, int port)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);

	if (port >= 0 && port < ps->num_ports)
		return port + ps->mdio_offset;
	return -EINVAL;
}

int
mv88e6xxx_phy_read(struct dsa_switch *ds, int port, int regnum)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int addr = mv88e6xxx_port_to_phy_addr(ds, port);
	int ret;

	if (addr < 0)
		return addr;

	mutex_lock(&ps->phy_mutex);
	ret = _mv88e6xxx_phy_read(ds, addr, regnum);
	mutex_unlock(&ps->phy_mutex);
	return ret;
}

int
mv88e6xxx_phy_write(struct dsa_switch *ds, int port, int regnum, u16 val)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int addr = mv88e6xxx_port_to_phy_addr(ds, port);
	int ret;

	if (addr < 0)
		return addr;

	mutex_lock(&ps->phy_mutex);
	ret = _mv88e6xxx_phy_write(ds, addr, regnum, val);
	mutex_unlock(&ps->phy_mutex);
	return ret;
}

int
mv88e6xxx_phy_read_indirect(struct dsa_switch *ds, int port, int regnum)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int addr = mv88e6xxx_port_to_phy_addr(ds, port);
	int ret;

	if (addr < 0)
		return addr;

	mutex_lock(&ps->phy_mutex);
	ret = _mv88e6xxx_phy_read_indirect(ds, addr, regnum);
	mutex_unlock(&ps->phy_mutex);
	return ret;
}

int
mv88e6xxx_phy_write_indirect(struct dsa_switch *ds, int port, int regnum,
			     u16 val)
{
	struct mv88e6xxx_priv_state *ps = ds_to_priv(ds);
	int addr = mv88e6xxx_port_to_phy_addr(ds, port);
	int ret;

	if (addr < 0)
		return addr;

	mutex_lock(&ps->phy_mutex);
	ret = _mv88e6xxx_phy_write_indirect(ds, addr, regnum, val);
	mutex_unlock(&ps->phy_mutex);
	return ret;
}

static int __init mv88e6xxx_init(void)
{
#if IS_ENABLED(CONFIG_NET_DSA_MV88E6131)
	register_switch_driver(&mv88e6131_switch_driver);
#endif
#if IS_ENABLED(CONFIG_NET_DSA_MV88E6123_61_65)
	register_switch_driver(&mv88e6123_61_65_switch_driver);
#endif
#if IS_ENABLED(CONFIG_NET_DSA_MV88E6352)
	register_switch_driver(&mv88e6352_switch_driver);
#endif
#if IS_ENABLED(CONFIG_NET_DSA_MV88E6171)
	register_switch_driver(&mv88e6171_switch_driver);
#endif
	return 0;
}
module_init(mv88e6xxx_init);

static void __exit mv88e6xxx_cleanup(void)
{
#if IS_ENABLED(CONFIG_NET_DSA_MV88E6171)
	unregister_switch_driver(&mv88e6171_switch_driver);
#endif
#if IS_ENABLED(CONFIG_NET_DSA_MV88E6352)
	unregister_switch_driver(&mv88e6352_switch_driver);
#endif
#if IS_ENABLED(CONFIG_NET_DSA_MV88E6123_61_65)
	unregister_switch_driver(&mv88e6123_61_65_switch_driver);
#endif
#if IS_ENABLED(CONFIG_NET_DSA_MV88E6131)
	unregister_switch_driver(&mv88e6131_switch_driver);
#endif
}
module_exit(mv88e6xxx_cleanup);

MODULE_AUTHOR("Lennert Buytenhek <buytenh@wantstofly.org>");
MODULE_DESCRIPTION("Driver for Marvell 88E6XXX ethernet switch chips");
MODULE_LICENSE("GPL");
