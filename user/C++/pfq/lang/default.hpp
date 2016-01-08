/***************************************************************
 *
 * (C) 2014 Nicola Bonelli <nicola@pfq.io>
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

#pragma once

#include <pfq/lang/lang.hpp>
#include <pfq/lang/util.hpp>
#include <pfq/util.hpp>

#include <functional>
#include <type_traits>
#include <vector>
#include <string>
#include <cmath>

#include <arpa/inet.h>

/*! \file default.hpp
 *  \brief This header contains the pfq-lang eDSL functions.
 */

namespace pfq
{
namespace lang
{
    using namespace std::placeholders;

    //
    // default combinators:
    //

    //! Combine two predicate expressions with a specific boolean 'and' operation.

    template <typename P1, typename P2, typename std::enable_if< is_predicate<P1>::value && is_predicate<P2>::value >::type * = nullptr>
    auto inline operator&(P1 const &p1, P2 const &p2)
    -> decltype(predicate(nullptr, p1, p2))
    {
        return predicate("and", p1, p2);
    }

    //! Combine two predicate expressions with a specific boolean 'or' operation.

    template <typename P1, typename P2, typename std::enable_if< is_predicate<P1>::value && is_predicate<P2>::value >::type * = nullptr>
    auto inline operator|(P1 const &p1, P2 const &p2)
    -> decltype(predicate(nullptr, p1, p2))
    {
        return predicate("or", p1, p2);
    }

    //! Combine two predicate expressions with a specific boolean 'xor' operation.

    template <typename P1, typename P2, typename std::enable_if< is_predicate<P1>::value && is_predicate<P2>::value >::type * = nullptr>
    auto inline operator^(P1 const &p1, P2 const &p2)
    -> decltype(predicate(nullptr, p1, p2))
    {
        return predicate("xor", p1, p2);
    }

    //! Return a new predicate that evaluates to true, when the given one evaluates to false, and vice versa.

    template <typename P>
    auto inline not_(P const &p)
    -> decltype(predicate(nullptr, p))
    {
        return predicate("not", p);
    }

    //
    // default comparators:
    //

    //! Return a predicate that evaluates to \c true, if the property is less than the given value.
    /*!
     * Example:
     *
     * when (ip_ttl < 64,  drop)
     */

    template <typename P, typename std::enable_if<is_property<P>::value>::type * = nullptr>
    auto inline
    operator<(P const &prop, uint64_t arg)
    -> decltype(predicate(nullptr, prop, arg))
    {
        return predicate("less", prop, arg);
    }

    template <typename P, typename std::enable_if<is_property<P>::value>::type * = nullptr>
    auto inline
    operator<=(P const &prop, uint64_t arg)
    -> decltype(predicate(nullptr, prop, arg))
    {
        return predicate("less_eq", prop, arg);
    }

    template <typename P, typename std::enable_if<is_property<P>::value>::type * = nullptr>
    auto inline
    operator>(P const &prop, uint64_t arg)
    -> decltype(predicate(nullptr, prop, arg))
    {
        return predicate("greater", prop, arg);
    }

    template <typename P, typename std::enable_if<is_property<P>::value>::type * = nullptr>
    auto inline
    operator>=(P const &prop, uint64_t arg)
    -> decltype(predicate(nullptr, prop, arg))
    {
        return predicate("greater_eq", prop, arg);
    }

    template <typename P, typename std::enable_if<is_property<P>::value>::type * = nullptr>
    auto inline
    operator==(P const &prop, uint64_t arg)
    -> decltype(predicate(nullptr, prop, arg))
    {
        return predicate("equal", prop, arg);
    }

    template <typename P, typename std::enable_if<is_property<P>::value>::type * = nullptr>
    auto inline
    operator!=(P const &prop, uint64_t arg)
    -> decltype(predicate(nullptr, prop, arg))
    {
        return predicate("not_equal", prop, arg);
    }

    //! Return a predicate that evaluates to \c true, if the property has at least one bit set among those specified by the given mask.

    template <typename P>
    auto inline any_bit(P const &prop, uint64_t mask)
    -> decltype(predicate(nullptr, prop, mask))
    {
        return predicate("any_bit", prop, mask);
    }

