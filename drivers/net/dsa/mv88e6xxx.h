/*
 * net/dsa/mv88e6xxx.h - Marvell 88e6xxx switch chip support
 * Copyright (c) 2008 Marvell Semiconductor
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __MV88E6XXX_H
#define __MV88E6XXX_H

#ifndef UINT64_MAX
#define UINT64_MAX		(u64)(~((u64)0))
#endif

#define SMI_CMD			0x00
#define SMI_CMD_BUSY		BIT(15)
#define SMI_CMD_CLAUSE_22	BIT(12)
#define SMI_CMD_OP_22_WRITE	((1 << 10) | SMI_CMD_BUSY | SMI_CMD_CLAUSE_22)
#define SMI_CMD_OP_22_READ	((2 << 10) | SMI_CMD_BUSY | SMI_CMD_CLAUSE_22)
#define SMI_CMD_OP_45_WRITE_ADDR	((0 << 10) | SMI_CMD_BUSY)
#define SMI_CMD_OP_45_WRITE_DATA	((1 << 10) | SMI_CMD_BUSY)
#define SMI_CMD_OP_45_READ_DATA		((2 << 10) | SMI_CMD_BUSY)
#define SMI_CMD_OP_45_READ_DATA_INC	((3 << 10) | SMI_CMD_BUSY)
#define SMI_DATA		0x01

#define REG_PORT(p)		(0x10 + (p))
#define PORT_STATUS		0x00
#define PORT_STATUS_PAUSE_EN	BIT(15)
#define PORT_STATUS_MY_PAUSE	BIT(14)
#define PORT_STATUS_HD_FLOW	BIT(13)
#define PORT_STATUS_PHY_DETECT	BIT(12)
#define PORT_STATUS_LINK	BIT(11)
#define PORT_STATUS_DUPLEX	BIT(10)
#define PORT_STATUS_SPEED_MASK	0x0300
#define PORT_STATUS_SPEED_10	0x0000
#define PORT_STATUS_SPEED_100	0x0100
#define PORT_STATUS_SPEED_1000	0x0200
#define PORT_STATUS_EEE		BIT(6) /* 6352 */
#define PORT_STATUS_AM_DIS	BIT(6) /* 6165 */
#define PORT_STATUS_MGMII	BIT(6) /* 6185 */
#define PORT_STATUS_TX_PAUSED	BIT(5)
#define PORT_STATUS_FLOW_CTRL	BIT(4)

#define PORT_PCS_CTRL		0x01
#define PORT_PCS_RX_DELAY	BIT(15)
#define PORT_PCS_TX_DELAY	BIT(14)
#define PORT_PCS_FORCE_SPD	BIT(13)
#define PORT_PCS_ALT_SPD	BIT(12)
#define PORT_PCS_MII_MODE_MAC	0
#define PORT_PCS_MII_MODE_PHY	BIT(11)
#define PORT_PCS_EEE_VAL	BIT(9)
#define PORT_PCS_FORCE_EEE	BIT(8)
#define PORT_PCS_FC_VAL	BIT(7)
#define PORT_PCS_FORCE_FC	BIT(6)
#define PORT_PCS_LINK_VAL	BIT(5)
#define PORT_PCS_FORCE_LINK	BIT(4)
#define PORT_PCS_DPX_FULL	BIT(3)
#define PORT_PCS_DPX_HALF	0
#define PORT_PCS_FORCE_DPX	BIT(2)
#define PORT_PCS_SPD_10		0
#define PORT_PCS_SPD_100	1
#define PORT_PCS_SPD_1000	2
#define PORT_PCS_SPD_2500	3

