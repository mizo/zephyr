/** @file
 * @brief IPv6 Fragment related functions
 */

/*
 * Copyright (c) 2018 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <logging/log.h>
LOG_MODULE_DECLARE(net_ipv6, CONFIG_NET_IPV6_LOG_LEVEL);

#include <errno.h>
#include <net/net_core.h>
#include <net/net_pkt.h>
#include <net/net_stats.h>
#include <net/net_context.h>
#include <net/net_mgmt.h>
#include <net/tcp.h>
#include "net_private.h"
#include "connection.h"
#include "icmpv6.h"
#include "udp_internal.h"
#include "tcp_internal.h"
#include "ipv6.h"
#include "nbr.h"
#include "6lo.h"
#include "route.h"
#include "net_stats.h"

/* Timeout for various buffer allocations in this file. */
#define NET_BUF_TIMEOUT K_MSEC(50)

#if defined(CONFIG_NET_IPV6_FRAGMENT_TIMEOUT)
#define IPV6_REASSEMBLY_TIMEOUT K_SECONDS(CONFIG_NET_IPV6_FRAGMENT_TIMEOUT)
#else
#define IPV6_REASSEMBLY_TIMEOUT K_SECONDS(5)
#endif /* CONFIG_NET_IPV6_FRAGMENT_TIMEOUT */

#define FRAG_BUF_WAIT K_MSEC(10) /* how long to max wait for a buffer */

static void reassembly_timeout(struct k_work *work);
static bool reassembly_init_done;

static struct net_ipv6_reassembly
reassembly[CONFIG_NET_IPV6_FRAGMENT_MAX_COUNT];

int net_ipv6_find_last_ext_hdr(struct net_pkt *pkt, u16_t *next_hdr_off,
			       u16_t *last_hdr_off)
{
	NET_PKT_DATA_ACCESS_CONTIGUOUS_DEFINE(ipv6_access, struct net_ipv6_hdr);
	struct net_ipv6_hdr *hdr;
	u8_t next_nexthdr;
	u8_t nexthdr;
	u16_t length;

	if (!pkt || !pkt->frags || !next_hdr_off || !last_hdr_off) {
		return -EINVAL;
	}

	net_pkt_cursor_init(pkt);

	hdr = (struct net_ipv6_hdr *)net_pkt_get_data_new(pkt, &ipv6_access);
	if (!hdr) {
		return -ENOBUFS;
	}

	net_pkt_acknowledge_data(pkt, &ipv6_access);

	nexthdr = hdr->nexthdr;

	/* Initial values */
	*next_hdr_off = offsetof(struct net_ipv6_hdr, nexthdr);
	*last_hdr_off = sizeof(struct net_ipv6_hdr);

	nexthdr = hdr->nexthdr;
	while (!net_ipv6_is_nexthdr_upper_layer(nexthdr)) {
		if (net_pkt_read_u8_new(pkt, &next_nexthdr)) {
			goto fail;
		}

		switch (nexthdr) {
		case NET_IPV6_NEXTHDR_HBHO:
		case NET_IPV6_NEXTHDR_DESTO:
			length = 0U;

			if (net_pkt_read_u8_new(pkt, (u8_t *)&length)) {
				goto fail;
			}

			length = length * 8 + 8 - 2;

			if (net_pkt_skip(pkt, length)) {
				goto fail;
			}

			break;
		case NET_IPV6_NEXTHDR_FRAG:
			if (net_pkt_skip(pkt, 7)) {
				goto fail;
			}

			break;
		case NET_IPV6_NEXTHDR_NONE:
			goto out;
		default:
			/* TODO: Add more IPv6 extension headers to check */
			goto fail;
		}

		*next_hdr_off = *last_hdr_off;
		*last_hdr_off = net_pkt_get_current_offset(pkt);

		nexthdr = next_nexthdr;
	}
out:
	return 0;
fail:
	return -EINVAL;
}