    //! Return a predicate that evaluates to \c true, if the property has all bits set among those specified in the given mask.

    template <typename P>
    auto inline all_bit(P const &prop, uint64_t mask)
    -> decltype(predicate(nullptr, prop, mask))
    {
        return predicate("all_bit", prop, mask);
    }

    namespace
    {
        //
        // default predicates:
        //

        //! Evaluate to \c true if the SkBuff is an IPv4 packet.

        auto is_ip          = predicate ("is_ip");

        //! Evaluate to \c true if the SkBuff is an IPv6 packet.

        auto is_ip6         = predicate ("is_ip6");

        //! Evaluate to \c true if the SkBuff is an UDP packet.

        auto is_udp         = predicate ("is_udp");

        //! Evaluate to \c true if the SkBuff is a TCP packet.

        auto is_tcp         = predicate ("is_tcp");

        //! Evaluate to \c true if the SkBuff is an ICMP packet.

        auto is_icmp        = predicate ("is_icmp");

        //! Evaluate to \c true if the SkBuff is an UDP packet, on top of IPv6.

        auto is_udp6        = predicate ("is_udp6");

        //! Evaluate to \c true if the SkBuff is a TCP packet, on top of IPv6.

        auto is_tcp6        = predicate ("is_tcp6");

        //! Evaluate to \c true if the SkBuff is an ICMP packet, on top of IPv6.

        auto is_icmp6       = predicate ("is_icmp6");

        //! Evaluate to \c true if the SkBuff is an UDP or TCP packet.

        auto is_flow        = predicate ("is_flow");

        //! Evaluate to \c true if the SkBuff is a TCP fragment.

        auto is_frag        = predicate ("is_frag");

        //! Evaluate to \c true if the SkBuff is the first TCP fragment.

        auto is_first_frag  = predicate ("is_first_frag");

        //! Evaluate to \c true if the SkBuff is a TCP fragment, but the first.

        auto is_more_frag   = predicate ("is_more_frag");

        //! Evaluate to \c true if the SkBuff has the given Layer3 protocol.

        auto is_l3_proto    = [] (uint16_t type) { return predicate ("is_l3_proto", type); };

        //! Evaluate to \c true if the SkBuff has the given Layer4 protocol.

        auto is_l4_proto    = [] (uint8_t proto) { return predicate ("is_l4_proto", proto); };

        //! Evaluate to \c true if the SkBuff has the given source or destination port.
        /*!
         * If the transport protocol is not present or has no port, the predicate evaluates to False.
         *
         * Example:
         *
         * has_port(80)
         */

        auto has_port       = [] (uint16_t port) { return predicate ("is_port", port); };

        //! Evaluate to \c true if the SkBuff has the given source port.
        /*!
         * If the transport protocol is not present or has no port, the predicate evaluates to False.
         */

        auto has_src_port   = [] (uint16_t port) { return predicate ("is_src_port", port); };

        //! Evaluate to \c true if the SkBuff has the given destination port.
        /*!
         * If the transport protocol is not present or has no port, the predicate evaluates to False.
         */

        auto has_dst_port   = [] (uint16_t port) { return predicate ("is_dst_port", port); };

        //! Evaluate to \c true if the source or destination IP address matches the given network address. I.e.,
        /*!
         * Example:
         *
         * has_addr ("192.168.0.0",24)
         */

        auto has_addr = [] (const char *addr, int prefix)
        {
            return predicate("has_addr", ipv4_t {addr}, prefix);
        };

        //! Evaluate to \c true if the source IP address matches the given network address.

        auto has_src_addr = [] (const char *addr, int prefix)
        {
            return predicate("has_src_addr", ipv4_t {addr}, prefix);
        };

        //! Evaluate to \c true if the destination IP address matches the given network address.

        auto has_dst_addr = [] (const char *addr, int prefix)
        {
            return predicate("has_dst_addr", ipv4_t{addr}, prefix);
        };

        //! Evaluate to \c true if the SkBuff has the given \c mark, set by mark function.
        /*!
         * Example:
         *
         * has_mark(11)
         *
         * \see mark
         */

        auto has_mark       = [] (uint32_t value) { return predicate("has_mark", value); };


