/* Six-port bench board: lan1..lan6 are the veth ports of the namespace bench, no SFP cages. */
#include "machine.h"
#include "linux.h"

const struct machine machine = {
	.machine_name = "BENCH6",
	.min_port = 0,
	.max_port = 5,
	.log_to_phys_port = { 1, 2, 3, 4, 5, 6, 7, 8, 9 },
	.phys_to_log_port = { 0, 1, 2, 3, 4, 5, 6, 7, 8 },
};

const char *const board_ifnames[9] = { "lan1", "lan2", "lan3", "lan4", "lan5", "lan6" };
