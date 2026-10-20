/*
 * The daemon's rtnetlink socket: a request goes out, the answer (acknowledgement or dump) is read
 * to its end before the call returns, so nothing is ever left in the socket for a later request.
 */
#include <errno.h>

#include "nl.h"

static struct mnl_socket *sock;
static unsigned seq;
/* a dump of a port with thousands of VLANs is one message of tens of kilobytes */
static char rbuf[65536];

struct nlmsghdr *nl_msg(char *buf, uint16_t type, uint16_t flags)
{
	struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);

	nlh->nlmsg_type = type;
	nlh->nlmsg_flags = NLM_F_REQUEST | flags;
	return nlh;
}

static int run(struct nlmsghdr *nlh, mnl_cb_t cb, void *data)
{
	int ret;

	if (!sock) {
		sock = mnl_socket_open(NETLINK_ROUTE);
		if (sock && mnl_socket_bind(sock, 0, MNL_SOCKET_AUTOPID) < 0) {
			int e = errno;

			mnl_socket_close(sock);
			sock = NULL;
			errno = e;
		}
		if (!sock)
			return -errno;
	}
	nlh->nlmsg_seq = ++seq;
	if (mnl_socket_sendto(sock, nlh, nlh->nlmsg_len) < 0)
		return -errno;
	do {
		ssize_t n = mnl_socket_recvfrom(sock, rbuf, sizeof rbuf);

		if (n < 0)
			return -errno;
		ret = mnl_cb_run(rbuf, n, seq, mnl_socket_get_portid(sock), cb, data);
	} while (ret > MNL_CB_STOP);
	return ret < 0 ? -errno : 0;
}

int nl_talk(struct nlmsghdr *nlh)
{
	nlh->nlmsg_flags |= NLM_F_ACK;
	return run(nlh, NULL, NULL);
}

int nl_dump(struct nlmsghdr *nlh, mnl_cb_t cb, void *data)
{
	nlh->nlmsg_flags |= NLM_F_DUMP;
	return run(nlh, cb, data);
}
