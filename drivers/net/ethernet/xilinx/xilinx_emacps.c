/*
 * Xilinx Ethernet: Linux driver for Ethernet.
 *
 * Author: Xilinx, Inc.
 *
 * 2010 (c) Xilinx, Inc. This file is licensed uner the terms of the GNU
 * General Public License version 2. This program is licensed "as is"
 * without any warranty of any kind, whether express or implied.
 *
 * This is a driver for xilinx processor sub-system (ps) ethernet device.
 * This driver is mainly used in Linux 2.6.30 and above and it does _not_
 * support Linux 2.4 kernel due to certain new features (e.g. NAPI) is
 * introduced in this driver.
 *
 * TODO:
 * 1. JUMBO frame is not enabled per EPs spec. Please update it if this
 *    support is added in and set MAX_MTU to 9000.
 * 2. For PEEP boards the Linux PHY driver state machine is not used. Hence
 *    no autonegotiation happens for PEEP. The speed of 100 Mbps is used and
 *    it is fixed. The speed cannot be changed to 10 Mbps or 1000 Mbps. However
 *    for Zynq there is no such issue and it can work at all 3 speeds after
 *    autonegotiation.
 * 3. The SLCR clock divisors are hard coded for PEEP board.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/platform_device.h>
#include <linux/phy.h>
#include <linux/mii.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/ethtool.h>
#include <linux/vmalloc.h>
#include <linux/version.h>
#include <linux/of.h>
#include <linux/kthread.h>
#include <linux/rtnetlink.h>
#include <mach/board.h>
#include <mach/slcr.h>

#include <linux/clocksource.h>
#include <linux/timecompare.h>
#include <linux/net_tstamp.h>
#include <linux/of_address.h>
#include <linux/of_mdio.h>

#ifdef CONFIG_FPGA_PERIPHERAL
#include <misc/fpgaperipheral.h>
#endif

/************************** Constant Definitions *****************************/

/* Must be shorter than length of ethtool_drvinfo.driver field to fit */
#define DRIVER_NAME		"xemacps"
#define DRIVER_DESCRIPTION	"Xilinx Tri-Mode Ethernet MAC driver"
#define DRIVER_VERSION		"1.00a"

/* Maximum value for hash bits. 2**6 */
#define XEMACPS_MAX_HASH_BITS	64

/* MDC clock division
 * currently supporting 8, 16, 32, 48, 64, 96, 128, 224.
 */
enum { MDC_DIV_8 = 0, MDC_DIV_16, MDC_DIV_32, MDC_DIV_48,
MDC_DIV_64, MDC_DIV_96, MDC_DIV_128, MDC_DIV_224 };

/* Specify the receive buffer size in bytes, 64, 128, 192, 10240 */
#define XEMACPS_RX_BUF_SIZE		1536

/* Number of receive buffer bytes as a unit, this is HW setup */
#define XEMACPS_RX_BUF_UNIT	64

/* Transmit and receive descriptor counts. The values are chosen to use a full
   page of memory. When allocating coherent DMA memory, it gives you full pages
   regardless of how much you ask for. We share a page between the transmit and
   receive descriptors. Descriptors are 8 bytes and a page is 4096 bytes which
   gives us 512 descriptors. We only need 64 transmit descriptors, which leaves
   448 receive descriptors. */
#define XEMACPS_SEND_BD_CNT	64
#define XEMACPS_RECV_BD_CNT	448

#define XEMACPS_NAPI_WEIGHT	64

/* Register offset definitions. Unless otherwise noted, register access is
 * 32 bit. Names are self explained here.
 */
#define XEMACPS_NWCTRL_OFFSET        0x00000000 /* Network Control reg */
#define XEMACPS_NWCFG_OFFSET         0x00000004 /* Network Config reg */
#define XEMACPS_NWSR_OFFSET          0x00000008 /* Network Status reg */
#define XEMACPS_USERIO_OFFSET        0x0000000C /* User IO reg */
#define XEMACPS_DMACR_OFFSET         0x00000010 /* DMA Control reg */
#define XEMACPS_TXSR_OFFSET          0x00000014 /* TX Status reg */
#define XEMACPS_RXQBASE_OFFSET       0x00000018 /* RX Q Base address reg */
#define XEMACPS_TXQBASE_OFFSET       0x0000001C /* TX Q Base address reg */
#define XEMACPS_RXSR_OFFSET          0x00000020 /* RX Status reg */
#define XEMACPS_ISR_OFFSET           0x00000024 /* Interrupt Status reg */
#define XEMACPS_IER_OFFSET           0x00000028 /* Interrupt Enable reg */
#define XEMACPS_IDR_OFFSET           0x0000002C /* Interrupt Disable reg */
#define XEMACPS_IMR_OFFSET           0x00000030 /* Interrupt Mask reg */
#define XEMACPS_PHYMNTNC_OFFSET      0x00000034 /* Phy Maintaince reg */
#define XEMACPS_RXPAUSE_OFFSET       0x00000038 /* RX Pause Time reg */
#define XEMACPS_TXPAUSE_OFFSET       0x0000003C /* TX Pause Time reg */
#define XEMACPS_HASHL_OFFSET         0x00000080 /* Hash Low address reg */
#define XEMACPS_HASHH_OFFSET         0x00000084 /* Hash High address reg */
#define XEMACPS_LADDR1L_OFFSET       0x00000088 /* Specific1 addr low reg */
#define XEMACPS_LADDR1H_OFFSET       0x0000008C /* Specific1 addr high reg */
#define XEMACPS_LADDR2L_OFFSET       0x00000090 /* Specific2 addr low reg */
#define XEMACPS_LADDR2H_OFFSET       0x00000094 /* Specific2 addr high reg */
#define XEMACPS_LADDR3L_OFFSET       0x00000098 /* Specific3 addr low reg */
#define XEMACPS_LADDR3H_OFFSET       0x0000009C /* Specific3 addr high reg */
#define XEMACPS_LADDR4L_OFFSET       0x000000A0 /* Specific4 addr low reg */
#define XEMACPS_LADDR4H_OFFSET       0x000000A4 /* Specific4 addr high reg */
#define XEMACPS_MATCH1_OFFSET        0x000000A8 /* Type ID1 Match reg */
#define XEMACPS_MATCH2_OFFSET        0x000000AC /* Type ID2 Match reg */
#define XEMACPS_MATCH3_OFFSET        0x000000B0 /* Type ID3 Match reg */
#define XEMACPS_MATCH4_OFFSET        0x000000B4 /* Type ID4 Match reg */
#define XEMACPS_WOL_OFFSET           0x000000B8 /* Wake on LAN reg */
#define XEMACPS_STRETCH_OFFSET       0x000000BC /* IPG Stretch reg */
#define XEMACPS_SVLAN_OFFSET         0x000000C0 /* Stacked VLAN reg */
#define XEMACPS_MODID_OFFSET         0x000000FC /* Module ID reg */
#define XEMACPS_OCTTXL_OFFSET        0x00000100 /* Octects transmitted Low
						reg */
#define XEMACPS_OCTTXH_OFFSET        0x00000104 /* Octects transmitted High
						reg */
#define XEMACPS_TXCNT_OFFSET         0x00000108 /* Error-free Frmaes
						transmitted counter */
#define XEMACPS_TXBCCNT_OFFSET       0x0000010C /* Error-free Broadcast
						Frames counter*/
#define XEMACPS_TXMCCNT_OFFSET       0x00000110 /* Error-free Multicast
						Frame counter */
#define XEMACPS_TXPAUSECNT_OFFSET    0x00000114 /* Pause Frames Transmitted
						Counter */
#define XEMACPS_TX64CNT_OFFSET       0x00000118 /* Error-free 64 byte Frames
						Transmitted counter */
#define XEMACPS_TX65CNT_OFFSET       0x0000011C /* Error-free 65-127 byte
						Frames Transmitted counter */
#define XEMACPS_TX128CNT_OFFSET      0x00000120 /* Error-free 128-255 byte
						Frames Transmitted counter */
#define XEMACPS_TX256CNT_OFFSET      0x00000124 /* Error-free 256-511 byte
						Frames transmitted counter */
#define XEMACPS_TX512CNT_OFFSET      0x00000128 /* Error-free 512-1023 byte
						Frames transmitted counter */
#define XEMACPS_TX1024CNT_OFFSET     0x0000012C /* Error-free 1024-1518 byte
						Frames transmitted counter */
#define XEMACPS_TX1519CNT_OFFSET     0x00000130 /* Error-free larger than 1519
						byte Frames transmitted
						   counter */
#define XEMACPS_TXURUNCNT_OFFSET     0x00000134 /* TX under run error
						    counter */
#define XEMACPS_SNGLCOLLCNT_OFFSET   0x00000138 /* Single Collision Frame
						Counter */
#define XEMACPS_MULTICOLLCNT_OFFSET  0x0000013C /* Multiple Collision Frame
						Counter */
#define XEMACPS_EXCESSCOLLCNT_OFFSET 0x00000140 /* Excessive Collision Frame
						Counter */
#define XEMACPS_LATECOLLCNT_OFFSET   0x00000144 /* Late Collision Frame
						Counter */
#define XEMACPS_TXDEFERCNT_OFFSET    0x00000148 /* Deferred Transmission
						Frame Counter */
#define XEMACPS_CSENSECNT_OFFSET     0x0000014C /* Carrier Sense Error
						Counter */
#define XEMACPS_OCTRXL_OFFSET        0x00000150 /* Octects Received register
						Low */
#define XEMACPS_OCTRXH_OFFSET        0x00000154 /* Octects Received register
						High */
#define XEMACPS_RXCNT_OFFSET         0x00000158 /* Error-free Frames
						Received Counter */
#define XEMACPS_RXBROADCNT_OFFSET    0x0000015C /* Error-free Broadcast
						Frames Received Counter */
#define XEMACPS_RXMULTICNT_OFFSET    0x00000160 /* Error-free Multicast
						Frames Received Counter */
#define XEMACPS_RXPAUSECNT_OFFSET    0x00000164 /* Pause Frames
						Received Counter */
#define XEMACPS_RX64CNT_OFFSET       0x00000168 /* Error-free 64 byte Frames
						Received Counter */
#define XEMACPS_RX65CNT_OFFSET       0x0000016C /* Error-free 65-127 byte
						Frames Received Counter */
#define XEMACPS_RX128CNT_OFFSET      0x00000170 /* Error-free 128-255 byte
						Frames Received Counter */
#define XEMACPS_RX256CNT_OFFSET      0x00000174 /* Error-free 256-512 byte
						Frames Received Counter */
#define XEMACPS_RX512CNT_OFFSET      0x00000178 /* Error-free 512-1023 byte
						Frames Received Counter */
#define XEMACPS_RX1024CNT_OFFSET     0x0000017C /* Error-free 1024-1518 byte
						Frames Received Counter */
#define XEMACPS_RX1519CNT_OFFSET     0x00000180 /* Error-free 1519-max byte
						Frames Received Counter */
#define XEMACPS_RXUNDRCNT_OFFSET     0x00000184 /* Undersize Frames Received
						Counter */
#define XEMACPS_RXOVRCNT_OFFSET      0x00000188 /* Oversize Frames Received
						Counter */
#define XEMACPS_RXJABCNT_OFFSET      0x0000018C /* Jabbers Received
						Counter */
#define XEMACPS_RXFCSCNT_OFFSET      0x00000190 /* Frame Check Sequence
						Error Counter */
#define XEMACPS_RXLENGTHCNT_OFFSET   0x00000194 /* Length Field Error
						Counter */
#define XEMACPS_RXSYMBCNT_OFFSET     0x00000198 /* Symbol Error Counter */
#define XEMACPS_RXALIGNCNT_OFFSET    0x0000019C /* Alignment Error Counter */
#define XEMACPS_RXRESERRCNT_OFFSET   0x000001A0 /* Receive Resource Error
						Counter */
#define XEMACPS_RXORCNT_OFFSET       0x000001A4 /* Receive Overrun Counter */
#define XEMACPS_RXIPCCNT_OFFSET      0x000001A8 /* IP header Checksum Error
						Counter */
#define XEMACPS_RXTCPCCNT_OFFSET     0x000001AC /* TCP Checksum Error
						Counter */
#define XEMACPS_RXUDPCCNT_OFFSET     0x000001B0 /* UDP Checksum Error
						Counter */

#define XEMACPS_1588S_OFFSET         0x000001D0 /* 1588 Timer Seconds */
#define XEMACPS_1588NS_OFFSET        0x000001D4 /* 1588 Timer Nanoseconds */
#define XEMACPS_1588ADJ_OFFSET       0x000001D8 /* 1588 Timer Adjust */
#define XEMACPS_1588INC_OFFSET       0x000001DC /* 1588 Timer Increment */
#define XEMACPS_PTPETXS_OFFSET       0x000001E0 /* PTP Event Frame
						Transmitted Seconds */
#define XEMACPS_PTPETXNS_OFFSET      0x000001E4 /* PTP Event Frame
						Transmitted Nanoseconds */
#define XEMACPS_PTPERXS_OFFSET       0x000001E8 /* PTP Event Frame Received
						Seconds */
#define XEMACPS_PTPERXNS_OFFSET      0x000001EC /* PTP Event Frame Received
						Nanoseconds */
#define XEMACPS_PTPPTXS_OFFSET       0x000001E0 /* PTP Peer Frame
						Transmitted Seconds */
#define XEMACPS_PTPPTXNS_OFFSET      0x000001E4 /* PTP Peer Frame
						Transmitted Nanoseconds */
#define XEMACPS_PTPPRXS_OFFSET       0x000001E8 /* PTP Peer Frame Received
						Seconds */
#define XEMACPS_PTPPRXNS_OFFSET      0x000001EC /* PTP Peer Frame Received
						Nanoseconds */

/* network control register bit definitions */
#define XEMACPS_NWCTRL_RXTSTAMP_MASK    0x00008000 /* RX Timestamp in CRC */
#define XEMACPS_NWCTRL_ZEROPAUSETX_MASK 0x00001000 /* Transmit zero quantum
						pause frame */
#define XEMACPS_NWCTRL_PAUSETX_MASK     0x00000800 /* Transmit pause frame */
#define XEMACPS_NWCTRL_HALTTX_MASK      0x00000400 /* Halt transmission
						after current frame */
#define XEMACPS_NWCTRL_STARTTX_MASK     0x00000200 /* Start tx (tx_go) */

#define XEMACPS_NWCTRL_STATWEN_MASK     0x00000080 /* Enable writing to
						stat counters */
#define XEMACPS_NWCTRL_STATINC_MASK     0x00000040 /* Increment statistic
						registers */
#define XEMACPS_NWCTRL_STATCLR_MASK     0x00000020 /* Clear statistic
						registers */
#define XEMACPS_NWCTRL_MDEN_MASK        0x00000010 /* Enable MDIO port */
#define XEMACPS_NWCTRL_TXEN_MASK        0x00000008 /* Enable transmit */
#define XEMACPS_NWCTRL_RXEN_MASK        0x00000004 /* Enable receive */
#define XEMACPS_NWCTRL_LOOPEN_MASK      0x00000002 /* local loopback */

/* name network configuration register bit definitions */
#define XEMACPS_NWCFG_BADPREAMBEN_MASK 0x20000000 /* disable rejection of
						non-standard preamble */
#define XEMACPS_NWCFG_IPDSTRETCH_MASK  0x10000000 /* enable transmit IPG */
#define XEMACPS_NWCFG_FCSIGNORE_MASK   0x04000000 /* disable rejection of
						FCS error */
#define XEMACPS_NWCFG_HDRXEN_MASK      0x02000000 /* RX half duplex */
#define XEMACPS_NWCFG_RXCHKSUMEN_MASK  0x01000000 /* enable RX checksum
						offload */
#define XEMACPS_NWCFG_PAUSECOPYDI_MASK 0x00800000 /* Do not copy pause
						Frames to memory */
#define XEMACPS_NWCFG_MDC_SHIFT_MASK   18         /* shift bits for MDC */
#define XEMACPS_NWCFG_MDCCLKDIV_MASK   0x001C0000 /* MDC Mask PCLK divisor */
#define XEMACPS_NWCFG_FCSREM_MASK      0x00020000 /* Discard FCS from
						received frames */
#define XEMACPS_NWCFG_LENGTHERRDSCRD_MASK 0x00010000
/* RX length error discard */
#define XEMACPS_NWCFG_RXOFFS_MASK      0x0000C000 /* RX buffer offset */
#define XEMACPS_NWCFG_PAUSEEN_MASK     0x00002000 /* Enable pause TX */
#define XEMACPS_NWCFG_RETRYTESTEN_MASK 0x00001000 /* Retry test */
#define XEMACPS_NWCFG_1000_MASK        0x00000400 /* Gigbit mode */
#define XEMACPS_NWCFG_EXTADDRMATCHEN_MASK 0x00000200
/* External address match enable */
#define XEMACPS_NWCFG_UCASTHASHEN_MASK 0x00000080 /* Receive unicast hash
						frames */
#define XEMACPS_NWCFG_MCASTHASHEN_MASK 0x00000040 /* Receive multicast hash
						frames */
#define XEMACPS_NWCFG_BCASTDI_MASK     0x00000020 /* Do not receive
						broadcast frames */
#define XEMACPS_NWCFG_COPYALLEN_MASK   0x00000010 /* Copy all frames */

#define XEMACPS_NWCFG_NVLANDISC_MASK   0x00000004 /* Receive only VLAN
						frames */
#define XEMACPS_NWCFG_FDEN_MASK        0x00000002 /* Full duplex */
#define XEMACPS_NWCFG_100_MASK         0x00000001 /* 10 or 100 Mbs */

