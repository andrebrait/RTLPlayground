/*
 * The web server: HTTP/1.1, one request per connection (Connection: close, as uIP does). Routes,
 * status codes and header handling follow httpd/httpd.c (the 8051 reference, which is bound to
 * uIP and so is not linked): the JSON pages come from page_impl.c unchanged, the static files
 * from the --www directory.
 *
 * Everything is non-blocking inside the daemon's poll loop. A connection reads its request into
 * a fixed buffer (header block at most HDR_MAX, body at most what its endpoint needs), builds
 * the whole response in memory, writes it, and lingers briefly so that a client still sending
 * (an over-long request) gets to read the verdict before the connection closes. A connection
 * that makes no progress for IDLE_MS, or has not finished its request after LIFE_MS, is closed.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cmd_parser.h"
#include "page_impl.h"
#include "rtl837x_common.h"
#include "platform.h"
#include "rtl837x_flash.h"
#include "linux.h"

#define HDR_MAX 8192
/* As httpd.c: a full configuration sector plus the multipart framing around it. */
#define CONFIG_UPLOAD_BUF (CONFIG_LEN + 384)
#define LOGIN_BODY_MAX 512
#define MAX_CONNS (HTTP_SLOTS - 1)
#define IDLE_MS 5000
#define LIFE_MS 30000
#define LINGER_MS 2000
#define LINGER_MAX (256 << 10)
#define FILE_MAX (8 << 20)
#define BOUNDARY_MAX 72

/* page_impl.c's response buffer, and what send_config() leaves of the configuration for us. */
extern uint8_t outbuf[TCP_OUTBUF_SIZE];
extern uint16_t slen;
static uint16_t cfg_rest_off, cfg_rest_len;

void config_stream_rest(uint16_t offset, __xdata uint16_t len) __banked
{
	cfg_rest_off = offset;
	cfg_rest_len = len;
}

struct req {
	bool post, authenticated;
	char path[256];
	const char *query;
	char ctype[128];
	char boundary[BOUNDARY_MAX + 5];	/* "\r\n--" + the boundary of the header */
	char cookie[40];
	long clen;			/* -1: no Content-Length header */
};

struct conn {
	int fd;
	enum { READ, WRITE, LINGER } st;
	int64_t deadline, born;
	size_t rlen, scan, body_off, olen, ooff, lingered;
	bool parsed, restart;
	uint8_t *out;
	struct req req;
	uint8_t rbuf[HDR_MAX + CONFIG_UPLOAD_BUF];
};

static int listen_fd = -1, www_fd = -1;
static struct conn *conns[MAX_CONNS];
static bool restart_requested;

bool http_restart_requested(void)
{
	return restart_requested;
}

/* ---- responses ------------------------------------------------------------------------- */

static void set_out(struct conn *c, const void *p, size_t n)
{
	free(c->out);
	c->out = malloc(n ? n : 1);
	if (!c->out) {
		c->olen = 0;
		return;
	}
	memcpy(c->out, p, n);
	c->olen = n;
}

#define SET_OUT_STR(c, s) set_out(c, s, sizeof(s) - 1)

#define HTML_HDR(status) "HTTP/1.1 " status "\r\nConnection: close\r\nContent-Type: text/html\r\n\r\n"

static void send_not_found(struct conn *c)
{
	SET_OUT_STR(c, HTML_HDR("404 Not found") "<!DOCTYPE HTML PUBLIC>\n<title>404 Not Found</title>\n<h1>Not Found</h1>\n");
}

static void send_bad_request(struct conn *c)
{
	SET_OUT_STR(c, HTML_HDR("400 Bad Request") "<!DOCTYPE HTML PUBLIC>\n<title>400 Bad Request</title>\n<h1>Bad Request</h1>\n");
}

static void send_to_login(struct conn *c)
{
	SET_OUT_STR(c, "HTTP/1.1 302 Found\r\nConnection: close\r\nLocation: login.html\r\n\r\n");
}

static void send_unauthorized(struct conn *c)
{
	SET_OUT_STR(c, "HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n\r\n");
}

static void send_length_required(struct conn *c)
{
	SET_OUT_STR(c, "HTTP/1.1 411 Length Required\r\nConnection: close\r\n\r\n");
}

