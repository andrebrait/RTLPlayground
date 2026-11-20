#ifndef METRICS_H
#define METRICS_H

#include <stdint.h>
#include <stdbool.h>

/* The exporter's own TCP port, the usual one for a node exporter */
#define METRICS_PORT 9100
/* The response goes out a buffer at a time: one Ethernet MSS. A peer with a
 * smaller MSS gets each buffer in several segments. */
#define METRICS_BUF_SIZE 1460

/* Serves GET /metrics on METRICS_PORT while non-zero. Set by "prometheus on|off". */
extern __xdata uint8_t prometheus_enabled;
/* True while a /metrics response still has rows to generate. */
extern __xdata bool metrics_more;
/* The response being sent, owned by metrics_tcp.c, filled by metrics.c */
extern __xdata uint8_t metrics_buf[METRICS_BUF_SIZE];
extern __xdata uint16_t metrics_len;

void prometheus_command(void) __banked;
/* Starts a response in metrics_buf: header, then rows while they fit. */
void metrics_start(__xdata bool with_mib) __banked;
/* Appends rows at metrics_len while they fit; clears metrics_more at the end. */
void metrics_fill(void) __banked;
/* Appends a string to metrics_buf. Not banked: BANK4 callers only. */
void metrics_put_str(__code const char *s);
/* The uIP application for connections to METRICS_PORT */
void metrics_appcall(void) __banked;

#endif