/* network status register bit definitaions */
#define XEMACPS_NWSR_MDIOIDLE_MASK     0x00000004 /* PHY management idle */
#define XEMACPS_NWSR_MDIO_MASK         0x00000002 /* Status of mdio_in */

/* MAC address register word 1 mask */
#define XEMACPS_LADDR_MACH_MASK        0x0000FFFF /* Address bits[47:32]
						bit[31:0] are in BOTTOM */

/* DMA control register bit definitions */
#define XEMACPS_DMACR_RXBUF_MASK     0x00FF0000 /* Mask bit for RX buffer
						size */
#define XEMACPS_DMACR_RXBUF_SHIFT    16         /* Shift bit for RX buffer
						size */
#define XEMACPS_DMACR_TCPCKSUM_MASK  0x00000800 /* enable/disable TX
						checksum offload */
#define XEMACPS_DMACR_TXSIZE_MASK    0x00000400 /* TX buffer memory size */
#define XEMACPS_DMACR_RXSIZE_MASK    0x00000300 /* RX buffer memory size */
#define XEMACPS_DMACR_ENDIAN_MASK    0x00000080 /* Endian configuration */
#define XEMACPS_DMACR_BLENGTH_MASK   0x0000001F /* Buffer burst length */
#define XEMACPS_DMACR_BLENGTH_INCR16 0x00000010 /* Buffer burst length */
#define XEMACPS_DMACR_BLENGTH_INCR8  0x00000008 /* Buffer burst length */
#define XEMACPS_DMACR_BLENGTH_INCR4  0x00000004 /* Buffer burst length */
#define XEMACPS_DMACR_BLENGTH_SINGLE 0x00000002 /* Buffer burst length */

/* transmit status register bit definitions */
#define XEMACPS_TXSR_HRESPNOK_MASK   0x00000100 /* Transmit hresp not OK */
#define XEMACPS_TXSR_COL1000_MASK    0x00000080 /* Collision Gbs mode */
#define XEMACPS_TXSR_URUN_MASK       0x00000040 /* Transmit underrun */
#define XEMACPS_TXSR_TXCOMPL_MASK    0x00000020 /* Transmit completed OK */
#define XEMACPS_TXSR_BUFEXH_MASK     0x00000010 /* Transmit buffs exhausted
						mid frame */
#define XEMACPS_TXSR_TXGO_MASK       0x00000008 /* Status of go flag */
#define XEMACPS_TXSR_RXOVR_MASK      0x00000004 /* Retry limit exceeded */
#define XEMACPS_TXSR_COL100_MASK     0x00000002 /* Collision 10/100  mode */
#define XEMACPS_TXSR_USEDREAD_MASK   0x00000001 /* TX buffer used bit set */

#define XEMACPS_TXSR_ERROR_MASK	(XEMACPS_TXSR_HRESPNOK_MASK | \
					XEMACPS_TXSR_COL1000_MASK | \
					XEMACPS_TXSR_URUN_MASK |   \
					XEMACPS_TXSR_BUFEXH_MASK | \
					XEMACPS_TXSR_RXOVR_MASK |  \
					XEMACPS_TXSR_COL100_MASK | \
					XEMACPS_TXSR_USEDREAD_MASK)

/* receive status register bit definitions */
#define XEMACPS_RXSR_HRESPNOK_MASK   0x00000008 /* Receive hresp not OK */
#define XEMACPS_RXSR_RXOVR_MASK      0x00000004 /* Receive overrun */
#define XEMACPS_RXSR_FRAMERX_MASK    0x00000002 /* Frame received OK */
#define XEMACPS_RXSR_BUFFNA_MASK     0x00000001 /* RX buffer used bit set */

#define XEMACPS_RXSR_ERROR_MASK	(XEMACPS_RXSR_HRESPNOK_MASK | \
					XEMACPS_RXSR_RXOVR_MASK | \
					XEMACPS_RXSR_BUFFNA_MASK)

/* interrupts bit definitions
 * Bits definitions are same in XEMACPS_ISR_OFFSET,
 * XEMACPS_IER_OFFSET, XEMACPS_IDR_OFFSET, and XEMACPS_IMR_OFFSET
 */
#define XEMACPS_IXR_PTPPSTX_MASK    0x02000000	/* PTP Psync transmitted */
#define XEMACPS_IXR_PTPPDRTX_MASK   0x01000000	/* PTP Pdelay_req transmitted */
#define XEMACPS_IXR_PTPSTX_MASK     0x00800000	/* PTP Sync transmitted */
#define XEMACPS_IXR_PTPDRTX_MASK    0x00400000	/* PTP Delay_req transmitted */
#define XEMACPS_IXR_PTPPSRX_MASK    0x00200000	/* PTP Psync received */
#define XEMACPS_IXR_PTPPDRRX_MASK   0x00100000	/* PTP Pdelay_req received */
#define XEMACPS_IXR_PTPSRX_MASK     0x00080000	/* PTP Sync received */
#define XEMACPS_IXR_PTPDRRX_MASK    0x00040000	/* PTP Delay_req received */
#define XEMACPS_IXR_PAUSETX_MASK    0x00004000	/* Pause frame transmitted */
#define XEMACPS_IXR_PAUSEZERO_MASK  0x00002000	/* Pause time has reached
						zero */
#define XEMACPS_IXR_PAUSENZERO_MASK 0x00001000	/* Pause frame received */
#define XEMACPS_IXR_HRESPNOK_MASK   0x00000800	/* hresp not ok */
#define XEMACPS_IXR_RXOVR_MASK      0x00000400	/* Receive overrun occurred */
#define XEMACPS_IXR_TXCOMPL_MASK    0x00000080	/* Frame transmitted ok */
#define XEMACPS_IXR_TXEXH_MASK      0x00000040	/* Transmit err occurred or
						no buffers*/
#define XEMACPS_IXR_RETRY_MASK      0x00000020	/* Retry limit exceeded */
#define XEMACPS_IXR_URUN_MASK       0x00000010	/* Transmit underrun */
#define XEMACPS_IXR_TXUSED_MASK     0x00000008	/* Tx buffer used bit read */
#define XEMACPS_IXR_RXUSED_MASK     0x00000004	/* Rx buffer used bit read */
#define XEMACPS_IXR_FRAMERX_MASK    0x00000002	/* Frame received ok */
#define XEMACPS_IXR_MGMNT_MASK      0x00000001	/* PHY management complete */

#define XEMACPS_IXR_ALL_MASK	(XEMACPS_IXR_FRAMERX_MASK | \
				 XEMACPS_IXR_RX_ERR_MASK)

#define XEMACPS_IXR_RX_ERR_MASK	(XEMACPS_IXR_HRESPNOK_MASK | \
					XEMACPS_IXR_RXUSED_MASK |  \
					XEMACPS_IXR_RXOVR_MASK)
/* PHY Maintenance bit definitions */
#define XEMACPS_PHYMNTNC_OP_MASK    0x40020000	/* operation mask bits */
#define XEMACPS_PHYMNTNC_OP_R_MASK  0x20000000	/* read operation */
#define XEMACPS_PHYMNTNC_OP_W_MASK  0x10000000	/* write operation */
#define XEMACPS_PHYMNTNC_ADDR_MASK  0x0F800000	/* Address bits */
#define XEMACPS_PHYMNTNC_REG_MASK   0x007C0000	/* register bits */
#define XEMACPS_PHYMNTNC_DATA_MASK  0x0000FFFF	/* data bits */
#define XEMACPS_PHYMNTNC_PHYAD_SHIFT_MASK   23	/* Shift bits for PHYAD */
#define XEMACPS_PHYMNTNC_PHREG_SHIFT_MASK   18	/* Shift bits for PHREG */

/* Wake on LAN bit definition */
#define XEMACPS_WOL_MCAST_MASK      0x00080000
#define XEMACPS_WOL_SPEREG1_MASK    0x00040000
#define XEMACPS_WOL_ARP_MASK        0x00020000
#define XEMACPS_WOL_MAGIC_MASK      0x00010000
#define XEMACPS_WOL_ARP_ADDR_MASK   0x0000FFFF

/* Buffer descriptor status words offset */
#define XEMACPS_BD_ADDR_OFFSET     0x00000000 /**< word 0/addr of BDs */
#define XEMACPS_BD_STAT_OFFSET     0x00000004 /**< word 1/status of BDs */

/* Transmit buffer descriptor status words bit positions.
 * Transmit buffer descriptor consists of two 32-bit registers,
 * the first - word0 contains a 32-bit address pointing to the location of
 * the transmit data.
 * The following register - word1, consists of various information to
 * control transmit process.  After transmit, this is updated with status
 * information, whether the frame was transmitted OK or why it had failed.
 */
#define XEMACPS_TXBUF_USED_MASK  0x80000000 /* Used bit. */
#define XEMACPS_TXBUF_WRAP_MASK  0x40000000 /* Wrap bit, last descriptor */
#define XEMACPS_TXBUF_RETRY_MASK 0x20000000 /* Retry limit exceeded */
#define XEMACPS_TXBUF_EXH_MASK   0x08000000 /* Buffers exhausted */
#define XEMACPS_TXBUF_LAC_MASK   0x04000000 /* Late collision. */
#define XEMACPS_TXBUF_NOCRC_MASK 0x00010000 /* No CRC */
#define XEMACPS_TXBUF_LAST_MASK  0x00008000 /* Last buffer */
#define XEMACPS_TXBUF_LEN_MASK   0x00003FFF /* Mask for length field */

#define XEMACPS_TXBUF_ERR_MASK   0x3C000000 /* Mask for length field */

/* Receive buffer descriptor status words bit positions.
 * Receive buffer descriptor consists of two 32-bit registers,
 * the first - word0 contains a 32-bit word aligned address pointing to the
 * address of the buffer. The lower two bits make up the wrap bit indicating
 * the last descriptor and the ownership bit to indicate it has been used.
 * The following register - word1, contains status information regarding why
 * the frame was received (the filter match condition) as well as other
 * useful info.
 */
#define XEMACPS_RXBUF_BCAST_MASK     0x80000000 /* Broadcast frame */
#define XEMACPS_RXBUF_MULTIHASH_MASK 0x40000000 /* Multicast hashed frame */
#define XEMACPS_RXBUF_UNIHASH_MASK   0x20000000 /* Unicast hashed frame */
#define XEMACPS_RXBUF_EXH_MASK       0x08000000 /* buffer exhausted */
#define XEMACPS_RXBUF_AMATCH_MASK    0x06000000 /* Specific address
						matched */
#define XEMACPS_RXBUF_IDFOUND_MASK   0x01000000 /* Type ID matched */
#define XEMACPS_RXBUF_IDMATCH_MASK   0x00C00000 /* ID matched mask */
#define XEMACPS_RXBUF_VLAN_MASK      0x00200000 /* VLAN tagged */
#define XEMACPS_RXBUF_PRI_MASK       0x00100000 /* Priority tagged */
#define XEMACPS_RXBUF_VPRI_MASK      0x000E0000 /* Vlan priority */
#define XEMACPS_RXBUF_CFI_MASK       0x00010000 /* CFI frame */
#define XEMACPS_RXBUF_EOF_MASK       0x00008000 /* End of frame. */
#define XEMACPS_RXBUF_SOF_MASK       0x00004000 /* Start of frame. */
#define XEMACPS_RXBUF_BAD_FCS        0x00002000 /* Frame has bad FCS */
#define XEMACPS_RXBUF_LEN_MASK       0x00001FFF /* Mask for length field */

#define XEMACPS_RXBUF_WRAP_MASK      0x00000002 /* Wrap bit, last BD */
#define XEMACPS_RXBUF_NEW_MASK       0x00000001 /* Used bit.. */
#define XEMACPS_RXBUF_ADD_MASK       0xFFFFFFFC /* Mask for address */


#define XSLCR_EMAC0_RCLK_CTRL_OFFSET	0x138 /* EMAC0 Rx Clk Control */
#define XSLCR_EMAC1_RCLK_CTRL_OFFSET	0x13C /* EMAC1 Rx Clk Control */

#define XSLCR_EMAC0_CLK_CTRL_OFFSET	0x140 /* EMAC0 Reference Clk Control */
#define XSLCR_EMAC1_CLK_CTRL_OFFSET	0x144 /* EMAC1 Reference Clk Control */

#define XSLCR_FPGA0_CLK_CTRL_OFFSET	0x170 /* PL Clock 0 Output Control */
#define XSLCR_FPGA1_CLK_CTRL_OFFSET	0x180 /* PL Clock 1 Output Control */
#define XSLCR_FPGA2_CLK_CTRL_OFFSET	0x190 /* PL Clock 2 Output Control */
#define XSLCR_FPGA3_CLK_CTRL_OFFSET	0x1A0 /* PL Clock 3 Output Control */

#define XSLCR_PSS_IDCODE		0x530 /* PS IDCODE */

#define XSLCR_PSS_IDCODE_REVISION_MASK	0xF0000000
#define XSLCR_PSS_IDCODE_REVISION_SHIFT	28

#define BOARD_TYPE_ZYNQ			0x01
#define BOARD_TYPE_PEEP			0x02

#define XEMACPS_DFLT_SLCR_DIV0_1000	8
#define XEMACPS_DFLT_SLCR_DIV1_1000	1
#define XEMACPS_DFLT_SLCR_DIV0_100	8
#define XEMACPS_DFLT_SLCR_DIV1_100	5
#define XEMACPS_DFLT_SLCR_DIV0_10	8
#define XEMACPS_DFLT_SLCR_DIV1_10	50
#define XEMACPS_SLCR_DIV_MASK		0xFC0FC0FF

/* State bits that can be set in net_local flags member. */
#define XEMACPS_STATE_DOWN	0
#define XEMACPS_STATE_RESET	1
#ifdef CONFIG_FPGA_PERIPHERAL
#define XEMACPS_STATE_FPGA_DOWN	2
#endif

#ifdef CONFIG_XILINX_PS_EMAC_HWTSTAMP
#define NS_PER_SEC 	1000000000ULL	/* Nanoseconds per second */
#define PEEP_TSU_CLK  	50000000ULL	/* PTP TSU CLOCK */
#endif

#define xemacps_read(base, reg)	\
	__raw_readl((u32)(base) + (u32)(reg))
#define xemacps_write(base, reg, val)	\
	__raw_writel((val), (u32)(base) + (u32)(reg))

#define XEMACPS_SET_BUFADDR_RX(bdptr, addr)				\
	xemacps_write((bdptr), XEMACPS_BD_ADDR_OFFSET,		\
	((xemacps_read((bdptr), XEMACPS_BD_ADDR_OFFSET) &		\
	~XEMACPS_RXBUF_ADD_MASK) | (u32)(addr)))

struct ring_info {
	struct sk_buff *skb;
	dma_addr_t     mapping;
};

/* DMA buffer descriptor structure. Each BD is two words */
struct xemacps_bd {
	u32 addr;
	u32 ctrl;
};


/* Describes the name and offset of an individual statistic register, as
   returned by `ethtool -S`. Also describes which net_device_stats statistics
   this register should contribute to. */
struct xemacps_statistic {
	char stat_string[ETH_GSTRING_LEN];
	int offset;
	u32 stat_bits;
};

/* Bitfield defs for net_device_stat statistics */
#define NDS_RXERR		1<<0
#define NDS_RXLENERR		1<<1
#define NDS_RXOVERERR		1<<2
#define NDS_RXCRCERR		1<<3
#define NDS_RXFRAMEERR		1<<4
#define NDS_RXFIFOERR		1<<5
#define NDS_TXERR		1<<6
#define NDS_TXABORTEDERR	1<<7
#define NDS_TXCARRIERERR	1<<8
#define NDS_TXFIFOERR		1<<9
#define NDS_COLLISIONS		1<<10

#define XEMACPS_STAT(name) XEMACPS_STAT_TITLE_BITS(name, #name, 0)
#define XEMACPS_STAT_TITLE(name, title) XEMACPS_STAT_TITLE_BITS(name, title, 0)
#define XEMACPS_STAT_BITS(name, bits) XEMACPS_STAT_TITLE_BITS(name, #name, bits)
#define XEMACPS_STAT_TITLE_BITS(name, title, bits) { 	\
	.stat_string = title,				\
	.offset = XEMACPS_##name##_OFFSET,		\
	.stat_bits = bits				\
}

/* list of xemacps statistic registers. The names MUST match the
   corresponding XEMACPS_*_OFFSET definitions. */