static void send_ok(struct conn *c)
{
	SET_OUT_STR(c, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n");
}

static void send_text(struct conn *c, const char *status, const char *text)
{
	char *s;
	int n = asprintf(&s, "HTTP/1.1 %s\r\nConnection: close\r\nContent-Type: text/plain\r\n\r\n%s", status, text);

	if (n < 0)
		return;
	free(c->out);
	c->out = (uint8_t *)s;
	c->olen = n;
}

/* What the shared page code left in outbuf, plus the rest of the configuration if send_config()
 * did not fit it. */
static void send_page(struct conn *c)
{
	set_out(c, outbuf, slen);
	while (c->out && cfg_rest_len) {
		uint8_t *n = realloc(c->out, c->olen + cfg_rest_len);
		uint16_t got;

		if (!n)
			break;
		c->out = n;
		got = config_read(cfg_rest_off, c->out + c->olen, cfg_rest_len);
		if (!got)
			break;
		c->olen += got;
		cfg_rest_off += got;
		cfg_rest_len -= got;
	}
}

/* ---- static files ---------------------------------------------------------------------- */

static const char *mime_of(const char *name)
{
	static const struct { const char *suffix, *mime; } types[] = {	/* as tools/fileadder.c getMime() */
		{ ".html", "text/html" }, { ".svg", "image/svg+xml" }, { ".ico", "image/svg+xml" },
		{ ".png", "image/png" }, { ".js", "text/javascript" }, { ".css", "text/css" },
	};
	size_t n = strlen(name);

	for (size_t i = 0; i < sizeof types / sizeof *types; i++) {
		size_t s = strlen(types[i].suffix);

		if (n >= s && !strcmp(name + n - s, types[i].suffix))
			return types[i].mime;
	}
	return "text/plain";
}

/* The file a request path names inside the --www directory: "/" is index.html, anything else
 * a single plain name. NULL for what cannot be a file. */
static const char *static_name(const char *path)
{
	if (!strcmp(path, "/"))
		return "index.html";
	path++;
	if (!*path || *path == '.')
		return NULL;
	for (const char *p = path; *p; p++)
		if (!strchr("._-", *p) && !(*p >= '0' && *p <= '9') && !(*p >= 'a' && *p <= 'z') && !(*p >= 'A' && *p <= 'Z'))
			return NULL;
	return path;
}

static bool static_exists(const char *name)
{
	struct stat st;

	return www_fd >= 0 && !fstatat(www_fd, name, &st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode);
}

#define NOTICE "<div style=\"position:fixed;top:0;left:0;right:0;padding:10px;text-align:center;" \
	"background:#cf222e;color:#fff;font:14px system-ui,sans-serif\">No password is set. " \
	"Set one on the switch console with <b>passwd &lt;password&gt;</b>, then log in here.</div>"

/* Answers with the file, or returns false if there is none. Files are served as they are on
 * disk: the 8051 compresses them to save flash and says so with Content-Encoding: gzip, which a
 * client decompresses to the same bytes, so none is sent here. login.html also carries a notice
 * while no password is set. */
static bool send_static(struct conn *c, const char *name)
{
	static const char tail[] = "; charset=UTF-8\r\nCache-Control: max-age=60, must-revalidate\r\nConnection: close\r\n"
		"Access-Control-Allow-Origin: *\r\nContent-Security-Policy: default-src 'self'; script-src 'self' 'unsafe-inline'; "
		"style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; form-action 'self'\r\n\r\n";
	bool notice = !strcmp(name, "login.html") && !auth_has_password();
	char hdr[512];
	struct stat st;
	size_t hl, got = 0;
	uint8_t *buf = NULL;
	int fd;

	if (www_fd < 0 || (fd = openat(www_fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW)) < 0)
		return false;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > FILE_MAX) {
		close(fd);
		return false;
	}
	hl = snprintf(hdr, sizeof hdr, "HTTP/1.1 200 OK\r\nContent-Type: %s%s", mime_of(name), tail);
	buf = malloc(hl + st.st_size + sizeof NOTICE);
	if (buf) {
		memcpy(buf, hdr, hl);
		while (got < (size_t)st.st_size) {
			ssize_t n = read(fd, buf + hl + got, st.st_size - got);

			if (n <= 0)
				break;
			got += n;
		}
	}
	close(fd);
	if (!buf || got != (size_t)st.st_size) {	/* unreadable: a server fault, not a missing file */
		free(buf);
		SET_OUT_STR(c, "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n");
		return true;
	}
	if (notice) {
		uint8_t *body = memmem(buf + hl, got, "<body>", 6);

		if (body) {
			body += 6;
			memmove(body + sizeof NOTICE - 1, body, buf + hl + got - body);
			memcpy(body, NOTICE, sizeof NOTICE - 1);
			got += sizeof NOTICE - 1;
		}
	}
	free(c->out);
	c->out = buf;
	c->olen = hl + got;
	return true;
}