#define PORT_JAMMING_CTRL	0x02
#define PORT_SWITCH_ID		0x03
#define PORT_SWITCH_ID_6085	0x04a0
#define PORT_SWITCH_ID_6095	0x0950
#define PORT_SWITCH_ID_6123	0x1210
#define PORT_SWITCH_ID_6123_A1	0x1212
#define PORT_SWITCH_ID_6123_A2	0x1213
#define PORT_SWITCH_ID_6131	0x1060
#define PORT_SWITCH_ID_6131_B2	0x1066
#define PORT_SWITCH_ID_6152	0x1a40
#define PORT_SWITCH_ID_6155	0x1a50
#define PORT_SWITCH_ID_6161	0x1610
#define PORT_SWITCH_ID_6161_A1	0x1612
#define PORT_SWITCH_ID_6161_A2	0x1613
#define PORT_SWITCH_ID_6165	0x1650
#define PORT_SWITCH_ID_6165_A1	0x1652
#define PORT_SWITCH_ID_6165_A2	0x1653
#define PORT_SWITCH_ID_6171	0x1710
#define PORT_SWITCH_ID_6172	0x1720
#define PORT_SWITCH_ID_6176	0x1760
#define PORT_SWITCH_ID_6182	0x1a60
#define PORT_SWITCH_ID_6185	0x1a70
#define PORT_SWITCH_ID_6341	0x3410
#define PORT_SWITCH_ID_6352	0x3520
#define PORT_SWITCH_ID_6352_A0	0x3521
#define PORT_SWITCH_ID_6352_A1	0x3522
#define PORT_CONTROL		0x04
#define PORT_CONTROL_STATE_MASK		0x03
#define PORT_CONTROL_STATE_DISABLED	0x00
#define PORT_CONTROL_STATE_BLOCKING	0x01
#define PORT_CONTROL_STATE_LEARNING	0x02
#define PORT_CONTROL_STATE_FORWARDING	0x03
#define PORT_CONTROL_1		0x05
#define PORT_BASE_VLAN		0x06
#define PORT_DEFAULT_VLAN	0x07
#define PORT_CONTROL_2		0x08
#define PORT_RATE_CONTROL	0x09
#define PORT_RATE_CONTROL_2	0x0a
#define PORT_ASSOC_VECTOR	0x0b
#define PORT_ATU_CTRL	0x0c
#define PORT_PRIORITY_OVERRIDE	0x0d
#define PORT_POLICY_CTRL	0x0e
#define PORT_ETHERTYPE	0x0f
#define PORT_IN_DISCARD_LO	0x10
#define PORT_IN_DISCARD_HI	0x11
#define PORT_IN_FILTERED	0x12
#define PORT_OUT_FILTERED	0x13
#define PORT_TAG_REMAP_0123	0x18
#define PORT_TAG_REMAP_4567	0x19

#define REG_GLOBAL		0x1b
#define GLOBAL_STATUS		0x00
#define GLOBAL_STATUS_PPU_STATE BIT(15) /* 6351 and 6171 */
/* Two bits for 6165, 6185 etc */
#define GLOBAL_STATUS_PPU_MASK		(0x3 << 14)
#define GLOBAL_STATUS_PPU_DISABLED_RST	(0x0 << 14)
#define GLOBAL_STATUS_PPU_INITIALIZING	(0x1 << 14)
#define GLOBAL_STATUS_PPU_DISABLED	(0x2 << 14)
#define GLOBAL_STATUS_PPU_POLLING	(0x3 << 14)
#define GLOBAL_MAC_01		0x01
#define GLOBAL_MAC_23		0x02
#define GLOBAL_MAC_45		0x03
#define GLOBAL_CONTROL		0x04
#define GLOBAL_CONTROL_SW_RESET		BIT(15)
#define GLOBAL_CONTROL_PPU_ENABLE	BIT(14)
#define GLOBAL_CONTROL_DISCARD_EXCESS	BIT(13) /* 6352 */
#define GLOBAL_CONTROL_SCHED_PRIO	BIT(11) /* 6152 */
#define GLOBAL_CONTROL_MAX_FRAME_1632	BIT(10) /* 6152 */
#define GLOBAL_CONTROL_RELOAD_EEPROM	BIT(9)  /* 6152 */
#define GLOBAL_CONTROL_DEVICE_EN	BIT(7)
#define GLOBAL_CONTROL_STATS_DONE_EN	BIT(6)
#define GLOBAL_CONTROL_VTU_PROBLEM_EN	BIT(5)
#define GLOBAL_CONTROL_VTU_DONE_EN	BIT(4)
#define GLOBAL_CONTROL_ATU_PROBLEM_EN	BIT(3)
#define GLOBAL_CONTROL_ATU_DONE_EN	BIT(2)
#define GLOBAL_CONTROL_TCAM_EN		BIT(1)
#define GLOBAL_CONTROL_EEPROM_DONE_EN	BIT(0)
#define GLOBAL_VTU_OP		0x05
#define GLOBAL_VTU_VID		0x06
#define GLOBAL_VTU_DATA_0_3	0x07
#define GLOBAL_VTU_DATA_4_7	0x08
#define GLOBAL_VTU_DATA_8_11	0x09
#define GLOBAL_ATU_CONTROL	0x0a
#define GLOBAL_ATU_OP		0x0b
#define GLOBAL_ATU_OP_BUSY	BIT(15)
#define GLOBAL_ATU_OP_NOP		(0 << 12)
#define GLOBAL_ATU_OP_FLUSH_ALL		((1 << 12) | GLOBAL_ATU_OP_BUSY)
#define GLOBAL_ATU_OP_FLUSH_NON_STATIC	((2 << 12) | GLOBAL_ATU_OP_BUSY)
#define GLOBAL_ATU_OP_LOAD_DB		((3 << 12) | GLOBAL_ATU_OP_BUSY)
#define GLOBAL_ATU_OP_GET_NEXT_DB	((4 << 12) | GLOBAL_ATU_OP_BUSY)
#define GLOBAL_ATU_OP_FLUSH_DB		((5 << 12) | GLOBAL_ATU_OP_BUSY)
#define GLOBAL_ATU_OP_FLUSH_NON_STATIC_DB ((6 << 12) | GLOBAL_ATU_OP_BUSY)
#define GLOBAL_ATU_OP_GET_CLR_VIOLATION	  ((7 << 12) | GLOBAL_ATU_OP_BUSY)
#define GLOBAL_ATU_DATA		0x0c
#define GLOBAL_ATU_DATA_STATE_MASK		0x0f
#define GLOBAL_ATU_DATA_STATE_UNUSED		0x00
#define GLOBAL_ATU_DATA_STATE_UC_MGMT		0x0d
#define GLOBAL_ATU_DATA_STATE_UC_STATIC		0x0e
#define GLOBAL_ATU_DATA_STATE_UC_PRIO_OVER	0x0f
#define GLOBAL_ATU_DATA_STATE_MC_NONE_RATE	0x05
#define GLOBAL_ATU_DATA_STATE_MC_STATIC		0x07
#define GLOBAL_ATU_DATA_STATE_MC_MGMT		0x0e
#define GLOBAL_ATU_DATA_STATE_MC_PRIO_OVER	0x0f
#define GLOBAL_ATU_MAC_01	0x0d
#define GLOBAL_ATU_MAC_23	0x0e
#define GLOBAL_ATU_MAC_45	0x0f
#define GLOBAL_IP_PRI_0		0x10
#define GLOBAL_IP_PRI_1		0x11
#define GLOBAL_IP_PRI_2		0x12
#define GLOBAL_IP_PRI_3		0x13
#define GLOBAL_IP_PRI_4		0x14
#define GLOBAL_IP_PRI_5		0x15
#define GLOBAL_IP_PRI_6		0x16
#define GLOBAL_IP_PRI_7		0x17
#define GLOBAL_IEEE_PRI		0x18
#define GLOBAL_CORE_TAG_TYPE	0x19
#define GLOBAL_MONITOR_CONTROL	0x1a