static const struct xemacps_statistic xemacps_statistics[] = {
	XEMACPS_STAT_TITLE(OCTTXL, "OCTTX"),
	/* OCTTXH is read by OCTTXL; cf xemacps_update_stats */
	XEMACPS_STAT(TXCNT),
	XEMACPS_STAT(TXBCCNT),
	XEMACPS_STAT(TXMCCNT),
	XEMACPS_STAT(TXPAUSECNT),
	XEMACPS_STAT(TX64CNT),
	XEMACPS_STAT(TX65CNT),
	XEMACPS_STAT(TX128CNT),
	XEMACPS_STAT(TX256CNT),
	XEMACPS_STAT(TX512CNT),
	XEMACPS_STAT(TX1024CNT),
	XEMACPS_STAT(TX1519CNT),
	XEMACPS_STAT_BITS(TXURUNCNT, NDS_TXERR|NDS_TXFIFOERR),
	XEMACPS_STAT_BITS(SNGLCOLLCNT, NDS_TXERR|NDS_COLLISIONS),
	XEMACPS_STAT_BITS(MULTICOLLCNT, NDS_TXERR|NDS_COLLISIONS),
	XEMACPS_STAT_BITS(EXCESSCOLLCNT,
		NDS_TXERR|NDS_TXABORTEDERR|NDS_COLLISIONS),
	XEMACPS_STAT_BITS(LATECOLLCNT, NDS_TXERR|NDS_COLLISIONS),
	XEMACPS_STAT(TXDEFERCNT),
	XEMACPS_STAT_BITS(CSENSECNT, NDS_TXERR|NDS_TXCARRIERERR),
	XEMACPS_STAT_TITLE(OCTRXL, "OCTRX"),
	/* OCTRXH is read by OCTRXL; cf xemacps_update_stats */
	XEMACPS_STAT(RXCNT),
	XEMACPS_STAT(RXBROADCNT),
	XEMACPS_STAT(RXMULTICNT),
	XEMACPS_STAT(RXPAUSECNT),
	XEMACPS_STAT(RX64CNT),
	XEMACPS_STAT(RX65CNT),
	XEMACPS_STAT(RX128CNT),
	XEMACPS_STAT(RX256CNT),
	XEMACPS_STAT(RX512CNT),
	XEMACPS_STAT(RX1024CNT),
	XEMACPS_STAT(RX1519CNT),
	XEMACPS_STAT_BITS(RXUNDRCNT, NDS_RXERR|NDS_RXLENERR),
	XEMACPS_STAT_BITS(RXOVRCNT, NDS_RXERR|NDS_RXLENERR),
	XEMACPS_STAT_BITS(RXJABCNT, NDS_RXERR|NDS_RXLENERR),
	XEMACPS_STAT_BITS(RXFCSCNT, NDS_RXERR|NDS_RXCRCERR),
	XEMACPS_STAT_BITS(RXLENGTHCNT, NDS_RXERR|NDS_RXLENERR),
	XEMACPS_STAT_BITS(RXSYMBCNT, NDS_RXERR),
	XEMACPS_STAT_BITS(RXALIGNCNT, NDS_RXERR|NDS_RXFRAMEERR),
	XEMACPS_STAT_BITS(RXRESERRCNT, NDS_RXERR|NDS_RXOVERERR),
	XEMACPS_STAT_BITS(RXORCNT, NDS_RXERR|NDS_RXFIFOERR),
	XEMACPS_STAT_BITS(RXIPCCNT, NDS_RXERR),
	XEMACPS_STAT_BITS(RXTCPCCNT, NDS_RXERR),
	XEMACPS_STAT_BITS(RXUDPCCNT, NDS_RXERR),
};
#define XEMACPS_STATS_LEN ARRAY_SIZE(xemacps_statistics)

/* Our private device data. */
struct net_local {
	void   __iomem         *baseaddr;
	struct device_node     *phy_node;
	struct ring_info       tx_skb[XEMACPS_SEND_BD_CNT];
	struct ring_info       rx_skb[XEMACPS_RECV_BD_CNT];

	struct xemacps_bd      *rx_bd;        /* virtual address */
	struct xemacps_bd      *tx_bd;        /* virtual address */

	dma_addr_t             rx_bd_dma;     /* physical address */
	dma_addr_t             tx_bd_dma;     /* physical address */

	u32                    tx_bd_ci;
	u32                    tx_bd_tail;
	u32                    rx_bd_ci;

	u32                    tx_bd_freecnt;

	bool                   needs_tx_stall_workaround;

	unsigned long          flags;

	struct platform_device *pdev;
	struct net_device      *ndev;   /* this device */

	struct napi_struct     napi;    /* napi information for device */
	struct net_device_stats stats;  /* Statistics for this device */

	spinlock_t             nwctrl_lock;
	u32                    nwctrl_base;

	unsigned long          tx_task_start_jiffies;
	struct delayed_work    tx_task;
	struct timer_list      tx_timer;
	bool                   rx_error;
	int                    rx_reset;
	unsigned long          rx_last_jiffies;
	struct timer_list      rx_timer;
	struct work_struct     reset_task;

#ifdef CONFIG_FPGA_PERIPHERAL
	struct notifier_block  fpga_notifier;
#endif
	int                    ni_polling_interval;
	int                    ni_polling_policy;
	int                    ni_polling_priority;

	struct task_struct     *ni_polling_task;

	/* Manage internal timer for packet timestamping */
	struct cyclecounter    cycles;
	struct timecounter     clock;
	struct timecompare     compare;
	struct hwtstamp_config hwtstamp_config;

	struct mii_bus         *mii_bus;
	struct phy_device      *phy_dev;
	unsigned int           link;
	unsigned int           speed;
	unsigned int           duplex;
	/* RX ip/tcp/udp checksum */
	unsigned               ip_summed;
	unsigned int 	       board_type;
	unsigned int 	       mdc_clk_div;
	unsigned int 	       slcr_div_reg;
	unsigned int 	       slcr_div0_1000Mbps;
	unsigned int 	       slcr_div1_1000Mbps;
	unsigned int 	       slcr_div0_100Mbps;
	unsigned int 	       slcr_div1_100Mbps;
	unsigned int 	       slcr_div0_10Mbps;
	unsigned int 	       slcr_div1_10Mbps;
	int                    gpiospeed;
#ifdef CONFIG_XILINX_PS_EMAC_HWTSTAMP
	unsigned int 	       ptpenetclk;
#endif
	u64                    ethtool_stats[XEMACPS_STATS_LEN];
};

static struct net_device_ops netdev_ops;

/**
 * xemacps_mdio_read - Read current value of phy register indicated by
 * phyreg.
 * @bus: mdio bus
 * @mii_id: mii id
 * @phyreg: phy register to be read
 *
 * @return: value read from specified phy register.
 *
 * note: This is for 802.3 clause 22 phys access. For 802.3 clause 45 phys
 * access, set bit 30 to be 1. e.g. change XEMACPS_PHYMNTNC_OP_MASK to
 * 0x00020000.
 */
static int xemacps_mdio_read(struct mii_bus *bus, int mii_id, int phyreg)
{
	struct net_local *lp = bus->priv;
	u32 regval;
	int value;
	volatile u32 ipisr;

	regval  = XEMACPS_PHYMNTNC_OP_MASK;
	regval |= XEMACPS_PHYMNTNC_OP_R_MASK;
	regval |= (mii_id << XEMACPS_PHYMNTNC_PHYAD_SHIFT_MASK);
	regval |= (phyreg << XEMACPS_PHYMNTNC_PHREG_SHIFT_MASK);

	xemacps_write(lp->baseaddr, XEMACPS_PHYMNTNC_OFFSET, regval);

	/* wait for end of transfer */
	do {
		cpu_relax();
		ipisr = xemacps_read(lp->baseaddr, XEMACPS_NWSR_OFFSET);
	} while ((ipisr & XEMACPS_NWSR_MDIOIDLE_MASK) == 0);

	value = xemacps_read(lp->baseaddr, XEMACPS_PHYMNTNC_OFFSET) &
			XEMACPS_PHYMNTNC_DATA_MASK;

	return value;
}

/**
 * xemacps_mdio_write - Write passed in value to phy register indicated
 * by phyreg.
 * @bus: mdio bus
 * @mii_id: mii id
 * @phyreg: phy register to be configured.
 * @value: value to be written to phy register.
 * return 0. This API requires to be int type or compile warning generated
 *
 * note: This is for 802.3 clause 22 phys access. For 802.3 clause 45 phys
 * access, set bit 30 to be 1. e.g. change XEMACPS_PHYMNTNC_OP_MASK to
 * 0x00020000.
 */
static int xemacps_mdio_write(struct mii_bus *bus, int mii_id, int phyreg,
	u16 value)
{
	struct net_local *lp = bus->priv;
	u32 regval;
	volatile u32 ipisr;

	regval  = XEMACPS_PHYMNTNC_OP_MASK;
	regval |= XEMACPS_PHYMNTNC_OP_W_MASK;
	regval |= (mii_id << XEMACPS_PHYMNTNC_PHYAD_SHIFT_MASK);
	regval |= (phyreg << XEMACPS_PHYMNTNC_PHREG_SHIFT_MASK);
	regval |= value;

	xemacps_write(lp->baseaddr, XEMACPS_PHYMNTNC_OFFSET, regval);

	/* wait for end of transfer */
	do {
		cpu_relax();
		ipisr = xemacps_read(lp->baseaddr, XEMACPS_NWSR_OFFSET);
	} while ((ipisr & XEMACPS_NWSR_MDIOIDLE_MASK) == 0);

	return 0;
}


/**
 * xemacps_mdio_reset - mdio reset. It seems to be required per open
 * source documentation phy.txt. But there is no reset in this device.
 * Provide function API for now.
 * @bus: mdio bus
 **/
static int xemacps_mdio_reset(struct mii_bus *bus)
{
	return 0;
}

static void xemacps_phy_init(struct net_device *ndev)
{
	struct net_local *lp = netdev_priv(ndev);
	u16 regval;
	int i = 0;

	/* set RX delay */
	regval = xemacps_mdio_read(lp->mii_bus, lp->phy_dev->addr, 20);
	/* 0x0080 for 100Mbps, 0x0060 for 1Gbps. */
	regval |= 0x0080;
	xemacps_mdio_write(lp->mii_bus, lp->phy_dev->addr, 20, regval);

	/* 0x2100 for 100Mbps, 0x0140 for 1Gbps. */
	xemacps_mdio_write(lp->mii_bus, lp->phy_dev->addr, 0, 0x2100);

	regval = xemacps_mdio_read(lp->mii_bus, lp->phy_dev->addr, 0);
	regval |= 0x8000;
	xemacps_mdio_write(lp->mii_bus, lp->phy_dev->addr, 0, regval);
	for (i = 0; i < 10; i++)
		mdelay(500);

#ifdef VERBOSE_DEBUG
	printk(KERN_DEBUG "GEM: phy register dump, start from 0, four in a row.");
	for (i = 0; i <= 30; i++) {
		if (!(i%4))
			printk("\n %02d:  ", i);
		regval = xemacps_mdio_read(lp->mii_bus, lp->phy_dev->addr, i);
		printk(" 0x%08x", regval);
	}
	printk("\n");
#endif
}

/**
 * xemacps_adjust_link - handles link status changes, such as speed,
 * duplex, up/down, ...
 * @ndev: network device
 */
static void xemacps_adjust_link(struct net_device *ndev)
{
	struct net_local *lp = netdev_priv(ndev);
	struct phy_device *phydev = lp->phy_dev;
	int status_change = 0;
	u32 regval;
	u32 regval1;

	if (test_bit(XEMACPS_STATE_DOWN, &lp->flags))
		return;

	regval1 = xslcr_read(lp->slcr_div_reg);
	regval1 &= XEMACPS_SLCR_DIV_MASK;

	if (phydev->link) {
		if ((lp->speed != phydev->speed) ||
		    (lp->duplex != phydev->duplex)) {
			regval = xemacps_read(lp->baseaddr,
				XEMACPS_NWCFG_OFFSET);
			if (phydev->duplex)
				regval |= XEMACPS_NWCFG_FDEN_MASK;
			else
				regval &= ~XEMACPS_NWCFG_FDEN_MASK;

			if (phydev->speed == SPEED_1000) {
				regval |= XEMACPS_NWCFG_1000_MASK;
				regval1 |= ((lp->slcr_div1_1000Mbps) << 20);
				regval1 |= ((lp->slcr_div0_1000Mbps) << 8);
				xslcr_write(lp->slcr_div_reg, regval1);
				if (0 <= lp->gpiospeed)
					gpio_set_value(lp->gpiospeed, 0);
			} else {
				regval &= ~XEMACPS_NWCFG_1000_MASK;
				if (0 <= lp->gpiospeed)
					gpio_set_value(lp->gpiospeed, 1);
			}

			if (phydev->speed == SPEED_100) {
				regval |= XEMACPS_NWCFG_100_MASK;
				regval1 |= ((lp->slcr_div1_100Mbps) << 20);
				regval1 |= ((lp->slcr_div0_100Mbps) << 8);
				xslcr_write(lp->slcr_div_reg, regval1);
			} else
				regval &= ~XEMACPS_NWCFG_100_MASK;

			if (phydev->speed == SPEED_10) {
				regval1 |= ((lp->slcr_div1_10Mbps) << 20);
				regval1 |= ((lp->slcr_div0_10Mbps) << 8);
				xslcr_write(lp->slcr_div_reg, regval1);
			}

			xemacps_write(lp->baseaddr, XEMACPS_NWCFG_OFFSET,
				regval);

			lp->speed = phydev->speed;
			lp->duplex = phydev->duplex;
			status_change = 1;
		}

		netif_carrier_on(ndev);
	} else
		netif_carrier_off(ndev);

	if (phydev->link != lp->link) {
		lp->link = phydev->link;
		status_change = 1;
	}

	if (status_change) {
		if (phydev->link)
			netdev_dbg(ndev, "link up (%d/%s)\n", phydev->speed,
				   DUPLEX_FULL == phydev->duplex ? "FULL" : "HALF");
		else
			netdev_dbg(ndev, "link down\n");
	}
}

/**
 * xemacps_mii_probe - probe mii bus, find the right bus_id to register
 * phy callback function.
 * @ndev: network interface device structure
 * return 0 on success, negative value if error
 **/
static int xemacps_mii_probe(struct net_device *ndev)
{
	struct net_local *lp = netdev_priv(ndev);
	struct phy_device *phydev = NULL;

	if (lp->phy_node) {
		phydev = of_phy_connect(lp->ndev,
					lp->phy_node,
					xemacps_adjust_link,
					0,
					PHY_INTERFACE_MODE_RGMII_ID);
	}
	if (!phydev) {
		netdev_err(ndev, "no PHY found\n");
		return -1;
	}

	netdev_dbg(ndev, "phydev %p, phydev->phy_id 0x%x, phydev->addr 0x%x\n",
		   phydev, phydev->phy_id, phydev->addr);

	phydev->supported &= (PHY_GBIT_FEATURES | SUPPORTED_Pause |
							SUPPORTED_Asym_Pause);
	phydev->advertising = phydev->supported;

	lp->link    = 0;
	lp->speed   = 0;
	lp->duplex  = -1;
	lp->phy_dev = phydev;

	if (lp->board_type == BOARD_TYPE_ZYNQ)
		phy_start(lp->phy_dev);
	else
		xemacps_phy_init(lp->ndev);

	netdev_dbg(ndev, "phy_addr 0x%x, phy_id 0x%08x\n", lp->phy_dev->addr,
		   lp->phy_dev->phy_id);
	netdev_dbg(ndev, "attach [%s] phy driver\n", lp->phy_dev->drv->name);

	return 0;
}

/**
 * xemacps_mii_init - Initialize and register mii bus to network device
 * @lp: local device instance pointer
 * return 0 on success, negative value if error
 **/
static int xemacps_mii_init(struct net_local *lp)
{
	int rc = -ENXIO, i;
	struct resource res;
	struct device_node *np = of_get_parent(lp->phy_node);

	lp->mii_bus = mdiobus_alloc();
	if (lp->mii_bus == NULL) {
		rc = -ENOMEM;
		goto err_out;
	}

	lp->mii_bus->name  = "XEMACPS mii bus";
	lp->mii_bus->read  = &xemacps_mdio_read;
	lp->mii_bus->write = &xemacps_mdio_write;
	lp->mii_bus->reset = &xemacps_mdio_reset;
	lp->mii_bus->priv = lp;
	lp->mii_bus->parent = &lp->ndev->dev;

	lp->mii_bus->irq = kmalloc(sizeof(int)*PHY_MAX_ADDR, GFP_KERNEL);
	if (!lp->mii_bus->irq) {
		rc = -ENOMEM;
		goto err_out_free_mdiobus;
	}

	for (i = 0; i < PHY_MAX_ADDR; i++)
		lp->mii_bus->irq[i] = PHY_POLL;
	of_address_to_resource(np, 0, &res);
	snprintf(lp->mii_bus->id, MII_BUS_ID_SIZE, "%.8llx",
		 (unsigned long long)res.start);
	if (of_mdiobus_register(lp->mii_bus, np))
		goto err_out_free_mdio_irq;
	return 0;

err_out_free_mdio_irq:
	kfree(lp->mii_bus->irq);
err_out_free_mdiobus:
	mdiobus_free(lp->mii_bus);
err_out:
	return rc;
}

/**
 * xemacps_update_hdaddr - Update device's MAC address when configured
 * MAC address is not valid, reconfigure with a good one.
 * @lp: local device instance pointer
 **/
static void __init xemacps_update_hwaddr(struct net_local *lp)
{
	u32 regvall;
	u16 regvalh;
	u8  addr[6];

	regvall = xemacps_read(lp->baseaddr, XEMACPS_LADDR1L_OFFSET);
	regvalh = xemacps_read(lp->baseaddr, XEMACPS_LADDR1H_OFFSET);
	addr[0] = regvall & 0xFF;
	addr[1] = (regvall >> 8) & 0xFF;
	addr[2] = (regvall >> 16) & 0xFF;
	addr[3] = (regvall >> 24) & 0xFF;
	addr[4] = regvalh & 0xFF;
	addr[5] = (regvalh >> 8) & 0xFF;

	if (is_valid_ether_addr(addr)) {
		memcpy(lp->ndev->dev_addr, addr, sizeof(addr));
	} else {
		dev_info(&lp->pdev->dev, "invalid address, use assigned\n");
		random_ether_addr(lp->ndev->dev_addr);
		netdev_info(lp->ndev, "MAC updated %02x:%02x:%02x:%02x:%02x:%02x\n",
			lp->ndev->dev_addr[0], lp->ndev->dev_addr[1],
			lp->ndev->dev_addr[2], lp->ndev->dev_addr[3],
			lp->ndev->dev_addr[4], lp->ndev->dev_addr[5]);
	}
}