        //! Evaluate to \c true if the state of the computation is set to the given \c value, possibly by put_state function.
        //
        /*!
         * Example:
         *
         * has_state(11)
         *
         * \see state
         */

        auto has_state      = [] (uint32_t value) { return predicate("has_state", value); };

        //! Evaluate to \c true if the SkBuff has a vlan tag.

        auto has_vlan       = predicate ("has_vlan");

        //! Evaluate to \c true if the SkBuff has the given vlan id.
        /*!
         * Example:
         *
         * has_vid(42)
         */

        auto has_vid        = [] (int value) { return predicate ("has_vid", value); };

        //! Predicate which evaluates to \c true when the packet has one of the
        /*!
         * vlan id specified by the list. Example:
         *
         * when (vland_id ({1,13,42,43}), log_msg("Got a packet!"))
         */

        auto vlan_id        = [] (std::vector<int> const &vs) {
                                    return predicate("vlan_id", vs);
                                };

        //! Monadic function, counterpart of \c vlan_id function. \see vlan_id

        auto vlan_id_filter = [] (std::vector<int> const &vs) {
                                    return function("vlan_id_filter", vs);
                              };
        //
        // default properties:
        //

        //! Evaluate to the state of the computation, possibly set by \c state function.
        /*
         * \see state
         */

        auto get_state  = property("get_state");

        //! Evaluate to the mark set by \c mark function.
        /*! By default packets are marked with 0.
         *
         * \see mark
         */

        auto get_mark   = property("get_mark");

        //! Evaluate to the /tos/ field of the IP header.

        auto ip_tos     = property("ip_tos");

        //! Evaluate to the /tot_len/ field of the IP header.

        auto ip_tot_len = property("ip_tot_len");

        //! Evaluate to the /ip_id/ field of the IP header.

        auto ip_id      = property("ip_id");

        //! Evaluate to the /frag/ field of the IP header.

        auto ip_frag    = property("ip_frag");

        //! Evaluate to the /TTL/ field of the IP header.

        auto ip_ttl     = property("ip_ttl");

        //! Evaluate to the /source port/ of the TCP header.

        auto tcp_source = property("tcp_source");

        //! Evaluate to the /destination port/ of the TCP header.

        auto tcp_dest   = property("tcp_dest");

        //! Evaluate to the /length/ field of the TCP header.

        auto tcp_hdrlen = property("tcp_hdrlen");

        //! Evaluate to the /source port/ of the UDP header.

        auto udp_source = property("udp_source");

        //! Evaluate to the /destination port/ of the UDP header.

        auto udp_dest   = property("udp_dest");

        //! Evaluate to the /length/ field of the UDP header.

        auto udp_len    = property("udp_len");

        //! Evaluate to the /type/ field of the ICMP header.

        auto icmp_type  = property("icmp_type");

        //! Evaluate to the /code/ field of the ICMP header.

        auto icmp_code  = property("icmp_code");

        //
        // default netfunctions:
        //

        //! Dispatch the packet across the sockets.
        /*!
         * Dispatch with a randomized algorithm in Round-Robin fashion.
         *
         * ip >> steer_rrobin
         */

        auto steer_rrobin = function("steer_rrobin");

        //! Dispatch the packet across the sockets.
        /*!
         * Dispatch with a randomized algorithm that maintains the integrity
         * of physical links. Example:
         *
         * ip >> steer_link
         */

        auto steer_link = function("steer_link");

        //! Dispatch the packet across the sockets
        /*!
         * Dispatch with a randomized algorithm that maintains the integrity
         * of vlan links. Example:
         *
         * steer_vlan
         */

        auto steer_vlan = function("steer_vlan");

        //! Dispatch the packet across the sockets
        /*!
         * Dispatch with a randomized algorithm that maintains the integrity
         * of IP flows. Example:
         *
         * steer_ip
         */

        auto steer_ip   = function("steer_ip");

        //! Dispatch the packet across the sockets
        /*!
         * Dispatch with a randomized algorithm that maintains the integrity
         * of IPv6 flows. Example:
         *
         * steer_ip6 >> log_msg("Steering an IPv6 packet")
         */

