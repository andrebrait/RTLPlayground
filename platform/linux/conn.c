/* Connection table shared by http.c and cli_sock.c; see conn.h. */
#include <errno.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "conn.h"

static void conn_close(struct conn **slot)
{
	close((*slot)->fd);
	free((*slot)->out);
	free(*slot);
	*slot = NULL;
}

/* Writes what the socket takes. true when the connection is over. */
static bool pump_write(const struct conn_proto *p, struct conn *c, int64_t now)
{
	while (c->ooff < c->olen) {
		ssize_t n = send(c->fd, c->out + c->ooff, c->olen - c->ooff, MSG_NOSIGNAL);

		if (n < 0)
			return errno != EAGAIN && errno != EINTR;
		c->ooff += n;
		c->deadline = now + p->idle_ms;
	}
	if (p->on_sent)
		p->on_sent(c);
	shutdown(c->fd, SHUT_WR);
	c->st = CONN_LINGER;
	c->deadline = now + p->linger_ms;
	return false;
}

static bool pump_read(const struct conn_proto *p, struct conn *c, int64_t now)
{
	size_t room;
	uint8_t *buf;
	ssize_t n;

	if (c->st == CONN_LINGER) {
		uint8_t junk[1024];

		n = read(c->fd, junk, sizeof junk);
		if (n < 0)
			return errno != EAGAIN && errno != EINTR;
		c->lingered += n;
		return n == 0 || c->lingered >= p->linger_max;
	}
	buf = p->rbuf(c, &room);
	n = read(c->fd, buf, room);
	if (n < 0)
		return errno != EAGAIN && errno != EINTR;
	if (n > 0) {
		c->deadline = now + p->idle_ms;
		if (c->deadline > c->born + p->life_ms)
			c->deadline = c->born + p->life_ms;
	}
	switch (p->on_read(c, n, now)) {
	case CONN_MORE:
		return false;
	case CONN_REPLY:
		c->st = CONN_WRITE;
		c->ooff = 0;
		c->deadline = now + p->idle_ms;
		return pump_write(p, c, now);
	default:
		return true;
	}
}

void conn_fill(struct conn_set *s, struct pollfd *fds, int64_t now, int *wait_ms)
{
	bool room = false;

	for (int i = 0; i < s->nslot; i++) {
		struct conn *c = s->slot[i];
		int64_t left;

		fds[s->nlisten + i] = (struct pollfd){ .fd = -1 };
		if (!c) {
			room = true;
			continue;
		}
		fds[s->nlisten + i] = (struct pollfd){ .fd = c->fd, .events = c->st == CONN_WRITE ? POLLOUT : POLLIN };
		left = c->deadline - now;
		if (left < *wait_ms)
			*wait_ms = left > 0 ? left : 0;
	}
	for (int i = 0; i < s->nlisten; i++)
		fds[i] = (struct pollfd){ .fd = room ? s->lfd[i] : -1, .events = POLLIN };
}

void conn_service(struct conn_set *s, struct pollfd *fds, int64_t now)
{
	const struct conn_proto *p = s->proto;

	for (int i = 0; i < s->nslot; i++) {
		struct conn *c = s->slot[i];
		struct pollfd *f = &fds[s->nlisten + i];
		bool done = false;

		if (!c || f->fd != c->fd)
			continue;
		if (f->revents & (POLLERR | POLLNVAL))
			done = true;
		else if (c->st == CONN_WRITE && (f->revents & (POLLOUT | POLLHUP)))
			done = pump_write(p, c, now);
		else if (c->st != CONN_WRITE && (f->revents & (POLLIN | POLLHUP)))
			done = pump_read(p, c, now);
		if (done || now >= c->deadline)
			conn_close(&s->slot[i]);
	}
	for (int l = 0; l < s->nlisten; l++) {
		if (fds[l].fd < 0 || !(fds[l].revents & POLLIN))
			continue;
		for (;;) {
			struct conn *c;
			int i, fd;

			/* no free slot: the rest waits in the kernel's backlog, not accepted-and-dropped */
			for (i = 0; i < s->nslot && s->slot[i]; i++)
				;
			if (i == s->nslot)
				break;
			fd = accept4(fds[l].fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
			if (fd < 0)
				break;
			if (!(c = calloc(1, p->size))) {
				close(fd);
				break;
			}
			c->fd = fd;
			c->st = CONN_READ;
			c->born = now;
			c->deadline = now + p->idle_ms;
			s->slot[i] = c;
		}
	}
}