/**
 * xemacps_set_hwaddr - Set device's MAC address from ndev->dev_addr
 * @lp: local device instance pointer
 **/
static void xemacps_set_hwaddr(struct net_local *lp)
{
	u32 regvall = 0;
	u16 regvalh = 0;
#ifdef __LITTLE_ENDIAN
	regvall = cpu_to_le32(*((u32 *)lp->ndev->dev_addr));
	regvalh = cpu_to_le16(*((u16 *)(lp->ndev->dev_addr + 4)));
#endif
#ifdef __BIG_ENDIAN
	regvall = cpu_to_be32(*((u32 *)lp->ndev->dev_addr));
	regvalh = cpu_to_be16(*((u16 *)(lp->ndev->dev_addr + 4)));
#endif
	/* LADDRXH has to be wriiten latter than LADDRXL to enable
	 * this address even if these 16 bits are zeros. */
	xemacps_write(lp->baseaddr, XEMACPS_LADDR1L_OFFSET, regvall);
	xemacps_write(lp->baseaddr, XEMACPS_LADDR1H_OFFSET, regvalh);
#ifdef DEBUG
	regvall = xemacps_read(lp->baseaddr, XEMACPS_LADDR1L_OFFSET);
	regvalh = xemacps_read(lp->baseaddr, XEMACPS_LADDR1H_OFFSET);
	netdev_dbg(lp->ndev, "GEM: MAC 0x%08x, 0x%08x, %02x:%02x:%02x:%02x:%02x:%02x\n",
		regvall, regvalh,
		(regvall & 0xff), ((regvall >> 8) & 0xff),
		((regvall >> 16) & 0xff), (regvall >> 24),
		(regvalh & 0xff), (regvalh >> 8));
#endif
}

/*
 * xemacps_reset_hw - Helper function to reset the underlying hardware.
 * This is called when we get into such deep trouble that we don't know
 * how to handle otherwise.
 * @lp: local device instance pointer
 */
static void xemacps_reset_hw(struct net_local *lp)
{
	u32 regisr;
	/* make sure we have the buffer for ourselves */
	wmb();

	/* Have a clean start */
	xemacps_write(lp->baseaddr, XEMACPS_NWCTRL_OFFSET, 0);
	lp->nwctrl_base = 0;

	/* Clear statistic counters */
	xemacps_write(lp->baseaddr, XEMACPS_NWCTRL_OFFSET,
		XEMACPS_NWCTRL_STATCLR_MASK);

	/* Clear TX and RX status */
	xemacps_write(lp->baseaddr, XEMACPS_TXSR_OFFSET, ~0UL);
	xemacps_write(lp->baseaddr, XEMACPS_RXSR_OFFSET, ~0UL);

	/* Disable all interrupts */
	xemacps_write(lp->baseaddr, XEMACPS_IDR_OFFSET, ~0UL);
	regisr = xemacps_read(lp->baseaddr, XEMACPS_ISR_OFFSET);
	xemacps_write(lp->baseaddr, XEMACPS_ISR_OFFSET, regisr);
}

#ifdef CONFIG_XILINX_PS_EMAC_HWTSTAMP

/**
 * xemacps_get_hwticks - get the current value of the GEM internal timer
 * @lp: local device instance pointer
 * return: nothing
 **/
static inline void
xemacps_get_hwticks(struct net_local *lp, u64 *sec, u64 *nsec)
{
	do {
		*nsec = xemacps_read(lp->baseaddr, XEMACPS_1588NS_OFFSET);
		*sec = xemacps_read(lp->baseaddr, XEMACPS_1588S_OFFSET);
	} while (*nsec > xemacps_read(lp->baseaddr, XEMACPS_1588NS_OFFSET));
}

/**
 * xemacps_read_clock - read raw cycle counter (to be used by time counter)
 */
static cycle_t xemacps_read_clock(const struct cyclecounter *tc)
{
	struct net_local *lp =
			container_of(tc, struct net_local, cycles);
	u64 stamp;
	u64 sec, nsec;

	xemacps_get_hwticks(lp, &sec, &nsec);
	stamp = (sec << 32) | nsec;

	return stamp;
}


/**
 * xemacps_systim_to_hwtstamp - convert system time value to hw timestamp
 * @adapter: board private structure
 * @shhwtstamps: timestamp structure to update
 * @regval: unsigned 64bit system time value.
 *
 * We need to convert the system time value stored in the RX/TXSTMP registers
 * into a hwtstamp which can be used by the upper level timestamping functions
 */
static void xemacps_systim_to_hwtstamp(struct net_local *lp,
				struct skb_shared_hwtstamps *shhwtstamps,
				u64 regval)
{
	u64 ns;

	ns = timecounter_cyc2time(&lp->clock, regval);
	timecompare_update(&lp->compare, ns);
	memset(shhwtstamps, 0, sizeof(struct skb_shared_hwtstamps));
	shhwtstamps->hwtstamp = ns_to_ktime(ns);
	shhwtstamps->syststamp = timecompare_transform(&lp->compare, ns);
}

static void
xemacps_rx_hwtstamp(struct net_local *lp,
			struct sk_buff *skb, unsigned msg_type)
{
	u64 time64, sec, nsec;

	if (!msg_type) {
		/* PTP Event Frame packets */
		sec = xemacps_read(lp->baseaddr, XEMACPS_PTPERXS_OFFSET);
		nsec = xemacps_read(lp->baseaddr, XEMACPS_PTPERXNS_OFFSET);
	} else {
		/* PTP Peer Event Frame packets */
		sec = xemacps_read(lp->baseaddr, XEMACPS_PTPPRXS_OFFSET);
		nsec = xemacps_read(lp->baseaddr, XEMACPS_PTPPRXNS_OFFSET);
	}
	time64 = (sec << 32) | nsec;
	xemacps_systim_to_hwtstamp(lp, skb_hwtstamps(skb), time64);
}

static void
xemacps_tx_hwtstamp(struct net_local *lp,
			struct sk_buff *skb, unsigned msg_type)
{
	u64 time64, sec, nsec;

	if (!msg_type) {
		/* PTP Event Frame packets */
		sec = xemacps_read(lp->baseaddr, XEMACPS_PTPETXS_OFFSET);
		nsec = xemacps_read(lp->baseaddr, XEMACPS_PTPETXNS_OFFSET);
	} else {
		/* PTP Peer Event Frame packets */
		sec = xemacps_read(lp->baseaddr, XEMACPS_PTPPTXS_OFFSET);
		nsec = xemacps_read(lp->baseaddr, XEMACPS_PTPPTXNS_OFFSET);
	}

	time64 = (sec << 32) | nsec;
	xemacps_systim_to_hwtstamp(lp, skb_hwtstamps(skb), time64);
	skb_tstamp_tx(skb, skb_hwtstamps(skb));
}

#endif /* CONFIG_XILINX_PS_EMAC_HWTSTAMP */

/**
 * xemacps_rx - process received packets when napi called
 * @lp: local device instance pointer
 * @budget: NAPI budget
 * return: number of BDs processed
 **/
static int xemacps_rx(struct net_local *lp, int budget)
{
	struct xemacps_bd *cur_p;
	u32 len;
	struct sk_buff *skb;
	struct sk_buff *new_skb;
	u32 new_skb_baddr;
	unsigned int numbdfree = 0;
	u32 size = 0;
	u32 packets = 0;
	u32 addr;
	u32 ctrl;

	cur_p = &lp->rx_bd[lp->rx_bd_ci];
	addr = ACCESS_ONCE(cur_p->addr);
	while ((addr & XEMACPS_RXBUF_NEW_MASK) && (numbdfree < budget)) {
		ctrl = ACCESS_ONCE(cur_p->ctrl);
		if (ctrl & XEMACPS_RXBUF_BAD_FCS) {
			new_skb_baddr = lp->rx_skb[lp->rx_bd_ci].mapping;
			goto rx_skip;
		}
		/* the packet length */
		len = ctrl & XEMACPS_RXBUF_LEN_MASK;
		skb = lp->rx_skb[lp->rx_bd_ci].skb;
		dma_unmap_single(lp->ndev->dev.parent,
				 lp->rx_skb[lp->rx_bd_ci].mapping,
				 XEMACPS_RX_BUF_SIZE,
				 DMA_FROM_DEVICE);

		/* setup received skb and send it upstream */
		skb_put(skb, len);  /* Tell the skb how much data we got. */
		skb->protocol = eth_type_trans(skb, lp->ndev);

		skb->ip_summed = lp->ip_summed;

#ifdef CONFIG_XILINX_PS_EMAC_HWTSTAMP
		if ((lp->hwtstamp_config.rx_filter == HWTSTAMP_FILTER_ALL) &&
		    (ntohs(skb->protocol) == 0x800)) {
			unsigned ip_proto, dest_port, msg_type;

			/* While the GEM can timestamp PTP packets, it does
			 * not mark the RX descriptor to identify them.  This
			 * is entirely the wrong place to be parsing UDP
			 * headers, but some minimal effort must be made.
			 * NOTE: the below parsing of ip_proto and dest_port
			 * depend on the use of Ethernet_II encapsulation,
			 * IPv4 without any options.
			 */
			ip_proto = *((u8 *)skb->mac_header + 14 + 9);
			dest_port = ntohs(*(((u16 *)skb->mac_header) +
						((14 + 20 + 2)/2)));
			msg_type = *((u8 *)skb->mac_header + 42);
			if ((ip_proto == IPPROTO_UDP) &&
			    (dest_port == 0x13F)) {
				/* Timestamp this packet */
				xemacps_rx_hwtstamp(lp, skb, msg_type & 0x2);
			}
		}
#endif /* CONFIG_XILINX_PS_EMAC_HWTSTAMP */
		size += len;
		packets++;
		netif_receive_skb(skb);

		new_skb = netdev_alloc_skb(lp->ndev, XEMACPS_RX_BUF_SIZE);
		if (new_skb == NULL) {
			dev_err(&lp->ndev->dev, "no memory for new sk_buff\n");
			return 0;
		}
		/* Get dma handle of skb->data */
		new_skb_baddr = (u32) dma_map_single(lp->ndev->dev.parent,
						     new_skb->data,
						     XEMACPS_RX_BUF_SIZE,
						     DMA_FROM_DEVICE);
		lp->rx_skb[lp->rx_bd_ci].skb = new_skb;
		lp->rx_skb[lp->rx_bd_ci].mapping = new_skb_baddr;
rx_skip:
		addr = (addr & ~XEMACPS_RXBUF_ADD_MASK) | (new_skb_baddr);
		addr &= (~XEMACPS_RXBUF_NEW_MASK);
		cur_p->addr = addr;
		wmb();

		lp->rx_bd_ci++;
		lp->rx_bd_ci = lp->rx_bd_ci % XEMACPS_RECV_BD_CNT;
		cur_p = &lp->rx_bd[lp->rx_bd_ci];
		addr = ACCESS_ONCE(cur_p->addr);
		numbdfree++;
	}
	lp->stats.rx_packets += packets;
	lp->stats.rx_bytes += size;
	return numbdfree;
}

/**
 * xemacps_rx_timer - check for potential receive stall and handle it
 * @arg: struct net_local *
 **/
static void xemacps_rx_timer(unsigned long arg)
{
	struct net_local *lp = (struct net_local *)arg;
	unsigned long flags;
	bool reset = false;

	/* This is the handler for the receive stall hardware bug. If
	   we haven't received any packets for a while and a receive
	   error has occured recently, we may have triggered this bug.
	   We can just toggle the RXEN bit to clear the bug condition
	   and start receiving packets again. Sometimes toggling RXEN
	   doesn't clear the stall the first time, so we check for a
	   while after a potential stall is detected. If we see that
	   we're still not receiving packets, we toggle RXEN again. */
	if (time_after(jiffies, lp->rx_last_jiffies + HZ)) {
		if (lp->rx_error)
			lp->rx_reset = 4;

		if (lp->rx_reset) {
			reset = true;
			--lp->rx_reset;
		}
	}

	if (reset) {
		spin_lock_irqsave(&lp->nwctrl_lock, flags);

		xemacps_write(lp->baseaddr, XEMACPS_NWCTRL_OFFSET,
			      (lp->nwctrl_base & ~XEMACPS_NWCTRL_RXEN_MASK));
		wmb();
		xemacps_write(lp->baseaddr, XEMACPS_NWCTRL_OFFSET,
			      lp->nwctrl_base);
		wmb();

		lp->rx_error = false;

		spin_unlock_irqrestore(&lp->nwctrl_lock, flags);
	}

	/* Reschedule the timer. */
	mod_timer(&lp->rx_timer, jiffies + HZ);
}

/**
 * xemacps_rx_poll - NAPI poll routine
 * napi: pointer to napi struct
 * budget:
 **/
static int xemacps_rx_poll(struct napi_struct *napi, int budget)
{
	struct net_local *lp = container_of(napi, struct net_local, napi);
	int work_done = 0;
	int temp_work_done;
	u32 regval;

	while (work_done < budget) {
		regval = xemacps_read(lp->baseaddr, XEMACPS_RXSR_OFFSET);
		if (regval & XEMACPS_RXSR_ERROR_MASK)
			lp->rx_error = true;
		xemacps_write(lp->baseaddr, XEMACPS_RXSR_OFFSET, regval);
		wmb();
		temp_work_done = xemacps_rx(lp, budget - work_done);
		work_done += temp_work_done;
		if (temp_work_done <= 0)
			break;
		/* We've received packets, so reset the receive stall
		   timeout. */
		lp->rx_last_jiffies = jiffies;
	}

	if (work_done >= budget)
		return work_done;

	napi_complete(napi);

	/* We disabled RX interrupts in interrupt service routine, now
	   it is time to enable it back. */
	xemacps_write(lp->baseaddr, XEMACPS_IER_OFFSET,
		      (XEMACPS_IXR_FRAMERX_MASK |
		       XEMACPS_IXR_RX_ERR_MASK));
	wmb();

	return work_done;
}

/**
 * xemacps_interrupt - interrupt main service routine
 * @irq: interrupt number
 * @dev_id: pointer to a network device structure
 * return IRQ_HANDLED or IRQ_NONE
 **/
static irqreturn_t xemacps_interrupt(int irq, void *dev_id)
{
	struct net_device *ndev = dev_id;
	struct net_local *lp = netdev_priv(ndev);
	u32 regisr;

	regisr = xemacps_read(lp->baseaddr, XEMACPS_ISR_OFFSET);

	if (unlikely(!regisr))
		return IRQ_NONE;

	/* Disable receive interrupts and schedule NAPI. */
	xemacps_write(lp->baseaddr, XEMACPS_IDR_OFFSET,
		      (XEMACPS_IXR_FRAMERX_MASK |
		       XEMACPS_IXR_RX_ERR_MASK));
	wmb();

	napi_schedule(&lp->napi);

	/* Acknowledge and clear the interrupts. */
	xemacps_write(lp->baseaddr, XEMACPS_ISR_OFFSET, regisr);
	wmb();

	return IRQ_HANDLED;
}

static int xemacps_polling_thread(void *info)
{
	struct net_device *ndev = info;
	struct net_local *lp = netdev_priv(ndev);
	int ni_polling_interval;
	int ni_polling_interval_us;
	struct sched_param param;

	ni_polling_interval = lp->ni_polling_interval;
	ni_polling_interval_us = ni_polling_interval * 1000;
	param.sched_priority = lp->ni_polling_priority;

	sched_setscheduler(current, lp->ni_polling_policy, &param);

	/* If we got changed to interrupt mode before the polling thread
	   started. */
	if (unlikely(0 > ni_polling_interval)) {
		while (!kthread_should_stop())
			msleep(1);
		return -EINTR;
	}

	while (!kthread_should_stop()) {
		local_bh_disable();
		xemacps_interrupt(ndev->irq, ndev);
		local_bh_enable();

		if (0 == ni_polling_interval)
			cpu_relax();
		else if (20 > ni_polling_interval)
			usleep_range(ni_polling_interval_us,
				     ni_polling_interval_us);
		else
			msleep(ni_polling_interval);
	}

	return 0;
}

static ssize_t xemacps_get_ni_polling_interval(struct device *dev,
					       struct device_attribute *attr,
					       char *buf)
{
	struct net_device *ndev = to_net_dev(dev);
	struct net_local *lp = netdev_priv(ndev);

	return sprintf(buf, "%d\n", lp->ni_polling_interval);
}

static int xemacps_start_packet_receive_mechanism(struct net_local *lp)
{
	int rc = 0;

	if (0 <= lp->ni_polling_interval) {
		lp->ni_polling_task =
			kthread_create(xemacps_polling_thread, lp->ndev,
				       "poll/%s", lp->ndev->name);

		if (IS_ERR(lp->ni_polling_task)) {
			rc = PTR_ERR(lp->ni_polling_task);
			lp->ni_polling_task = NULL;
			netdev_err(lp->ndev, "Unable to create polling thread, "
					 "error %d\n", rc);
		}
	} else {
		rc = request_irq(lp->ndev->irq, xemacps_interrupt,
				 IRQF_SAMPLE_RANDOM, lp->ndev->name, lp->ndev);
		if (rc) {
			netdev_err(lp->ndev, "Unable to request IRQ, "
					 "error %d\n", rc);
		}
	}

	return rc;
}

