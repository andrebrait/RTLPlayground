/*
 * swcli: runs console commands on the daemon through its CLI socket.
 *
 *   swcli [-s <socket>] <command words...>    one command: the words joined by spaces
 *   swcli [-s <socket>]                       lines from stdin, one command each (a prompt on a terminal)
 *
 * One connection per command. Exit status: 0, or 1 if the daemon cannot be reached.
 */
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define DEFAULT_SOCKET "/run/rtlplayground.sock"

static const char *sock_path = DEFAULT_SOCKET;

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
		line[strcspn(line, "\n")] = 0;
		if (run(line))
			return 1;
	}
}
