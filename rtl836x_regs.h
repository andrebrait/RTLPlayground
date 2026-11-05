#ifndef _RTL836X_REGS_H_
#define _RTL836X_REGS_H_

/*
 * The RTL8367C/RTL8370 registers the RTL836x port uses: 16 bit wide, names as in
 * Realtek's rtl8367c_reg.h without the RTL8367C_ prefix.
 */

/* Chip */
#define RTL836X_REG_CHIP_NUMBER			0x1300
#define RTL836X_REG_CHIP_VER			0x1301
#define RTL836X_REG_MAGIC_ID			0x13c2
#define RTL836X_MAGIC_ID_VALUE			0x0249	/* unlocks CHIP_NUMBER */
#define RTL836X_REG_CHIP_RESET			0x1322
#define CHIP_RESET_CHIP_RST			0x0001
#define CHIP_RESET_NIC_RST			0x0020
#define RTL836X_REG_DW8051_RDY			0x1336
#define RTL836X_REG_IO_MISC_FUNC		0x1d32
#define RTL836X_REG_MISCELLANEOUS_CONFIGURE0	0x130c
#define MISC_CFG0_NIC_ENABLE			0x0400
#define RTL836X_REG_PHY_AD			0x130f
#define PHY_AD_PDNPHY				0x0020
#define RTL836X_REG_M10_FORCE_SELECT		0x131c
#define RTL836X_REG_PORT0_STATUS		0x1352	/* + port */
#define PORT_STATUS_LINK			0x0010
#define PORT_STATUS_DUPLEX			0x0004	/* full duplex */
#define PORT_STATUS_SPEED_MASK			0x0003	/* 0: 10M, 1: 100M, 2: 1000M */
#define PORT_STATUS_TX_PAUSE			0x0040	/* TX_FLOWCTRL_CAP */
#define PORT_STATUS_RX_PAUSE			0x0020	/* RX_FLOWCTRL_CAP */
#define PORT_STATUS_EEE_CAP			0x0c00
#define RTL836X_REG_PORT0_EEE_CFG		0x0018	/* + (port << 5) */
#define RTL836X_EEE_100M			0x0800
#define RTL836X_EEE_1G				0x0400

/* Read-only MIB data latch; only the address selector is written. */
#define RTL836X_REG_MIB_COUNTER			0x1000
#define RTL836X_REG_MIB_ADDRESS			0x1004
#define RTL836X_REG_MIB_CTRL			0x1005
#define RTL836X_MIB_BUSY				0x0001
#define RTL836X_MIB_RESET			0x0002

/* NIC */
#define RTL836X_REG_NIC_RXRDRL			0x1a04
#define RTL836X_REG_NIC_RXRDRH			0x1a05
#define RTL836X_REG_NIC_TXASRL			0x1a08
#define RTL836X_REG_NIC_TXASRH			0x1a09
#define RTL836X_REG_NIC_RXCMDR			0x1a0c
#define RTL836X_REG_NIC_TXCMDR			0x1a0d
#define RTL836X_REG_NIC_IMS			0x1a0e
#define RTL836X_REG_NIC_IMR			0x1a0f
#define NIC_IMR_RXIE				0x0080
#define NIC_IMS_RXIS				0x0080
#define RTL836X_REG_NIC_RXCR1			0x1a15
#define RTL836X_REG_NIC_TXCR			0x1a16
#define RTL836X_REG_NIC_GCR			0x1a17
#define RTL836X_REG_NIC_TXSTOPRL		0x1a44
#define RTL836X_REG_NIC_TXSTOPRH		0x1a45
#define RTL836X_REG_NIC_RXSTOPRL		0x1a46
#define RTL836X_REG_NIC_RXSTOPRH		0x1a47
#define RTL836X_REG_NIC_RXFSTR			0x1a48
#define RTL836X_REG_NIC_CRXCPRL			0x1a50
#define RTL836X_REG_NIC_CRXCPRH			0x1a51
#define RTL836X_REG_NIC_CTXCPRL			0x1a52
#define RTL836X_REG_NIC_CTXCPRH			0x1a53
#define RTL836X_REG_NIC_STXCURPKTRL		0x1a56
#define RTL836X_REG_NIC_DROP_MODE		0x1a5c
/* RX ring as nic_setup() programs it with RXSTOPR 0x2ff: bytes 0x0000-0x17ff */
#define RTL836X_NIC_RX_RING_BYTES		0x1800

