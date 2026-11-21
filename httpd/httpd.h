#ifndef __HTTPD_H__
#define __HTTPD_H__

/* Since this file will be included by uip.h, we cannot include uip.h
   here. But we might need to include uipopt.h if we need the u8_t and
   u16_t datatypes. */
#include "uipopt.h"

/* Next, we define the uip_tcp_appstate_t datatype. This is the state
   of our application, and the memory required for this state is
   allocated together with each TCP connection. One application state
   for each TCP connection. */
typedef struct httpd_state {
   uint8_t tstate;
   uint8_t fw_reset_pending;
} uip_tcp_appstate_t;

/* Finally we define the application function to be called by uIP. */
void httpd_appcall(void);
#ifndef UIP_APPCALL
#if defined(PROMETHEUS) || defined(UNIFI)
/* Sends port 80 to the httpd, the exporter's port to metrics_appcall(), and
 * the inform client's connection to unifi_appcall() */
void tcp_appcall(void);
#define UIP_APPCALL tcp_appcall
#else
#define UIP_APPCALL httpd_appcall
#endif
#endif /* UIP_APPCALL */

void httpd_init(void) __banked;

#endif
