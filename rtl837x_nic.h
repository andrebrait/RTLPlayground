#ifndef RTL837X_NIC_H
#define RTL837X_NIC_H

#include <stdbool.h>
#include <stdint.h>

// #define RXTXDBG 1

bool nic_rx_header(uint16_t ring_ptr) __reentrant;
bool nic_rx_packet(__xdata uint8_t *buffer, uint16_t ring_ptr) __reentrant;

/* buffer starts at the eight-byte NIC descriptor; len excludes that
 * descriptor. ring_ptr is the ASIC TX position in eight-byte slots. */
void nic_tx_packet(__xdata uint8_t *buffer, uint16_t len, uint16_t ring_ptr);

/* RX DMA in eight-byte units. ring_ptr must start inside the configured
 * RX ring; the hardware wraps transfers that cross its end. */
bool nic_rx_dma(__xdata uint8_t *buffer, uint16_t ring_ptr, uint8_t words) __reentrant;

/* The NIC registers behind handle_rx(), idle() and tcpip_output(), per SoC family */
bool nic_rx_pending(void);		/* a received frame waits in the RX ring */
uint16_t nic_rx_ptr(void);		/* byte offset of its descriptor in the ring */
void nic_rx_release(void);		/* hand the frame's ring space back */
void nic_rx_ack(void);			/* acknowledge the RX interrupt */
uint16_t nic_tx_ptr(uint16_t len);	/* TX position for a len-byte frame, see nic_tx_packet() */
void nic_tx_start(void);		/* send the frame nic_tx_packet() moved */

#endif
