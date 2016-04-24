/***************************************************************
 *
 * (C) 2011-15 Nicola Bonelli <nicola@pfq.io>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 ****************************************************************/

#ifndef PFQ_LANG_PREDICATE_H
#define PFQ_LANG_PREDICATE_H

#include <pragma/diagnostic_push>

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/udp.h>
#include <linux/tcp.h>
#include <linux/icmp.h>
#include <linux/if_vlan.h>
#include <net/ip.h>

#include <pragma/diagnostic_pop>

#include <lang/module.h>


static inline bool
less(arguments_t args, SkBuff skb)
{
	property_t p = GET_ARG_0(property_t, args);
	const uint64_t data = GET_ARG_1(uint64_t, args);

	uint64_t ret = EVAL_PROPERTY(p, skb);

	if (IS_JUST(ret))
		return FROM_JUST(uint64_t, ret) < data;

	return false;
}

static inline bool
less_eq(arguments_t args, SkBuff skb)
{
	property_t p = GET_ARG_0(property_t, args);
	const uint64_t data = GET_ARG_1(uint64_t, args);

	uint64_t ret = EVAL_PROPERTY(p, skb);

	if (IS_JUST(ret))
		return FROM_JUST(uint64_t, ret) <= data;

	return false;
}

static inline bool
greater(arguments_t args, SkBuff skb)
{
	property_t p = GET_ARG_0(property_t, args);
	const uint64_t data = GET_ARG_1(uint64_t, args);

	uint64_t ret = EVAL_PROPERTY(p, skb);

	if (IS_JUST(ret))
		return FROM_JUST(uint64_t, ret) > data;

	return false;
}

static inline bool
greater_eq(arguments_t args, SkBuff skb)
{
	property_t p = GET_ARG_0(property_t, args);
	const uint64_t data = GET_ARG_1(uint64_t, args);

	uint64_t ret = EVAL_PROPERTY(p, skb);

	if (IS_JUST(ret))
		return FROM_JUST(uint64_t, ret) >= data;

	return false;
}

static inline bool
equal(arguments_t args, SkBuff skb)
{
	property_t p = GET_ARG_0(property_t, args);
	const uint64_t data = GET_ARG_1(uint64_t, args);

	uint64_t ret = EVAL_PROPERTY(p, skb);

	if (IS_JUST(ret))
		return FROM_JUST(uint64_t, ret) == data;

	return false;
}

static inline bool
not_equal(arguments_t args, SkBuff skb)
{
	property_t p = GET_ARG_0(property_t, args);
	const uint64_t data = GET_ARG_1(uint64_t, args);

	uint64_t ret = EVAL_PROPERTY(p, skb);

	if (IS_JUST(ret))
		return FROM_JUST(uint64_t, ret) != data;

	return false;
}

static inline bool
any_bit(arguments_t args, SkBuff skb)
{
	property_t p = GET_ARG_0(property_t, args);
	const uint64_t data = GET_ARG_1(uint64_t, args);

	uint64_t ret = EVAL_PROPERTY(p, skb);

	if (IS_JUST(ret))
		return (FROM_JUST(uint64_t, ret) & data) != 0;

	return false;
}

static inline bool
all_bit(arguments_t args, SkBuff skb)
{
	property_t p = GET_ARG_0(property_t, args);
	const uint64_t data = GET_ARG_1(uint64_t, args);

	uint64_t ret = EVAL_PROPERTY(p, skb);

	if (IS_JUST(ret))
		return (FROM_JUST(uint64_t, ret) & data) == data;

	return false;
}

/* basic predicates ... */

static inline bool
skb_header_available(struct sk_buff *skb, int offset, int len)
{
        if (skb->len - offset >= len)
                return true;
        return false;
}


static inline bool
is_ip(SkBuff skb)
{
	if (eth_hdr(PFQ_SKB(skb))->h_proto == __constant_htons(ETH_P_IP))
	        return skb_header_available(PFQ_SKB(skb), skb->mac_len, sizeof(struct iphdr));

        return false;
}

static inline bool
is_ip6(SkBuff skb)
{
	if (eth_hdr(PFQ_SKB(skb))->h_proto == __constant_htons(ETH_P_IPV6))
                return skb_header_available(PFQ_SKB(skb), skb->mac_len, sizeof(struct ipv6hdr));

        return false;
}

