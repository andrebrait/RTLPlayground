/*
 * Command line interface of the Prometheus exporter for the RTL837x platform
 * This code is in the Public Domain
 */

#include <stdint.h>
#include "rtl837x_common.h"
#include "cmd_parser.h"
#include "metrics.h"

#pragma codeseg BANK2
#pragma constseg BANK2

extern __xdata uint8_t cmd_words_len;
uint8_t cmd_compare(uint8_t start, __code const uint8_t * cmd);

void prometheus_command(void) __banked
{
	if (cmd_words_len == 2) {
		if (cmd_compare(1, "on")) {
			prometheus_enabled = 1;
			print_string("Prometheus exporter enabled\n");
			return;
		}
		if (cmd_compare(1, "off")) {
			prometheus_enabled = 0;
			print_string("Prometheus exporter disabled\n");
			return;
		}
		if (cmd_compare(1, "show")) {
			print_string(prometheus_enabled ? "Prometheus exporter: enabled\n"
							: "Prometheus exporter: disabled\n");
			return;
		}
	}
	err_status = ERR_INVALID_ARGUMENT;
	print_string("Usage: prometheus [on|off|show]\n");
}