static struct net_ipv6_reassembly *reassembly_get(u32_t id,
						  struct in6_addr *src,
						  struct in6_addr *dst)
{
	int i, avail = -1;

	for (i = 0; i < CONFIG_NET_IPV6_FRAGMENT_MAX_COUNT; i++) {

		if (k_delayed_work_remaining_get(&reassembly[i].timer) &&
		    reassembly[i].id == id &&
		    net_ipv6_addr_cmp(src, &reassembly[i].src) &&
		    net_ipv6_addr_cmp(dst, &reassembly[i].dst)) {
			return &reassembly[i];
		}

		if (k_delayed_work_remaining_get(&reassembly[i].timer)) {
			continue;
		}

		if (avail < 0) {
			avail = i;
		}
	}

	if (avail < 0) {
		return NULL;
	}

	k_delayed_work_submit(&reassembly[avail].timer,
			      IPV6_REASSEMBLY_TIMEOUT);

	net_ipaddr_copy(&reassembly[avail].src, src);
	net_ipaddr_copy(&reassembly[avail].dst, dst);

	reassembly[avail].id = id;

	return &reassembly[avail];
}

static bool reassembly_cancel(u32_t id,
			      struct in6_addr *src,
			      struct in6_addr *dst)
{
	int i, j;

	NET_DBG("Cancel 0x%x", id);

	for (i = 0; i < CONFIG_NET_IPV6_FRAGMENT_MAX_COUNT; i++) {
		s32_t remaining;

		if (reassembly[i].id != id ||
		    !net_ipv6_addr_cmp(src, &reassembly[i].src) ||
		    !net_ipv6_addr_cmp(dst, &reassembly[i].dst)) {
			continue;
		}

		remaining = k_delayed_work_remaining_get(&reassembly[i].timer);
		if (remaining) {
			k_delayed_work_cancel(&reassembly[i].timer);
		}

		NET_DBG("IPv6 reassembly id 0x%x remaining %d ms",
			reassembly[i].id, remaining);

		reassembly[i].id = 0U;

		for (j = 0; j < NET_IPV6_FRAGMENTS_MAX_PKT; j++) {
			if (!reassembly[i].pkt[j]) {
				continue;
			}

			NET_DBG("[%d] IPv6 reassembly pkt %p %zd bytes data",
				j, reassembly[i].pkt[j],
				net_pkt_get_len(reassembly[i].pkt[j]));

			net_pkt_unref(reassembly[i].pkt[j]);
			reassembly[i].pkt[j] = NULL;
		}

		return true;
	}

	return false;
}

static void reassembly_info(char *str, struct net_ipv6_reassembly *reass)
{
	int i, len;

	for (i = 0, len = 0; i < NET_IPV6_FRAGMENTS_MAX_PKT; i++) {
		if (reass->pkt[i]) {
			len += net_pkt_get_len(reass->pkt[i]);
		}
	}

	NET_DBG("%s id 0x%x src %s dst %s remain %d ms len %d", str, reass->id,
		log_strdup(net_sprint_ipv6_addr(&reass->src)),
		log_strdup(net_sprint_ipv6_addr(&reass->dst)),
		k_delayed_work_remaining_get(&reass->timer), len);
}

static void reassembly_timeout(struct k_work *work)
{
	struct net_ipv6_reassembly *reass =
		CONTAINER_OF(work, struct net_ipv6_reassembly, timer);

	reassembly_info("Reassembly cancelled", reass);

	reassembly_cancel(reass->id, &reass->src, &reass->dst);
}

