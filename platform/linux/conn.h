#ifndef _CONN_H_
#define _CONN_H_

/*
 * The connection machinery of the HTTP server and the CLI socket (conn.c): a table of non-blocking
 * stream connections in the daemon's poll loop. A connection reads into a buffer its protocol
 * names, the protocol says when a request is complete and builds the whole reply in memory, the
 * reply is written, and the connection lingers briefly (the daemon has shut down its side) so a
 * client that still sends, such as one with an over-long request, gets to read the verdict. A
 * connection that makes no progress for idle_ms, or that has not been answered after life_ms, is
 * closed.
 */
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct conn {		/* the first member of a protocol's own connection struct */
	int fd;
	enum { CONN_READ, CONN_WRITE, CONN_LINGER } st;
	int64_t deadline, born;
	size_t olen, ooff, lingered;
	uint8_t *out;	/* the reply: malloc'ed, freed with the connection */
};

enum conn_rc {
	CONN_MORE,	/* the request is not complete */
	CONN_REPLY,	/* c->out / c->olen hold the reply: write it */
	CONN_CLOSE,	/* nothing (more) to say */
};

struct conn_proto {
	size_t size;				/* sizeof the protocol's struct */
	int idle_ms, life_ms, linger_ms;
	size_t linger_max;			/* bytes drained while lingering */
	/* where the next bytes go and how many fit; room 0 reads as end of input */
	uint8_t *(*rbuf)(struct conn *c, size_t *room);
	/* n bytes arrived; n == 0: the client closed its side */
	enum conn_rc (*on_read)(struct conn *c, size_t n, int64_t now);
	void (*on_sent)(struct conn *c);	/* the reply is out; may be NULL */
};

#define CONN_LISTEN_MAX 3
struct conn_set {
	const struct conn_proto *proto;
	struct conn **slot;
	int nslot;
	int nlisten;				/* listening sockets in lfd, each fd or -1 */
	int lfd[CONN_LISTEN_MAX];
};

/* The poll slots of a set: nlisten listeners, then one per connection slot. fill() lowers *wait_ms
 * to the nearest connection deadline; a listener whose table is full is not polled (the clients
 * wait in the kernel's backlog, none is accepted and dropped). service() runs after poll() on the same slots. */
void conn_fill(struct conn_set *s, struct pollfd *fds, int64_t now, int *wait_ms);
void conn_service(struct conn_set *s, struct pollfd *fds, int64_t now);

#endif