#define GLOBAL_CONTROL_2	0x1c
#define GLOBAL_CONTROL_2_RMU_PORT_0	(0 << 8)
#define GLOBAL_CONTROL_2_RMU_PORT_1	(1 << 8)
#define GLOBAL_CONTROL_2_RMU_PORT_5	(2 << 8)
#define GLOBAL_CONTROL_2_RMU_ANY	(6 << 8)
#define GLOBAL_CONTROL_2_RMU_DISABLED	(7 << 8)
#define GLOBAL_CONTROL_2_HIST_RX	(1 << 6)
#define GLOBAL_CONTROL_2_HIST_TX	(2 << 6)
#define GLOBAL_CONTROL_2_HIST_RX_TX	(3 << 6)

#define GLOBAL_STATS_OP		0x1d
#define GLOBAL_STATS_OP_BUSY	BIT(15)
#define GLOBAL_STATS_OP_NOP		(0 << 12)
#define GLOBAL_STATS_OP_FLUSH_ALL	((1 << 12) | GLOBAL_STATS_OP_BUSY)
#define GLOBAL_STATS_OP_FLUSH_PORT	((2 << 12) | GLOBAL_STATS_OP_BUSY)
#define GLOBAL_STATS_OP_READ_CAPTURED	((4 << 12) | GLOBAL_STATS_OP_BUSY)
#define GLOBAL_STATS_OP_CAPTURE_PORT	((5 << 12) | GLOBAL_STATS_OP_BUSY)
#define GLOBAL_STATS_OP_HIST_RX		((1 << 10) | GLOBAL_STATS_OP_BUSY)
#define GLOBAL_STATS_OP_HIST_TX		((2 << 10) | GLOBAL_STATS_OP_BUSY)
#define GLOBAL_STATS_OP_HIST_RX_TX	((3 << 10) | GLOBAL_STATS_OP_BUSY)
#define GLOBAL_STATS_OP_BANK_1	BIT(9)
#define GLOBAL_STATS_COUNTER_32	0x1e
#define GLOBAL_STATS_COUNTER_01	0x1f