static void reassemble_packet(struct net_ipv6_reassembly *reass)
{
	NET_PKT_DATA_ACCESS_CONTIGUOUS_DEFINE(ipv6_access, struct net_ipv6_hdr);
	NET_PKT_DATA_ACCESS_DEFINE(frag_access, struct net_ipv6_frag_hdr);
	union {
		struct net_ipv6_hdr *hdr;
		struct net_ipv6_frag_hdr *frag_hdr;
	} ipv6;

	struct net_pkt *pkt;
	struct net_buf *last;
	u8_t next_hdr;
	int i, len;

	k_delayed_work_cancel(&reass->timer);

	NET_ASSERT(reass->pkt[0]);

	last = net_buf_frag_last(reass->pkt[0]->buffer);

	/* We start from 2nd packet which is then appended to
	 * the first one.
	 */
	for (i = 1; i < NET_IPV6_FRAGMENTS_MAX_PKT; i++) {
		int removed_len;

		pkt = reass->pkt[i];

		/* Get rid of IPv6 and fragment header which are at
		 * the beginning of the fragment.
		 */
		removed_len = net_pkt_ipv6_fragment_start(pkt) +
			      sizeof(struct net_ipv6_frag_hdr);

		NET_DBG("Removing %d bytes from start of pkt %p",
			removed_len, pkt->buffer);

		if (net_pkt_pull_new(pkt, removed_len)) {
			NET_ERR("Failed to pull headers");
			reassembly_cancel(reass->id, &reass->src, &reass->dst);
			return;
		}

		/* Attach the data to previous pkt */
		last->frags = pkt->buffer;
		last = net_buf_frag_last(pkt->buffer);

		pkt->buffer = NULL;
		reass->pkt[i] = NULL;

		net_pkt_unref(pkt);
	}

	pkt = reass->pkt[0];
	reass->pkt[0] = NULL;

	/* Next we need to strip away the fragment header from the first packet
	 * and set the various pointers and values in packet.
	 */
	net_pkt_cursor_init(pkt);

	if (net_pkt_skip(pkt, net_pkt_ipv6_fragment_start(pkt))) {
		NET_ERR("Failed to move to fragment header");
		goto error;
	}

	ipv6.frag_hdr = (struct net_ipv6_frag_hdr *)net_pkt_get_data_new(
							pkt, &frag_access);
	if (!ipv6.frag_hdr) {
		NET_ERR("Failed to get fragment header");
		goto error;
	}

	next_hdr = ipv6.frag_hdr->nexthdr;

	if (net_pkt_pull_new(pkt, sizeof(struct net_ipv6_frag_hdr))) {
		NET_ERR("Failed to remove fragment header");
		goto error;
	}

	net_pkt_cursor_init(pkt);

	/* This one updates the previous header's nexthdr value */
	if (net_pkt_skip(pkt, net_pkt_ipv6_hdr_prev(pkt)) ||
	    net_pkt_write_u8_new(pkt, next_hdr)) {
		goto error;
	}

	net_pkt_cursor_init(pkt);

	ipv6.hdr = (struct net_ipv6_hdr *)net_pkt_get_data_new(pkt,
							       &ipv6_access);
	if (!ipv6.hdr) {
		goto error;
	}


	/* Fix the total length of the IPv6 packet. */
	len = net_pkt_ipv6_ext_len(pkt);
	if (len > 0) {
		NET_DBG("Old pkt %p IPv6 ext len is %d bytes", pkt, len);
		net_pkt_set_ipv6_ext_len(pkt,
				len - sizeof(struct net_ipv6_frag_hdr));
	}

	len = net_pkt_get_len(pkt) - sizeof(struct net_ipv6_hdr);

	ipv6.hdr->len = htons(len);

	net_pkt_set_data(pkt, &ipv6_access);

	NET_DBG("New pkt %p IPv6 len is %d bytes", pkt, len);

	/* We need to use the queue when feeding the packet back into the
	 * IP stack as we might run out of stack if we call processing_data()
	 * directly. As the packet does not contain link layer header, we
	 * MUST NOT pass it to L2 so there will be a special check for that
	 * in process_data() when handling the packet.
	 */
	if (net_recv_data(net_pkt_iface(pkt), pkt) >= 0) {
		return;
	}
error:
	net_pkt_unref(pkt);
}

