/*
 * The daemon: one poll() loop over the console (with --console: stdin in, stdout out), the HTTP
 * listeners and their connections, and the CLI socket. The loop itself waits only in poll(); the
 * exceptions are named in doc/linux.md (synchronous netlink calls, the exec check of the DHCP
 * client, the bounded wait for a stopped DHCP client in a command that sets a static address, and
 * the one at exit).
 */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cmd_editor.h"
#include "cmd_parser.h"
#include "platform.h"
#include "linux.h"

extern __xdata char passwd[21];

/* The longest poll() sleep: the DHCP timers (mgmt_service) are served at this rate, and a signal that
 * arrives between the check of `stop` and poll() is noticed after at most this long. */
#define WAKE_MS 1000

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

int main(int argc, char **argv)
{
	int console = 0;
#ifdef RTLP_TEST_HOOKS
	int dump_state = 0;
#endif
	const char *http = "[::]:80", *cli = "/run/rtlplayground.sock", *www = DEFAULT_WWW, *cfg = "/cfg", *dhcp_script = NULL;
	bool http_any = false;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--console")) {
			console = 1;
#ifdef RTLP_TEST_HOOKS
		} else if (!strcmp(argv[i], "--dump-state")) {
			dump_state = 1;	/* test aid: print platform state on exit, see below */
#endif
		} else if (!strcmp(argv[i], "--http-any")) {
			http_any = true;
		} else if (i + 1 < argc && !strcmp(argv[i], "--http")) {
			http = argv[++i];
		} else if (i + 1 < argc && !strcmp(argv[i], "--cli-socket")) {
			cli = argv[++i];
		} else if (i + 1 < argc && !strcmp(argv[i], "--www")) {
			www = argv[++i];
		} else if (i + 1 < argc && !strcmp(argv[i], "--cfg-dir")) {
			cfg = argv[++i];
		} else if (i + 1 < argc && !strcmp(argv[i], "--dhcp-script")) {
			dhcp_script = argv[++i];
		} else {
			fprintf(stderr, "usage: %s [--console] [--http <addr:port> [--http-any]] [--cli-socket <path>] [--www <dir>] [--cfg-dir <dir>] [--dhcp-script <udhcpc script>]\n", argv[0]);
			return 2;
		}
	}

	prctl(PR_SET_NAME, "rtlplayground");	/* /reset re-executes /proc/self/exe, which would name the process "exe" */
	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	cmd_editor_init();
	/* Nothing of the network is touched before this has shown that no other daemon runs on the
	 * configuration directory and the CLI socket and that the web port is free. */
	if (config_open(cfg) || cli_open(cli) || http_open(http, www, http_any))
		return 1;
	ports_init();
	bridge_init();
	ports_reset();
	mgmt_init(dhcp_script);
	stp_state_init();
	http_retry(now_ms());	/* the listener that needed swbr */
	/* Runs the startup configuration (or the default); the shared parser records commands in the
	 * history only from here on. */
	config_startup();
	bridge_commit();
	if (console) {
		print_cmd_prompt();
		console_flush();
	}

	while (!stop) {
		/* slot 0 is the console (fd -1: ignored), then the HTTP slots, then the CLI slots */
		struct pollfd fds[1 + HTTP_SLOTS + CLI_SLOTS];
		int64_t now = now_ms();
		int wait = WAKE_MS;

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
		mgmt_service(now);
		if (http_restart_requested()) {
			/* GET /reset: the answer is out; start over with the same arguments. The sockets are
			 * close-on-exec, so the new process binds them afresh. */
			console_flush();
			cli_close();
			mgmt_exit();
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
		console_flush();
	}
	console_flush();
	cli_close();
	mgmt_exit();
#ifdef RTLP_TEST_HOOKS
	if (dump_state) {
		/* One `name=value` line per piece of state a test needs to see; add to it as tests need. */
		printf("\nlldp_enabled=%d\n", lldp_enabled);
		printf("has_password=%d\npasswd_plain_left=%d\n", auth_has_password(), passwd[0] != 0);
		fflush(stdout);
	}
#endif
	return 0;
}