/* ---- request parsing ------------------------------------------------------------------- */

/* The value of header `name` (lower case, with the colon) if the line is that header. */
static const char *header_value(const char *line, const char *name)
{
	size_t n = strlen(name);

	if (strncasecmp(line, name, n))
		return NULL;
	line += n;
	while (*line == ' ' || *line == '\t')
		line++;
	return line;
}

/* Only the pair `session=` of a Cookie header (any order, any number of pairs). */
static void parse_cookie(struct req *r, const char *v)
{
	while (*v) {
		while (*v == ' ' || *v == ';')
			v++;
		if (!strncmp(v, "session=", 8)) {
			size_t n = strcspn(v + 8, "; ");

			if (n >= sizeof r->cookie)
				n = sizeof r->cookie - 1;
			memcpy(r->cookie, v + 8, n);
			r->cookie[n] = 0;
			return;
		}
		v += strcspn(v, ";");
	}
}

/* Parses the header block rbuf[0..hdr_end), modifying it. false = malformed. */
static bool parse_request(struct conn *c, size_t hdr_end)
{
	struct req *r = &c->req;
	char *p = (char *)c->rbuf, *next;
	size_t n;

	memset(r, 0, sizeof *r);
	r->clen = -1;
	if (memchr(p, 0, hdr_end))
		return false;
	p[hdr_end] = 0;
	if ((next = strstr(p, "\r\n"))) {
		*next = 0;
		next += 2;
	}
	if (!strncmp(p, "GET ", 4)) {
		p += 4;
	} else if (!strncmp(p, "POST ", 5)) {
		r->post = true;
		p += 5;
	} else {
		return false;
	}
	n = strcspn(p, " ?");
	if (*p != '/' || n >= sizeof r->path)
		return false;
	memcpy(r->path, p, n);
	r->query = p[n] == '?' ? p + n + 1 : "";

	while (next && *next) {
		char *line = next;
		const char *v;

		if ((next = strstr(line, "\r\n"))) {
			*next = 0;
			next += 2;
		}
		if ((v = header_value(line, "content-type:"))) {
			if (strlen(v) >= sizeof r->ctype)
				return false;
			strcpy(r->ctype, v);
		} else if ((v = header_value(line, "content-length:"))) {
			char *end;

			r->clen = strtol(v, &end, 10);
			while (*end == ' ' || *end == '\t')
				end++;
			if (end == v || *end || r->clen < 0)
				return false;
		} else if ((v = header_value(line, "cookie:"))) {
			parse_cookie(r, v);
		}
	}
	r->authenticated = r->cookie[0] && auth_session_valid(r->cookie);
	if (!strncasecmp(r->ctype, "multipart/form-data", 19)) {
		const char *b = strcasestr(r->ctype, "boundary=");

		if (b) {
			b += 9;
			if (*b == '"')
				b++;
			n = strcspn(b, "\";");
			if (n >= BOUNDARY_MAX)
				return false;
			memcpy(r->boundary, "\r\n--", 4);
			memcpy(r->boundary + 4, b, n);
			r->boundary[4 + n] = 0;
		}
	}
	return true;
}

/* The number after the first '=' of the query ("?vid=5"), as parse_short() reads it; saturates. */
static uint16_t query_number(const char *q)
{
	unsigned v = 0;

	q = strchr(q, '=');
	if (!q)
		return 0;
	for (q++; *q >= '0' && *q <= '9'; q++)
		v = v > 6552 ? 0xffff : v * 10 + (*q - '0');
	return v;
}

/* ---- endpoints -------------------------------------------------------------------------- */