/* CPU port */
#define RTL836X_REG_CPU_PORT_MASK		0x1219
#define RTL836X_REG_CPU_CTRL			0x121a

/* Reserved multicast actions, rtl8367c_setAsicRma and rtl8367c_setAsicRmaLldp. */
#define RTL836X_REG_RMA_CTRL00			0x0800	/* + destination MAC's last byte */
#define RMA_OPERATION_MASK			0x0180
#define RTL836X_REG_RMA_CTRL_LLDP			0x0832
#define RTL836X_REG_RMA_LLDP_EN			0x0833

/* LEDs and GPIO */
#define RTL836X_REG_LED_SYS_CONFIG		0x1b00
#define RTL836X_REG_LED_CONFIGURATION		0x1b03
#define RTL836X_REG_CPU_FORCE_LED0_CFG0		0x1b08
#define RTL836X_REG_CPU_FORCE_LED1_CFG0		0x1b0a
#define RTL836X_REG_CPU_FORCE_LED2_CFG0		0x1b0c
#define RTL836X_REG_PARA_LED_IO_EN1		0x1b24
#define RTL836X_REG_PARA_LED_IO_EN2		0x1b25
#define RTL836X_REG_GPIO_67C_O_X0		0x1d1d
#define RTL836X_REG_GPIO_67C_OE_X0		0x1d21
#define RTL836X_REG_GPIO_MODE_67C_X0		0x1d25

/* PHY: OCP address bits 15:10 in GPHY_OCP_MSB_0, the rest in the 0x2000 window */
#define RTL836X_REG_PHYID_CFG0			0x1d01
#define RTL836X_REG_GPHY_OCP_MSB_0		0x1d15
#define RTL836X_PHY_WINDOW			0x2000
#define RTL836X_OCP_MII				0xa400	/* MII register r is OCP 0xa400 + 2 r */
#define RTL836X_OCP_EEE_ADV			0xa5d0	/* bits 1: 100M, 2: 1G */

/* RLDP, also the random number generator */
#define RTL836X_REG_RLDP_CTRL0			0x18e0
#define RLDP_GEN_RANDOM				3
#define RTL836X_REG_RLDP_CTRL4			0x18e4
#define RTL836X_REG_RLDP_RAND_NUM0		0x18e5
#define RTL836X_REG_RLDP_RAND_NUM1		0x18e6

/* Switch setup */
#define RTL836X_REG_ACL_ACCESS_MODE		0x06eb
#define RTL836X_REG_LINE_RATE_HSG_H		0x03fa
#define RTL836X_REG_MAX_LENGTH_CFG		0x0883	/* per port: bit port 10/100M, 8 + port 1G; 0 CFG0, 1 CFG1 */
#define RTL836X_REG_MAX_LEN_RX_TX		0x0884
#define RTL836X_REG_MAX_LEN_CFG0		0x088c	/* + cfg, frame length in bits 13:0 */
#define RTL836X_REG_PORT_ISOLATION_PORT0_MASK	0x08a2	/* + chip port */
#define RTL836X_REG_PORT_SECURITY_CTRL		0x08c8
#define RTL836X_REG_PORT_TRUNK_CTRL		0x08cf	/* hash fields 6:0 for all groups; 8 dumb mode, 7 flood: not ours */
#define PORT_TRUNK_CTRL_HASH			0x007f
#define RTL836X_REG_PORT_TRUNK_GROUP_MASK	0x08d0	/* group n: nibble n, bit k is chip port 4 n + k; the RTL8370N has 2 */
#define RTL836X_REG_MAX_LENGTH_LIMINT_IPG	0x1200
#define RTL836X_REG_MIRROR_CTRL			0x121c	/* source 3:0, monitor 7:4, RX 9, TX 10 */
#define MIRROR_CTRL_RX_TX			0x0600
#define RTL836X_REG_MIRROR_SRC_PMSK		0x12fb	/* chip port mask, shared by RX and TX */

