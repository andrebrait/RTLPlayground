#ifndef METRICS_H
#define METRICS_H

#include <stdint.h>
#include <stdbool.h>

/* Serves GET /metrics while non-zero. Set by "prometheus on|off". */
extern __xdata uint8_t prometheus_enabled;
/* True while a /metrics response still has rows to generate. */
extern __xdata bool metrics_more;

void prometheus_command(void) __banked;
/* Starts a response in outbuf: header, then rows while they fit. */
void metrics_start(__xdata bool with_mib) __banked;
/* Appends rows at slen while they fit in outbuf; clears metrics_more at the end. */
void metrics_fill(void) __banked;

#endif