/* POST bodies: 1 when the whole body (NUL-terminated in place) is in, 0 when a response is set
 * (length missing or over `max`) or more has to arrive. */
static int body_complete(struct conn *c, long max)
{
	long have = c->rlen - c->body_off;

	if (!c->req.clen || c->req.clen < 0) {
		send_length_required(c);
		return 0;
	}
	if (c->req.clen > max) {
		send_bad_request(c);
		return 0;
	}
	if (have < c->req.clen)
		return 0;
	c->rbuf[c->body_off + c->req.clen] = 0;
	return 1;
}

/* Commands of a POST /cmd. Output is the response body; "OK" if a command printed nothing and
 * succeeded, status 400 if one failed (what the parser printed explains it). */
static void run_cmd_body(struct conn *c, char *body)
{
	static const char hdr[] = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/plain\r\n\r\n";
	const char *o;
	size_t n, hl = sizeof hdr - 1;
	bool trunc;
	char *buf;

	console_capture_begin();
	execute_commands((uint8_t *)body);
	auth_take_plain();
	o = console_capture_end(&n, &trunc);
	buf = malloc(hl + n + sizeof CMD_TRUNCATED + 3);
	if (!buf)
		return;
	memcpy(buf, hdr, hl);
	memcpy(buf + hl, o, n);
	if (trunc) {
		memcpy(buf + hl + n, CMD_TRUNCATED, sizeof CMD_TRUNCATED - 1);
		n += sizeof CMD_TRUNCATED - 1;
	}
	if (err_status == ERR_OK && !n) {
		memcpy(buf + hl, "OK\n", 3);
		n = 3;
	}
	if (err_status != ERR_OK)
		memcpy(buf + 9, "400 NO", 6);	/* as long as "200 OK" */
	free(c->out);
	c->out = (uint8_t *)buf;
	c->olen = hl + n;
}

/* "pwd=<percent-encoded>": the decoded value, false if malformed or too long. */
static bool login_password(const char *body, char *pw, size_t size)
{
	size_t n = 0;

	if (strncmp(body, "pwd=", 4))
		return false;
	for (body += 4; *body && *body != '&'; body++, n++) {
		unsigned hi, lo;
		char ch = *body;

		if (n + 1 >= size)
			return false;
		if (ch == '+') {
			ch = ' ';
		} else if (ch == '%') {
			if (sscanf(body + 1, "%1x%1x", &hi, &lo) != 2)
				return false;
			ch = hi << 4 | lo;
			body += 2;
		}
		pw[n] = ch;
	}
	pw[n] = 0;
	return true;
}

static void run_login_body(struct conn *c, const char *body)
{
	char pw[128], id[33], resp[200];
	bool ok = login_password(body, pw, sizeof pw);

	if (ok && !auth_has_password()) {
		fprintf(stderr, "rtlplayground: web login refused: no password is set; set one on the console with `passwd <password>`\n");
		ok = false;
	} else if (ok) {
		ok = auth_check_password(pw);
	}
	memset(pw, 0, sizeof pw);
	if (ok && auth_login(id)) {
		snprintf(resp, sizeof resp, "HTTP/1.1 302 Found\r\nConnection: close\r\nLocation: index.html\r\n"
			 "Set-Cookie: session=%s; HttpOnly; SameSite=Strict\r\n\r\n", id);
		set_out(c, resp, strlen(resp));
	} else {
		send_to_login(c);
	}
}

/* The part of a multipart body holding the configuration (the one with a filename), found the
 * way httpd.c config_take() does. `b` starts with the CRLF that ends the header block, so the
 * first boundary is preceded by "\r\n" like all others. */
