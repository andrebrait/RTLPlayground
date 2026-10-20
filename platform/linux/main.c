/*
 * The daemon: one poll() loop. With --console it reads the console from stdin and answers on
 * stdout; later sockets (HTTP, CLI) join the same loop.
 */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "cmd_editor.h"
#include "cmd_parser.h"
#include "linux.h"

#define TICK_MS 1000

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	stop = 1;
}

int main(int argc, char **argv)
{
	int console = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--console")) {
			console = 1;
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

	while (!stop) {
		struct pollfd fds[1];
		int n = 0;

		if (console)
			fds[n++] = (struct pollfd){ .fd = STDIN_FILENO, .events = POLLIN };
		if (poll(fds, n, TICK_MS) < 0 && errno != EINTR) {
			perror("poll");
			return 1;
		}
		/* Once per TICK_MS the loop passes here with nothing readable; periodic work goes here. */
		if (console && (fds[0].revents & (POLLIN | POLLHUP))) {
			uint8_t buf[256];
			ssize_t len = read(STDIN_FILENO, buf, sizeof buf);

			if (len == 0)
				break;	/* EOF: everything fed has been run and flushed below */
			if (len > 0)
				console_feed(buf, len);
		}
		console_flush();
	}
	console_flush();
	return 0;
}