static inline bool
is_udp(SkBuff skb)
{
	if (eth_hdr(PFQ_SKB(skb))->h_proto == __constant_htons(ETH_P_IP))
	{
		struct iphdr _iph;
		const struct iphdr *ip;

		ip = skb_header_pointer(PFQ_SKB(skb), skb->mac_len, sizeof(_iph), &_iph);
		if (ip == NULL)
                        return false;

		if (ip->protocol != IPPROTO_UDP)
                        return false;

                return skb_header_available(PFQ_SKB(skb), skb->mac_len  + (ip->ihl<<2), sizeof(struct udphdr));
	}

        return false;
}


static inline bool
is_tcp(SkBuff skb)
{
	if (eth_hdr(PFQ_SKB(skb))->h_proto == __constant_htons(ETH_P_IP))
	{
		struct iphdr _iph;
		const struct iphdr *ip;

		ip = skb_header_pointer(PFQ_SKB(skb), skb->mac_len, sizeof(_iph), &_iph);
		if (ip == NULL)
                        return false;

		if (ip->protocol != IPPROTO_TCP)
                        return false;

		return skb_header_available(PFQ_SKB(skb), skb->mac_len + (ip->ihl<<2), sizeof(struct tcphdr));
	}

        return false;
}


static inline bool
is_icmp(SkBuff skb)
{
	if (eth_hdr(PFQ_SKB(skb))->h_proto == __constant_htons(ETH_P_IP))
	{
		struct iphdr _iph;
		const struct iphdr *ip;

		ip = skb_header_pointer(PFQ_SKB(skb), skb->mac_len, sizeof(_iph), &_iph);
		if (ip == NULL)
                        return false;

		if (ip->protocol != IPPROTO_ICMP)
                        return false;

		return skb_header_available(PFQ_SKB(skb), skb->mac_len + (ip->ihl<<2), sizeof(struct icmphdr));
	}

        return false;
}


static inline bool
has_addr(SkBuff skb, __be32 addr, __be32 mask)
{
	if (eth_hdr(PFQ_SKB(skb))->h_proto == __constant_htons(ETH_P_IP))
	{
		struct iphdr _iph;
		const struct iphdr *ip;

		ip = skb_header_pointer(PFQ_SKB(skb), skb->mac_len, sizeof(_iph), &_iph);
		if (ip == NULL)
			return false;

		if ((ip->saddr & mask) == (addr & mask) ||
		    (ip->daddr & mask) == (addr & mask))
			return true;
	}

        return false;
}


static inline bool
has_src_addr(SkBuff skb, __be32 addr, __be32 mask)
{
	if (eth_hdr(PFQ_SKB(skb))->h_proto == __constant_htons(ETH_P_IP))
	{
		struct iphdr _iph;
		const struct iphdr *ip;

		ip = skb_header_pointer(PFQ_SKB(skb), skb->mac_len, sizeof(_iph), &_iph);
		if (ip == NULL)
			return false;

		if ((ip->saddr & mask) == (addr & mask))
			return true;
	}

        return false;
}

static inline bool
has_dst_addr(SkBuff skb, __be32 addr, __be32 mask)
{
	if (eth_hdr(PFQ_SKB(skb))->h_proto == __constant_htons(ETH_P_IP))
	{
		struct iphdr _iph;
		const struct iphdr *ip;

		ip = skb_header_pointer(PFQ_SKB(skb), skb->mac_len, sizeof(_iph), &_iph);
		if (ip == NULL)
			return false;

		if ((ip->daddr & mask) == (addr & mask))
			return true;
	}

        return false;
}


static inline bool
is_flow(SkBuff skb)
{
	if (eth_hdr(PFQ_SKB(skb))->h_proto == __constant_htons(ETH_P_IP))
	{
		struct iphdr _iph;
		const struct iphdr *ip;

		ip = skb_header_pointer(PFQ_SKB(skb), skb->mac_len, sizeof(_iph), &_iph);
		if (ip == NULL)
			return false;

		if (ip->protocol != IPPROTO_UDP &&
		    ip->protocol != IPPROTO_TCP)
                        return false;

		return skb_header_available(PFQ_SKB(skb), skb->mac_len + (ip->ihl<<2), ip->protocol == IPPROTO_UDP ?
					    sizeof(struct udphdr) : sizeof(struct tcphdr));
	}

        return false;
}


static inline bool
is_l3_proto(SkBuff skb, u16 type)
{
	return eth_hdr(PFQ_SKB(skb))->h_proto == __constant_htons(type);
}


static inline bool
is_l4_proto(SkBuff skb, u8 protocol)
{
	if (eth_hdr(PFQ_SKB(skb))->h_proto == __constant_htons(ETH_P_IP))
	{
		struct iphdr _iph;
		const struct iphdr *ip;

		ip = skb_header_pointer(PFQ_SKB(skb), skb->mac_len, sizeof(_iph), &_iph);
		if (ip == NULL)
			return false;

                return ip->protocol == protocol;
	}

	return false;
}