#define REG_GLOBAL2		0x1c
#define GLOBAL2_INT_SOURCE	0x00
#define GLOBAL2_INT_MASK	0x01
#define GLOBAL2_MGMT_EN_2X	0x02
#define GLOBAL2_MGMT_EN_0X	0x03
#define GLOBAL2_FLOW_CONTROL	0x04
#define GLOBAL2_SWITCH_MGMT	0x05
#define GLOBAL2_DEVICE_MAPPING	0x06
#define GLOBAL2_TRUNK_MASK	0x07
#define GLOBAL2_TRUNK_MAPPING	0x08
#define GLOBAL2_INGRESS_OP	0x09
#define GLOBAL2_INGRESS_DATA	0x0a
#define GLOBAL2_PVT_ADDR	0x0b
#define GLOBAL2_PVT_DATA	0x0c
#define GLOBAL2_SWITCH_MAC	0x0d
#define GLOBAL2_SWITCH_MAC_BUSY BIT(15)
#define GLOBAL2_ATU_STATS	0x0e
#define GLOBAL2_PRIO_OVERRIDE	0x0f
#define GLOBAL2_EEPROM_OP	0x14
#define GLOBAL2_EEPROM_OP_BUSY	BIT(15)
#define GLOBAL2_EEPROM_OP_WRITE		((3 << 12) | GLOBAL2_EEPROM_OP_BUSY)
#define GLOBAL2_EEPROM_OP_READ		((4 << 12) | GLOBAL2_EEPROM_OP_BUSY)
#define GLOBAL2_EEPROM_OP_RESTART	((6 << 12) | GLOBAL2_EEPROM_OP_BUSY)
#define GLOBAL2_EEPROM_OP_LOAD	BIT(11)
#define GLOBAL2_EEPROM_DATA	0x15

#define GLOBAL2_PTP_AVB_OP	0x16
#define GLOBAL2_PTP_AVB_OP_BUSY	BIT(15)
#define GLOBAL2_PTP_AVB_OP_READ		((0 << 13) | GLOBAL2_PTP_AVB_OP_BUSY)
#define GLOBAL2_PTP_AVB_OP_READ_INCR	((2 << 13) | GLOBAL2_PTP_AVB_OP_BUSY)
#define GLOBAL2_PTP_AVB_OP_WRITE	((3 << 13) | GLOBAL2_PTP_AVB_OP_BUSY)
#define GLOBAL2_PTP_AVB_OP_PORT(p)		(((p) & 0x1f) << 8)
#define GLOBAL2_PTP_AVB_OP_PORT_PTP_GLOBAL	0x1f
#define GLOBAL2_PTP_AVB_OP_PORT_TAI_GLOBAL	0x1e
#define GLOBAL2_PTP_AVB_OP_PORT_AVB_GLOBAL	0x1f
#define GLOBAL2_PTP_AVB_OP_PORT_QAV_GLOBAL	0x1f
#define GLOBAL2_PTP_AVB_OP_PORT_QBV_GLOBAL	0x1f
#define GLOBAL2_PTP_AVB_OP_BLOCK(b)		(((b) & 0x07) << 5)
#define GLOBAL2_PTP_AVB_OP_BLOCK_PTP	0
#define GLOBAL2_PTP_AVB_OP_BLOCK_AVB	1
#define GLOBAL2_PTP_AVB_OP_BLOCK_QAV	2
#define GLOBAL2_PTP_AVB_OP_BLOCK_QBV	3
#define GLOBAL2_PTP_AVB_OP_ADDR(a)		(((a) & 0x1f) << 0)
#define GLOBAL2_PTP_AVB_DATA	0x17

#define GLOBAL2_SMI_OP		0x18
#define GLOBAL2_SMI_OP_BUSY		BIT(15)
#define GLOBAL2_SMI_OP_CLAUSE_22	BIT(12)
#define GLOBAL2_SMI_OP_22_WRITE		((1 << 10) | GLOBAL2_SMI_OP_BUSY | \
					 GLOBAL2_SMI_OP_CLAUSE_22)
#define GLOBAL2_SMI_OP_22_READ		((2 << 10) | GLOBAL2_SMI_OP_BUSY | \
					 GLOBAL2_SMI_OP_CLAUSE_22)
#define GLOBAL2_SMI_OP_45_WRITE_ADDR	((0 << 10) | GLOBAL2_SMI_OP_BUSY)
#define GLOBAL2_SMI_OP_45_WRITE_DATA	((1 << 10) | GLOBAL2_SMI_OP_BUSY)
#define GLOBAL2_SMI_OP_45_READ_DATA	((2 << 10) | GLOBAL2_SMI_OP_BUSY)
#define GLOBAL2_SMI_DATA	0x19

