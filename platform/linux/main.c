/*
 * The daemon: one poll() loop. With --console it reads the console from stdin and answers on
 * stdout; later sockets (HTTP, CLI) join the same loop.
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

#define TICK_MS 1000

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	stop = 1;
}

static int64_t now_ms(void)
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

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--console")) {
			console = 1;
		} else if (!strcmp(argv[i], "--dump-state")) {
			dump_state = 1;	/* test aid: print platform state on exit, see below */
		} else {
			fprintf(stderr, "usage: %s [--console]\n", argv[0]);
			return 2;
		}
	}

	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	cmd_editor_init();
	if (console) {
		print_cmd_prompt();
		console_flush();
	}

	int64_t deadline = now_ms() + TICK_MS;

	while (!stop) {
		struct pollfd fds[1];
		int n = 0;
		int64_t wait = deadline - now_ms();

		if (console)
			fds[n++] = (struct pollfd){ .fd = STDIN_FILENO, .events = POLLIN };
		if (poll(fds, n, wait > 0 ? wait : 0) < 0 && errno != EINTR) {
			perror("poll");
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
	if (dump_state) {
		/* One `name=value` line per piece of state a test needs to see; add to it as tests need. */
		printf("\nlldp_enabled=%d\n", lldp_enabled);
		fflush(stdout);
	}
	return 0;
}