static inline bool
is_frag(SkBuff skb)
{
	if (eth_hdr(PFQ_SKB(skb))->h_proto == __constant_htons(ETH_P_IP))
	{
		struct iphdr _iph;
		const struct iphdr *ip;

		ip = skb_header_pointer(PFQ_SKB(skb), skb->mac_len, sizeof(_iph), &_iph);
		if (ip == NULL)
			return false;

                return (ip->frag_off & __constant_htons(IP_MF|IP_OFFSET)) != 0;
	}

	return false;
}

static inline bool
is_first_frag(SkBuff skb)
{
	if (eth_hdr(PFQ_SKB(skb))->h_proto == __constant_htons(ETH_P_IP))
	{
		struct iphdr _iph;
		const struct iphdr *ip;

		ip = skb_header_pointer(PFQ_SKB(skb), skb->mac_len, sizeof(_iph), &_iph);
		if (ip == NULL)
			return false;

                return (ip->frag_off & __constant_htons(IP_MF|IP_OFFSET)) == __constant_htons(IP_MF);
	}

	return false;
}

static inline bool
is_more_frag(SkBuff skb)
{
	if (eth_hdr(PFQ_SKB(skb))->h_proto == __constant_htons(ETH_P_IP))
	{
		struct iphdr _iph;
		const struct iphdr *ip;

		ip = skb_header_pointer(PFQ_SKB(skb), skb->mac_len, sizeof(_iph), &_iph);
		if (ip == NULL)
			return false;

		return (ip->frag_off & __constant_htons(IP_OFFSET)) != 0;
	}

	return false;
}

static inline bool
has_src_port(SkBuff skb, uint16_t port)
{
	if (eth_hdr(PFQ_SKB(skb))->h_proto == __constant_htons(ETH_P_IP))
	{
		struct iphdr _iph;
		const struct iphdr *ip;

		ip = skb_header_pointer(PFQ_SKB(skb), skb->mac_len, sizeof(_iph), &_iph);
		if (ip == NULL)
			return false;

		switch(ip->protocol)
		{
		case IPPROTO_UDP: {
			struct udphdr _udph; const struct udphdr *udp;
			udp = skb_header_pointer(PFQ_SKB(skb), skb->mac_len + (ip->ihl<<2), sizeof(struct udphdr), &_udph);
			if (udp == NULL)
				return false;

			return udp->source == cpu_to_be16(port);
		}
		case IPPROTO_TCP: {
			struct tcphdr _tcph; const struct tcphdr *tcp;
			tcp = skb_header_pointer(PFQ_SKB(skb), skb->mac_len + (ip->ihl<<2), sizeof(struct tcphdr), &_tcph);
			if (tcp == NULL)
				return false;

			return tcp->source == cpu_to_be16(port);
		}

		default:
			return false;
		}
	}

	return false;
}

static inline bool
has_dst_port(SkBuff skb, uint16_t port)
{
	if (eth_hdr(PFQ_SKB(skb))->h_proto == __constant_htons(ETH_P_IP))
	{
		struct iphdr _iph;
		const struct iphdr *ip;

		ip = skb_header_pointer(PFQ_SKB(skb), skb->mac_len, sizeof(_iph), &_iph);
		if (ip == NULL)
			return false;

		switch(ip->protocol)
		{
		case IPPROTO_UDP: {
			struct udphdr _udph; const struct udphdr *udp;
			udp = skb_header_pointer(PFQ_SKB(skb), skb->mac_len + (ip->ihl<<2), sizeof(struct udphdr), &_udph);
			if (udp == NULL)
				return false;

			return udp->dest == cpu_to_be16(port);
		}
		case IPPROTO_TCP: {
			struct tcphdr _tcph; const struct tcphdr *tcp;
			tcp = skb_header_pointer(PFQ_SKB(skb), skb->mac_len + (ip->ihl<<2), sizeof(struct tcphdr), &_tcph);
			if (tcp == NULL)
				return false;

			return tcp->dest == cpu_to_be16(port);
		}

		default:
			return false;
		}
	}

	return false;
}


static inline bool
has_port(SkBuff skb, uint16_t port)
{
	return has_src_port(skb, port) || has_dst_port(skb, port);
}


static inline bool
has_vlan(SkBuff skb)
{
	return (skb->vlan_tci & VLAN_VID_MASK);
}

static inline bool
has_vid(SkBuff skb, int vid)
{
	return (skb->vlan_tci & VLAN_VID_MASK) == vid;
}


#endif /* PFQ_LANG_PREDICATE_H */