#define GLOBAL2_SCRATCH_MISC	0x1a
#define GLOBAL2_SCRATCH_MISC_UPDATE	BIT(15)
#define GLOBAL2_SCRATCH_MISC_REG_OFFSET 8
#define GLOBAL2_SCRATCH_MISC_DATA_MASK 0xff
#define MISC_REG_SCRATCH_0	0x00
#define MISC_REG_SCRATCH_1	0x01
#define MISC_REG_GPIO_X_STALL_VECTOR_Y(x, y) (0x20 + (2 * (x)) + (y))
#define MISC_REG_GPIO_CONFIG_LO	0x60
#define MISC_REG_GPIO_CONFIG_HI	0x61
#define MISC_REG_GPIO_DIR(pin)	(0x62 + ((pin) / 8))
#define MISC_REG_GPIO_DIR_OFFSET(pin)	((pin) & 0x7)
#define MISC_REG_GPIO_DIR_MASK(pin)	(1 << MISC_REG_GPIO_DIR_OFFSET(pin))
#define MISC_REG_GPIO_DIR_IN	1
#define MISC_REG_GPIO_DIR_OUT	0
#define MISC_REG_GPIO_DATA(pin)	(0x64 + ((pin) / 8))
#define MISC_REG_GPIO_MODE(pin) (0x68 + ((pin) / 2))
#define MISC_REG_GPIO_MODE_OFFSET(pin)	((pin) & 0x1 ? 4 : 0)
#define MISC_REG_GPIO_MODE_MASK(pin)	(0x7 << MISC_REG_GPIO_MODE_OFFSET(pin))
#define MISC_REG_GPIO_MODE_GPIO	0
#define MISC_REG_GPIO_MODE_TRIG	1
#define MISC_REG_GPIO_MODE_EVREQ	2
#define MISC_REG_GPIO_MODE_EXTCLK	3
#define MISC_REG_GPIO_MODE_RX_CLK0	4
#define MISC_REG_GPIO_MODE_RX_CLK1	5
#define MISC_REG_GPIO_MODE_I2C	7

#define GLOBAL2_WDOG_CONTROL	0x1b
#define GLOBAL2_QOS_WEIGHT	0x1c
#define GLOBAL2_MISC		0x1d

/* Global PTP registers. Use with PTP_AVB_OP_BLOCK_PTP and
 * GLOBAL2_PTP_AVB_OP_PORT_PTP_GLOBAL */
#define PTP_GLOBAL_ETHERTYPE	0x00
#define PTP_GLOBAL_MSG_TYPE	0x01
#define PTP_GLOBAL_TS_ARRIVAL_PTR	0x02
#define PTP_GLOBAL_CONFIG		0x07
#define PTP_GLOBAL_IRQ_STATUS	0x08

/* Global PTP mode register */
#define PTP_GLOBAL_CONFIG_UPD       0x8000
#define PTP_GLOBAL_CONFIG_IDX_MASK  0x7F00
#define PTP_GLOBAL_CONFIG_DATA_MASK 0x00FF

#define PTP_GLOBAL_CONFIG_MODE_IDX  0x0
#define PTP_GLOBAL_CONFIG_MODE_TS_AT_PHY 0x00
#define PTP_GLOBAL_CONFIG_MODE_TS_AT_MAC 0x80

/* Per-port PTP registers. Use with PTP_AVB_OP_BLOCK_PTP */
#define PTP_PORT_CONFIG_0	0x00
#define PTP_PORT_CONFIG_0_TRANS_1588		(0 << 12)
#define PTP_PORT_CONFIG_0_TRANS_8021AS		(1 << 12)
#define PTP_PORT_CONFIG_0_TRANS_TO_VAL(t)	((t) >> 12)
#define PTP_PORT_CONFIG_0_ENABLE_TRANS_CHECK	(0 << 11)
#define PTP_PORT_CONFIG_0_DISABLE_TRANS_CHECK	(1 << 11)
#define PTP_PORT_CONFIG_0_ENABLE_OVERWRITE	(0 << 1)
#define PTP_PORT_CONFIG_0_DISABLE_OVERWRITE	(1 << 1)
#define PTP_PORT_CONFIG_0_ENABLE_TS		(0 << 0)
#define PTP_PORT_CONFIG_0_DISABLE_TS		(1 << 0)
#define PTP_PORT_CONFIG_1	0x01
#define PTP_PORT_CONFIG_2	0x02
#define PTP_PORT_CONFIG_2_EMBED_ARRIVAL_0	(0x10 << 8)
#define PTP_PORT_CONFIG_2_DEPARTURE_IRQ_EN	(1 << 1)
#define PTP_PORT_CONFIG_2_ARRIVAL_IRQ_EN	(1 << 0)

#define PTP_PORT_LED_CONFIG	0x03
#define PTP_PORT_ARRIVAL_0_STATUS	0x08
#define PTP_PORT_ARRIVAL_0_TIME_LO	0x09
#define PTP_PORT_ARRIVAL_0_TIME_HI	0x0a
#define PTP_PORT_ARRIVAL_0_SEQUENCE	0x0b
#define PTP_PORT_ARRIVAL_1_STATUS	0x0c
#define PTP_PORT_ARRIVAL_1_TIME_LO	0x0d
#define PTP_PORT_ARRIVAL_1_TIME_HI	0x0e
#define PTP_PORT_ARRIVAL_1_SEQUENCE	0x0f
#define PTP_PORT_DEPARTURE_STATUS	0x10
#define PTP_PORT_DEPARTURE_STATUS_STATUS_MASK	(3 << 1)
#define PTP_PORT_DEPARTURE_STATUS_STATUS_NORMAL	(0 << 1)
#define PTP_PORT_DEPARTURE_STATUS_STATUS_OVERWRITTEN	(1 << 1)
#define PTP_PORT_DEPARTURE_STATUS_STATUS_DISCARDED	(2 << 1)
#define PTP_PORT_DEPARTURE_STATUS_VALID	BIT(0)
#define PTP_PORT_DEPARTURE_TIME_LO	0x11
#define PTP_PORT_DEPARTURE_TIME_HI	0x12
#define PTP_PORT_DEPARTURE_SEQUENCE	0x13

