#ifndef _RTL837X_CLI_H_
#define _RTL837X_CLI_H_

#include <stdint.h>

/* Health instrumentation, built with HEALTH=1. Without it the hooks below
 * compile to nothing and the "health" command does not exist. */
#ifdef HEALTH
/* Phase ids, in main-loop order */
#define HEALTH_PH_LINK	0
#define HEALTH_PH_SFP	1
#define HEALTH_PH_RX	2
#define HEALTH_PH_TX	3
#define HEALTH_PH_STP	4
#define HEALTH_PH_CMD	5
#define HEALTH_PHASES	6
#define HEALTH_LLDP     7

extern __xdata uint16_t health_rx_frames;
#define health_rx_frame()	health_rx_frames++

void health_loop_start(void) __banked;	/* once per idle() pass, resets the phase baseline */
void health_phase(uint8_t id) __banked;	/* after each phase; accounts ticks since last checkpoint */
void health_stack_paint(void) __banked;	/* paint the unused stack area once at boot */
#else
#define health_rx_frame()
#define health_loop_start()
#define health_phase(id)
#define health_stack_paint()
#endif

#endif