void net_ipv6_frag_foreach(net_ipv6_frag_cb_t cb, void *user_data)
{
	int i;

	for (i = 0; reassembly_init_done &&
		     i < CONFIG_NET_IPV6_FRAGMENT_MAX_COUNT; i++) {
		if (!k_delayed_work_remaining_get(&reassembly[i].timer)) {
			continue;
		}

		cb(&reassembly[i], user_data);
	}
}

/* Verify that we have all the fragments received and in correct order.
 */
static bool fragment_verify(struct net_ipv6_reassembly *reass)
{
	u16_t offset;
	int i, prev_len;

	prev_len = net_pkt_get_len(reass->pkt[0]);
	offset = net_pkt_ipv6_fragment_offset(reass->pkt[0]);

	NET_DBG("pkt %p offset %u", reass->pkt[0], offset);

	if (offset != 0) {
		return false;
	}

	for (i = 1; i < NET_IPV6_FRAGMENTS_MAX_PKT; i++) {
		offset = net_pkt_ipv6_fragment_offset(reass->pkt[i]);

		NET_DBG("pkt %p offset %u prev_len %d", reass->pkt[i],
			offset, prev_len);

		if (prev_len < offset) {
			/* Something wrong with the offset value */
			return false;
		}

		prev_len = net_pkt_get_len(reass->pkt[i]);
	}

	return true;
}

static int shift_packets(struct net_ipv6_reassembly *reass, int pos)
{
	int i;

	for (i = pos + 1; i < NET_IPV6_FRAGMENTS_MAX_PKT; i++) {
		if (!reass->pkt[i]) {
			NET_DBG("Moving [%d] %p (offset 0x%x) to [%d]",
				pos, reass->pkt[pos],
				net_pkt_ipv6_fragment_offset(reass->pkt[pos]),
				i);

			/* Do we have enough space in packet array to make
			 * the move?
			 */
			if (((i - pos) + 1) >
			    (NET_IPV6_FRAGMENTS_MAX_PKT - i)) {
				return -ENOMEM;
			}

			memmove(&reass->pkt[i], &reass->pkt[pos],
				sizeof(void *) * (i - pos));

			return 0;
		}
	}

	return -EINVAL;
}

