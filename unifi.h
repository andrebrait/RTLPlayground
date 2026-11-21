#ifndef _UNIFI_H_
#define _UNIFI_H_

#include <stdint.h>
#include "dns.h"

/* The message buffer: one inform out, then its reply in. The largest reply
 * seen in the lab, a 6.9 KB system_cfg, is 7.6 KB encrypted. */
#define UNIFI_BUF_SIZE	10240
#define UNIFI_PATH_LEN	48
#define UNIFI_TYPE_LEN	16
#define UNIFI_CFGVER_LEN	17	/* 16 hex digits */

struct uip_conn;

struct unifi_state {
	uint8_t enabled;
	uint8_t phase;		/* U_* in unifi.c */
	uint8_t keyed;		/* gcm holds the tables for key */
	uint8_t abort;		/* reset the connection on its next poll */
	uint8_t err;		/* UE_* of the last failed inform, 0 if none */
	char host[DNS_NAME_LEN];	/* inform URL: host name or dotted address */
	char path[UNIFI_PATH_LEN];
	uint16_t port;
	uint8_t addr[4];	/* address of the controller last used */
	uint8_t key[16];	/* the default key until adopted */
	uint8_t adopted;	/* key is a controller's authkey */
	uint8_t booted;		/* boot counted for this boot's IVs */
	uint8_t unsaved;	/* the state differs from flash */
	uint8_t flash_fails;	/* saves that did not read back */
	uint32_t boot;		/* boot counter, part of the IV */
	char cfgver[UNIFI_CFGVER_LEN];	/* the controller's cfgversion, echoed */
	uint16_t interval;	/* seconds between two informs */
	uint32_t next;		/* uptime seconds of the next inform */
	uint32_t count;		/* informs sent */
	uint32_t fails;		/* informs that failed */
	uint32_t msg;		/* message counter, part of the IV */
	uint16_t status;	/* HTTP status of the last reply */
	char type[UNIFI_TYPE_LEN];	/* _type of the last decrypted reply */
	uint16_t pos;		/* write position, or bytes received */
	uint16_t start, end;	/* request bytes in buf */
	uint16_t off, sent;	/* request bytes acknowledged, and in flight */
	uint16_t hdr;		/* reply: bytes of HTTP header, 0 until complete */
	uint16_t clen;		/* reply: Content-Length */
	__xdata uint8_t *cp;	/* crypt position */
	uint16_t left;		/* bytes still to crypt */
	__xdata struct uip_conn *conn;
};

extern __xdata struct unifi_state unifi;

void unifi_init(void) __banked;
void unifi_tick(void) __banked;
void unifi_appcall(void) __banked;
void unifi_parse(void) __banked;
void unifi_reset(void) __banked;

#endif