        auto steer_ip6  = function("steer_ip6");

        //! Dispatch the packet across the sockets
        /*!
         * Dispatch with a randomized algorithm that maintains the integrity
         * of TCP/UDP flows. Example:
         *
         * steer_flow >> log_msg ("Steering a flow")
         */

        auto steer_flow = function("steer_flow");

        //! Dispatch the packet across the sockets
        /*!
         * Dispatch with a randomized algorithm that maintains the integrity
         * of RTP/RTCP flows. Example:
         *
         * steer_rtp
         */

        auto steer_rtp  = function("steer_rtp");

        //! Dispatch the packet across the sockets
        /*!
         * Dispatch with a randomized algorithm that maintains the integrity
         * of sub networks. Example:
         *
         * steer_net("192.168.0.0", 16, 24)
         */

        auto steer_net  = [] (const char *net, int prefix, int subprefix)
        {
            struct supernet {
                uint32_t addr;
                int      prefix;
                int      subprefix;
            } na = { 0, prefix, subprefix };

            if (inet_pton(AF_INET, net, &na.addr) <= 0)
                throw std::runtime_error("pfq::lang::steer_net");

            return function("steer_net", na);
        };

        //! Dispatch the packet across the sockets
        /*!
         * Dispatch with a randomized algorithm. The function uses as /hash/ the field
         * of /size/ bits taken at /offset/ bytes from the beginning of the packet.
         *
         */

        auto steer_field = [] (int off_bytes, int size_bits) {
                                return function("steer_field", off_bytes, size_bits);
                           };
        //
        // default filters:
        //

        //! Transform the given predicate in its counterpart monadic version.
        /*!
         * Example:
         *
         * filter (is_udp) >> kernel
         *
         * is logically equivalent to:
         *
         * udp >> kernel
         */

        template <typename Predicate>
        auto filter(Predicate p)
            -> decltype (function(nullptr, p))
        {
            static_assert(is_predicate<Predicate>::value, "filter: argument 0: predicate expected");
            return function("filter", p);
        }

        //! Evaluate to \c Pass SkBuff if it is an IPv4 packet, \c Drop it otherwise.

        auto ip             = function("ip");

        //! Evaluate to \c Pass SkBuff if it is an IPv6 packet, \c Drop it otherwise.

        auto ip6            = function("ip6");

        //! Evaluate to \c Pass SkBuff if it is an UDP packet, \c Drop it otherwise.

        auto udp            = function("udp");

        //! Evaluate to \c Pass SkBuff if it is a TCP packet, \c Drop it otherwise.

        auto tcp            = function("tcp");

        //! Evaluate to \c Pass SkBuff if it is an ICMP packet, \c Drop it otherwise.

        auto icmp           = function("icmp");

        //! Evaluate to \c Pass SkBuff if it is an UDP packet (on top of IPv6), \c Drop it otherwise.

        auto udp6           = function("udp6");

        //! Evaluate to \c Pass SkBuff if it is a TCP packet (on top of IPv6), \c Drop it otherwise.

        auto tcp6           = function("tcp6");

        //! Evaluate to \c Pass SkBuff if it is an ICMP packet (on top of IPv6), \c Drop it otherwise.

        auto icmp6          = function("icmp6");

        //! Evaluate to \c Pass SkBuff if it has a vlan tag, \c Drop it otherwise.

        auto vlan           = function("vlan");

        //! Evaluate to \c Pass SkBuff if it is a TCP or UDP packet, \c Drop it otherwise.

        auto flow           = function("flow");

        //! Evaluate to \c Pass SkBuff if it is a RTP/RTCP packet, \c Drop it otherwise.

        auto rtp            = function("rtp");

        //! Evaluate to \c Pass SkBuff if it is not a fragment, \c Drop it otherwise.

        auto no_frag        = function("no_frag");

        //! Evaluate to \c Pass SkBuff if it is not a fragment or if it is the first fragment, \c Drop it otherwise.

        auto no_more_frag   = function("no_more_frag");

        //! Send a copy of the packet to the kernel.
        /*!
         *
         * To avoid loop, this function is ignored for packets sniffed from the kernel.
         */

        auto kernel         = function("kernel");