enum net_verdict net_ipv6_handle_fragment_hdr(struct net_pkt *pkt,
					      struct net_ipv6_hdr *hdr,
					      u8_t nexthdr)
{
	struct net_ipv6_reassembly *reass = NULL;
	u16_t flag;
	bool found;
	u8_t more;
	u32_t id;
	int i;

	if (!reassembly_init_done) {
		/* Static initializing does not work here because of the array
		 * so we must do it at runtime.
		 */
		for (i = 0; i < CONFIG_NET_IPV6_FRAGMENT_MAX_COUNT; i++) {
			k_delayed_work_init(&reassembly[i].timer,
					    reassembly_timeout);
		}

		reassembly_init_done = true;
	}

	/* Each fragment has a fragment header, however since we already
	 * read the nexthdr part of it, we are not going to use
	 * net_pkt_get_data_new() and access the header directly: the cursor
	 * being 1 byte too far, let's just read the next relevant pieces.
	 */
	if (net_pkt_skip(pkt, 1) || /* reserved */
	    net_pkt_read_be16_new(pkt, &flag) ||
	    net_pkt_read_be32_new(pkt, &id)) {
		goto drop;
	}

	reass = reassembly_get(id, &hdr->src, &hdr->dst);
	if (!reass) {
		NET_DBG("Cannot get reassembly slot, dropping pkt %p", pkt);
		goto drop;
	}

	more = flag & 0x01;
	net_pkt_set_ipv6_fragment_offset(pkt, flag & 0xfff8);

	if (!reass->pkt[0]) {
		NET_DBG("Storing pkt %p to slot %d offset 0x%x",
			pkt, 0, net_pkt_ipv6_fragment_offset(pkt));
		reass->pkt[0] = pkt;

		reassembly_info("Reassembly 1st pkt", reass);

		/* Wait for more fragments to receive. */
		goto accept;
	}

	/* The fragments might come in wrong order so place them
	 * in reassembly chain in correct order.
	 */
	for (i = 0, found = false; i < NET_IPV6_FRAGMENTS_MAX_PKT; i++) {
		if (reass->pkt[i]) {
			if (net_pkt_ipv6_fragment_offset(reass->pkt[i]) <
			    net_pkt_ipv6_fragment_offset(pkt)) {
				continue;
			}

			/* Make room for this fragment. If there is no room,
			 * then it will discard the whole reassembly.
			 */
			if (shift_packets(reass, i)) {
				break;
			}
		}

		NET_DBG("Storing pkt %p to slot %d offset 0x%x",
			pkt, i, net_pkt_ipv6_fragment_offset(pkt));
		reass->pkt[i] = pkt;
		found = true;

		break;
	}

	if (!found) {
		/* We could not add this fragment into our saved fragment
		 * list. We must discard the whole packet at this point.
		 */
		NET_DBG("No slots available for 0x%x", reass->id);
		net_pkt_unref(pkt);
		goto drop;
	}

	if (more) {
		if (net_pkt_get_len(pkt) % 8) {
			/* Fragment length is not multiple of 8, discard
			 * the packet and send parameter problem error.
			 */
			net_icmpv6_send_error(pkt, NET_ICMPV6_PARAM_PROBLEM,
					      NET_ICMPV6_PARAM_PROB_OPTION, 0);
			goto drop;
		}

		reassembly_info("Reassembly nth pkt", reass);

		NET_DBG("More fragments to be received");
		goto accept;
	}

	reassembly_info("Reassembly last pkt", reass);

	if (!fragment_verify(reass)) {
		NET_DBG("Reassembled IPv6 verify failed, dropping id %u",
			reass->id);

		/* Let the caller release the already inserted pkt */
		if (i < NET_IPV6_FRAGMENTS_MAX_PKT) {
			reass->pkt[i] = NULL;
		}

		net_pkt_unref(pkt);
		goto drop;
	}

	/* The last fragment received, reassemble the packet */
	reassemble_packet(reass);

accept:
	return NET_OK;

drop:
	if (reass) {
		if (reassembly_cancel(reass->id, &reass->src, &reass->dst)) {
			return NET_OK;
		}
	}

	return NET_DROP;
}

#define BUF_ALLOC_TIMEOUT K_MSEC(100)

