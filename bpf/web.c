/*
 * Gatekeeper - DoS protection system.
 * Copyright (C) 2016 Digirati LTDA.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * This BPF program is intended as an example for a simple web server
 * that runs the services HTTP, HTTPS, SSH, and FTP.
 *
 * This BPF program builds upon the BPF program grantedv2, so
 * there are primary and secondary limits. The secondary limit is only used
 * for fragmented packets.
 */

#include <net/ethernet.h>
#include <netinet/ip_icmp.h>
#include <netinet/icmp6.h>
#include <netinet/tcp.h>

#include "grantedv2.h"

#ifdef ntohs
#undef ntohs
#endif
#define ntohs(x) __builtin_bswap16(x)

SEC("init") uint64_t
web_init(struct gk_bpf_init_ctx *ctx)
{
	return grantedv2_init_inline(ctx);
}

SEC("pkt") uint64_t
web_pkt(struct gk_bpf_pkt_ctx *ctx)
{
	struct grantedv2_state *state =
		(struct grantedv2_state *)pkt_ctx_to_cookie(ctx);
	struct rte_mbuf *pkt = pkt_ctx_to_pkt(ctx);
	uint32_t pkt_len = pkt->pkt_len;
	struct tcphdr *tcp_hdr;
	uint64_t ret = grantedv2_pkt_begin(ctx, state, pkt_len);

	if (ret != GK_BPF_PKT_RET_FORWARD) {
		/* Primary budget exceeded. */
		return ret;
	}

	/* Allowed L4 protocols. */
	switch (ctx->l4_proto) {
	case IPPROTO_ICMP: {
		struct icmphdr *icmp_hdr;

		if (ctx->l3_proto != ETHERTYPE_IP) {
			/* ICMP must be on top of IPv4. */
			return GK_BPF_PKT_RET_DECLINE;
		}
		if (ctx->fragmented)
			return GK_BPF_PKT_RET_DECLINE;
		if (pkt->l4_len < sizeof(*icmp_hdr)) {
			/* Malformed ICMP header. */
			return GK_BPF_PKT_RET_DECLINE;
		}
		icmp_hdr = rte_pktmbuf_mtod_offset(pkt,
			struct icmphdr *, pkt->l2_len + pkt->l3_len);
		switch (icmp_hdr->type) {
		case ICMP_ECHOREPLY:
		case ICMP_DEST_UNREACH:
		case ICMP_SOURCE_QUENCH:
		case ICMP_ECHO:
		case ICMP_TIME_EXCEEDED:
			break;
		default:
			return GK_BPF_PKT_RET_DECLINE;
		}
		goto secondary_budget;
	}
	case IPPROTO_ICMPV6: {
		struct icmp6_hdr *icmp6_hdr;

		if (ctx->l3_proto != ETHERTYPE_IPV6) {
			/* ICMPv6 must be on top of IPv6. */
			return GK_BPF_PKT_RET_DECLINE;
		}
		if (ctx->fragmented)
			return GK_BPF_PKT_RET_DECLINE;
		if (pkt->l4_len < sizeof(*icmp6_hdr)) {
			/* Malformed ICMPv6 header. */
			return GK_BPF_PKT_RET_DECLINE;
		}
		icmp6_hdr = rte_pktmbuf_mtod_offset(pkt,
			struct icmp6_hdr *, pkt->l2_len + pkt->l3_len);
		switch (icmp6_hdr->icmp6_type) {
		case ICMP6_DST_UNREACH:
		case ICMP6_PACKET_TOO_BIG:
		case ICMP6_TIME_EXCEEDED:
		case ICMP6_PARAM_PROB:
		case ICMP6_ECHO_REQUEST:
		case ICMP6_ECHO_REPLY:
			break;
		default:
			return GK_BPF_PKT_RET_DECLINE;
		}
		goto secondary_budget;
	}
	case IPPROTO_TCP:
		break;
	default:
		return GK_BPF_PKT_RET_DECLINE;
	}

	/*
	 * Only TCP packets from here on.
	 */

	if (ctx->fragmented)
		goto secondary_budget;
	if (pkt->l4_len < sizeof(*tcp_hdr)) {
		/* Malformed TCP header. */
		return GK_BPF_PKT_RET_DECLINE;
	}
	tcp_hdr = rte_pktmbuf_mtod_offset(pkt, struct tcphdr *,
	       pkt->l2_len + pkt->l3_len);

	/*
	 * For information on active and passive modes of FTP,
	 * refer to the following page:
	 * http://slacksite.com/other/ftp.html
	 */

	/* Listening sockets. */
	switch (ntohs(tcp_hdr->th_dport)) {

	/*
	 * ATTENTION
	 *    These ports must match the one configured in the FTP
	 *    daemon. See the following page for an example:
	 *    http://slacksite.com/other/ftp-appendix1.html
	 */
	case 51000 ... 51999:	/* FTP data (passive mode) */

	case 21:	/* FTP command */
	case 80:	/* HTTP */
	case 443:	/* HTTPS */
	case 22:	/* SSH */
		if (tcp_hdr->syn && tcp_hdr->ack) {
			/* Amplification attack. */
			return GK_BPF_PKT_RET_DECLINE;
		}
		break;

	case 20:	/* FTP data (active mode) */
		/*
		 * Accept connections of the active mode of FTP originated
		 * from our web server.
		 */
		if (tcp_hdr->syn && !tcp_hdr->ack) {
			/* All listening ports were already tested. */
			return GK_BPF_PKT_RET_DECLINE;
		}
		break;

	default:
		/* Accept connections originated from our web server. */

		if (tcp_hdr->syn && !tcp_hdr->ack) {
			/* All listening ports were already tested. */
			return GK_BPF_PKT_RET_DECLINE;
		}

		/* Authorized external services. */
		switch (ntohs(tcp_hdr->th_sport)) {
		case 80:	/* HTTP  */
		case 443:	/* HTTPS */
			break;
		default:
			return GK_BPF_PKT_RET_DECLINE;
		}
		break;
	}

	goto forward;

secondary_budget:
	ret = grantedv2_pkt_test_2nd_limit(state, pkt_len);
	if (ret != GK_BPF_PKT_RET_FORWARD)
		return ret;
forward:
	return grantedv2_pkt_end(ctx, state);
}