        //! Broadcast the packet to all the sockets that have joined the group for which this computation is specified.

        auto broadcast      = function("broadcast");

        //! Drop the packet. The computation evaluates to \c Drop.

        auto drop           = function("drop");

        //! Unit operation implements left- and right-identity for Action monad.

        auto unit           = function("unit");

        //! Unit operation implements left- and right-identity for Action monad.

        auto log_msg        = [] (std::string msg) { return function("log_msg", std::move(msg)); };

        //! Dump the payload of packet to syslog.
        /*!
         * Example:
         *
         * icmp >> log_buff
         *
         */

        auto log_buff       = function("log_buff");

        //! Log the packet to syslog, with a syntax similar to tcpdump.
        /*!
         * Example:
         *
         * icmp >> log_msg ("This is an ICMP packet:") >> log_packet
         *
         */

        auto log_packet     = function("log_packet");

        //! Forward the packet to the given device.
        /*!
         * This function is lazy, in that the action is logged and performed
         * when the computation is completely evaluated.
         *
         * forward ("eth1")
         */

        auto forward    = [] (std::string dev) { return function("forward", std::move(dev)); };

        //! Forward the packet to the given device.
        /*! This operation breaks the purity of the language, and it is possibly slower
         * than the lazy "forward" counterpart.
         *
         * forwardIO ("eth1")
         */

        auto forwardIO  = [] (std::string dev) { return function("forwardIO", std::move(dev)); };

        //! Forward the packet to the given device and evaluates to \c Drop.
        /*!
         * Example:
         *
         * when(is_udp, bridge ("eth1")) >> kernel
         *
         * Conditional bridge, forward the packet to eth1 if UDP, send it to the kernel otherwise.
         */

        auto bridge     = [] (std::string dev) { return function("bridge", std::move(dev)); };

        //! Forward the packet to the given device.
        /*! It evaluates to \c Pass SkBuff or \c Drop,
         * depending on the value returned by the predicate. Example:
         *
         * tee("eth1", is_udp) >> kernel
         *
         * Logically equivalent to:
         *
         * forward ("eth1") >> udp >> kernel
         *
         * Only a little bit more efficient.
         */

        template <typename Predicate>
        auto tee_(std::string dev, Predicate p)
            -> decltype(function(nullptr, dev, p))
        {
            static_assert(is_predicate<Predicate>::value, "tee: argument 1: predicate expected");
            return function("tee", dev, p);
        }

        //! Evaluate to \c Pass SkBuff, or forward the packet to the given device.
        /*!
         * It evaluates to \c Drop, depending on the value returned by the predicate. Example:
         *
         * tap ("eth1", is_udp) >> kernel
         *
         * Logically equivalent to:
         *
         * unless (is_udp,  forward ("eth1") >> drop ) >> kernel
         *
         * Only a little bit more efficient.
         */

        template <typename Predicate>
        auto tap(std::string dev, Predicate p)
            -> decltype(function(nullptr, dev, p))
        {
            static_assert(is_predicate<Predicate>::value, "tap: argument 1: predicate expected");
            return function("tap", dev, p);
        }

        //! Mark the packet with the given value.
        /*
         *  This function is unsafe in that it breaks the pure functional paradigm.
         *  Consider using `put_state` instead.
         *
         * Example:
         *
         * mark (42)
         */

        auto mark           = [] (uint32_t value) { return function("mark", value); };

        //! Set the state of the computation to the given value.
        /*
         * Example:
         *
         * state (42)
         */

        auto put_state      = [] (uint32_t value) { return function("put_state", value); };

        //! Increment the i-th counter of the current group.
        /*
         * Example:
         *
         * inc (10)
         */

        auto inc            = [] (int value) { return function("inc", value); };

        //! Decrement the i-th counter of the current group.
        /*
         * Example:
         *
         * dec (10)
         */

        auto dec            = [] (int value) { return function("dec", value); };

        //! Monadic version of \c is_l3_proto predicate.
        /*!
         * Predicates are used in conditional expressions, while monadic functions
         * are combined with Kleisli operator:
         *
         * l3_proto (0x842) >> log_msg ("Wake-on-LAN packet!")
         *
         * \see is_l3_proto
         */

