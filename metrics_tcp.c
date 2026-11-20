/*
 * The Prometheus exporter's own TCP connection on METRICS_PORT.
 *
 * A scrape no longer shares the httpd's connection slot and outbuf: the web
 * interface and a scrape can run at the same time. uip.c admits one
 * connection per listening port, so this file keeps a single connection's
 * state. The response is generated into metrics_buf one segment's worth at a
 * time; uIP keeps no copy of sent data, so a retransmission resends the
 * bytes still in the buffer.
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

__xdata uint8_t metrics_buf[METRICS_BUF_SIZE];
__xdata uint16_t metrics_len;

/* Waiting for the request (0), or sending the response. Bytes, not bool:
 * a bool here costs a bit and a byte of the full internal RAM. */
static __xdata uint8_t m_sending;
/* The request asked for the MIB families: m_request() sets it, so no
 * comparison has to be held across the call to metrics_start() */
static __xdata bool m_mib;
/* Bytes of metrics_buf already acknowledged, and in flight */
static __xdata uint16_t m_off, m_sent;

static void m_put(__code const char *s)
{
	while (*s)
		metrics_buf[metrics_len++] = *s++;
}

/* The next segment of metrics_buf, at most one MSS */
static void m_send(void)
{
	m_sent = metrics_len - m_off;
	if (m_sent > uip_mss())
		m_sent = uip_mss();
	uip_send(metrics_buf + m_off, m_sent);
}

/* Is the request "GET /metrics" or "GET /metrics?mib=1", followed by a space?
 * Sets m_mib for the second. */
static bool m_request(__xdata const uint8_t *p, uint16_t len)
{
	static __code const char get[] = "GET /metrics";
	static __code const char mib[] = "?mib=1";
	uint8_t i;

	m_mib = false;
	if (len < sizeof(get))
		return 0;
	for (i = 0; get[i]; i++)
		if (p[i] != get[i])
			return 0;
	if (p[i] == ' ')
		return 1;
	if (len < sizeof(get) + sizeof(mib) - 1)
		return 0;
	p += i;
	for (i = 0; mib[i]; i++)
		if (p[i] != mib[i])
			return 0;
	if (p[i] != ' ')
		return 0;
	m_mib = true;
	return 1;
}

void metrics_appcall(void) __banked
{
	if (uip_connected()) {
		if (!prometheus_enabled)
			uip_abort();
		m_sending = 0;
		return;
	}
	if (uip_closed() || uip_aborted() || uip_timedout()) {
		m_sending = 0;
		return;
	}
	if (m_sending && uip_acked()) {
		m_off += m_sent;
		m_sent = 0;
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
		if (m_request((__xdata const uint8_t *)uip_appdata, uip_len) && prometheus_enabled) {
			metrics_start(m_mib);
		} else {
			metrics_len = 0;
			m_put("HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");
			metrics_more = false;
		}
		m_sending = 1;
		m_off = 0;
		m_send();
	}
}
