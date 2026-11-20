/* GET /metrics on METRICS_PORT through the real metrics_tcp.c, httpd.c and
 * uIP: refusal while disabled, routing, streaming several buffer fills,
 * retransmission, one scrape at a time, and the web interface working while a
 * scrape runs. The generator is replaced by a stub that fills metrics_buf with
 * known bytes. */
#define main httpd_tx_main
#include "test_httpd_tx.c"
#undef main
#include "metrics.h"

#define FILL_LEN 1000
#define FILLS 3

uint8_t prometheus_enabled;
bool metrics_more;
static int starts, fills;
static bool start_mib, empty_last;

/* The real generator can end on an empty fill: its last rows had no sample. */
void metrics_fill(void)
{
	if (!(empty_last && fills == FILLS - 1))
		for (int i = 0; i < FILL_LEN; i++)
			metrics_buf[metrics_len++] = 'a' + fills;
	metrics_more = ++fills < FILLS;
}

void metrics_start(bool with_mib)
{
	starts++;
	start_mib = with_mib;
	fills = 0;
	memcpy(metrics_buf, "HTTP/1.1 200 OK\r\n\r\n", 19);
	metrics_len = 19;
	metrics_more = true;
	metrics_fill();
}

/* A connection to the exporter, after a fresh uip_init() and httpd_init() */
static void scrape_start(void)
{
	cli_sport = 40001;
	cli_dport = METRICS_PORT;
	session_start(MSS_FULL);
}

static void get(const char *path)
{
	char req[128];
	snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: sw\r\n\r\n", path);
	client_send(TCP_ACK | TCP_PSH, req, (int)strlen(req));
}

/* The body is fills_sent runs of FILL_LEN bytes, 'a', 'b', 'c'... */
static int body_ok(int fills_sent)
{
	int off = body_offset();
	if (off < 0 || stream_len != off + fills_sent * FILL_LEN)
		return 0;
	for (int i = 0; i < fills_sent * FILL_LEN; i++)
		if (stream[off + i] != 'a' + i / FILL_LEN)
			return 0;
	return 1;
}

static struct uip_conn *conn_on(uint16_t port)
{
	for (int c = 0; c < UIP_CONNS; c++)
		if (uip_conns[c].tcpstateflags != UIP_CLOSED && uip_conns[c].lport == HTONS(port))
			return &uip_conns[c];
	return NULL;
}

/* The server sent its FIN: the connection left ESTABLISHED */
static int scrape_closed(void)
{
	struct uip_conn *c = conn_on(METRICS_PORT);
	return !c || (c->tcpstateflags & UIP_TS_MASK) != UIP_ESTABLISHED;
}

static void scenario_disabled(void)
{
	prometheus_enabled = 0;
	starts = 0;
	scrape_start();
	get("/metrics");
	drain(MSS_FULL);
	CHECK(!starts && stream_len == 0 && !conn_on(METRICS_PORT),
	      "metrics_disabled_reset: the exporter's port resets connections while it is off");
}

static void scenario_streams(void)
{
	prometheus_enabled = 1;
	starts = 0;
	scrape_start();
	get("/metrics");
	drain(MSS_FULL);
	CHECK(starts == 1 && fills == FILLS && !metrics_more && body_ok(FILLS) && scrape_closed(),
	      "metrics_streams_until_done: every fill arrives in order, then the connection closes");
}

static void scenario_routes(void)
{
	prometheus_enabled = 1;
	scrape_start();
	get("/metrics?mib=1");
	drain(MSS_FULL);
	CHECK(!memcmp(stream, "HTTP/1.1 200", 12) && start_mib,
	      "metrics_mib_query: ?mib=1 reaches the generator");
	scrape_start();
	get("/metrics");
	drain(MSS_FULL);
	CHECK(!start_mib, "metrics without a query leaves the MIB families out");
	starts = 0;
	scrape_start();
	get("/index.html");
	drain(MSS_FULL);
	CHECK(!starts && !memcmp(stream, "HTTP/1.1 404", 12) && scrape_closed(),
	      "metrics_other_path_404: the exporter's port answers anything else with 404");
	starts = 0;
	cli_sport = 40000;
	cli_dport = 80;
	session_start(MSS_FULL);
	get("/metrics");
	drain(MSS_FULL);
	CHECK(!starts && stream_len > 12 && memcmp(stream, "HTTP/1.1 200", 12),
	      "metrics_not_on_port_80: the web server no longer serves /metrics");
}