        auto l3_proto       = [] (uint16_t type) { return function ("l3_proto", type); };

        //! Monadic version of \c is_l4_proto predicate.
        /*!
         * Predicates are used in conditional expressions, while monadic functions
         * are combined with Kleisli operator:
         *
         * l4_proto(89) >> log_msg("OSFP packet!")
         *
         * \see is_l4_proto
         */

        auto l4_proto       = [] (uint8_t proto) { return function ("l4_proto", proto); };

        //! Monadic version of \c has_port predicate.
        /*!
         * Predicates are used in conditional expressions, while monadic functions
         * are combined with Kleisli operator:
         *
         * port(80) >> log_msg ("http packet!")
         *
         * \see has_port
         */

        auto port           = [] (uint16_t p) { return function ("port", p); };

        //! Monadic version of \c has_src_port predicate.  \see has_src_port

        auto src_port       = [] (uint16_t p) { return function ("src_port", p); };

        //! Monadic version of \c has_dst_port predicate.  \see has_dst_port

        auto dst_port       = [] (uint16_t p) { return function ("dst_port", p); };

        //! Monadic version of \c has_addr predicate.
        /*!
         * Predicates are used in conditional expressions, while monadic functions
         * are combined with kleisli operator:
         *
         * addr ("192.168.0.0",24) >> log_packet
         *
         * \see has_addr
         */

        auto addr = [] (const char *net, int prefix)
        {
            return function("addr", ipv4_t{net}, prefix);
        };

        //! Monadic version of \c has_src_addr predicate.  \see has_src_addr

        auto src_addr = [] (const char *net, int prefix)
        {
            return function("src_addr", ipv4_t{net}, prefix);
        };

        //! Monadic version of \c has_dst_addr predicate.  \see has_dst_addr

        auto dst_addr = [] (const char *net, int prefix)
        {
            return function("dst_addr", ipv4_t{net}, prefix);
        };

        //! Conditional execution of monadic NetFunctions.
        /*!
         * The function takes a predicate and evaluates to given the NetFunction when it evalutes to \c true,
         * otherwise does nothing.
         * Example:
         *
         * when (is_tcp, log_msg ("This is a TCP Packet"))
         *
         */

        template <typename Predicate, typename Fun>
        auto when(Predicate p, Fun f)
            -> decltype(function(nullptr, p, f))
        {
            static_assert(is_predicate<Predicate>::value,  "when: argument 0: predicate expected");
            static_assert(is_monadic_function<Fun>::value, "when: argument 1: monadic function expected");

            return function("when", p, f);
        }

        //! The reverse of \c when. \see when

        template <typename Predicate, typename Fun>
        auto unless(Predicate p, Fun f)
            -> decltype(function(nullptr, p, f))
        {
            static_assert(is_predicate<Predicate>::value,  "unless: argument 0: predicate expected");
            static_assert(is_monadic_function<Fun>::value, "unless: argument 1: monadic function expected");

            return function("unless", p, f);
        }

        //! conditional execution of monadic netfunctions.
        /*!
         * The function takes a predicate and evaluates to the first or the second expression,
         * depending on the value returned by the predicate.  Example:
         *
         * conditional (is_udp,  forward ("eth1"),  forward ("eth2"))
         *
         */

        template <typename Predicate, typename F1, typename F2>
        auto conditional(Predicate p, F1 f1, F2 f2)
            -> decltype(function(nullptr, p, f1, f2))
        {
            static_assert(is_predicate<Predicate>::value,  "conditional: argument 0: predicate expected");
            static_assert(is_monadic_function<F1>::value,  "conditional: argument 1: monadic function expected");
            static_assert(is_monadic_function<F2>::value,  "conditional: argument 1: monadic function expected");

            return function("conditional", p, f1, f2);
        }

        //! Function that inverts a monadic NetFunction.
        /*!
         * Useful to invert filters:
         *
         * inv (ip) >> log_msg ("This is not an IPv4 Packet")
         *
         */

        template <typename Fun>
        auto inv(Fun f)
            -> decltype(function(nullptr, f))
        {
            static_assert(is_monadic_function<Fun>::value, "inv: argument 0: monadic function expected");

            return function("inv", f);
        }

