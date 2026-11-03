/*
 * RTL836x NIC (rtl836x_nic.c): RX frames become the RTL837x layout handle_rx()
 * expects, TX frames get the RTL8370N descriptor. Only the register
 * controller and the DMA engine are modelled.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t SFR_DATA_24, SFR_DATA_16, SFR_DATA_8, SFR_DATA_0;
#define SFR_DATA_U16 ((uint16_t)(SFR_DATA_8 << 8 | SFR_DATA_0))

static uint16_t registers[0x4000];
static unsigned stx_reads_left;	/* STXCURPKTR reads until the NIC takes the last frame, 0: never */

void reg_read(uint16_t reg)
{
	if (reg == 0x1a56 && stx_reads_left && !--stx_reads_left)
		registers[reg]++;
	SFR_DATA_8 = registers[reg] >> 8;
	SFR_DATA_0 = registers[reg];
}

void reg_write(uint16_t reg)
{
	registers[reg] = (uint16_t)SFR_DATA_24 << 8 | SFR_DATA_16;
}

uint8_t rx_headers[16];

#include "../rtl836x_nic.c"

static struct {
	uint8_t data[UIP_CONF_BUFFER_ALLOC_SIZE];
	uint8_t after[16];
} buf;
static uint8_t ring[RTL836X_NIC_RX_RING_BYTES];
static uint8_t sent[256 * 8];	/* what the last TX DMA moved */
static uint16_t sent_ring;
static uint8_t sent_words;

/* RX transfers start inside the ring and wrap at its end, as the hardware does */
bool nic_rx_dma(uint8_t *buffer, uint16_t ring_ptr, uint8_t words)
{
	if (ring_ptr & 0x8000) {
		memcpy(sent, buffer, words * 8);
		sent_ring = ring_ptr;
		sent_words = words;
		return true;
	}
	assert(ring_ptr < sizeof(ring) && !(ring_ptr & 7) && words);
	for (unsigned i = 0; i < words * 8u; i++)
		buffer[i] = ring[(ring_ptr + i) % sizeof(ring)];
	return true;
}

static void ring_put(uint16_t at, const uint8_t *data, unsigned len)
{
	for (unsigned i = 0; i < len; i++)
		ring[(at + i) % sizeof(ring)] = data[i];
}

/* Descriptor at at, frame behind it: DA SA type, then a counting payload; byte 3 = ingress port */
static void put_frame(uint16_t at, uint16_t len, uint8_t flags)
{
	uint8_t hdr[8] = { 0x10, flags, 0x60, 0x05, 0, 0, len >> 8, len };
	uint8_t frame[1600];

	for (unsigned i = 0; i < len; i++)
		frame[i] = (uint8_t)(i + 1);
	frame[12] = 0x08;
	frame[13] = 0x00;
	ring_put(at, hdr, 8);
	ring_put(at + 8, frame, len);
}

static void check_rx(uint16_t at, uint16_t len)
{
	memset(&buf, 0xa5, sizeof(buf));
	memset(rx_headers, 0x6c, sizeof(rx_headers));
	put_frame(at, len, 0);
	assert(nic_rx_header(at));
	assert(rx_headers[4] == (uint8_t)(len + 12) && rx_headers[5] == (len + 12) >> 8);
	assert(!rx_headers[0] && !rx_headers[1] && !rx_headers[2] && !rx_headers[3]
	       && !rx_headers[6] && !rx_headers[7]);
	assert(nic_rx_packet(buf.data, at + 8));
	/* An RTL837x-style RX tag naming ingress port 5, then a VID 0 802.1Q tag */
	static const uint8_t tags[12] = { 0x88, 0x99, 0x04, 0, 0, 0, 0, 0x05, 0x81, 0x00, 0, 0 };
	for (unsigned i = 0; i < 12; i++)
		assert(buf.data[i] == i + 1 && buf.data[12 + i] == tags[i]);
	assert(buf.data[24] == 0x08 && buf.data[25] == 0x00);
	for (unsigned i = 14; i < len; i++)
		assert(buf.data[12 + i] == (uint8_t)(i + 1));
	for (unsigned i = 0; i < sizeof(buf.after); i++)
		assert(buf.after[i] == 0xa5);
}

int main(void)
{
	printf("[test] RX: BE length, DA/SA at 0, tags with the ingress port, type at 24\n");
	check_rx(0x0100, 60);
	check_rx(0x0100, 1514);

	printf("[test] RX: the largest frame that fits behind the tags\n");
	check_rx(0x0100, 1544);

	printf("[test] RX: the payload after the last descriptor slot starts at 0\n");
	check_rx(RTL836X_NIC_RX_RING_BYTES - 8, 98);

	printf("[test] RX: a frame crossing the ring end\n");
	check_rx(RTL836X_NIC_RX_RING_BYTES - 32, 98);

	printf("[test] RX: error bit, runt and oversize frames are dropped\n");
	put_frame(0x0200, 60, 0x01);
	assert(!nic_rx_header(0x0200));
	put_frame(0x0200, 13, 0);
	assert(!nic_rx_header(0x0200));
	put_frame(0x0200, 1545, 0);
	assert(!nic_rx_header(0x0200));
	assert(!nic_rx_header(RTL836X_NIC_RX_RING_BYTES));

	printf("[test] TX: short frame padded to 60, length BE in descriptor bytes 6-7\n");
	memset(&buf, 0xee, sizeof(buf));
	nic_tx_packet(buf.data + 4, 42, 0x0123);
	assert(sent_ring == (0x0123 << 3 | 0x8000) && sent_words == (60 + 15) >> 3);
	assert(sent[4] == 0 && sent[5] == 0 && sent[6] == 0 && sent[7] == 60);
	for (unsigned i = 8 + 42; i < 8 + 60; i++)
		assert(sent[i] == 0);

	printf("[test] TX: long frame keeps its length\n");
	nic_tx_packet(buf.data + 4, 1000, 0);
	assert(sent[6] == 0x03 && sent[7] == 0xe8 && sent_words == (1000 + 15) >> 3);

	printf("[test] RX registers: pending, pointer, release, ack\n");
	assert(!nic_rx_pending());
	registers[0x1a05] = 1;
	assert(nic_rx_pending());
	registers[0x1a50] = 0x34;
	registers[0x1a51] = 0x01;
	assert(nic_rx_ptr() == 0x0134 << 3);
	nic_rx_release();
	assert(registers[0x1a0c] == 1);
	nic_rx_ack();
	assert(registers[0x1a0e] == 0x0080);

	printf("[test] TX registers: the next frame waits until the NIC took the last one\n");
	registers[0x1a08] = 0x03ff;	/* TXASR: all free */
	registers[0x1a52] = 0x0010;
	assert(nic_tx_ptr(60) == 0x0010);
	nic_tx_start();
	assert(registers[0x1a0d] == 1);
	stx_reads_left = 5;
	assert(nic_tx_ptr(60) == 0x0010 && stx_reads_left == 0);
	nic_tx_start();
	printf("[test] TX registers: a NIC that never takes the frame does not hang the sender\n");
	assert(nic_tx_ptr(60) == 0x0010);

	printf("PASS\n");
	return 0;
}