static ssize_t xemacps_set_ni_polling_interval(struct device *dev,
					       struct device_attribute *attr,
					       const char *buf, size_t count)
{
	struct net_device *ndev = to_net_dev(dev);
	struct net_local *lp = netdev_priv(ndev);
	int interval;

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	if (0 > kstrtoint(buf, 0, &interval))
		return -EINVAL;

	if (lp->ni_polling_interval != interval) {
		/* Synchronize with xemacps_open/close. */
		rtnl_lock();

		lp->ni_polling_interval = interval;

		if (!(test_bit(XEMACPS_STATE_DOWN, &lp->flags))) {
			/* Stop whatever mechanism is currently active. */
			if (lp->ni_polling_task) {
				kthread_stop(lp->ni_polling_task);
				lp->ni_polling_task = NULL;
			} else
				free_irq(ndev->irq, ndev);

			/* Start up whatever we've just selected. */
			xemacps_start_packet_receive_mechanism(lp);

			/* Start the polling task if it exists. */
			if (lp->ni_polling_task)
				wake_up_process(lp->ni_polling_task);
		}

		rtnl_unlock();
	}

	return count;
}

static DEVICE_ATTR(ni_polling_interval, S_IWUGO | S_IRUGO,
		   xemacps_get_ni_polling_interval,
		   xemacps_set_ni_polling_interval);

static ssize_t xemacps_get_ni_polling_policy(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	struct net_device *ndev = to_net_dev(dev);
	struct net_local *lp = netdev_priv(ndev);

	switch (lp->ni_polling_policy) {
		case SCHED_NORMAL:
			return sprintf(buf, "SCHED_NORMAL (SCHED_OTHER)\n");
		case SCHED_FIFO:
			return sprintf(buf, "SCHED_FIFO\n");
		case SCHED_RR:
			return sprintf(buf, "SCHED_RR\n");
		case SCHED_BATCH:
			return sprintf(buf, "SCHED_BATCH\n");
		case SCHED_IDLE:
			return sprintf(buf, "SCHED_IDLE\n");
		default:
			return sprintf(buf, "unknown\n");
	}
}

static ssize_t xemacps_set_ni_polling_policy(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	struct net_device *ndev = to_net_dev(dev);
	struct net_local *lp = netdev_priv(ndev);
	char policy_str [16] = { 0, };
	int policy;

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	if (1 != sscanf(buf, "%15s", policy_str))
		return -EINVAL;

	if ((0 == strcmp(policy_str, "SCHED_NORMAL") ||
	    (0 == strcmp(policy_str, "SCHED_OTHER"))))
		policy = SCHED_NORMAL;
	else if (0 == strcmp(policy_str, "SCHED_FIFO"))
		policy = SCHED_FIFO;
	else if (0 == strcmp(policy_str, "SCHED_RR"))
		policy = SCHED_RR;
	else if (0 == strcmp(policy_str, "SCHED_BATCH"))
		policy = SCHED_BATCH;
	else if (0 == strcmp(policy_str, "SCHED_IDLE"))
		policy = SCHED_IDLE;
	else
		return -EINVAL;

	lp->ni_polling_policy = policy;

	/* Synchronize with xemacps_open/close. */
	rtnl_lock();

	if (lp->ni_polling_task) {
		const struct sched_param param = {
			.sched_priority = lp->ni_polling_priority,
		};
		sched_setscheduler(lp->ni_polling_task,
				   lp->ni_polling_policy, &param);
	}

	rtnl_unlock();

	return count;
}

static DEVICE_ATTR(ni_polling_policy, S_IWUGO | S_IRUGO,
		   xemacps_get_ni_polling_policy,
		   xemacps_set_ni_polling_policy);

static ssize_t xemacps_get_ni_polling_priority(struct device *dev,
					       struct device_attribute *attr,
					       char *buf)
{
	struct net_device *ndev = to_net_dev(dev);
	struct net_local *lp = netdev_priv(ndev);

	return sprintf(buf, "%d\n", lp->ni_polling_priority);
}

static ssize_t xemacps_set_ni_polling_priority(struct device *dev,
					       struct device_attribute *attr,
					       const char *buf, size_t count)
{
	struct net_device *ndev = to_net_dev(dev);
	struct net_local *lp = netdev_priv(ndev);
	int priority;

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	if (0 > kstrtoint(buf, 0, &priority))
		return -EINVAL;

	lp->ni_polling_priority = priority;

	/* Synchronize with xemacps_open/close. */
	rtnl_lock();

	if (lp->ni_polling_task) {
		const struct sched_param param = {
			.sched_priority = lp->ni_polling_priority,
		};
		sched_setscheduler(lp->ni_polling_task,
				   lp->ni_polling_policy, &param);
	}

	rtnl_unlock();

	return count;
}

static DEVICE_ATTR(ni_polling_priority, S_IWUGO | S_IRUGO,
		   xemacps_get_ni_polling_priority,
		   xemacps_set_ni_polling_priority);

/*
 * Free all packets presently in the descriptor rings.
 */
static void xemacps_clean_rings(struct net_local *lp)
{
	int i;

	for (i = 0; i < XEMACPS_RECV_BD_CNT; i++) {
		if (lp->rx_skb[i].skb) {
			dma_unmap_single(lp->ndev->dev.parent,
					 lp->rx_skb[i].mapping,
					 XEMACPS_RX_BUF_SIZE,
					 DMA_FROM_DEVICE);

			dev_kfree_skb(lp->rx_skb[i].skb);
			lp->rx_skb[i].skb = NULL;
			lp->rx_skb[i].mapping = 0;
		}
	}

	for (i = 0; i < XEMACPS_SEND_BD_CNT; i++) {
		if (lp->tx_skb[i].skb) {
			dma_unmap_single(lp->ndev->dev.parent,
					 lp->tx_skb[i].mapping,
					 lp->tx_skb[i].skb->len,
					 DMA_TO_DEVICE);

			dev_kfree_skb(lp->tx_skb[i].skb);
			lp->tx_skb[i].skb = NULL;
			lp->tx_skb[i].mapping = 0;
		}
	}
}

/**
 * xemacps_descriptor_free - Free allocated TX and RX BDs
 * @lp: local device instance pointer
 **/
static void xemacps_descriptor_free(struct net_local *lp)
{
	int size;

	xemacps_clean_rings(lp);

	if (lp->rx_bd) {
		size = (XEMACPS_RECV_BD_CNT + XEMACPS_SEND_BD_CNT) *
			sizeof(struct xemacps_bd);
		dma_free_coherent(&lp->pdev->dev, size,
			lp->rx_bd, lp->rx_bd_dma);
		lp->rx_bd = NULL;
		lp->tx_bd = NULL;
	}
}


/**
 * xemacps_descriptor_init - Allocate both TX and RX BDs
 * @lp: local device instance pointer
 * return 0 on success, negative value if error
 **/
static int xemacps_descriptor_init(struct net_local *lp)
{
	int size;
	struct sk_buff *new_skb;
	u32 new_skb_baddr;
	u32 i;
	struct xemacps_bd *cur_p;

	/* Reset the indexes which are used for accessing the BDs */
	lp->tx_bd_ci = 0;
	lp->tx_bd_tail = 0;
	lp->rx_bd_ci = 0;

	size = (XEMACPS_RECV_BD_CNT + XEMACPS_SEND_BD_CNT) *
		sizeof(struct xemacps_bd);
	lp->rx_bd = dma_alloc_coherent(&lp->pdev->dev, size,
			&lp->rx_bd_dma, GFP_KERNEL);
	if (!lp->rx_bd)
		goto err_out;

	memset(lp->rx_bd, 0, sizeof(*lp->rx_bd) * XEMACPS_RECV_BD_CNT);
	for (i = 0; i < XEMACPS_RECV_BD_CNT; i++) {
		cur_p = &lp->rx_bd[i];
		new_skb = netdev_alloc_skb(lp->ndev, XEMACPS_RX_BUF_SIZE);

		if (new_skb == NULL) {
			dev_err(&lp->ndev->dev, "alloc_skb error %d\n", i);
			goto err_out;
		}

		/* Get dma handle of skb->data */
		new_skb_baddr = (u32) dma_map_single(lp->ndev->dev.parent,
						     new_skb->data,
						     XEMACPS_RX_BUF_SIZE,
						     DMA_FROM_DEVICE);
		cur_p->addr = (cur_p->addr & ~XEMACPS_RXBUF_ADD_MASK) | (new_skb_baddr);
		lp->rx_skb[i].skb = new_skb;
		lp->rx_skb[i].mapping = new_skb_baddr;
		wmb();
	}
	/* wrap bit set for last BD, bdptr is moved to last here */
	cur_p->ctrl = 0;
	cur_p->addr |= XEMACPS_RXBUF_WRAP_MASK;

	lp->tx_bd = lp->rx_bd + XEMACPS_RECV_BD_CNT;
	lp->tx_bd_dma = lp->rx_bd_dma + (XEMACPS_RECV_BD_CNT *
		sizeof(struct xemacps_bd));

	memset(lp->tx_bd, 0, sizeof(*lp->tx_bd) * XEMACPS_SEND_BD_CNT);
	for (i = 0; i < XEMACPS_SEND_BD_CNT; i++) {
		cur_p = &lp->tx_bd[i];
		cur_p->ctrl = XEMACPS_TXBUF_USED_MASK;
	}
	/* wrap bit set for last BD, bdptr is moved to last here */
	cur_p->ctrl = (XEMACPS_TXBUF_WRAP_MASK | XEMACPS_TXBUF_USED_MASK);
	lp->tx_bd_freecnt = XEMACPS_SEND_BD_CNT;

	for (i = 0; i < XEMACPS_RECV_BD_CNT; i++) {
		cur_p = &lp->rx_bd[i];
		cur_p->ctrl = 0;
		/* Assign ownership back to hardware */
		cur_p->addr &= (~XEMACPS_RXBUF_NEW_MASK);
	}
	wmb();

	netdev_dbg(lp->ndev, "lp->tx_bd %p lp->tx_bd_dma %p lp->tx_skb %p\n",
		   lp->tx_bd, (void *)lp->tx_bd_dma, lp->tx_skb);
	netdev_dbg(lp->ndev, "lp->rx_bd %p lp->rx_bd_dma %p lp->rx_skb %p\n",
		   lp->rx_bd, (void *)lp->rx_bd_dma, lp->rx_skb);
	return 0;

err_out:
	xemacps_descriptor_free(lp);
	return -ENOMEM;
}



#ifdef CONFIG_XILINX_PS_EMAC_HWTSTAMP
/*
 * Initialize the GEM Time Stamp Unit
 */
static void xemacps_init_tsu(struct net_local *lp)
{

	memset(&lp->cycles, 0, sizeof(lp->cycles));
	lp->cycles.read = xemacps_read_clock;
	lp->cycles.mask = CLOCKSOURCE_MASK(64);
	lp->cycles.mult = 1;
	lp->cycles.shift = 0;

	/* Set registers so that rollover occurs soon to test this. */
	xemacps_write(lp->baseaddr, XEMACPS_1588NS_OFFSET, 0x00000000);
	xemacps_write(lp->baseaddr, XEMACPS_1588S_OFFSET, 0xFF800000);

	/* program the timer increment register with the numer of nanoseconds
	 * per clock tick.
	 *
	 * Note: The value is calculated based on the current operating
	 * frequency 50MHz
	 */
	xemacps_write(lp->baseaddr, XEMACPS_1588INC_OFFSET,
			(NS_PER_SEC/lp->ptpenetclk));

	timecounter_init(&lp->clock, &lp->cycles,
				ktime_to_ns(ktime_get_real()));
	/*
	 * Synchronize our NIC clock against system wall clock.
	 */
	memset(&lp->compare, 0, sizeof(lp->compare));
	lp->compare.source = &lp->clock;
	lp->compare.target = ktime_get_real;
	lp->compare.num_samples = 10;
	timecompare_update(&lp->compare, 0);

	/* Initialize hwstamp config */
	lp->hwtstamp_config.rx_filter = HWTSTAMP_FILTER_NONE;
	lp->hwtstamp_config.tx_type = HWTSTAMP_TX_OFF;

}
#endif /* CONFIG_XILINX_PS_EMAC_HWTSTAMP */

/**
 * xemacps_init_hw - Initialize hardware to known good state
 * @lp: local device instance pointer
 **/
static void xemacps_init_hw(struct net_local *lp)
{
	u32 regval;

	xemacps_reset_hw(lp);
	xemacps_set_hwaddr(lp);

	/* network configuration */
	regval  = 0;
	regval |= XEMACPS_NWCFG_FDEN_MASK;
	regval |= XEMACPS_NWCFG_RXCHKSUMEN_MASK;
	regval |= XEMACPS_NWCFG_PAUSECOPYDI_MASK;
	regval |= XEMACPS_NWCFG_PAUSEEN_MASK;
	regval |= XEMACPS_NWCFG_100_MASK;
	regval |= XEMACPS_NWCFG_HDRXEN_MASK;

	if (lp->board_type == BOARD_TYPE_ZYNQ)
		regval |= (lp->mdc_clk_div << XEMACPS_NWCFG_MDC_SHIFT_MASK);
	if (lp->ndev->flags & IFF_PROMISC)	/* copy all */
		regval |= XEMACPS_NWCFG_COPYALLEN_MASK;
	if (!(lp->ndev->flags & IFF_BROADCAST))	/* No broadcast */
		regval |= XEMACPS_NWCFG_BCASTDI_MASK;
	xemacps_write(lp->baseaddr, XEMACPS_NWCFG_OFFSET, regval);

	/* Init TX and RX DMA Q address */
	xemacps_write(lp->baseaddr, XEMACPS_RXQBASE_OFFSET, lp->rx_bd_dma);
	xemacps_write(lp->baseaddr, XEMACPS_TXQBASE_OFFSET, lp->tx_bd_dma);

	/* DMACR configurations */
	regval  = (((XEMACPS_RX_BUF_SIZE / XEMACPS_RX_BUF_UNIT) +
		((XEMACPS_RX_BUF_SIZE % XEMACPS_RX_BUF_UNIT) ? 1 : 0)) <<
		XEMACPS_DMACR_RXBUF_SHIFT);
	regval |= XEMACPS_DMACR_RXSIZE_MASK;
	regval |= XEMACPS_DMACR_TXSIZE_MASK;
	regval |= XEMACPS_DMACR_TCPCKSUM_MASK;
#ifdef __LITTLE_ENDIAN
	regval &= ~XEMACPS_DMACR_ENDIAN_MASK;
#endif
#ifdef __BIG_ENDIAN
	regval |= XEMACPS_DMACR_ENDIAN_MASK;
#endif
	regval |= XEMACPS_DMACR_BLENGTH_INCR16;
	xemacps_write(lp->baseaddr, XEMACPS_DMACR_OFFSET, regval);

	/* Enable TX, RX and MDIO port */
	lp->nwctrl_base = XEMACPS_NWCTRL_MDEN_MASK |
			  XEMACPS_NWCTRL_TXEN_MASK |
			  XEMACPS_NWCTRL_RXEN_MASK;
	xemacps_write(lp->baseaddr, XEMACPS_NWCTRL_OFFSET, lp->nwctrl_base);

#ifdef CONFIG_XILINX_PS_EMAC_HWTSTAMP
	/* Initialize the Time Stamp Unit */
	xemacps_init_tsu(lp);
#endif

	/* Enable interrupts */
	regval  = XEMACPS_IXR_ALL_MASK;
	xemacps_write(lp->baseaddr, XEMACPS_IER_OFFSET, regval);
	if (lp->ni_polling_task)
		wake_up_process(lp->ni_polling_task);
}

static int xemacps_up(struct net_device *ndev)
{
	struct net_local *lp = netdev_priv(ndev);
	int rc;

	lp->rx_error = false;
	lp->rx_last_jiffies = jiffies;

	rc = xemacps_start_packet_receive_mechanism(lp);
	if (rc)
		return rc;

	rc = xemacps_descriptor_init(lp);
	if (rc) {
		netdev_err(ndev, "Unable to allocate DMA memory, "
				 "error %d\n", rc);
		return rc;
	}

	xemacps_init_hw(lp);
	napi_enable(&lp->napi);
	rc = xemacps_mii_probe(ndev);
	if (rc != 0) {
		netdev_err(ndev, "%s mii_probe fail.\n", lp->mii_bus->name);
		if (rc == (-2)) {
			mdiobus_unregister(lp->mii_bus);
			kfree(lp->mii_bus->irq);
			mdiobus_free(lp->mii_bus);
		}
		return -ENXIO;
	}

	/* Schedule the receive stall timer. */
	mod_timer(&lp->rx_timer, jiffies + HZ);

	clear_bit(XEMACPS_STATE_DOWN, &lp->flags);

	netif_start_queue(ndev);

	return 0;
}

static int xemacps_down(struct net_device *ndev)
{
	struct net_local *lp = netdev_priv(ndev);

	set_bit(XEMACPS_STATE_DOWN, &lp->flags);

	/* Prevent our Rx and Tx polling loops from being scheduled. */
	if (lp->ni_polling_task) {
		kthread_stop(lp->ni_polling_task);
		lp->ni_polling_task = NULL;
	} else
		free_irq(ndev->irq, ndev);

	/* Disable Rx polling and wait for outstanding Rx polling
	   to complete. */
	napi_disable(&lp->napi);

	/* Wait for any outstanding Tx polling to complete. */
	cancel_delayed_work_sync(&lp->tx_task);

	/* Disable further calls to xemacps_start_xmit. */
	netif_stop_queue(ndev);

	/* Make sure any calls to xemacps_start_xmit have completed. */
	netif_tx_lock(ndev);
	netif_tx_unlock(ndev);

	/* Wait for any outstanding timer calls to complete. */
	del_timer_sync(&lp->tx_timer);
	del_timer_sync(&lp->rx_timer);

	/* If we're not resetting, cancel the reset task. */
	if (!test_bit(XEMACPS_STATE_RESET, &lp->flags))
		cancel_work_sync(&lp->reset_task);

	/* Turn off carrier. */
	netif_carrier_off(ndev);

	phy_disconnect(lp->phy_dev);
	lp->phy_dev = NULL;

	xemacps_descriptor_free(lp);
	xemacps_reset_hw(lp);

	return 0;
}

