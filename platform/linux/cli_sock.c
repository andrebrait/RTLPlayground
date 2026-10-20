/*
 * The CLI socket: an AF_UNIX stream socket (mode 0600) over which `swcli` runs one console command
 * per connection. The client sends one line; the daemon runs it as the web UI's POST /cmd does,
 * answers with the command's output and closes. The socket is for the local administrator, so
 * there is no login. Like the HTTP connections these are non-blocking and time out when idle.
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
#include "linux.h"

#define MAX_CONNS (CLI_SLOTS - 1)
#define IDLE_MS 5000
#define LINE_MAX_LEN 511

struct conn {
	int fd;
	bool writing;
	int64_t deadline;
	size_t rlen, olen, ooff;
	char *out;
	char line[LINE_MAX_LEN + 1];
};

static int listen_fd = -1;
static char sock_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static struct conn *conns[MAX_CONNS];

int cli_open(const char *path)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	struct stat st;
	mode_t old;

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
	listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (listen_fd < 0) {
		fprintf(stderr, "rtlplayground: cli socket: %s\n", strerror(errno));
		return -1;
	}
	old = umask(0177);
	if (bind(listen_fd, (struct sockaddr *)&sa, sizeof sa) || listen(listen_fd, 8)) {
		fprintf(stderr, "rtlplayground: cli socket %s: %s\n", path, strerror(errno));
		umask(old);
		close(listen_fd);
		listen_fd = -1;
		return -1;
	}
	umask(old);
	strcpy(sock_path, path);
	return 0;
}

void cli_close(void)
{
	if (sock_path[0])
		unlink(sock_path);
	sock_path[0] = 0;
}

static void conn_close(struct conn **slot)
{
	close((*slot)->fd);
	free((*slot)->out);
	free(*slot);
	*slot = NULL;
}

/* Runs the line and prepares the reply. */
static void run_line(struct conn *c, int64_t now)
{
	const char *o;
	size_t n;
	bool trunc;
	char *nl = memchr(c->line, '\n', c->rlen);

	if (nl)
		*nl = 0;
	else
		c->line[c->rlen] = 0;
	console_capture_begin();
	execute_commands((uint8_t *)c->line);
	auth_take_plain();
	o = console_capture_end(&n, &trunc);
	c->out = malloc(n + sizeof CMD_TRUNCATED);
	if (c->out) {
		memcpy(c->out, o, n);
		if (trunc) {
			memcpy(c->out + n, CMD_TRUNCATED, sizeof CMD_TRUNCATED - 1);
			n += sizeof CMD_TRUNCATED - 1;
		}
	}
	c->olen = c->out ? n : 0;
	c->writing = true;
	c->deadline = now + IDLE_MS;
}

static bool pump_write(struct conn *c, int64_t now)
{
	while (c->ooff < c->olen) {
		ssize_t n = send(c->fd, c->out + c->ooff, c->olen - c->ooff, MSG_NOSIGNAL);

		if (n < 0)
			return errno != EAGAIN && errno != EINTR;
		c->ooff += n;
		c->deadline = now + IDLE_MS;
	}
	return true;
}

static bool pump_read(struct conn *c, int64_t now)
{
	ssize_t n = read(c->fd, c->line + c->rlen, LINE_MAX_LEN - c->rlen);

	if (n < 0)
		return errno != EAGAIN && errno != EINTR;
	c->rlen += n;
	if (n == 0 || memchr(c->line, '\n', c->rlen)) {
		if (n == 0 && !c->rlen)
			return true;	/* the client left without a command */
		run_line(c, now);
		return pump_write(c, now);
	}
	if (c->rlen >= LINE_MAX_LEN) {	/* no end of line in sight: the shared parser would reject it too */
		run_line(c, now);	/* ... and says why */
		return pump_write(c, now);
	}
	c->deadline = now + IDLE_MS;
	return false;
}

void cli_fill(struct pollfd *fds, int64_t now, int *wait_ms)
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
		fds[1 + i] = (struct pollfd){ .fd = c->fd, .events = c->writing ? POLLOUT : POLLIN };
		left = c->deadline - now;
		if (left < *wait_ms)
			*wait_ms = left > 0 ? left : 0;
	}
	fds[0] = (struct pollfd){ .fd = room ? listen_fd : -1, .events = POLLIN };
}

void cli_service(struct pollfd *fds, int64_t now)
{
	for (int i = 0; i < MAX_CONNS; i++) {
		struct conn *c = conns[i];
		bool done = false;

		if (!c || fds[1 + i].fd != c->fd)
			continue;
		if (fds[1 + i].revents & (POLLERR | POLLNVAL))
			done = true;
		else if (c->writing && (fds[1 + i].revents & (POLLOUT | POLLHUP)))
			done = pump_write(c, now);
		else if (!c->writing && (fds[1 + i].revents & (POLLIN | POLLHUP)))
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
			c->deadline = now + IDLE_MS;
			conns[i] = c;
		}
	}
}