static bool config_part(const uint8_t *b, size_t n, const char *bf, const uint8_t **text, size_t *len)
{
	size_t bl = strlen(bf), last = 0, pos = 0;

	/* the closing boundary is the boundary followed by "--" */
	while (last + bl + 1 < n && !(!memcmp(b + last, bf, bl) && b[last + bl] == '-' && b[last + bl + 1] == '-'))
		last++;
	if (last + bl + 1 >= n)
		return false;
	while (pos < last) {
		const uint8_t *hdr, *sep;
		size_t body, end, run = 0;

		if (memcmp(b + pos, bf, bl)) {
			pos++;
			continue;
		}
		hdr = b + pos + bl;
		sep = memmem(hdr, last - (pos + bl), "\r\n\r\n", 4);
		if (!sep)
			return false;
		body = sep + 4 - b;
		for (end = body; end < last && memcmp(b + end, bf, bl); end++)
			;
		if (memmem(hdr, sep + 4 - hdr, "filename", 8)) {
			if (end - body + 1 > CONFIG_LEN)	/* the text and its NUL must fit the area */
				return false;
			for (size_t i = body; i < end; i++) {
				if (b[i] == '\n')
					run = 0;
				else if (++run >= CMD_BUF_SIZE - 1)
					return false;
			}
			*text = b + body;
			*len = end - body;
			return true;
		}
		pos = end;
	}
	return false;
}

static void run_config_body(struct conn *c)
{
	const uint8_t *text;
	size_t len;

	if (!config_part(c->rbuf + c->body_off - 2, c->req.clen + 2, c->req.boundary, &text, &len))
		send_bad_request(c);
	else if (config_save(text, len))
		send_ok(c);
	else
		send_text(c, "500 Internal Server Error", "The configuration could not be stored\n");
}

static void route_post(struct conn *c)
{
	struct req *r = &c->req;

	if (!r->ctype[0]) {	/* httpd.c answers a POST without a Content-Type with 404 */
		send_not_found(c);
	} else if (!strcmp(r->path, "/upload")) {
		if (!r->authenticated)
			send_unauthorized(c);
		else
			send_text(c, "501 Not Implemented", "Firmware upload is done with sysupgrade on Linux\n");
	} else if (!strcmp(r->path, "/config")) {
		if (!r->authenticated)
			send_unauthorized(c);
		else if (!r->boundary[0])
			send_bad_request(c);
		else if (body_complete(c, CONFIG_UPLOAD_BUF - 1))
			run_config_body(c);
	} else if (!strcmp(r->path, "/cmd")) {
		if (!r->authenticated)
			send_unauthorized(c);
		else if (body_complete(c, CMD_BUF_SIZE - 1))
			run_cmd_body(c, (char *)c->rbuf + c->body_off);
	} else if (!strcmp(r->path, "/login")) {
		if (strncasecmp(r->ctype, "application/x-www-form-urlencoded", 33))
			send_bad_request(c);
		else if (body_complete(c, LOGIN_BODY_MAX))
			run_login_body(c, (char *)c->rbuf + c->body_off);
	} else {
		send_not_found(c);
	}
}

static void route_get(struct conn *c)
{
	struct req *r = &c->req;
	const char *q = r->path, *name = static_name(q);

	slen = 0;
	cfg_rest_len = 0;
	/* Before a password exists nobody has a session: the front page is the login page. */
	if (name && !strcmp(name, "index.html") && !auth_has_password())
		name = "login.html";
	if (name) {
		bool public = !strcmp(name, "login.html") || !strcmp(name, "favicon.ico");

		if (public || r->authenticated) {
			if (send_static(c, name))
				return;
		} else if (static_exists(name)) {	/* strangers get the login page instead of a file */
			send_to_login(c);
			return;
		}
	}
	if (!r->authenticated) {
		send_unauthorized(c);
		return;
	}
	if (!strcmp(q, "/status.json")) {
		send_status();
	} else if (!strcmp(q, "/information.json")) {
		send_basic_info();
	} else if (!strcmp(q, "/vlan.json")) {
		send_vlan(query_number(r->query));
	} else if (!strcmp(q, "/counters.json")) {
		uint16_t port = query_number(r->query);

		if (send_counters(port > 255 ? 255 : port)) {
			send_bad_request(c);
			return;
		}
	} else if (!strcmp(q, "/eee.json")) {
		send_eee();
	} else if (!strcmp(q, "/bandwidth.json")) {
		send_bandwidth();
	} else if (!strcmp(q, "/storm.json")) {
		send_storm();
	} else if (!strcmp(q, "/l2.json")) {
		send_l2(query_number(r->query));
	} else if (!strcmp(q, "/l2_del.json")) {
		l2_delete(query_number(r->query));
	} else if (!strcmp(q, "/mirror.json")) {
		send_mirror();
	} else if (!strcmp(q, "/isolation.json")) {
		send_isolation();
	} else if (!strcmp(q, "/mtu.json")) {
		send_mtu();
	} else if (!strcmp(q, "/lag.json")) {
		send_lag();
	} else if (!strcmp(q, "/stp.json")) {
		send_stp();
	} else if (!strcmp(q, "/lldp.json")) {
		send_lldp();
	} else if (!strcmp(q, "/lacp.json")) {
		send_lacp();
	} else if (!strcmp(q, "/vlanlist")) {
		send_vlanlist();
	} else if (!strcmp(q, "/config")) {
		send_config();
	} else if (!strcmp(q, "/cmd_log")) {
		send_cmd_log();
	} else if (!strcmp(q, "/cmd_log_clear")) {
		clear_command_history();
		send_mtu();	/* a dummy response, as httpd.c */
	} else if (!strcmp(q, "/reset")) {
		/* The 8051 resets its chip; the daemon starts over once the answer is out. */
		send_text(c, "200 OK", "Restarting\n");
		c->restart = true;
		return;
	} else {
		send_not_found(c);
		return;
	}
	send_page(c);
}