/**
 * xemacps_open - Called when a network device is made active
 * @ndev: network interface device structure
 * return 0 on success, negative value if error
 *
 * The open entry point is called when a network interface is made active
 * by the system (IFF_UP). At this point all resources needed for transmit
 * and receive operations are allocated, the interrupt handler is
 * registered with OS, the watchdog timer is started, and the stack is
 * notified that the interface is ready.
 *
 * note: if error(s), allocated resources before error require to be
 * released or system issues (such as memory) leak might happen.
 **/
static int xemacps_open(struct net_device *ndev)
{
	struct net_local *lp = netdev_priv(ndev);

	dev_dbg(&lp->pdev->dev, "open\n");
	if (!is_valid_ether_addr(ndev->dev_addr))
		return  -EADDRNOTAVAIL;

#ifdef CONFIG_FPGA_PERIPHERAL
	/* If we're being opened while the FPGA is being reprogrammed, we can
	   just return. The interface will be brought up when the FPGA is back
	   up. */
	if (test_bit(XEMACPS_STATE_FPGA_DOWN, &lp->flags))
		return 0;
#endif
	return xemacps_up(ndev);
}

/**
 * xemacps_close - disable a network interface
 * @ndev: network interface device structure
 * return 0
 *
 * The close entry point is called when a network interface is de-activated
 * by OS. The hardware is still under the driver control, but needs to be
 * disabled. A global MAC reset is issued to stop the hardware, and all
 * transmit and receive resources are freed.
 **/
static int xemacps_close(struct net_device *ndev)
{
#ifdef CONFIG_FPGA_PERIPHERAL
	struct net_local *lp = netdev_priv(ndev);

	/* If we're being closed while the FPGA is being reprogrammed, the
	   interface is already down. We can just return. */
	if (test_bit(XEMACPS_STATE_FPGA_DOWN, &lp->flags))
		return 0;
#endif
	/* Shut down the interface. */
	return xemacps_down(ndev);
}

static void xemacps_reset_task(struct work_struct *work)
{
	struct net_local *lp =
		container_of(work, struct net_local, reset_task);

	/* Synchronize with xemacps_open/close. */
	rtnl_lock();

#ifdef CONFIG_FPGA_PERIPHERAL
	BUG_ON(test_bit(XEMACPS_STATE_FPGA_DOWN, &lp->flags));
#endif
	set_bit(XEMACPS_STATE_RESET, &lp->flags);

	if (!(test_bit(XEMACPS_STATE_DOWN, &lp->flags))) {
		/* Shut down the interface and bring it back up. */
		xemacps_down(lp->ndev);
		xemacps_up(lp->ndev);
	}

	clear_bit(XEMACPS_STATE_RESET, &lp->flags);

	rtnl_unlock();
}

/**
 * xemacps_set_mac_address - set network interface mac address
 * @ndev: network interface device structure
 * @addr: pointer to MAC address
 * return 0 on success, negative value if error
 **/
static int xemacps_set_mac_address(struct net_device *ndev, void *addr)
{
	struct net_local *lp = netdev_priv(ndev);
	struct sockaddr *hwaddr = (struct sockaddr *)addr;

	if (netif_running(ndev))
		return -EBUSY;

	if (!is_valid_ether_addr(hwaddr->sa_data))
		return -EADDRNOTAVAIL;
	netdev_dbg(ndev, "hwaddr 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x\n",
		hwaddr->sa_data[0], hwaddr->sa_data[1], hwaddr->sa_data[2],
		hwaddr->sa_data[3], hwaddr->sa_data[4], hwaddr->sa_data[5]);
	memcpy(ndev->dev_addr, hwaddr->sa_data, ndev->addr_len);

	xemacps_set_hwaddr(lp);
	return 0;
}

/*
 * Transmit handling
 */

/**
 * xemacps_tx_clean - clean up completed transmit buffers
 * @lp: local device instance pointer
 * return number of transmit buffers cleaned
 **/
static int xemacps_tx_clean(struct net_local *lp)
{
	struct ring_info *curr_inf;
	struct xemacps_bd *curr_bd;
	int bdcount;
	u32 regval;

	/* Read and clear the transmit status register. We don't use this
	   value for anything, but we must keep it current. Not reading and
	   clearing this register seems to lead to the transmitter getting
	   confused. */
	regval = xemacps_read(lp->baseaddr, XEMACPS_TXSR_OFFSET);
	xemacps_write(lp->baseaddr, XEMACPS_TXSR_OFFSET, regval);
	wmb();

	bdcount = 0;

	for (;;) {
		curr_bd = &(lp->tx_bd[lp->tx_bd_ci]);
		curr_inf = &(lp->tx_skb[lp->tx_bd_ci]);

		regval = ACCESS_ONCE(curr_bd->ctrl);

		/* Break out if this bd doesn't have a send buffer or has not
		   yet completed. */
		if (!(curr_inf->skb && (regval & XEMACPS_TXBUF_USED_MASK)))
			break;

		if (unlikely(regval & XEMACPS_TXBUF_ERR_MASK))
			++lp->stats.tx_errors;
		else {
			++lp->stats.tx_packets;
			lp->stats.tx_bytes += curr_inf->skb->len;
		}

		dma_unmap_single(&lp->pdev->dev, curr_inf->mapping,
				 curr_inf->skb->len,
				 DMA_TO_DEVICE);

		dev_kfree_skb(curr_inf->skb);
		curr_inf->skb = NULL;

		regval &= (XEMACPS_TXBUF_USED_MASK | XEMACPS_TXBUF_WRAP_MASK);
		curr_bd->ctrl = regval;
		wmb();

		++lp->tx_bd_ci;
		lp->tx_bd_ci %= XEMACPS_SEND_BD_CNT;

		++bdcount;
	}

	lp->tx_bd_freecnt += bdcount;
	return bdcount;
}

/**
 * xemacps_tx_timer - deferred cleaning of transmit buffers
 * @arg: struct net_local *
 **/
static void xemacps_tx_timer(unsigned long arg)
{
	struct net_local *lp = (struct net_local *)arg;

	netif_tx_lock(lp->ndev);
	xemacps_tx_clean(lp);
	netif_tx_unlock(lp->ndev);
}

/**
 * xemacps_tx_task - reenable transmit after transmit buffers have been cleaned
 * @work: work struct
 **/
static void xemacps_tx_task(struct work_struct *work)
{
	struct net_local *lp =
		container_of(work, struct net_local, tx_task.work);
	int cleaned;

	netif_tx_lock(lp->ndev);
	cleaned = xemacps_tx_clean(lp);
	netif_tx_unlock(lp->ndev);

	if (cleaned)
		/* Start it back up. */
		netif_start_queue(lp->ndev);
	else if (time_after(jiffies, lp->tx_task_start_jiffies + HZ)) {
		/* Realistically, I don't know what circumstances could lead
		   to this, since in my testing we clean some descriptors the
		   first time through and restart the transmit queue. */
		dev_info(&lp->pdev->dev,
			"transmit didn't complete, resetting interface\n");
		schedule_work(&lp->reset_task);
	} else
		/* In the testing I've done, we never get here. We always
		   clean some descriptors the first time through and restart
		   the transmit queue. */
		schedule_delayed_work(&lp->tx_task, 1);
}

/**
 * xemacps_start_xmit - transmit a packet (called by kernel)
 * @skb: socket buffer
 * @ndev: network interface device structure
 * return NETDEV_TX_OK or NETDEV_TX_BUSY
 **/
static netdev_tx_t xemacps_start_xmit(struct sk_buff *skb,
				      struct net_device *ndev)
{
	struct net_local *lp = netdev_priv(ndev);
	struct ring_info *curr_inf;
	struct xemacps_bd *curr_bd;
	unsigned long flags;
	u32 regval;

	xemacps_tx_clean(lp);

	/* Realistically, I don't think we should ever get here. In the testing
	   I've done, I saw a maximum of 14 transmit packets in use at the same
	   time. I manually shortened the transmit ring to make sure this code
	   path is tested. */
	if (unlikely(lp->tx_bd_freecnt == 0)) {
		netif_stop_queue(ndev);
		lp->tx_task_start_jiffies = jiffies;
		schedule_delayed_work(&lp->tx_task, 1);
		return NETDEV_TX_BUSY;
	}

	curr_inf = &(lp->tx_skb[lp->tx_bd_tail]);
	curr_bd = &(lp->tx_bd[lp->tx_bd_tail]);

	curr_inf->mapping = dma_map_single(&lp->pdev->dev, skb->data,
					   skb_headlen(skb), DMA_TO_DEVICE);

