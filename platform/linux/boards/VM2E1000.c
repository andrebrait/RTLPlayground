/* Two-port board for a real-NIC check: a VM with two e1000e NICs (ens19, ens20) on an isolated bridge. */
#include "machine.h"
#include "linux.h"

const struct machine machine = {
	.machine_name = "VM2E1000",
	.min_port = 0,
	.max_port = 1,
	.log_to_phys_port = { 1, 2, 3, 4, 5, 6, 7, 8, 9 },
	.phys_to_log_port = { 0, 1, 2, 3, 4, 5, 6, 7, 8 },
};

const char *const board_ifnames[9] = { "ens19", "ens20" };
