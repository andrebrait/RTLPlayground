/* GET /metrics through the real httpd.c and uIP: routing without a session,
 * 404 while disabled, streaming several outbuf fills and retransmission.
 * The generator is replaced by a stub that fills outbuf with known bytes. */
#define main httpd_tx_main
#include "test_httpd_tx.c"
#undef main
#include "metrics.h"

#define FILL_LEN 2400
#define FILLS 3

extern uint8_t outbuf[TCP_OUTBUF_SIZE];
extern uint16_t slen;
uint8_t prometheus_enabled;
bool metrics_more;
static int starts, fills;
static bool start_mib, empty_last;

/* The real generator can end on an empty fill: its last rows had no sample. */
void metrics_fill(void)
{
	if (!(empty_last && fills == FILLS - 1))
		for (int i = 0; i < FILL_LEN; i++)
			outbuf[slen++] = 'a' + fills;
	metrics_more = ++fills < FILLS;
}

void metrics_start(bool with_mib)
{
	starts++;
	start_mib = with_mib;
	fills = 0;
	slen = strtox(outbuf, "HTTP/1.1 200 OK\r\n\r\n");
	metrics_more = true;
	metrics_fill();
}

static void get(const char *path)
{
	char req[128];
	snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: sw\r\n\r\n", path);
	client_send(TCP_ACK | TCP_PSH, req, (int)strlen(req));
}

/* The body is FILLS runs of FILL_LEN bytes, 'a', 'b', 'c'... */
static int body_ok(void)
{
	int off = body_offset();
	if (off < 0 || stream_len != off + FILLS * FILL_LEN)
		return 0;
	for (int i = 0; i < FILLS * FILL_LEN; i++)
		if (stream[off + i] != 'a' + i / FILL_LEN)
			return 0;
	return 1;
}

static void scenario_empty_last_fill(void)
{
	int off;
	prometheus_enabled = 1;
	empty_last = true;
	session_start(MSS_FULL);
	get("/metrics");
	drain(MSS_FULL);
	off = body_offset();
	CHECK(fills == FILLS && off >= 0 && stream_len == off + (FILLS - 1) * FILL_LEN &&
	      uip_conns[0].appstate.tstate == 3 /* TSTATE_CLOSED */,
	      "metrics_empty_last_fill: an empty final fill still closes the connection");
	empty_last = false;
}

static void scenario_disabled(void)
{
	prometheus_enabled = 0;
	starts = 0;
	session_start(MSS_FULL);
	get("/metrics");
	drain(MSS_FULL);
	CHECK(stream_len > 12 && !memcmp(stream, "HTTP/1.1 404", 12) && !starts,
	      "metrics_disabled_404: /metrics is Not Found while the exporter is off");
}

static void scenario_streams(void)
{
	prometheus_enabled = 1;
	starts = 0;
	session_start(MSS_FULL);
	get("/metrics");
	drain(MSS_FULL);
	CHECK(starts == 1 && fills == FILLS && !metrics_more && body_ok() &&
	      uip_conns[0].appstate.tstate == 3 /* TSTATE_CLOSED, private to httpd.c */,
	      "metrics_streams_until_done: every fill arrives in order, then the connection closes");
}

static void scenario_no_cookie(void)
{
	prometheus_enabled = 1;
	session_start(MSS_FULL);
	get("/metrics?mib=1");
	drain(MSS_FULL);
	CHECK(!memcmp(stream, "HTTP/1.1 200", 12) && start_mib,
	      "metrics_no_cookie_needed: served without a session, ?mib=1 reaches the generator");
	session_start(MSS_FULL);
	get("/metrics");
	drain(MSS_FULL);
	CHECK(!start_mib, "metrics without a query leaves the MIB families out");
}

static void scenario_rexmit(void)
{
	uint8_t original[MSS_FULL];
	int original_len, fills_before, same;

	prometheus_enabled = 1;
	session_start(MSS_FULL);
	get("/metrics");
	for (int i = 0; i < 10 && fills < 2; i++)
		client_ack(MSS_FULL);
	/* last_tx is the first segment of the second fill: lose it. */
	original_len = last_tx_len;
	memcpy(original, last_tx, original_len);
	stream_len -= original_len;
	cli_rcv_nxt -= original_len;
	fills_before = fills;
	last_tx_len = 0;
	run_periodic(UIP_RTO + 2);
	same = last_tx_len == original_len && !memcmp(last_tx, original, original_len);
	CHECK(fills == 2 && original[0] == 'b' && same && fills == fills_before,
	      "metrics_rexmit_resends_same_bytes: a lost segment is resent, not regenerated");
	drain(MSS_FULL);
	CHECK(body_ok(), "the scrape still arrives whole after the retransmission");
}

static void scenario_restart_after_abort(void)
{
	prometheus_enabled = 1;
	session_start(MSS_FULL);
	get("/metrics");
	client_ack(MSS_FULL);
	/* The client goes away mid-scrape; the next client asks for a file. */
	session_start(MSS_FULL);
	fills = 0;
	request_file(MSS_FULL);
	drain(MSS_FULL);
	CHECK(!metrics_more && fills == 0 && first_body_mismatch() == -1 &&
	      stream_len == body_offset() + FILE_LEN,
	      "metrics_restart_after_abort: a new request drops the unfinished scrape");
	session_start(MSS_FULL);
	starts = 0;
	get("/metrics");
	drain(MSS_FULL);
	CHECK(starts == 1 && body_ok(), "the next /metrics starts from the beginning");
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "-v"))
		verbose = 1;
	printf("== httpd: GET /metrics ==\n");
	scenario_disabled();
	scenario_streams();
	scenario_no_cookie();
	scenario_rexmit();
	scenario_restart_after_abort();
	scenario_empty_last_fill();
	printf("\n%s (%d failure%s)\n", failures ? "BENCH: FAILURES" : "BENCH: ALL PASS",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