static int send_ipv6_fragment(struct net_pkt *pkt,
			      u16_t fit_len,
			      u16_t frag_offset,
			      u16_t next_hdr_off,
			      u8_t next_hdr,
			      bool final)
{
	NET_PKT_DATA_ACCESS_DEFINE(frag_access, struct net_ipv6_frag_hdr);
	int ret = -ENOBUFS;
	struct net_ipv6_frag_hdr *frag_hdr;
	struct net_pkt *frag_pkt;

	frag_pkt = net_pkt_alloc_with_buffer(net_pkt_iface(pkt), fit_len +
					     net_pkt_ipv6_ext_len(pkt) +
					     NET_IPV6_FRAGH_LEN,
					     AF_INET6, 0, BUF_ALLOC_TIMEOUT);
	if (!frag_pkt) {
		return -ENOMEM;
	}

	net_pkt_cursor_init(pkt);

	/* We copy original headers back to the fragment packet
	 * Note that we insert the right next header to point to fragment header
	 */
	if (net_pkt_copy_new(frag_pkt, pkt, next_hdr_off) ||
	    net_pkt_write_u8_new(frag_pkt, NET_IPV6_NEXTHDR_FRAG) ||
	    net_pkt_skip(pkt, 1) ||
	    net_pkt_copy_new(frag_pkt, pkt, net_pkt_ip_hdr_len(pkt) +
			     net_pkt_ipv6_ext_len(pkt) - next_hdr_off - 1)) {
		goto fail;
	}

	/* And we append the fragmentation header */
	frag_hdr = (struct net_ipv6_frag_hdr *)net_pkt_get_data_new(
						frag_pkt, &frag_access);
	if (!frag_hdr) {
		goto fail;
	}

	frag_hdr->nexthdr = next_hdr;
	frag_hdr->reserved = 0;
	frag_hdr->id = net_pkt_ipv6_fragment_id(pkt);
	frag_hdr->offset = htons(((frag_offset / 8) << 3) | !final);

	if (net_pkt_set_data(frag_pkt, &frag_access)) {
		goto fail;
	}

	net_pkt_set_ipv6_ext_len(frag_pkt,
				 net_pkt_ipv6_ext_len(pkt) +
				 sizeof(struct net_ipv6_frag_hdr));

	/* Finally we copy the payload part of this fragment from
	 * the original packet
	 */
	if (net_pkt_skip(pkt, frag_offset) ||
	    net_pkt_copy_new(frag_pkt, pkt, fit_len)) {
		goto fail;
	}

	net_pkt_cursor_init(frag_pkt);

	if (net_ipv6_finalize_new(frag_pkt, 0) < 0) {
		goto fail;
	}

	/* If everything has been ok so far, we can send the packet. */
	ret = net_send_data(frag_pkt);
	if (ret < 0) {
		goto fail;
	}

	/* Let this packet to be sent and hopefully it will release
	 * the memory that can be utilized for next sent IPv6 fragment.
	 */
	k_yield();

	return 0;

fail:
	NET_DBG("Cannot send fragment (%d)", ret);
	net_pkt_unref(frag_pkt);

	return ret;
}

int net_ipv6_send_fragmented_pkt(struct net_if *iface, struct net_pkt *pkt,
				 u16_t pkt_len)
{
	u16_t next_hdr_off;
	u16_t last_hdr_off;
	u16_t frag_offset;
	size_t length;
	u8_t next_hdr;
	u8_t last_hdr;
	int fit_len;
	int ret;

	net_pkt_set_ipv6_fragment_id(pkt, sys_rand32_get());

	ret = net_ipv6_find_last_ext_hdr(pkt, &next_hdr_off, &last_hdr_off);
	if (ret < 0) {
		return ret;
	}

	net_pkt_cursor_init(pkt);

	if (net_pkt_skip(pkt, next_hdr_off) ||
	    net_pkt_read_u8_new(pkt, &next_hdr) ||
	    net_pkt_skip(pkt, last_hdr_off) ||
	    net_pkt_read_u8_new(pkt, &last_hdr)) {
		return -ENOBUFS;
	}

	/* The Maximum payload can fit into each packet after IPv6 header,
	 * Extenstion headers and Fragmentation header.
	 */
	fit_len = NET_IPV6_MTU - NET_IPV6_FRAGH_LEN -
		(net_pkt_ip_hdr_len(pkt) + net_pkt_ipv6_ext_len(pkt));
	if (fit_len <= 0) {
		/* Must be invalid extension headers length */
		NET_DBG("No room for IPv6 payload MTU %d hdrs_len %d",
			NET_IPV6_MTU, NET_IPV6_FRAGH_LEN +
			net_pkt_ip_hdr_len(pkt) + net_pkt_ipv6_ext_len(pkt));
		return -EINVAL;
	}

	frag_offset = 0U;

	length = net_pkt_get_len(pkt) -
		(net_pkt_ip_hdr_len(pkt) + net_pkt_ipv6_ext_len(pkt));
	while (length) {
		bool final = false;

		if (fit_len >= length) {
			final = true;
			fit_len = length;
		}

		ret = send_ipv6_fragment(pkt, fit_len, frag_offset,
					 next_hdr_off, next_hdr, final);
		if (ret < 0) {
			return ret;
		}

		length -= fit_len;
		frag_offset += fit_len;
	}

	return 0;
}
