/*
 * The CLI socket: an AF_UNIX stream socket (mode 0600) over which `swcli` runs one console command
 * per connection. The client sends one line; the daemon runs it as the web UI's POST /cmd does,
 * answers with the command's output and closes. The socket is for the local administrator, so
 * there is no login. Like the HTTP connections these are non-blocking, time out when idle and
 * cannot live longer than LIFE_MS; after the reply the daemon shuts down its side and drains
 * what the client still sends for a moment, so the client reads the reply even when it sent more
 * than one line (an over-long one).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "cmd_parser.h"
#include "platform.h"
#include "rtl837x_common.h"
#include "conn.h"
#include "linux.h"

#define MAX_CONNS (CLI_SLOTS - 1)
#define IDLE_MS 5000
#define LIFE_MS 10000
#define LINGER_MS 2000
#define LINGER_MAX (64 << 10)
#define LINE_MAX_LEN 511

struct cli_conn {
	struct conn c;
	size_t rlen;
	char line[LINE_MAX_LEN + 1];
};

static char sock_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static struct conn *conns[MAX_CONNS];

static enum conn_rc on_read(struct conn *b, size_t n, int64_t now);
static uint8_t *rbuf(struct conn *b, size_t *room);

static const struct conn_proto proto = {
	.size = sizeof(struct cli_conn), .idle_ms = IDLE_MS, .life_ms = LIFE_MS, .linger_ms = LINGER_MS,
	.linger_max = LINGER_MAX, .rbuf = rbuf, .on_read = on_read,
};
static struct conn_set set = { .proto = &proto, .slot = conns, .nslot = MAX_CONNS, .nlisten = 1, .lfd = { -1 } };

int cli_open(const char *path)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	struct stat st;
	mode_t old;
	int fd;

	if (strlen(path) >= sizeof sa.sun_path) {
		fprintf(stderr, "rtlplayground: --cli-socket path too long\n");
		return -1;
	}
	strcpy(sa.sun_path, path);
	/* A socket left by an earlier run is replaced; anything else at that path is not ours to remove. */
	if (!lstat(path, &st)) {
		if (!S_ISSOCK(st.st_mode)) {
			fprintf(stderr, "rtlplayground: %s exists and is not a socket\n", path);
			return -1;
		}
		unlink(path);
	}
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		fprintf(stderr, "rtlplayground: cli socket: %s\n", strerror(errno));
		return -1;
	}
	old = umask(0177);
	if (bind(fd, (struct sockaddr *)&sa, sizeof sa) || listen(fd, 8)) {
		fprintf(stderr, "rtlplayground: cli socket %s: %s\n", path, strerror(errno));
		umask(old);
		close(fd);
		return -1;
	}
	umask(old);
	set.lfd[0] = fd;
	strcpy(sock_path, path);
	return 0;
}

void cli_close(void)
{
	if (sock_path[0])
		unlink(sock_path);
	sock_path[0] = 0;
}

static uint8_t *rbuf(struct conn *b, size_t *room)
{
	struct cli_conn *c = (struct cli_conn *)b;

	*room = LINE_MAX_LEN - c->rlen;
	return (uint8_t *)c->line + c->rlen;
}

/* Runs the line and prepares the reply. */
static void run_line(struct cli_conn *c)
{
	const char *o;
	size_t n;
	bool trunc;
	char *nl = memchr(c->line, '\n', c->rlen);

	if (nl)
		*nl = 0;
	else
		c->line[c->rlen] = 0;
	o = console_run((uint8_t *)c->line, &n, &trunc);
	memset(c->line, 0, sizeof c->line);
	c->c.out = malloc(n + sizeof CMD_TRUNCATED);
	if (c->c.out) {
		memcpy(c->c.out, o, n);
		if (trunc) {
			memcpy(c->c.out + n, CMD_TRUNCATED, sizeof CMD_TRUNCATED - 1);
			n += sizeof CMD_TRUNCATED - 1;
		}
	}
	c->c.olen = c->c.out ? n : 0;
}

static enum conn_rc on_read(struct conn *b, size_t n, int64_t now)
{
	struct cli_conn *c = (struct cli_conn *)b;

	c->rlen += n;
	if (n == 0 && !c->rlen)
		return CONN_CLOSE;	/* the client left without a command */
	/* a complete line, the end of the input, or no end of line in sight: the shared parser would
	 * reject an over-long one too ... and says why */
	if (n == 0 || memchr(c->line, '\n', c->rlen) || c->rlen >= LINE_MAX_LEN) {
		run_line(c);
		return CONN_REPLY;
	}
	return CONN_MORE;
}

void cli_fill(struct pollfd *fds, int64_t now, int *wait_ms)
{
	conn_fill(&set, fds, now, wait_ms);
}

void cli_service(struct pollfd *fds, int64_t now)
{
	conn_service(&set, fds, now);
}