static void scenario_rexmit(void)
{
	uint8_t original[MSS_FULL];
	int original_len, fills_before, same;

	prometheus_enabled = 1;
	scrape_start();
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
	CHECK(body_ok(FILLS), "the scrape still arrives whole after the retransmission");
}

static void scenario_empty_last_fill(void)
{
	prometheus_enabled = 1;
	empty_last = true;
	scrape_start();
	get("/metrics");
	drain(MSS_FULL);
	CHECK(fills == FILLS && body_ok(FILLS - 1) && scrape_closed(),
	      "metrics_empty_last_fill: an empty final fill still closes the connection");
	empty_last = false;
}

/* The client model's state, so two connections can take turns */
struct client {
	uint32_t seq, rcv_nxt;
	uint16_t window, sport, dport;
	int len;
	uint8_t stream[STREAM_MAX];
};
static struct client saved;

static void client_swap(struct client *c)
{
	static struct client t;
	t.seq = cli_seq; t.rcv_nxt = cli_rcv_nxt; t.window = cli_window;
	t.sport = cli_sport; t.dport = cli_dport; t.len = stream_len;
	memcpy(t.stream, stream, sizeof(stream));
	cli_seq = c->seq; cli_rcv_nxt = c->rcv_nxt; cli_window = c->window;
	cli_sport = c->sport; cli_dport = c->dport; stream_len = c->len;
	memcpy(stream, c->stream, sizeof(stream));
	*c = t;
}

/* Another client on the same uIP, without re-initialising it */
static void connect_more(uint16_t sport, uint16_t dport)
{
	cli_sport = sport;
	cli_dport = dport;
	cli_seq = 5000;
	cli_rcv_nxt = 0;
	cli_window = MSS_FULL;
	stream_len = 0;
	last_tx_len = 0;
	client_send(TCP_SYN, NULL, 0);
	client_ack(MSS_FULL);
}

static void scenario_alongside_web(void)
{
	prometheus_enabled = 1;
	starts = 0;
	scrape_start();
	get("/metrics");
	client_ack(MSS_FULL);	/* the scrape is under way */
	client_swap(&saved);
	connect_more(40002, 80);
	request_file(MSS_FULL);
	for (int i = 0; i < 20 && stream_len < body_offset() + FILE_LEN; i++)
		client_ack(MSS_FULL);
	CHECK(body_offset() >= 0 && stream_len == body_offset() + FILE_LEN &&
	      first_body_mismatch() == -1,
	      "metrics_alongside_web: a file is served while a scrape is open");
	client_swap(&saved);
	for (int i = 0; i < 20 && !scrape_closed(); i++)
		client_ack(MSS_FULL);
	CHECK(starts == 1 && body_ok(FILLS) && scrape_closed(),
	      "the scrape completes after the web request");
}

static void scenario_one_scrape(void)
{
	prometheus_enabled = 1;
	starts = 0;
	scrape_start();
	get("/metrics");
	client_ack(MSS_FULL);
	client_swap(&saved);
	connect_more(40003, METRICS_PORT);
	CHECK(stream_len == 0 && cli_rcv_nxt == 0,
	      "metrics_one_scrape: a second connection to the exporter gets no SYNACK while one is open");
	client_swap(&saved);
	drain(MSS_FULL);
	CHECK(starts == 1 && body_ok(FILLS) && scrape_closed(), "the first scrape is unaffected");
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "-v"))
		verbose = 1;
	printf("== GET /metrics on the exporter's port ==\n");
	scenario_disabled();
	scenario_streams();
	scenario_routes();
	scenario_rexmit();
	scenario_empty_last_fill();
	scenario_alongside_web();
	scenario_one_scrape();
	printf("\n%s (%d failure%s)\n", failures ? "BENCH: FAILURES" : "BENCH: ALL PASS",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