#define TAI_GLOBAL_CONFIG	0x00
#define TAI_GLOBAL_CONFIG_ENABLE_OVERWRITE	BIT(15)
#define TAI_GLOBAL_CONFIG_DISABLE_OVERWRITE	0
#define TAI_GLOBAL_CONFIG_ENABLE_CAPTURE_COUNTER	BIT(14)
#define TAI_GLOBAL_CONFIG_DISABLE_CAPTURE_COUNTER	0
#define TAI_GLOBAL_CONFIG_EVREQ_RISING	0
#define TAI_GLOBAL_CONFIG_EVREQ_FALLING	BIT(13)
#define TAI_GLOBAL_CONFIG_TRIG_ACTIVE_HI	0
#define TAI_GLOBAL_CONFIG_TRIG_ACTIVE_LO	BIT(12)
#define TAI_GLOBAL_CONFIG_IRL_ENABLE	BIT(10)
#define TAI_GLOBAL_CONFIG_TRIG_IRQ_EN	BIT(9)
#define TAI_GLOBAL_CONFIG_EVREQ_IRQ_EN	BIT(8)
#define TAI_GLOBAL_CONFIG_TRIG_LOCK	BIT(7)
#define TAI_GLOBAL_CONFIG_BLOCK_UPDATE	BIT(3)
#define TAI_GLOBAL_CONFIG_MULTI_PTP	BIT(2)
#define TAI_GLOBAL_CONFIG_TRIG_MODE_ONESHOT	BIT(1)
#define TAI_GLOBAL_CONFIG_TRIG_MODE_CLOCK	0
#define TAI_GLOBAL_CONFIG_TRIG_ENABLE	BIT(0)
#define TAI_GLOBAL_CONFIG_TRIG_DISABLE	0

#define TAI_GLOBAL_CLOCK_PERIOD	0x01
#define TAI_GLOBAL_TRIG_GEN_AMOUNT_LO	0x02
#define TAI_GLOBAL_TRIG_GEN_AMOUNT_HI	0x03
#define TAI_GLOBAL_TRIG_CLOCK_COMP	0x04
#define TAI_GLOBAL_TRIG_CONFIG	0x05
#define TAI_GLOBAL_IRL_AMOUNT	0x06
#define TAI_GLOBAL_IRL_COMP	0x07
#define TAI_GLOBAL_IRL_COMP_PS	0x08

#define TAI_GLOBAL_EVENT_STATUS	0x09
#define TAI_GLOBAL_EVENT_STATUS_CAPTURE_EVREQ	0
#define TAI_GLOBAL_EVENT_STATUS_CAPTURE_TRIG	BIT(14)
#define TAI_GLOBAL_EVENT_STATUS_ERROR	BIT(9)
#define TAI_GLOBAL_EVENT_STATUS_VALID	BIT(8)
#define TAI_GLOBAL_EVENT_STATUS_CTR_MASK	0xff

#define TAI_GLOBAL_EVENT_TIME_LO	0x0a
#define TAI_GLOBAL_EVENT_TIME_HI	0x0b
#define TAI_GLOBAL_TIME_LO	0x0e
#define TAI_GLOBAL_TIME_HI	0x0f
#define TAI_GLOBAL_TRIG_TIME_LO	0x10
#define TAI_GLOBAL_TRIG_TIME_HI	0x11
#define TAI_GLOBAL_LOCK_STATUS	0x12
#define TAI_GLOBAL_CLOCK_CONFIG	0x1e

#define MV88E6XXX_NUM_EXTTS	1
#define MV88E6XXX_NUM_PEROUT	1
#define MV88E6XXX_NUM_GPIO	11

/* 6341-specific configuration indices and macros */
#define MONITOR_MGMT_CTRL	0x1a
#define MGMT_UPDATE_DATA	(1 << 15)
#define RSVD2CPU_ENA_0x_LOW_IDX	0x00
#define RSVD2CPU_ENA_0x_HIGH_IDX	0x01
#define CPU_DEST_IDX	0x30
#define MGMT_PTR_WRITE(index, value) (MGMT_UPDATE_DATA \
				| ((index) & 0x03F) << 8 | ((value) & 0x0FF))

