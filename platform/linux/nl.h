#ifndef _NL_H_
#define _NL_H_

#include <stddef.h>
#include <stdint.h>

/* the 8051 storage-class shim defines __data, which linux/socket.h uses as a member name */
#pragma push_macro("__data")
#undef __data
#include <libmnl/libmnl.h>
#include <linux/if_addr.h>
#include <linux/if_bridge.h>
#include <linux/if_link.h>
#include <linux/rtnetlink.h>
#pragma pop_macro("__data")

/* One rtnetlink socket for the daemon (nl.c), opened on first use. Both calls complete the
 * request before they return: 0, or -errno (the kernel's answer, or the socket's). */

/* Starts a message in buf (at least NL_MSG_MAX bytes); the caller adds its header and attributes. */
#define NL_MSG_MAX 1024
struct nlmsghdr *nl_msg(char *buf, uint16_t type, uint16_t flags);
/* Sends nlh (NLM_F_REQUEST|NLM_F_ACK added) and waits for the acknowledgement. */
int nl_talk(struct nlmsghdr *nlh);
/* Sends nlh (a GET of one object) and runs cb on its single answer message; 0 or -errno. */
int nl_get(struct nlmsghdr *nlh, mnl_cb_t cb, void *data);
/* cmd_error("<what>: <strerror(-err)>\n") */
void nl_error(const char *what, int err);
/* Sends nlh (NLM_F_REQUEST|NLM_F_DUMP added); cb runs for every message of the answer and
 * returns MNL_CB_OK. */
int nl_dump(struct nlmsghdr *nlh, mnl_cb_t cb, void *data);

#endif