/* ---- connections ------------------------------------------------------------------------ */

static void start_write(struct conn *c, int64_t now);

/* Runs the request once its header block (and, for a POST, its body) is complete. */
static void process(struct conn *c, int64_t now)
{
	if (!c->parsed) {
		uint8_t *end = NULL;
		size_t from = c->scan > 3 ? c->scan - 3 : 0;

		if (c->rlen >= 4)
			end = memmem(c->rbuf + from, c->rlen - from, "\r\n\r\n", 4);
		c->scan = c->rlen;
		if (!end) {
			if (c->rlen >= HDR_MAX) {
				send_bad_request(c);
				start_write(c, now);
			}
			return;
		}
		if ((size_t)(end - c->rbuf) + 4 > HDR_MAX || !parse_request(c, end - c->rbuf)) {
			send_bad_request(c);
			start_write(c, now);
			return;
		}
		c->parsed = true;
		c->body_off = end - c->rbuf + 4;
	}
	if (c->req.post)
		route_post(c);
	else
		route_get(c);
	if (c->out)
		start_write(c, now);
}

static void conn_close(struct conn **slot)
{
	struct conn *c = *slot;

	close(c->fd);
	free(c->out);
	free(c);
	*slot = NULL;
}

static void start_write(struct conn *c, int64_t now)
{
	c->st = WRITE;
	c->ooff = 0;
	c->deadline = now + IDLE_MS;
}

/* Writes what the socket takes. true when the connection is over. */
static bool pump_write(struct conn *c, int64_t now)
{
	while (c->ooff < c->olen) {
		ssize_t n = send(c->fd, c->out + c->ooff, c->olen - c->ooff, MSG_NOSIGNAL);

		if (n < 0)
			return errno != EAGAIN && errno != EINTR;
		c->ooff += n;
		c->deadline = now + IDLE_MS;
	}
	if (c->restart)
		restart_requested = true;
	shutdown(c->fd, SHUT_WR);
	c->st = LINGER;
	c->deadline = now + LINGER_MS;
	return false;
}

static bool pump_read(struct conn *c, int64_t now)
{
	uint8_t junk[1024];
	ssize_t n;

	if (c->st == LINGER) {
		n = read(c->fd, junk, sizeof junk);
		if (n < 0)
			return errno != EAGAIN && errno != EINTR;
		c->lingered += n;
		return n == 0 || c->lingered >= LINGER_MAX;
	}
	n = read(c->fd, c->rbuf + c->rlen, sizeof c->rbuf - c->rlen);
	if (n < 0)
		return errno != EAGAIN && errno != EINTR;
	if (n == 0)
		return true;
	c->rlen += n;
	c->deadline = now + IDLE_MS;
	if (c->deadline > c->born + LIFE_MS)
		c->deadline = c->born + LIFE_MS;
	process(c, now);
	return c->st == WRITE && pump_write(c, now);
}

/* ---- the listener ----------------------------------------------------------------------- */