/* TX_TSTAMP_TIMEOUT: This limits the time spent polling for a TX
 * timestamp. When working properly, hardware will produce a timestamp
 * within 1ms. Software may enounter delays due to MDIO contention, so
 * the timeout is set accordingly.
 */
#define TX_TSTAMP_TIMEOUT	msecs_to_jiffies(20)
#define TAI_WORK_INTERVAL	msecs_to_jiffies(100)

struct mv88e6xxx_port_priv_state {
	u8 port_id;
	u8 fid;
	u8 stp_state;

	/* This spinlock serializes access to the TX timestamping
	 * parameters.
	 */
	spinlock_t tx_tstamp_lock;
	struct work_struct tx_tstamp_work;
	u16 tx_seq_id;
	unsigned long tx_tstamp_start;
	struct sk_buff *tx_skb;

	/* The mutex serializes access to the per-port PTP timestamping
	 * configuration between timestamping clients. The spinlock serializes
	 * access to the parts of the configuration that have to be checked
	 * from the RX and TX paths.
	 */
	struct mutex ptp_mutex;
	spinlock_t ptp_lock;

	struct hwtstamp_config tstamp_config;

	bool ts_enable;
	u16 ts_msg_types;
	bool check_trans_spec;
	int check_trans_spec_val;
};

struct mv88e6xxx_priv_state {
	/* When using multi-chip addressing, this mutex protects
	 * access to the indirect access registers.  (In single-chip
	 * mode, this mutex is effectively useless.)
	 */
	struct mutex	smi_mutex;

#ifdef CONFIG_NET_DSA_MV88E6XXX_NEED_PPU
	/* Handles automatic disabling and re-enabling of the PHY
	 * polling unit.
	 */
	struct mutex		ppu_mutex;
	int			ppu_disabled;
	struct work_struct	ppu_work;
	struct timer_list	ppu_timer;
#endif

	/* This mutex serialises access to the statistics unit.
	 * Hold this mutex over snapshot + dump sequences.
	 */
	struct mutex	stats_mutex;

	/* This mutex serializes phy access for chips with
	 * indirect phy addressing. It is unused for chips
	 * with direct phy access.
	 */
	struct mutex	phy_mutex;

	/* This mutex serializes eeprom access for chips with
	 * eeprom support.
	 */
	struct mutex eeprom_mutex;

	/* This mutex serializes PTP/AVB access for chips with
	 * PTP/AVB support.
	 */
	struct mutex ptp_mutex;

	int		id; /* switch product id */
	int		num_ports;	/* number of switch ports */
	int		mdio_offset;    /* MDIO address of port 0 */

	/* hw bridging */

	u32 fid_mask;
	u16 bridge_mask[DSA_MAX_PORTS];

	unsigned long port_state_update_mask;

	struct mv88e6xxx_port_priv_state port_priv[DSA_MAX_PORTS];

	struct work_struct bridge_work;

	/* This spinlock serializes access to the upper 32-bits of the
	 * PHC time, and the offset. Must be a spinlock because
	 * incoming timetamped PTP packets are processing in a soft
	 * IRQ context.
	 */
	spinlock_t phc_lock;
	u32 phc_rollovers;
	u32 latest_phc_counter;
	u64 phc_offset_ns;

	/* This mutex serializes access to the rest of the PTP
	 * hardware clock resources.
	 */
	struct mutex phc_mutex;

	struct ptp_clock *ptp_clock;
	struct ptp_clock_info ptp_clock_caps;

	struct ptp_pin_desc pin_config[MV88E6XXX_NUM_GPIO];

	u16 trig_config;
	u16 evcap_config;

	struct delayed_work tai_work;

	struct clk *xtal_in;
	struct clk *ptp_extclk;
};

enum stat_type {
	BANK0,
	BANK1,
	PORT,
};

struct mv88e6xxx_hw_stat {
	char string[ETH_GSTRING_LEN];
	int sizeof_stat;
	int reg;
	enum stat_type type;
};

int mv88e6xxx_switch_reset(struct dsa_switch *ds, bool ppu_active);
int mv88e6xxx_setup_port_common(struct dsa_switch *ds, int port);
int mv88e6xxx_setup_common(struct dsa_switch *ds);
int __mv88e6xxx_reg_read(struct mii_bus *bus, int sw_addr, int addr, int reg);
int mv88e6xxx_reg_read(struct dsa_switch *ds, int addr, int reg);
int __mv88e6xxx_reg_write(struct mii_bus *bus, int sw_addr, int addr,
			  int reg, u16 val);
