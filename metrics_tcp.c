/*
 * The Prometheus exporter's own TCP connection on METRICS_PORT.
 *
 * It runs next to the httpd, with its own connection slot. uip.c admits one
 * connection per listening port, so this file keeps a single connection's
 * state. The response is generated into metrics_buf one buffer at a time and
 * sent in segments of at most one MSS; uIP keeps no copy of sent data, so a
 * retransmission resends the bytes still in the buffer.
 *
 * This code is in the Public Domain
 */

#pragma codeseg BANK4
#pragma constseg BANK4

#include <stdint.h>
#include <stdbool.h>
#include "rtl837x_common.h"
#include "uip.h"
#include "metrics.h"

/* metrics.c's string writer: not banked, so declared here and not in
 * metrics.h, where files in other banks could call it */
void metrics_put_str(__code const char *s);

__xdata uint8_t metrics_buf[METRICS_BUF_SIZE];
__xdata uint16_t metrics_len;

/* Waiting for the request (0), or sending the response */
static __xdata uint8_t m_sending;
/* The request asked for the MIB families: m_request() sets it, so no
 * comparison has to be held across the call to metrics_start() */
static __xdata bool m_mib;
/* Bytes of metrics_buf already acknowledged, and in flight */
static __xdata uint16_t m_off, m_sent;

/* The next segment of metrics_buf, at most one MSS */
static void m_send(void)
{
	m_sent = metrics_len - m_off;
	if (m_sent > uip_mss())
		m_sent = uip_mss();
	uip_send(metrics_buf + m_off, m_sent);
}

/* Is the request "GET /metrics", with or without a query, followed by a space?
 * Sets m_mib when the query has a "mib=1" parameter. Reads uip_appdata
 * itself, keeps its counters in XDATA and returns a byte, not a bool built
 * from && and ||: each of those costs internal RAM. */
static __xdata uint16_t m_i, m_p;
static __xdata uint8_t m_j;

static uint8_t m_request(void)
{
	static __code const char get[] = "GET /metrics";
	static __code const char mib[] = "mib=1";
	__xdata const uint8_t *p = uip_appdata;

	m_mib = false;
	for (m_i = 0; get[m_i]; m_i++) {
		if (m_i >= uip_len)
			return 0;
		if (p[m_i] != get[m_i])
			return 0;
	}
	if (m_i < uip_len && p[m_i] == '?') {
		do {
			m_p = ++m_i;	/* past '?' or '&' */
			for (m_j = 0; mib[m_j] && m_i < uip_len && p[m_i] == mib[m_j]; m_j++)
				m_i++;
			while (m_i < uip_len && p[m_i] != '&' && p[m_i] != ' ')
				m_i++;
			if (!mib[m_j] && m_i - m_p == sizeof(mib) - 1)
				m_mib = true;
		} while (m_i < uip_len && p[m_i] == '&');
	}
	if (m_i >= uip_len)
		return 0;
	return p[m_i] == ' ';
}

void metrics_appcall(void) __banked
{
	if (uip_connected()) {
		m_sending = 0;
		if (!prometheus_enabled) {
			uip_abort();
			return;
		}
		/* No return: the segment that completes the handshake can carry
		 * the request */
	}
	if (uip_closed() || uip_aborted() || uip_timedout()) {
		m_sending = 0;
		return;
	}
	if (m_sending && uip_acked()) {
		m_off += m_sent;
		if (m_off == metrics_len) {
			metrics_len = 0;
			m_off = 0;
			if (metrics_more)
				metrics_fill();
			if (!metrics_len) {
				/* Everything is acknowledged */
				m_sending = 0;
				uip_close();
				return;
			}
		}
		m_send();
		return;
	}
	if (m_sending && uip_rexmit()) {
		uip_send(metrics_buf + m_off, m_sent);
		return;
	}
	if (!m_sending && uip_newdata()) {
		if (m_request() && prometheus_enabled) {
			metrics_start(m_mib);
		} else {
			metrics_len = 0;
			metrics_put_str("HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");
			metrics_more = false;
		}
		m_sending = 1;
		m_off = 0;
		m_send();
	}
}
