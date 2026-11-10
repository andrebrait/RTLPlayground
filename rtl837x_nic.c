#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl837x_sfr.h"
#include "rtl837x_nic.h"

extern __xdata uint8_t rx_headers[16];
extern __xdata uint8_t sfr_data[4];


#ifndef RTLP_HOST_TEST
/* One RX DMA operation, starting inside the configured ring. The hardware
 * wraps transfers that cross its end; the initial address must be normalized.
 * Host tests supply a simulated DMA engine for this hardware boundary. */
bool nic_rx_dma(__xdata uint8_t *buffer, uint16_t ring_ptr, uint8_t words) __reentrant
{
	uint16_t guard = 0;

	SFR_NIC_DATA_U16LE = (uint16_t)buffer;
	SFR_NIC_RING_U16LE = ring_ptr;
	SFR_NIC_CTRL = words;
	while (SFR_NIC_CTRL != 0) {
		if (++guard == 0) {
			print_string("NIC: RX transfer did not complete\n");
			return false;
		}
	}
	return true;
}
#endif

static bool nic_rx_copy(__xdata uint8_t *buffer, uint16_t ring_ptr, uint8_t words) __reentrant
{
	if (ring_ptr > RTL837X_NIC_RX_RING_BYTES || (ring_ptr & 7))
		return false;
	if (ring_ptr == RTL837X_NIC_RX_RING_BYTES)
		ring_ptr = 0;

	return nic_rx_dma(buffer, ring_ptr, words);
}


bool nic_rx_header(uint16_t ring_ptr) __reentrant
{
	if (ring_ptr >= RTL837X_NIC_RX_RING_BYTES)
		return false;
	return nic_rx_copy(rx_headers, ring_ptr, 1);
}


bool nic_rx_packet(__xdata uint8_t *buffer, uint16_t ring_ptr) __reentrant
{
	uint16_t len = ((uint16_t)rx_headers[5] << 8) | rx_headers[4];

	if (!len || len > UIP_CONF_BUFFER_SIZE)
		return false;
	return nic_rx_copy(buffer, ring_ptr, (len + 7) >> 3);
}


/*
 * Transfers data in XMEM to the ASIC for transmission by the NIC
 */
void nic_tx_packet(__xdata uint8_t *buffer, uint16_t len, uint16_t ring_ptr)
{
	uint16_t guard = 0;

	SFR_NIC_DATA_U16LE = (uint16_t)(uintptr_t)buffer;

#ifdef RXTXDBG
	print_string("TX: \n");
	for (uint8_t i = 0; i < 100; i++) {
		print_byte(uip_buf[i]);
		write_char(' ');
	}
	write_char('\n');
#endif

	ring_ptr <<= 3;
	ring_ptr |= 0x8000;
	SFR_NIC_RING_U16LE = ring_ptr;

	len += 0xf;
	len >>= 3;
	SFR_NIC_CTRL = len;
	while (SFR_NIC_CTRL != 0) {
		if (++guard == 0) {
			print_string("NIC: TX transfer did not complete\n");
			return;
		}
	}
}


bool nic_rx_pending(void)
{
	reg_read(RTL837X_REG_NIC_RX_BUFF_DATA);
	return SFR_DATA_U16 != 0;
}


uint16_t nic_rx_ptr(void)
{
	reg_read(RTL837X_REG_CPU_RX_CURR_PKT);
	return SFR_DATA_U16 << 3;
}


void nic_rx_release(void)
{
	REG_SET(RTL837X_REG_NIC_RXCMD, 1);
}


void nic_rx_ack(void)
{
	REG_SET(RTL837X_NIC_INT_STS, NIC_INT_RXIS);
}


uint16_t nic_tx_ptr(uint16_t len)
{
	(void)len;
	reg_read_m(RTL837X_REG_CPU_TX_CURR_PKT);
	return ((uint16_t)sfr_data[2]) << 8 | sfr_data[3];
}


void nic_tx_start(void)
{
	// New position of the ring-pointer on the NIC-side indicates number of bytes transmitted
	reg_read_m(RTL837X_REG_NIC_TX_CURR_PKT);

	// Do actual TX of data on ASIC side
	REG_SET(RTL837X_REG_NIC_TXCMD, 1);
}