        //! Function that returns the parallel of two monadic NetFunctions.
        /*!
         * Logic 'or' for manadic filters:
         *
         * par (udp, icmp) >> log_msg ("This is an UDP or ICMP Packet")
         *
         */

        template <typename F1, typename F2>
        auto par(F1 f1, F2 f2)
            -> decltype(function(nullptr, f1, f2))
        {
            static_assert(is_monadic_function<F1>::value, "par: argument 0: monadic function expected");
            static_assert(is_monadic_function<F2>::value, "par: argument 1: monadic function expected");

            return function("par", f1, f2);
        }

        //
        // bloom filters:
        //

        //! Predicate that evaluates to \c true when the source or the destination address
        // of the packet matches the ones specified by the bloom list.
        /*!
         * The first \c int argument specifies the size of the bloom filter.
         * The second \c vector argument specifies the list of IP/network addresses of the bloom filter.
         * The third \c int argument specifies the network prefix.
         * Example:
         *
         * when (bloom (1024, {"192.168.0.13", "192.168.0.42"}, 32), log_packet ) >> kernel
         *
         */

        auto bloom      = [] (int m, std::vector<std::string> const &ips, int prefix) {
                                auto addrs = fmap(details::inet_addr, ips);
                                return predicate("bloom", m, std::move(addrs), prefix);
                          };

        //! Similarly to \c bloom, evaluates to \c true when the source address
        //! of the packet matches the ones specified by the bloom list.  \see bloom

        auto bloom_src  = [] (int m, std::vector<std::string> const &ips, int prefix) {
                                auto addrs = fmap(details::inet_addr, ips);
                                return predicate("bloom_src", m, std::move(addrs), prefix);
                          };

        //! Similarly to \c bloom, evaluates to \c true when the destination address
        //! of the packet matches the ones specified by the bloom list.  \see bloom

        auto bloom_dst  = [] (int m, std::vector<std::string> const &ips, int prefix) {
                                auto addrs = fmap(details::inet_addr, ips);
                                return predicate("bloom_dst", m, std::move(addrs), prefix);
                          };

        //! Monadic counterpart of \c bloom function.  \see bloom

        auto bloom_filter      = [] (int m, std::vector<std::string> const &ips, int prefix) {
                                    auto addrs = fmap(details::inet_addr, ips);
                                    return function("bloom_filter", m, std::move(addrs), prefix);
                                };

        //! Monadic counterpart of \c bloom_src function.  \see bloom_src

        auto bloom_src_filter  = [] (int m, std::vector<std::string> const &ips, int prefix) {
                                    auto addrs = fmap(details::inet_addr, ips);
                                    return function("bloom_src_filter", m, std::move(addrs), prefix);
                                };

        //! Monadic counterpart of \c bloom_dst function. \see bloom_dst

        auto bloom_dst_filter  = [] (int m, std::vector<std::string> const &ips, int prefix) {
                                    auto addrs = fmap(details::inet_addr, ips);
                                    return function("bloom_dst_filter", m, std::move(addrs), prefix);
                                };
        //
        // bloom filter, utility functions:
        //

        constexpr int bloomK = 4;

        //! Bloom filter: utility function that computes the optimal /M/, given the parameter /N/ and
        // the false-positive probability /p/.

        inline int bloom_calc_m(int n, double p)
        {
            return static_cast<int>(std::ceil( -static_cast<double>(bloomK) * n / std::log( 1.0 - std::pow(p, 1.0 / bloomK) )));
        }

        //! Bloom filter: utility function that computes the optimal /N/, given the parameter /M/ and
        // the false-positive probability /p/.

        inline int bloom_calc_n(int m, double p)
        {
            return static_cast<int>(std::ceil( -static_cast<double>(m) * std::log( 1.0 - std::pow(p, 1.0 / bloomK) ) / bloomK ));
        }

        //! Bloom filter: utility function that computes the false positive P, given /N/ and /M/ parameters.

        inline double
        bloom_calc_p(int n, int m)
        {
            return std::pow(1 - std::pow(1 - 1.0/m, n * bloomK), bloomK);
        }

    }

} // namespace lang
} // naemspace pfq