	if (unlikely(dma_mapping_error(&lp->pdev->dev, curr_inf->mapping))) {
		/* There's nothing we can do about this. */
		dev_err(&lp->pdev->dev, "transmit DMA mapping error\n");
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	curr_inf->skb = skb;

	curr_bd->addr = curr_inf->mapping;
	wmb();

	regval = ACCESS_ONCE(curr_bd->ctrl);
	regval &= XEMACPS_TXBUF_WRAP_MASK;
	regval |= skb_headlen(skb);
	regval |= XEMACPS_TXBUF_LAST_MASK;
	curr_bd->ctrl = regval;
	wmb();

	++lp->tx_bd_tail;
	lp->tx_bd_tail %= XEMACPS_SEND_BD_CNT;

	--lp->tx_bd_freecnt;

	spin_lock_irqsave(&lp->nwctrl_lock, flags);
	xemacps_write(lp->baseaddr, XEMACPS_NWCTRL_OFFSET,
		      (lp->nwctrl_base | XEMACPS_NWCTRL_STARTTX_MASK));
	wmb();
	spin_unlock_irqrestore(&lp->nwctrl_lock, flags);

	if (unlikely((lp->needs_tx_stall_workaround))) {
		int loop_count = 0;

		/* We poll the transmit status register waiting for the packet
		   to be sent. At 1Gb, I've seen a maximum loop count of ~300.
		   At 100Mb, ~2300, and at 10Mb, ~22000. There are things we
		   could do to lower the impact of this polling loop, but I
		   don't think it's worth it to spend the time on a problem
		   that doesn't exist in Zynq chips we'll actually ship with.
		   This limits the transmit side to one buffer at a time in
		   progress, which precludes the condition that causes the
		   transmit stall hardware bug. */
		do {
			/* Wait for something other than TXGO to be set in the
			   transmit status register. */
			regval = xemacps_read(lp->baseaddr, XEMACPS_TXSR_OFFSET);
			regval &= ~XEMACPS_TXSR_TXGO_MASK;
			if (regval)
				break;
			cpu_relax();
			++loop_count;
		} while (100000 > loop_count);

		/* I don't know what circumstances could lead to this case. I
		   have never seen it in the testing I've done. */
		if (unlikely(!regval)) {
			dev_info(&lp->pdev->dev,
			  "transmit didn't complete, resetting interface\n");

			/* We don't own this SKB, make sure we don't try to
			   free it during the upcoming reset. */
			curr_inf->skb = NULL;

			schedule_work(&lp->reset_task);
			return NETDEV_TX_BUSY;
		}
	}

	/* If no other packets are transmitted in the meantime, the timer
	   callback will clean things up later. */
	mod_timer(&lp->tx_timer, jiffies + HZ);

	return NETDEV_TX_OK;
}

/*
 * Get the MAC Address bit from the specified position
 */
static unsigned get_bit(u8 *mac, unsigned bit)
{
	unsigned byte;

	byte = mac[bit / 8];
	byte >>= (bit & 0x7);
	byte &= 1;

	return byte;
}

/*
 * Calculate a GEM MAC Address hash index
 */
static unsigned calc_mac_hash(u8 *mac)
{
	int index_bit, mac_bit;
	unsigned hash_index;

	hash_index = 0;
	mac_bit = 5;
	for (index_bit = 5; index_bit >= 0; index_bit--) {
		hash_index |= (get_bit(mac,  mac_bit) ^
					get_bit(mac, mac_bit + 6) ^
					get_bit(mac, mac_bit + 12) ^
					get_bit(mac, mac_bit + 18) ^
					get_bit(mac, mac_bit + 24) ^
					get_bit(mac, mac_bit + 30) ^
					get_bit(mac, mac_bit + 36) ^
					get_bit(mac, mac_bit + 42))
						<< index_bit;
		mac_bit--;
	}

	return hash_index;
}

/**
 * xemacps_set_hashtable - Add multicast addresses to the internal
 * multicast-hash table. Called from xemac_set_rx_mode().
 * @ndev: network interface device structure
 *
 * The hash address register is 64 bits long and takes up two
 * locations in the memory map.  The least significant bits are stored
 * in EMAC_HSL and the most significant bits in EMAC_HSH.
 *
 * The unicast hash enable and the multicast hash enable bits in the
 * network configuration register enable the reception of hash matched
 * frames. The destination address is reduced to a 6 bit index into
 * the 64 bit hash register using the following hash function.  The
 * hash function is an exclusive or of every sixth bit of the
 * destination address.
 *
 * hi[5] = da[5] ^ da[11] ^ da[17] ^ da[23] ^ da[29] ^ da[35] ^ da[41] ^ da[47]
 * hi[4] = da[4] ^ da[10] ^ da[16] ^ da[22] ^ da[28] ^ da[34] ^ da[40] ^ da[46]
 * hi[3] = da[3] ^ da[09] ^ da[15] ^ da[21] ^ da[27] ^ da[33] ^ da[39] ^ da[45]
 * hi[2] = da[2] ^ da[08] ^ da[14] ^ da[20] ^ da[26] ^ da[32] ^ da[38] ^ da[44]
 * hi[1] = da[1] ^ da[07] ^ da[13] ^ da[19] ^ da[25] ^ da[31] ^ da[37] ^ da[43]
 * hi[0] = da[0] ^ da[06] ^ da[12] ^ da[18] ^ da[24] ^ da[30] ^ da[36] ^ da[42]
 *
 * da[0] represents the least significant bit of the first byte
 * received, that is, the multicast/unicast indicator, and da[47]
 * represents the most significant bit of the last byte received.  If
 * the hash index, hi[n], points to a bit that is set in the hash
 * register then the frame will be matched according to whether the
 * frame is multicast or unicast.  A multicast match will be signalled
 * if the multicast hash enable bit is set, da[0] is 1 and the hash
 * index points to a bit set in the hash register.  A unicast match
 * will be signalled if the unicast hash enable bit is set, da[0] is 0
 * and the hash index points to a bit set in the hash register.  To
 * receive all multicast frames, the hash register should be set with
 * all ones and the multicast hash enable bit should be set in the
 * network configuration register.
 **/
static void xemacps_set_hashtable(struct net_device *ndev)
{
	struct netdev_hw_addr *curr;
	u32 regvalh, regvall, hash_index;
	u8 *mc_addr;
	struct net_local *lp;

	lp = netdev_priv(ndev);

	regvalh = regvall = 0;

	netdev_for_each_mc_addr(curr, ndev) {
		if (!curr)	/* end of list */
			break;
		mc_addr = curr->addr;
		hash_index = calc_mac_hash(mc_addr);

		if (hash_index >= XEMACPS_MAX_HASH_BITS) {
			netdev_err(ndev, "hash calculation out of range %d\n",
				   hash_index);
			break;
		}
		if (hash_index < 32)
			regvall |= (1 << hash_index);
		else
			regvalh |= (1 << (hash_index - 32));
	}

	xemacps_write(lp->baseaddr, XEMACPS_HASHL_OFFSET, regvall);
	xemacps_write(lp->baseaddr, XEMACPS_HASHH_OFFSET, regvalh);
}

/**
 * xemacps_set_rx_mode - enable/disable promiscuous and multicast modes
 * @ndev: network interface device structure
 **/
static void xemacps_set_rx_mode(struct net_device *ndev)
{
	struct net_local *lp = netdev_priv(ndev);
	u32 regval;

	regval = xemacps_read(lp->baseaddr, XEMACPS_NWCFG_OFFSET);

	/* promisc mode */
	if (ndev->flags & IFF_PROMISC)
		regval |= XEMACPS_NWCFG_COPYALLEN_MASK;
	if (!(ndev->flags & IFF_PROMISC))
		regval &= ~XEMACPS_NWCFG_COPYALLEN_MASK;

	/* All multicast mode */
	if (ndev->flags & IFF_ALLMULTI) {
		regval |= XEMACPS_NWCFG_MCASTHASHEN_MASK;
		xemacps_write(lp->baseaddr, XEMACPS_HASHL_OFFSET, ~0UL);
		xemacps_write(lp->baseaddr, XEMACPS_HASHH_OFFSET, ~0UL);
	/* Specific multicast mode */
	} else if ((ndev->flags & IFF_MULTICAST)
			&& (netdev_mc_count(ndev) > 0)) {
		regval |= XEMACPS_NWCFG_MCASTHASHEN_MASK;
		xemacps_set_hashtable(ndev);
	/* Disable multicast mode */
	} else {
		xemacps_write(lp->baseaddr, XEMACPS_HASHL_OFFSET, 0x0);
		xemacps_write(lp->baseaddr, XEMACPS_HASHH_OFFSET, 0x0);
		regval &= ~XEMACPS_NWCFG_MCASTHASHEN_MASK;
	}

	/* broadcast mode */
	if (ndev->flags & IFF_BROADCAST)
		regval &= ~XEMACPS_NWCFG_BCASTDI_MASK;
	/* No broadcast */
	if (!(ndev->flags & IFF_BROADCAST))
		regval |= XEMACPS_NWCFG_BCASTDI_MASK;

	xemacps_write(lp->baseaddr, XEMACPS_NWCFG_OFFSET, regval);
}

#define MIN_MTU 60
#define MAX_MTU 1500
/**
 * xemacps_change_mtu - Change maximum transfer unit
 * @ndev: network interface device structure
 * @new_mtu: new vlaue for maximum frame size
 * return: 0 on success, negative value if error.
 **/
static int xemacps_change_mtu(struct net_device *ndev, int new_mtu)
{
	if ((new_mtu < MIN_MTU) ||
		((new_mtu + ndev->hard_header_len) > MAX_MTU))
		return -EINVAL;

	ndev->mtu = new_mtu;	/* change mtu in net_device structure */
	return 0;
}

/**
 * xemacps_get_settings - get device specific settings.
 * Usage: Issue "ethtool ethX" under linux prompt.
 * @ndev: network device
 * @ecmd: ethtool command structure
 * return: 0 on success, negative value if error.
 **/
static int
xemacps_get_settings(struct net_device *ndev, struct ethtool_cmd *ecmd)
{
	struct net_local *lp = netdev_priv(ndev);
	struct phy_device *phydev = lp->phy_dev;

	if (!phydev)
		return -ENODEV;

	return phy_ethtool_gset(phydev, ecmd);
}

/**
 * xemacps_set_settings - set device specific settings.
 * Usage: Issue "ethtool -s ethX speed 1000" under linux prompt
 * to change speed
 * @ndev: network device
 * @ecmd: ethtool command structure
 * return: 0 on success, negative value if error.
 **/
static int
xemacps_set_settings(struct net_device *ndev, struct ethtool_cmd *ecmd)
{
	struct net_local *lp = netdev_priv(ndev);
	struct phy_device *phydev = lp->phy_dev;

	if (!phydev)
		return -ENODEV;

	return phy_ethtool_sset(phydev, ecmd);
}

/**
 * xemacps_get_drvinfo - report driver information
 * Usage: Issue "ethtool -i ethX" under linux prompt
 * @ndev: network device
 * @ed: device driver information structure
 **/
static void
xemacps_get_drvinfo(struct net_device *ndev, struct ethtool_drvinfo *ed)
{
	struct net_local *lp = netdev_priv(ndev);

	memset(ed, 0, sizeof(struct ethtool_drvinfo));
	strcpy(ed->driver, lp->pdev->dev.driver->name);
	strcpy(ed->version, DRIVER_VERSION);
}

/**
 * xemacps_get_ringparam - get device dma ring information.
 * Usage: Issue "ethtool -g ethX" under linux prompt
 * @ndev: network device
 * @erp: ethtool ring parameter structure
 **/
static void
xemacps_get_ringparam(struct net_device *ndev, struct ethtool_ringparam *erp)
{
	memset(erp, 0, sizeof(struct ethtool_ringparam));

	erp->rx_max_pending = XEMACPS_RECV_BD_CNT;
	erp->tx_max_pending = XEMACPS_SEND_BD_CNT;
	erp->rx_pending = 0;
	erp->tx_pending = 0;
}

/**
 * xemacps_get_rx_csum - get device rxcsum status
 * Usage: Issue "ethtool -k ethX" under linux prompt
 * @ndev: network device
 * return 0 csum off, else csum on
 **/
static u32
xemacps_get_rx_csum(struct net_device *ndev)
{
	struct net_local *lp = netdev_priv(ndev);

	return (lp->ip_summed & CHECKSUM_UNNECESSARY) != 0;
}

/**
 * xemacps_set_rx_csum - set device rx csum enable/disable
 * Usage: Issue "ethtool -K ethX rx on|off" under linux prompt
 * @ndev: network device
 * @data: 0 to disable, other to enable
 * return 0 on success, negative value if error
 * note : If there is no need to turn on/off checksum engine e.g always on,
 * xemacps_set_rx_csum can be removed.
 **/
static int
xemacps_set_rx_csum(struct net_device *ndev, u32 data)
{
	struct net_local *lp = netdev_priv(ndev);

	if (data)
		lp->ip_summed = CHECKSUM_UNNECESSARY;
	else
		lp->ip_summed = CHECKSUM_NONE;

	return 0;
}

/**
 * xemacps_get_tx_csum - get device txcsum status
 * Usage: Issue "ethtool -k ethX" under linux prompt
 * @ndev: network device
 * return 0 csum off, 1 csum on
 **/
static u32
xemacps_get_tx_csum(struct net_device *ndev)
{
	return (ndev->features & NETIF_F_IP_CSUM) != 0;
}

/**
 * xemacps_set_tx_csum - set device tx csum enable/disable
 * Usage: Issue "ethtool -K ethX tx on|off" under linux prompt
 * @ndev: network device
 * @data: 0 to disable, other to enable
 * return 0 on success, negative value if error
 * note : If there is no need to turn on/off checksum engine e.g always on,
 * xemacps_set_tx_csum can be removed.
 **/
static int
xemacps_set_tx_csum(struct net_device *ndev, u32 data)
{
	if (data)
		ndev->features |= NETIF_F_IP_CSUM;
	else
		ndev->features &= ~NETIF_F_IP_CSUM;
	return 0;
}

/**
 * xemacps_get_wol - get device wake on lan status
 * Usage: Issue "ethtool ethX" under linux prompt
 * @ndev: network device
 * @ewol: wol status
 **/
static void
xemacps_get_wol(struct net_device *ndev, struct ethtool_wolinfo *ewol)
{
	struct net_local *lp = netdev_priv(ndev);
	u32 regval;

	ewol->supported = WAKE_MAGIC | WAKE_ARP | WAKE_UCAST | WAKE_MCAST;
	regval = xemacps_read(lp->baseaddr, XEMACPS_WOL_OFFSET);
	if (regval & XEMACPS_WOL_MCAST_MASK)
		ewol->wolopts |= WAKE_MCAST;
	if (regval & XEMACPS_WOL_ARP_MASK)
		ewol->wolopts |= WAKE_ARP;
	if (regval & XEMACPS_WOL_SPEREG1_MASK)
		ewol->wolopts |= WAKE_UCAST;
	if (regval & XEMACPS_WOL_MAGIC_MASK)
		ewol->wolopts |= WAKE_MAGIC;
}

/**
 * xemacps_set_wol - set device wake on lan configuration
 * Usage: Issue "ethtool -s ethX wol u|m|b|g" under linux prompt to enable
 * specified type of packet.
 * Usage: Issue "ethtool -s ethX wol d" under linux prompt to disable
 * this feature.
 * @ndev: network device
 * @ewol: wol status
 * return 0 on success, negative value if not supported
 **/
static int
xemacps_set_wol(struct net_device *ndev, struct ethtool_wolinfo *ewol)
{
	struct net_local *lp = netdev_priv(ndev);
	u32 regval;

	if (ewol->wolopts & ~(WAKE_MAGIC | WAKE_ARP | WAKE_UCAST | WAKE_MCAST))
		return -EOPNOTSUPP;

	regval  = xemacps_read(lp->baseaddr, XEMACPS_WOL_OFFSET);
	regval &= ~(XEMACPS_WOL_MCAST_MASK | XEMACPS_WOL_ARP_MASK |
		XEMACPS_WOL_SPEREG1_MASK | XEMACPS_WOL_MAGIC_MASK);

	if (ewol->wolopts & WAKE_MAGIC)
		regval |= XEMACPS_WOL_MAGIC_MASK;
	if (ewol->wolopts & WAKE_ARP)
		regval |= XEMACPS_WOL_ARP_MASK;
	if (ewol->wolopts & WAKE_UCAST)
		regval |= XEMACPS_WOL_SPEREG1_MASK;
	if (ewol->wolopts & WAKE_MCAST)
		regval |= XEMACPS_WOL_MCAST_MASK;

	xemacps_write(lp->baseaddr, XEMACPS_WOL_OFFSET, regval);

	return 0;
}

/**
 * xemacps_get_pauseparam - get device pause status
 * Usage: Issue "ethtool -a ethX" under linux prompt
 * @ndev: network device
 * @epauseparam: pause parameter
 *
 * note: hardware supports only tx flow control
 **/
static void
xemacps_get_pauseparam(struct net_device *ndev,
		struct ethtool_pauseparam *epauseparm)
{
	struct net_local *lp = netdev_priv(ndev);
	u32 regval;

	epauseparm->autoneg  = 0;
	epauseparm->rx_pause = 0;

	regval = xemacps_read(lp->baseaddr, XEMACPS_NWCFG_OFFSET);
	epauseparm->tx_pause = regval & XEMACPS_NWCFG_PAUSEEN_MASK;
}

/**
 * xemacps_set_pauseparam - set device pause parameter(flow control)
 * Usage: Issue "ethtool -A ethX tx on|off" under linux prompt
 * @ndev: network device
 * @epauseparam: pause parameter
 * return 0 on success, negative value if not supported
 *
 * note: hardware supports only tx flow control
 **/
static int
xemacps_set_pauseparam(struct net_device *ndev,
		struct ethtool_pauseparam *epauseparm)
{
	struct net_local *lp = netdev_priv(ndev);
	u32 regval;

	if (netif_running(ndev)) {
		netdev_err(ndev, "Please stop netif before apply configuration\n");
		return -EFAULT;
	}

	regval = xemacps_read(lp->baseaddr, XEMACPS_NWCFG_OFFSET);

	if (epauseparm->tx_pause)
		regval |= XEMACPS_NWCFG_PAUSEEN_MASK;
	if (!(epauseparm->tx_pause))
		regval &= ~XEMACPS_NWCFG_PAUSEEN_MASK;

	xemacps_write(lp->baseaddr, XEMACPS_NWCFG_OFFSET, regval);

	return 0;
}

/**
 * xemacps_update_stats - update device statistics
 * @lp: xemacps device
 *
 * Note: This is necessary because xemacps statistic registers are cleared on
 * read.
 */
static void xemacps_update_stats(struct net_local *lp)
{
	int i;

	for (i=0; i<XEMACPS_STATS_LEN; i++) {
		u32 off = xemacps_statistics[i].offset;
		u64 val = xemacps_read(lp->baseaddr, off);

		switch (off) {
		case XEMACPS_OCTTXL_OFFSET:
		case XEMACPS_OCTRXL_OFFSET:
			/* Add XEMACPS_OCTTXH, XEMACPS_OCTRXH */
			lp->ethtool_stats[i] +=
				((u64)xemacps_read(lp->baseaddr, off+4))<<32;
			/* fall through */
		default:
			lp->ethtool_stats[i] += val;
			break;
		}
	}
}

/**
 * xemacps_get_stats - get device statistic raw data in 64bit mode
 * @ndev: network device
 **/
static struct net_device_stats* xemacps_get_stats(struct net_device *ndev)
{
	struct net_local *lp = netdev_priv(ndev);
	struct net_device_stats *nstat = &lp->stats;
	int i;

	xemacps_update_stats(lp);
	nstat->rx_errors=0;
	nstat->rx_length_errors=0;
	nstat->rx_over_errors=0;
	nstat->rx_crc_errors=0;
	nstat->rx_frame_errors=0;
	nstat->rx_fifo_errors=0;
	nstat->tx_errors=0;
	nstat->tx_aborted_errors=0;
	nstat->tx_carrier_errors=0;
	nstat->tx_fifo_errors=0;
	nstat->collisions=0;

	for (i=0; i<XEMACPS_STATS_LEN; i++) {
		u32 bits = xemacps_statistics[i].stat_bits;
		u64 val = lp->ethtool_stats[i];

		nstat->rx_errors += (bits & NDS_RXERR) ? val : 0;
		nstat->rx_length_errors += (bits & NDS_RXLENERR) ? val : 0;
		nstat->rx_over_errors += (bits & NDS_RXOVERERR) ? val : 0;
		nstat->rx_crc_errors += (bits & NDS_RXCRCERR) ? val : 0;
		nstat->rx_frame_errors += (bits & NDS_RXFRAMEERR) ? val : 0;
		nstat->rx_fifo_errors += (bits & NDS_RXFIFOERR) ? val : 0;
		nstat->tx_errors += (bits & NDS_TXERR) ? val : 0;
		nstat->tx_aborted_errors += (bits & NDS_TXABORTEDERR) ? val : 0;
		nstat->tx_carrier_errors += (bits & NDS_TXCARRIERERR) ? val : 0;
		nstat->tx_fifo_errors += (bits & NDS_TXFIFOERR) ? val : 0;
		nstat->collisions += (bits & NDS_COLLISIONS) ? val : 0;
	}
	return nstat;
}

static void xemacps_get_ethtool_stats(struct net_device *netdev,
	struct ethtool_stats *stats,
	u64 *data)
{
	struct net_local *lp;

	lp = netdev_priv(netdev);
	xemacps_update_stats(lp);
	memcpy(data, &lp->ethtool_stats, sizeof(u64)*XEMACPS_STATS_LEN);
}

static int xemacps_get_sset_count(struct net_device *netdev, int sset)
{
	switch (sset) {
	case ETH_SS_STATS:
		return XEMACPS_STATS_LEN;
	default:
		return -EOPNOTSUPP;
	}
}

static void xemacps_get_ethtool_strings(struct net_device *netdev,
	u32 sset, u8 *p)
{
	int i;

	switch (sset) {
	case ETH_SS_STATS:
		for (i=0; i < XEMACPS_STATS_LEN; i++, p += ETH_GSTRING_LEN) {
			memcpy(p, xemacps_statistics[i].stat_string,
				ETH_GSTRING_LEN);
		}
		break;
	}
}

static struct ethtool_ops xemacps_ethtool_ops = {
	.get_settings   = xemacps_get_settings,
	.set_settings   = xemacps_set_settings,
	.get_drvinfo    = xemacps_get_drvinfo,
	.get_link       = ethtool_op_get_link,       /* ethtool default */
	.get_ringparam  = xemacps_get_ringparam,
	.get_rx_csum    = xemacps_get_rx_csum,
	.set_rx_csum    = xemacps_set_rx_csum,
	.get_tx_csum    = xemacps_get_tx_csum,
	.set_tx_csum    = xemacps_set_tx_csum,
	.get_wol        = xemacps_get_wol,
	.set_wol        = xemacps_set_wol,
	.get_sg         = ethtool_op_get_sg,         /* ethtool default */
	.get_tso        = ethtool_op_get_tso,        /* ethtool default */
	.get_pauseparam = xemacps_get_pauseparam,
	.set_pauseparam = xemacps_set_pauseparam,
	.get_ethtool_stats = xemacps_get_ethtool_stats,
	.get_strings    = xemacps_get_ethtool_strings,
	.get_sset_count = xemacps_get_sset_count,
};

#ifdef CONFIG_XILINX_PS_EMAC_HWTSTAMP
static int xemacps_hwtstamp_ioctl(struct net_device *netdev,
				struct ifreq *ifr, int cmd)
{
	struct hwtstamp_config config;
	struct net_local *lp;
	u32 regval;

	lp = netdev_priv(netdev);

	if (copy_from_user(&config, ifr->ifr_data, sizeof(config)))
		return -EFAULT;

	/* reserved for future extensions */
	if (config.flags)
		return -EINVAL;

	if ((config.tx_type != HWTSTAMP_TX_OFF) &&
		(config.tx_type != HWTSTAMP_TX_ON))
		return -ERANGE;

	switch (config.rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		break;
	case HWTSTAMP_FILTER_PTP_V1_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
	case HWTSTAMP_FILTER_ALL:
	case HWTSTAMP_FILTER_PTP_V1_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
		config.rx_filter = HWTSTAMP_FILTER_ALL;
		regval = xemacps_read(lp->baseaddr, XEMACPS_NWCTRL_OFFSET);
		xemacps_write(lp->baseaddr, XEMACPS_NWCTRL_OFFSET,
			(regval | XEMACPS_NWCTRL_RXTSTAMP_MASK));
		break;
	default:
		return -ERANGE;
	}

	config.tx_type = HWTSTAMP_TX_ON;
	lp->hwtstamp_config = config;

