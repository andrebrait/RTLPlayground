/*
 * swcli: runs console commands on the daemon through its CLI socket.
 *
 *   swcli [-s <socket>] <command words...>    one command: the words joined by spaces
 *   swcli [-s <socket>]                       lines from stdin, one command each (a prompt on a terminal)
 *
 * One connection per command. Exit status: 0, or 1 if the daemon cannot be reached.
 *
 * `swcli passwd <password>` puts the password in the process list and the shell history; the
 * interactive mode (commands on stdin) keeps it out of both. A line longer than the buffer is
 * refused, never split into two commands.
 */
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define DEFAULT_SOCKET "/run/rtlplayground.sock"

static const char *sock_path = DEFAULT_SOCKET;

static void usage(void)
{
	puts("usage: swcli [-s <socket>] <command words...>   run one command on the daemon\n"
	     "       swcli [-s <socket>]                       read commands from stdin, one per line\n"
	     "socket: " DEFAULT_SOCKET "\n"
	     "A password given as an argument (swcli passwd <password>) is visible in the process list and\n"
	     "the shell history; feed it on stdin instead: echo 'passwd <password>' | swcli");
}

/* Sends one line, copies the reply to stdout. */
static int run(const char *line)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	char buf[4096];
	ssize_t n;
	size_t len = strlen(line), sent = 0;
	int fd;

	if (strlen(sock_path) >= sizeof sa.sun_path) {
		fprintf(stderr, "swcli: socket path too long\n");
		return 1;
	}
	strcpy(sa.sun_path, sock_path);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa)) {
		perror(sock_path);
		return 1;
	}
	while (sent < len) {
		n = write(fd, line + sent, len - sent);
		if (n <= 0) {
			perror("swcli: write");
			close(fd);
			return 1;
		}
		sent += n;
	}
	if (write(fd, "\n", 1) != 1) {
		perror("swcli: write");
		close(fd);
		return 1;
	}
	while ((n = read(fd, buf, sizeof buf)) > 0)
		fwrite(buf, 1, n, stdout);
	fflush(stdout);
	close(fd);
	return n < 0;
}

int main(int argc, char **argv)
{
	char line[1024];
	int i = 1;

	if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
		usage();
		return 0;
	}
	if (argc > 2 && !strcmp(argv[1], "-s")) {
		sock_path = argv[2];
		i = 3;
	}
	if (i < argc) {
		size_t len = 0;

		line[0] = 0;
		for (; i < argc; i++) {
			size_t n = strlen(argv[i]);

			if (len + n + 2 > sizeof line) {
				fprintf(stderr, "swcli: command too long\n");
				return 1;
			}
			len += snprintf(line + len, sizeof line - len, "%s%s", len ? " " : "", argv[i]);
		}
		return run(line);
	}
	for (;;) {
		if (isatty(0)) {
			fputs("swcli> ", stdout);
			fflush(stdout);
		}
		if (!fgets(line, sizeof line, stdin))
			return 0;
		if (!strchr(line, '\n') && !feof(stdin)) {	/* longer than the buffer: not a command to run in pieces */
			int ch;

			while ((ch = getchar()) != '\n' && ch != EOF)
				;
			fprintf(stderr, "swcli: line too long (over %zu bytes), not sent\n", sizeof line - 1);
			continue;
		}
		line[strcspn(line, "\n")] = 0;
		if (run(line))
			return 1;
	}
}
