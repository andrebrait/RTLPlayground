/*
 * The daemon: one poll() loop over the console (with --console: stdin in, stdout out), the HTTP
 * listener and its connections, and the CLI socket. Nothing in it blocks.
 */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cmd_editor.h"
#include "cmd_parser.h"
#include "platform.h"
#include "linux.h"

extern __xdata char passwd[21];

#define TICK_MS 1000

#define DEFAULT_WWW "/usr/share/rtlplayground/www"

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	stop = 1;
}

int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Periodic work, once per TICK_MS. */
static void tick(void)
{
}

int main(int argc, char **argv)
{
	int console = 0, dump_state = 0;
	const char *http = "[::]:80", *cli = "/run/rtlplayground.sock", *www = DEFAULT_WWW, *cfg = "/cfg";

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--console")) {
			console = 1;
		} else if (!strcmp(argv[i], "--dump-state")) {
			dump_state = 1;	/* test aid: print platform state on exit, see below */
		} else if (i + 1 < argc && !strcmp(argv[i], "--http")) {
			http = argv[++i];
		} else if (i + 1 < argc && !strcmp(argv[i], "--cli-socket")) {
			cli = argv[++i];
		} else if (i + 1 < argc && !strcmp(argv[i], "--www")) {
			www = argv[++i];
		} else if (i + 1 < argc && !strcmp(argv[i], "--cfg-dir")) {
			cfg = argv[++i];
		} else {
			fprintf(stderr, "usage: %s [--console] [--http <addr:port>] [--cli-socket <path>] [--www <dir>] [--cfg-dir <dir>]\n", argv[0]);
			return 2;
		}
	}

	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	if (http_open(http, www) || cli_open(cli))
		return 1;
	cmd_editor_init();
	ports_init();
	/* Runs the startup configuration (or the default); the shared parser records commands in the
	 * history only from here on. */
	config_open(cfg);
	config_startup();
	if (console) {
		print_cmd_prompt();
		console_flush();
	}

	int64_t deadline = now_ms() + TICK_MS;

	while (!stop) {
		/* slot 0 is the console (fd -1: ignored), then the HTTP slots, then the CLI slots */
		struct pollfd fds[1 + HTTP_SLOTS + CLI_SLOTS];
		int64_t now = now_ms();
		int wait = deadline > now ? deadline - now : 0;

		fds[0] = (struct pollfd){ .fd = console ? STDIN_FILENO : -1, .events = POLLIN };
		http_fill(fds + 1, now, &wait);
		cli_fill(fds + 1 + HTTP_SLOTS, now, &wait);
		if (poll(fds, 1 + HTTP_SLOTS + CLI_SLOTS, wait) < 0 && errno != EINTR) {
			perror("poll");
			return 1;
		}
		now = now_ms();
		http_service(fds + 1, now);
		cli_service(fds + 1 + HTTP_SLOTS, now);
		if (http_restart_requested()) {
			/* GET /reset: the answer is out; start over with the same arguments. The sockets are
			 * close-on-exec, so the new process binds them afresh. */
			console_flush();
			cli_close();
			execv("/proc/self/exe", argv);
			perror("execv /proc/self/exe");
			return 1;
		}
		if (console && (fds[0].revents & (POLLIN | POLLHUP))) {
			uint8_t buf[256];
			ssize_t len = read(STDIN_FILENO, buf, sizeof buf);

			if (len == 0)
				break;	/* EOF: everything fed has been run and flushed below */
			if (len > 0)
				console_feed(buf, len);
		}
		/* The deadline is checked after every wake-up, not only on a poll timeout, so
		 * continuous input cannot starve the tick. */
		if (now_ms() >= deadline) {
			tick();
			deadline += TICK_MS;
			if (deadline < now_ms())	/* stalled for more than a tick: do not catch up */
				deadline = now_ms() + TICK_MS;
		}
		console_flush();
	}
	console_flush();
	cli_close();
	if (dump_state) {
		/* One `name=value` line per piece of state a test needs to see; add to it as tests need. */
		printf("\nlldp_enabled=%d\n", lldp_enabled);
		printf("has_password=%d\npasswd_plain_left=%d\n", auth_has_password(), passwd[0] != 0);
		fflush(stdout);
	}
	return 0;
}