	return copy_to_user(ifr->ifr_data, &config, sizeof(config)) ?
		-EFAULT : 0;
}
#endif /* CONFIG_XILINX_PS_EMAC_HWTSTAMP */

/**
 * xemacps_ioctl - ioctl entry point
 * @ndev: network device
 * @rq: interface request ioctl
 * @cmd: command code
 *
 * Called when user issues an ioctl request to the network device.
 **/
static int xemacps_ioctl(struct net_device *ndev, struct ifreq *rq, int cmd)
{
	struct net_local *lp = netdev_priv(ndev);
	struct phy_device *phydev = lp->phy_dev;

	if (!netif_running(ndev))
		return -EINVAL;

	if (!phydev)
		return -ENODEV;

	switch (cmd) {
	case SIOCGMIIPHY:
	case SIOCGMIIREG:
	case SIOCSMIIREG:
		return phy_mii_ioctl(phydev, rq, cmd);
#ifdef CONFIG_XILINX_PS_EMAC_HWTSTAMP
	case SIOCSHWTSTAMP:
		return xemacps_hwtstamp_ioctl(ndev, rq, cmd);
#endif
	default:
		return -EOPNOTSUPP;
	}

}

#ifdef CONFIG_FPGA_PERIPHERAL

int xemacps_fpga_notifier(struct notifier_block *nb, unsigned long val, void *data)
{
	struct net_local *lp =
		container_of(nb, struct net_local, fpga_notifier);

	switch(val) {
		case FPGA_PERIPHERAL_DOWN:
			dev_dbg(&lp->pdev->dev,
				"xemacps_fpga_notifier: going down\n");

			/* Synchronize with xemacps_open/close. */
			rtnl_lock();
			if (!test_bit(XEMACPS_STATE_FPGA_DOWN, &lp->flags)) {
				/* If the interface has been opened. */
				if (netif_running(lp->ndev))
					xemacps_down(lp->ndev);

				set_bit(XEMACPS_STATE_FPGA_DOWN, &lp->flags);
			}
			rtnl_unlock();
			break;

		case FPGA_PERIPHERAL_UP:
			dev_dbg(&lp->pdev->dev,
				"xemacps_fpga_notifier: coming up\n");

			BUG_ON(!test_bit(XEMACPS_STATE_FPGA_DOWN, &lp->flags));

			/* Synchronize with xemacps_open/close. */
			rtnl_lock();

			clear_bit(XEMACPS_STATE_FPGA_DOWN, &lp->flags);

			/* If the interface has been opened. */
			if (netif_running(lp->ndev))
				xemacps_up(lp->ndev);

			rtnl_unlock();
			break;
		case FPGA_PERIPHERAL_FAILED:
			/* This interface is not coming back up. */
			break;
		default:
			dev_err(&lp->pdev->dev, "unsupported FPGA notifier value %lu\n", val);
			break;
	}

	return notifier_from_errno(0);
}

#endif

/**
 * xemacps_probe - Platform driver probe
 * @pdev: Pointer to platform device structure
 *
 * Return 0 on success, negative value if error
 **/
static int __init xemacps_probe(struct platform_device *pdev)
{
	struct resource *r_mem = NULL;
	struct resource *r_irq = NULL;
	struct net_device *ndev;
	struct net_local *lp;
	struct device_node *np;
	const void *prop;
	u32 regval = 0;
	int rc = -ENXIO;
	int create_mdio_bus = 1;
	int enetnum;

	r_mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	r_irq = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!r_mem || !r_irq) {
		dev_err(&pdev->dev, "no IO resource defined.\n");
		rc = -ENXIO;
		goto err_out;
	}

	ndev = alloc_etherdev(sizeof(*lp));
	if (!ndev) {
		dev_err(&pdev->dev, "etherdev allocation failed.\n");
		rc = -ENOMEM;
		goto err_out;
	}

	SET_NETDEV_DEV(ndev, &pdev->dev);

	lp = netdev_priv(ndev);
	lp->pdev = pdev;
	lp->ndev = ndev;

	lp->baseaddr = ioremap(r_mem->start, (r_mem->end - r_mem->start + 1));
	if (!lp->baseaddr) {
		dev_err(&pdev->dev, "failed to map baseaddress.\n");
		rc = -ENOMEM;
		goto err_out_free_netdev;
	}

	ndev->irq = platform_get_irq(pdev, 0);

	ndev->netdev_ops	 = &netdev_ops;
	ndev->ethtool_ops        = &xemacps_ethtool_ops;
	ndev->base_addr          = r_mem->start;
	ndev->features           = NETIF_F_IP_CSUM;
	netif_napi_add(ndev, &lp->napi, xemacps_rx_poll, XEMACPS_NAPI_WEIGHT);

	lp->ip_summed = CHECKSUM_UNNECESSARY;
	lp->board_type = BOARD_TYPE_ZYNQ;

	/* Clear statistic counters. The network stack will start polling for
	   stats as soon as we register below, and there may be stale data in
	   the stats registers. */
	xemacps_write(lp->baseaddr, XEMACPS_NWCTRL_OFFSET,
		XEMACPS_NWCTRL_STATCLR_MASK);

	rc = register_netdev(ndev);
	if (rc) {
		dev_err(&pdev->dev, "Cannot register net device, aborting.\n");
		goto err_out_iounmap;
	}

	netdev_dbg(ndev, "BASEADDRESS hw: %p virt: %p\n",
			(void *)r_mem->start, lp->baseaddr);

	if (ndev->irq == 54) { /* If it is ENET0 */
		enetnum = 0;
		lp->slcr_div_reg = XSLCR_EMAC0_CLK_CTRL_OFFSET;
	} else {	     /* If it is ENET1 */
		enetnum = 1;
		lp->slcr_div_reg = XSLCR_EMAC1_CLK_CTRL_OFFSET;
	}

	np = of_get_next_parent(lp->pdev->dev.of_node);
	np = of_get_next_parent(np);
	prop = of_get_property(np, "compatible", NULL);

	if (prop != NULL) {
		if ((strcmp((const char *)prop, "xlnx,zynq-ep107")) == 0)
			lp->board_type = BOARD_TYPE_PEEP;
		 else
			lp->board_type = BOARD_TYPE_ZYNQ;
	} else
		lp->board_type = BOARD_TYPE_ZYNQ;
	if (lp->board_type == BOARD_TYPE_ZYNQ) {
		prop = of_get_property(lp->pdev->dev.of_node,
					"xlnx,slcr-div0-1000Mbps", NULL);
		if (prop) {
			lp->slcr_div0_1000Mbps = (u32)be32_to_cpup(prop);
		} else {
			lp->slcr_div0_1000Mbps = XEMACPS_DFLT_SLCR_DIV0_1000;
		}
		prop = of_get_property(lp->pdev->dev.of_node,
					"xlnx,slcr-div1-1000Mbps", NULL);
		if (prop) {
			lp->slcr_div1_1000Mbps = (u32)be32_to_cpup(prop);
		} else {
			lp->slcr_div1_1000Mbps = XEMACPS_DFLT_SLCR_DIV1_1000;
		}
		prop = of_get_property(lp->pdev->dev.of_node,
					"xlnx,slcr-div0-100Mbps", NULL);
		if (prop) {
			lp->slcr_div0_100Mbps = (u32)be32_to_cpup(prop);
		} else {
			lp->slcr_div0_100Mbps = XEMACPS_DFLT_SLCR_DIV0_100;
		}
		prop = of_get_property(lp->pdev->dev.of_node,
					"xlnx,slcr-div1-100Mbps", NULL);
		if (prop) {
			lp->slcr_div1_100Mbps = (u32)be32_to_cpup(prop);
		} else {
			lp->slcr_div1_100Mbps = XEMACPS_DFLT_SLCR_DIV1_100;
		}
		prop = of_get_property(lp->pdev->dev.of_node,
					"xlnx,slcr-div0-10Mbps", NULL);
		if (prop) {
			lp->slcr_div0_10Mbps = (u32)be32_to_cpup(prop);
		} else {
			lp->slcr_div0_10Mbps = XEMACPS_DFLT_SLCR_DIV0_10;
		}
		prop = of_get_property(lp->pdev->dev.of_node,
					"xlnx,slcr-div1-10Mbps", NULL);
		if (prop) {
			lp->slcr_div1_10Mbps = (u32)be32_to_cpup(prop);
		} else {
			lp->slcr_div1_10Mbps = XEMACPS_DFLT_SLCR_DIV1_10;
		}
	}
#ifdef CONFIG_XILINX_PS_EMAC_HWTSTAMP
	if (lp->board_type == BOARD_TYPE_ZYNQ) {
		prop = of_get_property(lp->pdev->dev.of_node,
					"xlnx,ptp-enet-clock", NULL);
		if (prop) {
			lp->ptpenetclk = (u32)be32_to_cpup(prop);
		} else {
			lp->ptpenetclk = 133333328;
		}
	} else
		lp->ptpenetclk = PEEP_TSU_CLK;
#endif

	lp->phy_node = of_parse_phandle(lp->pdev->dev.of_node,
						"phy-handle", 0);

	/* Look for MDCCLKDIV. */
	prop = of_get_property(lp->pdev->dev.of_node,
				"xlnx,mdc-clk-div", NULL);
	if(prop) {
		lp->mdc_clk_div = be32_to_cpup(prop);
	} else {
		lp->mdc_clk_div = MDC_DIV_224;
	}

	if (lp->board_type == BOARD_TYPE_ZYNQ) {
		/* Set MDIO clock divider */
		regval = (lp->mdc_clk_div << XEMACPS_NWCFG_MDC_SHIFT_MASK);
		xemacps_write(lp->baseaddr, XEMACPS_NWCFG_OFFSET, regval);
	}

	if (of_get_property(lp->pdev->dev.of_node, "xlnx,no_mdio_bus", NULL)) {
		create_mdio_bus = 0;
	}

	/* Look for EMIO FPGA clock configuration. */
	prop = of_get_property(lp->pdev->dev.of_node, "xlnx,emio-fpga-clk", NULL);

	if (prop) {
		int fpga_clk = be32_to_cpup(prop);

		if ((0 <= fpga_clk) && (3 >= fpga_clk)) {
			int rx_clk_ctrl = XSLCR_EMAC1_RCLK_CTRL_OFFSET;
			int tx_clk_ctrl = XSLCR_EMAC1_CLK_CTRL_OFFSET;

			switch(fpga_clk)
			{
				case 0:
					lp->slcr_div_reg = XSLCR_FPGA0_CLK_CTRL_OFFSET;
					break;
				case 1:
					lp->slcr_div_reg = XSLCR_FPGA1_CLK_CTRL_OFFSET;
					break;
				case 2:
					lp->slcr_div_reg = XSLCR_FPGA2_CLK_CTRL_OFFSET;
					break;
				default:
				case 3:
					lp->slcr_div_reg = XSLCR_FPGA3_CLK_CTRL_OFFSET;
					break;
			}

			if (0 == enetnum) {
				rx_clk_ctrl = XSLCR_EMAC0_RCLK_CTRL_OFFSET;
				tx_clk_ctrl = XSLCR_EMAC0_CLK_CTRL_OFFSET;
			}

			/* Set the Rx and Tx clock source to be the PL. */
			xslcr_write(rx_clk_ctrl, 0x00000011);
			xslcr_write(tx_clk_ctrl, 0x00000041);
#ifdef CONFIG_FPGA_PERIPHERAL
			/* Register a blocking notifier for FPGA reprogramming
			   notifications. */
			lp->fpga_notifier.notifier_call = xemacps_fpga_notifier;

			blocking_notifier_chain_register(
				&fpgaperipheral_notifier_list,
				&lp->fpga_notifier);
#endif
		} else {
			dev_err(&pdev->dev, "Invalid EMIO FPGA clock configuration %d\n", fpga_clk);
		}
	}

	/* Look for a GPIO to indicate link speed to the PL as 10/100 (high)
	   or 1000 (low). */
	prop = of_get_property(lp->pdev->dev.of_node, "xlnx,emio-gpio-speed", NULL);

	if (prop)
		lp->gpiospeed = be32_to_cpup(prop);
	else
		lp->gpiospeed = -1;

	regval = XEMACPS_NWCTRL_MDEN_MASK;
	xemacps_write(lp->baseaddr, XEMACPS_NWCTRL_OFFSET, regval);

	if (create_mdio_bus && xemacps_mii_init(lp) != 0) {
		netdev_err(ndev, "error in xemacps_mii_init\n");
		goto err_out_unregister_netdev;
	}

	/* Read PSS_IDCODE and get the revision field. */
	regval = xslcr_read(XSLCR_PSS_IDCODE);
	regval &= XSLCR_PSS_IDCODE_REVISION_MASK;
	regval >>= XSLCR_PSS_IDCODE_REVISION_SHIFT;

	/* Transmit stall hardware bug is fixed if PSS_IDCODE revision field is
	   greater than zero. */
	if (0 == regval)
		lp->needs_tx_stall_workaround = true;

	/* Default to interrupt mode. */
	lp->ni_polling_interval = -1;

	if (0 != sysfs_create_file(&ndev->dev.kobj,
				   &dev_attr_ni_polling_interval.attr)) {
		netdev_err(ndev, "error creating sysfs file\n");
		goto err_out_unregister_netdev;
	}

	/* Default to SCHED_FIFO policy. */
	lp->ni_polling_policy = SCHED_FIFO;

	if (0 != sysfs_create_file(&ndev->dev.kobj,
				   &dev_attr_ni_polling_policy.attr)) {
		netdev_err(ndev, "error creating sysfs file\n");
		goto err_out_sysfs_remove_file1;
	}

	/* Default to priority 10. */
	lp->ni_polling_priority = 10;

	if (0 != sysfs_create_file(&ndev->dev.kobj,
				   &dev_attr_ni_polling_priority.attr)) {
		netdev_err(ndev, "error creating sysfs file\n");
		goto err_out_sysfs_remove_file2;
	}

	spin_lock_init(&lp->nwctrl_lock);

	INIT_DELAYED_WORK(&lp->tx_task, xemacps_tx_task);
	setup_timer(&lp->tx_timer, xemacps_tx_timer, (unsigned long)lp);
	setup_timer(&lp->rx_timer, xemacps_rx_timer, (unsigned long)lp);
	INIT_WORK(&lp->reset_task, xemacps_reset_task);

	xemacps_update_hwaddr(lp);

	/* Carrier off reporting is important to ethtool even BEFORE open. */
	netif_carrier_off(ndev);

	set_bit(XEMACPS_STATE_DOWN, &lp->flags);

	platform_set_drvdata(pdev, ndev);

	netdev_info(ndev, "pdev->id %d, baseaddr 0x%08lx, irq %d\n",
		    pdev->id, ndev->base_addr, ndev->irq);

	return 0;

err_out_sysfs_remove_file2:
	sysfs_remove_file(&ndev->dev.kobj,
			  &dev_attr_ni_polling_policy.attr);
err_out_sysfs_remove_file1:
	sysfs_remove_file(&ndev->dev.kobj,
			  &dev_attr_ni_polling_interval.attr);
err_out_unregister_netdev:
	unregister_netdev(ndev);
err_out_iounmap:
	iounmap(lp->baseaddr);
err_out_free_netdev:
	free_netdev(ndev);
err_out:
	platform_set_drvdata(pdev, NULL);
	return rc;
}

/**
 * xemacps_remove - called when platform driver is unregistered
 * @pdev: Pointer to the platform device structure
 *
 * Note: it's currently only safe to remove the second MAC driver. Removing
 * the first one will cause a crash. You can remove the first one if the
 * second one has already been removed, but that doesn't seem very useful.
 * Our current EtherCAT use case is for secondary Ethernet only, but that
 * may change in the future, in which case we would need to resolve this
 * problem. The cause of this problem is the shared MDIO interface attached
 * to the primary MAC.
 *
 * return: 0 on success
 **/
static int __exit xemacps_remove(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct net_local *lp;

	if (ndev) {
		lp = netdev_priv(ndev);
#ifdef CONFIG_FPGA_PERIPHERAL
		blocking_notifier_chain_unregister(
			&fpgaperipheral_notifier_list,
			&lp->fpga_notifier);
#endif
		if (lp->phy_dev)
			phy_disconnect(lp->phy_dev);

		if (lp->mii_bus) {
			mdiobus_unregister(lp->mii_bus);
			kfree(lp->mii_bus->irq);
			mdiobus_free(lp->mii_bus);
		}
		sysfs_remove_file(&ndev->dev.kobj,
				  &dev_attr_ni_polling_priority.attr);
		sysfs_remove_file(&ndev->dev.kobj,
				  &dev_attr_ni_polling_policy.attr);
		sysfs_remove_file(&ndev->dev.kobj,
				  &dev_attr_ni_polling_interval.attr);
		unregister_netdev(ndev);
		iounmap(lp->baseaddr);
		free_netdev(ndev);
		platform_set_drvdata(pdev, NULL);
	}

	return 0;
}

int xemacps_dev_remove (struct device *dev)
{
	return xemacps_remove(to_platform_device(dev));
}

/**
 * xemacps_suspend - Suspend event
 * @pdev: Pointer to platform device structure
 * @state: State of the device
 *
 * Return 0
 **/
static int xemacps_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct net_device *ndev = platform_get_drvdata(pdev);

	netif_device_detach(ndev);
	return 0;
}

/**
 * xemacps_resume - Resume after previous suspend
 * @pdev: Pointer to platform device structure
 *
 * Return 0
 **/
static int xemacps_resume(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);

	netif_device_attach(ndev);
	return 0;
}

static struct net_device_ops netdev_ops = {
	.ndo_open		= xemacps_open,
	.ndo_stop		= xemacps_close,
	.ndo_start_xmit		= xemacps_start_xmit,
	.ndo_set_rx_mode	= xemacps_set_rx_mode,
	.ndo_set_mac_address    = xemacps_set_mac_address,
	.ndo_do_ioctl		= xemacps_ioctl,
	.ndo_change_mtu		= xemacps_change_mtu,
	.ndo_get_stats		= xemacps_get_stats,
};


static struct of_device_id xemacps_of_match[] __devinitdata = {
	{ .compatible = "xlnx,ps7-ethernet-1.00.a", },
	{ /* end of table */}
};
MODULE_DEVICE_TABLE(of, xemacps_of_match);


static struct platform_driver xemacps_driver = {
	.probe   = xemacps_probe,
	.remove  = __exit_p(xemacps_remove),
	.suspend = xemacps_suspend,
	.resume  = xemacps_resume,
	.driver  = {
		.name  = DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table = xemacps_of_match,
		.remove = xemacps_dev_remove,
	},
};

module_platform_driver(xemacps_driver);

MODULE_AUTHOR("Xilinx, Inc.");
MODULE_DESCRIPTION("Xilinx Ethernet driver");
MODULE_LICENSE("GPL v2");