int http_open(const char *spec, const char *www_dir)
{
	char host[64];
	const char *colon = strrchr(spec, ':'), *port;
	struct addrinfo hints = { .ai_flags = AI_PASSIVE | AI_NUMERICHOST | AI_NUMERICSERV, .ai_socktype = SOCK_STREAM }, *ai;
	size_t hl;
	int one = 1, rc;

	if (!colon) {
		fprintf(stderr, "rtlplayground: --http wants <addr:port>, e.g. [::]:80 or 0.0.0.0:8080\n");
		return -1;
	}
	port = colon + 1;
	hl = colon - spec;
	if (hl >= 2 && spec[0] == '[' && spec[hl - 1] == ']') {
		spec++;
		hl -= 2;
	}
	if (hl >= sizeof host) {
		fprintf(stderr, "rtlplayground: --http address too long\n");
		return -1;
	}
	memcpy(host, spec, hl);
	host[hl] = 0;

	www_fd = open(www_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (www_fd < 0)
		fprintf(stderr, "rtlplayground: cannot open --www %s (%s): the web UI will not be served\n", www_dir, strerror(errno));

	for (int attempt = 0; attempt < 2; attempt++) {
		if ((rc = getaddrinfo(host, port, &hints, &ai))) {
			fprintf(stderr, "rtlplayground: --http %s:%s: %s\n", host, port, gai_strerror(rc));
			return -1;
		}
		listen_fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
		if (listen_fd < 0 && errno == EAFNOSUPPORT && !strcmp(host, "::")) {
			/* a kernel without IPv6: serve IPv4 */
			freeaddrinfo(ai);
			strcpy(host, "0.0.0.0");
			continue;
		}
		break;
	}
	if (listen_fd < 0) {
		fprintf(stderr, "rtlplayground: http socket: %s\n", strerror(errno));
		freeaddrinfo(ai);
		return -1;
	}
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (ai->ai_family == AF_INET6) {
		int zero = 0;

		setsockopt(listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
	}
	if (bind(listen_fd, ai->ai_addr, ai->ai_addrlen) || listen(listen_fd, 16)) {
		fprintf(stderr, "rtlplayground: http %s:%s: %s\n", host, port, strerror(errno));
		freeaddrinfo(ai);
		close(listen_fd);
		listen_fd = -1;
		return -1;
	}
	freeaddrinfo(ai);
	return 0;
}

void http_fill(struct pollfd *fds, int64_t now, int *wait_ms)
{
	bool room = false;

	for (int i = 0; i < MAX_CONNS; i++) {
		struct conn *c = conns[i];
		int64_t left;

		fds[1 + i] = (struct pollfd){ .fd = -1 };
		if (!c) {
			room = true;
			continue;
		}
		fds[1 + i] = (struct pollfd){ .fd = c->fd, .events = c->st == WRITE ? POLLOUT : POLLIN };
		left = c->deadline - now;
		if (left < *wait_ms)
			*wait_ms = left > 0 ? left : 0;
	}
	/* a full table leaves new connections in the kernel's backlog until a slot frees up */
	fds[0] = (struct pollfd){ .fd = room ? listen_fd : -1, .events = POLLIN };
}

void http_service(struct pollfd *fds, int64_t now)
{
	for (int i = 0; i < MAX_CONNS; i++) {
		struct conn *c = conns[i];
		bool done = false;

		if (!c || fds[1 + i].fd != c->fd)
			continue;
		if (fds[1 + i].revents & (POLLERR | POLLNVAL))
			done = true;
		else if (c->st == WRITE && (fds[1 + i].revents & (POLLOUT | POLLHUP)))
			done = pump_write(c, now);
		else if (c->st != WRITE && (fds[1 + i].revents & (POLLIN | POLLHUP)))
			done = pump_read(c, now);
		if (done || now >= c->deadline)
			conn_close(&conns[i]);
	}
	if (fds[0].fd >= 0 && (fds[0].revents & POLLIN)) {
		for (;;) {
			int fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
			struct conn *c;
			int i;

			if (fd < 0)
				break;
			for (i = 0; i < MAX_CONNS && conns[i]; i++)
				;
			if (i == MAX_CONNS || !(c = calloc(1, sizeof *c))) {
				close(fd);
				break;
			}
			c->fd = fd;
			c->st = READ;
			c->born = now;
			c->deadline = now + IDLE_MS;
			conns[i] = c;
		}
	}
}
