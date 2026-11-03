/*
 * NIC of the RTL836x (RTL8370N), as the TP-Link boot code and the console
 * image drive it. Frames are converted to and from the RTL837x layout, so
 * handle_rx(), uIP and tcpip_output() see what they see on the RTL837x.
 */
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl836x_regs.h"
#include "rtl837x_nic.h"
#ifndef RTLP_HOST_TEST	/* the host test supplies the SFRs */
#include "rtl837x_sfr.h"
#endif

/* An RTL837x RX frame has an 8-byte CPU tag and a 4-byte VLAN tag behind DA and SA */
#define RX_TAGS_SIZE	(RTL_TAG_SIZE + VLAN_TAG_SIZE)

extern __xdata uint8_t rx_headers[16];

static __xdata uint8_t rx_hdr[8];	/* the RTL8370N's RX descriptor */
static __xdata uint16_t tx_last;	/* STXCURPKTR after the last send */
static __xdata bool tx_started;


#ifndef RTLP_HOST_TEST
/* One DMA in eight-byte units between XDATA and the NIC buffer. Despite the
 * name also used for TX: bit 15 of ring_ptr set moves data to the NIC. */
bool nic_rx_dma(__xdata uint8_t *buffer, uint16_t ring_ptr, uint8_t words) __reentrant
{
	uint16_t guard = 0;

	SFR_NIC_DATA_U16LE = (uint16_t)buffer;
	SFR_NIC_RING_U16LE = ring_ptr;
	SFR_NIC_CTRL = words;
	while (SFR_NIC_CTRL != 0) {
		if (++guard == 0) {
			print_string("NIC: transfer did not complete\n");
			return false;
		}
	}
	return true;
}
#endif


/* Counters split over a low register and the high one after it */
static uint16_t reg_read16(uint16_t reg_low)
{
	uint16_t v;

	reg_read(reg_low + 1);
	v = SFR_DATA_U16 << 8;
	reg_read(reg_low);
	return v | SFR_DATA_U16;
}


/*
 * Reads the RX descriptor at ring_ptr and gives rx_headers[] the RTL837x
 * meaning: bytes 4-5 the length including the tags nic_rx_packet() inserts,
 * the rest zero, so no RX checksum error is ever flagged.
 */
bool nic_rx_header(uint16_t ring_ptr) __reentrant
{
	uint16_t len;

	if (ring_ptr >= RTL836X_NIC_RX_RING_BYTES || !nic_rx_dma(rx_hdr, ring_ptr, 1))
		return false;
	len = (uint16_t)rx_hdr[6] << 8 | rx_hdr[7];	/* big endian, without the FCS */
	if ((rx_hdr[1] & 1) || len < ETHER_HEADER_SIZE
	    || RX_TAGS_SIZE + ((len + 7) & ~7) > UIP_CONF_BUFFER_ALLOC_SIZE)
		return false;
	memset(rx_headers, 0, 8);
	len += RX_TAGS_SIZE;
	rx_headers[4] = len;
	rx_headers[5] = len >> 8;
	return true;
}


/* ring_ptr is the descriptor's + 8; the data after the last slot starts at 0 */
bool nic_rx_packet(__xdata uint8_t *buffer, uint16_t ring_ptr) __reentrant
{
	uint16_t len = ((uint16_t)rx_headers[5] << 8 | rx_headers[4]) - RX_TAGS_SIZE;

	if (ring_ptr >= RTL836X_NIC_RX_RING_BYTES)
		ring_ptr -= RTL836X_NIC_RX_RING_BYTES;
	if (!nic_rx_dma(buffer + RX_TAGS_SIZE, ring_ptr, (len + 7) >> 3))
		return false;
	/* DA and SA to the front, empty tags behind them: the type lands where the RTL837x has it */
	for (uint8_t i = 0; i < 12; i++) {
		buffer[i] = buffer[RX_TAGS_SIZE + i];
		buffer[RX_TAGS_SIZE + i] = 0;
	}
	return true;
}


/* buffer starts at the 8-byte descriptor, len excludes it; ring_ptr from nic_tx_ptr() */
void nic_tx_packet(__xdata uint8_t *buffer, uint16_t len, uint16_t ring_ptr)
{
	for (; len < 60; len++)	/* the minimum frame without FCS: whether the MAC pads is unknown */
		buffer[8 + len] = 0;
	buffer[4] = 0;
	buffer[5] = 0;
	buffer[6] = len >> 8;	/* the RTL8370N takes the length big endian in bytes 6-7 */
	buffer[7] = len;
	nic_rx_dma(buffer, (ring_ptr << 3) | 0x8000, (len + 15) >> 3);
}


bool nic_rx_pending(void)
{
	return reg_read16(RTL836X_REG_NIC_RXRDRL) != 0;
}


uint16_t nic_rx_ptr(void)
{
	return reg_read16(RTL836X_REG_NIC_CRXCPRL) << 3;
}


void nic_rx_release(void)
{
	REG_SET(RTL836X_REG_NIC_RXCMDR, 1);
}


void nic_rx_ack(void)
{
	REG_SET(RTL836X_REG_NIC_IMS, NIC_IMS_RXIS);
}


/*
 * Waits until the NIC has taken the previous frame and has room for len bytes
 * plus the descriptor, as the boot code does. The waits are bounded: a stuck
 * NIC costs frames, not the firmware.
 */
uint16_t nic_tx_ptr(uint16_t len)
{
	uint16_t guard = 0;

	if (tx_started) {
		do {
			reg_read(RTL836X_REG_NIC_STXCURPKTRL);
		} while (SFR_DATA_U16 == tx_last && ++guard);
	}
	while ((reg_read16(RTL836X_REG_NIC_TXASRL) << 3) < len + 8 && ++guard)
		;
	return reg_read16(RTL836X_REG_NIC_CTXCPRL);
}


void nic_tx_start(void)
{
	reg_read(RTL836X_REG_NIC_STXCURPKTRL);
	tx_last = SFR_DATA_U16;
	tx_started = true;
	REG_SET(RTL836X_REG_NIC_TXCMDR, 1);
}
