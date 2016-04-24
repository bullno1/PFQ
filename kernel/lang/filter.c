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

#include <pragma/diagnostic_push>

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/inetdevice.h>

#include <pragma/diagnostic_pop>

#include <lang/module.h>
#include <lang/filter.h>


static ActionSkBuff
filter_generic(arguments_t args, SkBuff b)
{
	predicate_t pred_ = GET_ARG(predicate_t, args);

	if (EVAL_PREDICATE(pred_, b))
		return Pass(b);

	return Drop(b);
}

static ActionSkBuff
filter_l3_proto(arguments_t args, SkBuff b)
{
	const u16 type = GET_ARG(u16, args);
        return is_l3_proto(b, type) ? Pass(b) : Drop(b);
}

static ActionSkBuff
filter_l4_proto(arguments_t args, SkBuff b)
{
	const u8 proto = GET_ARG(u8, args);
        return is_l4_proto(b, proto) ? Pass(b) : Drop(b);
}

static ActionSkBuff
filter_port(arguments_t args, SkBuff b)
{
	const u16 port = GET_ARG(u16, args);
        return has_port(b, port) ? Pass(b) : Drop(b);
}

static ActionSkBuff
filter_src_port(arguments_t args, SkBuff b)
{
	const u16 port = GET_ARG(u16, args);
        return has_src_port(b, port) ? Pass(b) : Drop(b);
}

static ActionSkBuff
filter_dst_port(arguments_t args, SkBuff b)
{
	const u16 port = GET_ARG(u16, args);
        return has_dst_port(b, port) ? Pass(b) : Drop(b);
}



static int filter_addr_init(arguments_t args)
{
	struct CIDR_ *data;
	CIDR_INIT(args, 0);
	data = GET_PTR_0(struct CIDR_, args);
	pr_devel("[PFQ|init] filter: addr:%pI4 mask:%pI4\n", &data->addr, &data->mask);
	return 0;
}


static ActionSkBuff
filter_addr(arguments_t args, SkBuff b)
{
	struct CIDR_ *data = GET_PTR_0(struct CIDR_, args);
	return has_addr(b, data->addr, data->mask) ? Pass(b) : Drop(b);
}


static ActionSkBuff
filter_src_addr(arguments_t args, SkBuff b)
{
	struct CIDR_ *data = GET_PTR_0(struct CIDR_, args);
	return has_src_addr(b, data->addr, data->mask) ? Pass(b) : Drop(b);
}

static ActionSkBuff
filter_dst_addr(arguments_t args, SkBuff b)
{
	struct CIDR_ *data = GET_PTR_0(struct CIDR_, args);
	return has_dst_addr(b, data->addr, data->mask) ? Pass(b) : Drop(b);
}

static ActionSkBuff
filter_no_frag(arguments_t args, SkBuff b)
{
	return is_frag(b) ? Drop(b) : Pass(b);
}

static ActionSkBuff
filter_no_more_frag(arguments_t args, SkBuff b)
{
	return is_more_frag(b) ? Drop(b) : Pass(b);
}


struct pfq_lang_function_descr filter_functions[] = {

        { "unit",	  "SkBuff -> Action SkBuff",	unit			},
        { "ip",           "SkBuff -> Action SkBuff",	filter_ip		},
        { "ip6",          "SkBuff -> Action SkBuff",	filter_ip6		},
        { "udp",          "SkBuff -> Action SkBuff",	filter_udp		},
        { "tcp",          "SkBuff -> Action SkBuff",	filter_tcp		},
        { "icmp",         "SkBuff -> Action SkBuff",	filter_icmp		},
        { "flow",         "SkBuff -> Action SkBuff",	filter_flow		},
        { "vlan",         "SkBuff -> Action SkBuff",	filter_vlan		},
	{ "no_frag",	  "SkBuff -> Action SkBuff",	filter_no_frag		},
	{ "no_more_frag", "SkBuff -> Action SkBuff",	filter_no_more_frag     },

        { "port",	  "Word16 -> SkBuff -> Action SkBuff",		 filter_port     },
        { "src_port",	  "Word16 -> SkBuff -> Action SkBuff",		 filter_src_port },
        { "dst_port",	  "Word16 -> SkBuff -> Action SkBuff",		 filter_dst_port },

        { "addr",	  "CIDR -> SkBuff -> Action SkBuff", filter_addr     , filter_addr_init },
        { "src_addr",	  "CIDR -> SkBuff -> Action SkBuff", filter_src_addr , filter_addr_init },
        { "dst_addr",	  "CIDR -> SkBuff -> Action SkBuff", filter_dst_addr , filter_addr_init },

	{ "l3_proto",     "Word16 -> SkBuff -> Action SkBuff",           filter_l3_proto },
        { "l4_proto",     "Word8  -> SkBuff -> Action SkBuff",           filter_l4_proto },
        { "filter",       "(SkBuff -> Bool) -> SkBuff -> Action SkBuff", filter_generic  },

        { NULL }};