/* VLAN, see the mapping in rtl836x_port.c */
#define RTL836X_REG_PORT0_MISC_CFG		0x000e	/* + (port << 5) */
#define PORT_MISC_CFG_VLAN_EGRESS_MODE		0x0030
#define RTL836X_REG_TABLE_ACCESS_CTRL		0x0500
#define TABLE_ACCESS_CVLAN_READ			0x0003
#define TABLE_ACCESS_CVLAN_WRITE		0x000b
#define RTL836X_REG_TABLE_ACCESS_ADDR		0x0501
#define RTL836X_REG_TABLE_LUT_ADDR		0x0502
#define TABLE_LUT_ADDR_BUSY			0x2000
#define RTL836X_REG_TABLE_WRITE_DATA0		0x0510
#define RTL836X_REG_TABLE_READ_DATA0		0x0520
/* RTL8370B LUT: 4096 SRAM entries and 64 CAM entries (SDK HAL). */
#define RTL836X_LUT_SIZE				0x1040
#define TABLE_ACCESS_LUT_ADDR			0x0014
#define TABLE_ACCESS_LUT_NEXT_UC			0x0034
#define TABLE_ACCESS_LUT_WRITE			0x000c
#define TABLE_LUT_HIT				0x1000
#define RTL836X_REG_VLAN_PVID_CTRL0		0x0700	/* + (port >> 1) */
#define RTL836X_REG_VLAN_MEMBER_CONFIGURATION0	0x0728	/* + 4 * index + word */
#define RTL836X_VLAN_MC_ENTRIES			32
#define RTL836X_REG_VLAN_CTRL			0x07a8
#define VLAN_CTRL_ENABLE			0x0001
#define RTL836X_REG_VLAN_INGRESS		0x07a9
#define RTL836X_REG_VLAN_ACCEPT_FRAME_TYPE_CTRL0	0x07aa	/* + (port >> 3) */
#define RTL836X_CPU_PORT			10	/* the NIC */

/* Rate control: RTL8367C SDK ingress, scheduling, meter and storm APIs. */
#define RTL836X_REG_INGRESS_RATE0			0x000f	/* + port * 32, low word then high three bits */
#define RTL836X_REG_EGRESS_RATE0			0x038c	/* + port * 2 */
#define RTL836X_REG_INGRESS_BYPASS			0x1214
#define RTL836X_REG_EGRESS_IFG			0x0300
#define RTL836X_REG_STORM_CTRL0			0x0a40	/* + type: broadcast, multicast, unknown UC, unknown MC */
#define RTL836X_REG_STORM_SELECT0			0x0a44	/* + type * 8 + port / 2 */
#define RTL836X_REG_METER_RATE0			0x1400
#define RTL836X_REG_METER_RATE32			0x1740
#define RTL836X_REG_METER_MODE0			0x1440
#define RTL836X_REG_METER_MODE32			0x1780
#define RTL836X_REG_METER_IFG0			0x1712
#define RTL836X_REG_METER_IFG32			0x17b4
#define RTL836X_REG_IGMP_MLD_CFG0			0x1c00

/* Spanning tree: MSTI0 port states, 2 bits per chip port: 0 disabled, 1 blocking, 2 learning, 3 forwarding.
 * CTRL0 holds chip ports 0-7, CTRL1 chip ports 8-10 (rtl8367c_setAsicSpanningTreeStatus). */
#define RTL836X_REG_VLAN_MSTI0_CTRL0		0x0a00
#define RTL836X_REG_VLAN_MSTI0_CTRL1		0x0a01

/* L2 flush (rtl8367c_reg.h:8872-8894). FORCE_FLUSH takes chip ports 0-7 as bits 7:0, FORCE_FLUSH1 takes
 * chip ports 8-10 as bits 2:0. Their busy flags are FORCE_FLUSH 15:8 and FORCE_FLUSH1 5:3. */
#define RTL836X_REG_FORCE_FLUSH1		0x0a35
#define RTL836X_REG_FORCE_FLUSH			0x0a36
#define FORCE_FLUSH_BUSY_MASK			0xff00
#define FORCE_FLUSH1_BUSY_MASK			0x0038
#define RTL836X_REG_L2_FLUSH_CTRL1		0x0a37	/* VID 11:0, FID 15:12; port flushes ignore them */
#define RTL836X_REG_L2_FLUSH_CTRL2		0x0a38
#define LUT_FLUSH_MODE_MASK			0x0003	/* 0 port, 1 port + VID, 2 port + FID/MSTI */
#define LUT_FLUSH_TYPE_MASK			0x0004	/* 0 dynamic unicast only, 1 static too */

/* SDK rtl8367c_setAsicRegBits: val is shifted to the mask's lowest bit */
void reg_bits(uint16_t reg, __xdata uint16_t mask, __xdata uint16_t val) __banked;

#endif
