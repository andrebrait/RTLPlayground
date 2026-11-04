#ifndef _MACHINE_DEFAULTS_H_
#define _MACHINE_DEFAULTS_H_

/* What the switch starts with at boot. The boot code sets these and /defaults.json reports them,
 * so both come from here. A <machine>.h may override any of them; machine.h includes this file
 * after it. Settings without a default here (flow control, MTU) are never left out of a saved
 * config. Only the values a file uses end up in the image. */

#ifndef MACHINE_DEFAULT_EEE
#define MACHINE_DEFAULT_EEE 1			/* copper ports advertise EEE */
#endif
#ifndef MACHINE_DEFAULT_LLDP
#define MACHINE_DEFAULT_LLDP 0			/* LLDP off */
#endif
#ifndef MACHINE_DEFAULT_LLDP_PORTS
#define MACHINE_DEFAULT_LLDP_PORTS 0xffff	/* bit per logical port: may send LLDP */
#endif

#endif
