#ifndef METRICS_H
#define METRICS_H

#include <stdint.h>

/* Serves GET /metrics while non-zero. Set by "prometheus on|off". */
extern __xdata uint8_t prometheus_enabled;

void prometheus_command(void) __banked;

#endif