int mv88e6xxx_reg_write(struct dsa_switch *ds, int addr, int reg, u16 val);
int mv88e6xxx_config_prio(struct dsa_switch *ds);
int mv88e6xxx_set_addr_direct(struct dsa_switch *ds, u8 *addr);
int mv88e6xxx_set_addr_indirect(struct dsa_switch *ds, u8 *addr);
int mv88e6xxx_phy_read(struct dsa_switch *ds, int port, int regnum);
int mv88e6xxx_phy_write(struct dsa_switch *ds, int port, int regnum, u16 val);
int mv88e6xxx_phy_read_indirect(struct dsa_switch *ds, int port, int regnum);
int mv88e6xxx_phy_write_indirect(struct dsa_switch *ds, int port, int regnum,
				 u16 val);
void mv88e6xxx_ppu_state_init(struct dsa_switch *ds);
int mv88e6xxx_phy_read_ppu(struct dsa_switch *ds, int addr, int regnum);
int mv88e6xxx_phy_write_ppu(struct dsa_switch *ds, int addr,
			    int regnum, u16 val);
void mv88e6xxx_poll_link(struct dsa_switch *ds);
void mv88e6xxx_get_strings(struct dsa_switch *ds, int port, uint8_t *data);
void mv88e6xxx_get_ethtool_stats(struct dsa_switch *ds, int port,
				 uint64_t *data);
int mv88e6xxx_get_sset_count(struct dsa_switch *ds);
int mv88e6xxx_get_sset_count_basic(struct dsa_switch *ds);
int mv88e6xxx_get_regs_len(struct dsa_switch *ds, int port);
void mv88e6xxx_get_regs(struct dsa_switch *ds, int port,
			struct ethtool_regs *regs, void *_p);
int  mv88e6xxx_get_temp(struct dsa_switch *ds, int *temp);
int mv88e6xxx_phy_wait(struct dsa_switch *ds);
int mv88e6xxx_eeprom_load_wait(struct dsa_switch *ds);
int mv88e6xxx_eeprom_busy_wait(struct dsa_switch *ds);
int mv88e6xxx_phy_read_indirect(struct dsa_switch *ds, int addr, int regnum);
int mv88e6xxx_phy_write_indirect(struct dsa_switch *ds, int addr, int regnum,
				 u16 val);
int mv88e6xxx_get_eee(struct dsa_switch *ds, int port, struct ethtool_eee *e);
int mv88e6xxx_set_eee(struct dsa_switch *ds, int port,
		      struct phy_device *phydev, struct ethtool_eee *e);
int mv88e6xxx_join_bridge(struct dsa_switch *ds, int port, u32 br_port_mask);
int mv88e6xxx_leave_bridge(struct dsa_switch *ds, int port, u32 br_port_mask);
int mv88e6xxx_port_stp_update(struct dsa_switch *ds, int port, u8 state);
int mv88e6xxx_port_fdb_add(struct dsa_switch *ds, int port,
			   const unsigned char *addr, u16 vid);
int mv88e6xxx_port_fdb_del(struct dsa_switch *ds, int port,
			   const unsigned char *addr, u16 vid);
int mv88e6xxx_port_fdb_getnext(struct dsa_switch *ds, int port,
			       unsigned char *addr, bool *is_static);
int mv88e6xxx_phy_page_read(struct dsa_switch *ds, int port, int page, int reg);
int mv88e6xxx_phy_page_write(struct dsa_switch *ds, int port, int page,
			     int reg, int val);

int mv88e6xxx_port_set_ts_config(struct dsa_switch *ds, int port,
				 struct ifreq *ifr);
int mv88e6xxx_port_get_ts_config(struct dsa_switch *ds, int port,
				 struct ifreq *ifr);
bool mv88e6xxx_port_rxtstamp(struct dsa_switch *ds, int port,
			     struct sk_buff *skb, unsigned int type);
void mv88e6xxx_port_txtstamp(struct dsa_switch *ds, int port,
			     struct sk_buff *clone, unsigned int type);
int mv88e6xxx_get_ts_info(struct dsa_switch *ds, int port,
			  struct ethtool_ts_info *info);

int mv88e6xxx_setup_phc(struct dsa_switch *ds);

extern struct dsa_switch_driver mv88e6131_switch_driver;
extern struct dsa_switch_driver mv88e6123_61_65_switch_driver;
extern struct dsa_switch_driver mv88e6352_switch_driver;
extern struct dsa_switch_driver mv88e6171_switch_driver;

#define REG_READ(addr, reg)						\
	({								\
		int __ret;						\
									\
		__ret = mv88e6xxx_reg_read(ds, addr, reg);		\
		if (__ret < 0)						\
			return __ret;					\
		__ret;							\
	})

#define REG_WRITE(addr, reg, val)					\
	({								\
		int __ret;						\
									\
		__ret = mv88e6xxx_reg_write(ds, addr, reg, val);	\
		if (__ret < 0)						\
			return __ret;					\
	})



#endif
