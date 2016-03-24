/*
 * Copyright (c) 2012-2014 Nicola Bonelli <nicola@pfq.io>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 * products derived from this software without specific prior written
 * permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * PFQ sniffing API implementation for Linux platform
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <pcap-int.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

#include <pcap.h>
#include "pcap/sll.h"
#include "pcap/vlan.h"

#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/pf_q.h>

#include <pfq/pfq.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/mman.h>
#include <poll.h>
#include <ctype.h>

static void pfq_cleanup_linux(pcap_t *);
static	int pfq_activate_linux(pcap_t *);
static	int pfq_inject_linux(pcap_t *, const void *, size_t);
static	int pfq_setdirection_linux(pcap_t *, pcap_direction_t);
static	int pfq_read_linux(pcap_t *, int, pcap_handler, u_char *);
static	int pfq_stats_linux(pcap_t *, struct pcap_stat *);


#define MUST_CLEAR_PROMISC	0x00000001

pcap_t
*pfq_create(const char *device, char *ebuf, size_t size)
{
	pcap_t *p;

	p = pcap_create_common(device, ebuf, size);
	if (p == NULL)
		return NULL;

	p->activate_op = pfq_activate_linux;
	return p;
}


static int
set_kernel_filter(pcap_t *handle, struct sock_fprog *fcode)
{
	return pfq_group_fprog(handle->md.pfq.q, handle->opt.pfq.group, fcode);
}


static int
reset_kernel_filter(pcap_t *handle)
{
	return pfq_group_fprog_reset(handle->md.pfq.q, handle->opt.pfq.group);
}


static int
fix_offset(struct bpf_insn *p)
{
	/*
	 * What's the offset?
	 */
	if (p->k >= SLL_HDR_LEN) {
		/*
		 * It's within the link-layer payload; that starts at an
		 * offset of 0, as far as the kernel packet filter is
		 * concerned, so subtract the length of the link-layer
		 * header.
		 */
		p->k -= SLL_HDR_LEN;
	} else if (p->k == 0) {
		/*
		 * It's the packet type field; map it to the special magic
		 * kernel offset for that field.
		 */
		p->k = SKF_AD_OFF + SKF_AD_PKTTYPE;
	} else if (p->k == 14) {
		/*
		 * It's the protocol field; map it to the special magic
		 * kernel offset for that field.
		 */
		p->k = SKF_AD_OFF + SKF_AD_PROTOCOL;
	} else if ((bpf_int32)(p->k) > 0) {
		/*
		 * It's within the header, but it's not one of those
		 * fields; we can't do that in the kernel, so punt
		 * to userland.
		 */
		return -1;
	}
	return 0;
}


static int
fix_program(pcap_t *handle, struct sock_fprog *fcode, int is_mmapped)
{
	size_t prog_size;
	register int i;
	register struct bpf_insn *p;
	struct bpf_insn *f;
	int len;

	/*
	 * Make a copy of the filter, and modify that copy if
	 * necessary.
	 */
	prog_size = sizeof(*handle->fcode.bf_insns) * handle->fcode.bf_len;
	len = handle->fcode.bf_len;
	f = (struct bpf_insn *)malloc(prog_size);
	if (f == NULL) {
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE,
			 "malloc: %s", pcap_strerror(errno));
		return -1;
	}
	memcpy(f, handle->fcode.bf_insns, prog_size);
	fcode->len = len;
	fcode->filter = (struct sock_filter *) f;

	for (i = 0; i < len; ++i) {
		p = &f[i];
		/*
		 * What type of instruction is this?
		 */
		switch (BPF_CLASS(p->code)) {

		case BPF_RET:
			/*
			 * It's a return instruction; are we capturing
			 * in memory-mapped mode?
			 */
			if (!is_mmapped) {
				/*
				 * No; is the snapshot length a constant,
				 * rather than the contents of the
				 * accumulator?
				 */
				if (BPF_MODE(p->code) == BPF_K) {
					/*
					 * Yes - if the value to be returned,
					 * i.e. the snapshot length, is
					 * anything other than 0, make it
					 * 65535, so that the packet is
					 * truncated by "recvfrom()",
					 * not by the filter.
					 *
					 * XXX - there's nothing we can
					 * easily do if it's getting the
					 * value from the accumulator; we'd
					 * have to insert code to force
					 * non-zero values to be 65535.
					 */
					if (p->k != 0)
						p->k = 65535;
				}
			}
			break;

		case BPF_LD:
		case BPF_LDX:
			/*
			 * It's a load instruction; is it loading
			 * from the packet?
			 */
			switch (BPF_MODE(p->code)) {

			case BPF_ABS:
			case BPF_IND:
			case BPF_MSH:
				/*
				 * Yes; are we in cooked mode?
				 */
				if (handle->md.cooked) {
					/*
					 * Yes, so we need to fix this
					 * instruction.
					 */
					if (fix_offset(p) < 0) {
						/*
						 * We failed to do so.
						 * Return 0, so our caller
						 * knows to punt to userland.
						 */
						return 0;
					}
				}
				break;
			}
			break;
		}
	}
	return 1;	/* we succeeded */
}


static int
pfq_setfilter_linux(pcap_t *handle, struct bpf_program *filter)
{
	struct sock_fprog fcode;
	int can_filter_in_kernel;
	int err = 0;

	if (!handle)
		return -1;
	if (!filter) {
	        strncpy(handle->errbuf, "[PFQ] setfilter: No filter specified",
			PCAP_ERRBUF_SIZE);
		return -1;
	}

	/* Make our private copy of the filter */

	if (install_bpf_program(handle, filter) < 0)
		/* install_bpf_program() filled in errbuf */
		return -1;

	/*
	 * Run user level packet filter by default. Will be overriden if
	 * installing a kernel filter succeeds.
	 */
	handle->md.use_bpf = 0;

	switch (fix_program(handle, &fcode, 1)) {

	case -1:
	default:
		/*
		 * Fatal error; just quit.
		 * (The "default" case shouldn't happen; we
		 * return -1 for that reason.)
		 */
		return -1;

	case 0:
		/*
		 * The program performed checks that we can't make
		 * work in the kernel.
		 */
		can_filter_in_kernel = 0;
		break;

	case 1:
		/*
		 * We have a filter that'll work in the kernel.
		 */
		can_filter_in_kernel = 1;
		break;
	}

	if (can_filter_in_kernel) {

		if ((err = set_kernel_filter(handle, &fcode)) == 0) {

			/* Installation succeded - using kernel filter. */
			handle->md.use_bpf = 1;
		}
		else if (err == -1) {	/* Non-fatal error */

			/*
			 * Print a warning if we weren't able to install
			 * the filter for a reason other than "this kernel
			 * isn't configured to support socket filters.
			 */
			if (errno != ENOPROTOOPT && errno != EOPNOTSUPP) {
				fprintf(stderr,
				    "[PFQ] Kernel filter failed: %s\n",
					pcap_strerror(errno));
			}
		}
	}
	else
		fprintf(stderr, "[PFQ] could not set BPF filter in kernel!\n");

	/*
	 * If we're not using the kernel filter, get rid of any kernel
	 * filter that might've been there before, e.g. because the
	 * previous filter could work in the kernel, or because some other
	 * code attached a filter to the socket by some means other than
	 * calling "pcap_setfilter()".  Otherwise, the kernel filter may
	 * filter out packets that would pass the new userland filter.
	 */
	if (!handle->md.use_bpf)
		reset_kernel_filter(handle);

	/*
	 * Free up the copy of the filter that was made by "fix_program()".
	 */
	if (fcode.filter != NULL)
		free(fcode.filter);

	if (err == -2)
		/* Fatal error */
		return -1;

	return 0;
}


typedef int (*pfq_token_handler_t)(const char *);

static int
string_for_each_token(const char *ds, const char *sep, pfq_token_handler_t handler)
{
        char * mutable = strdup(ds);
        char *str, *token, *saveptr;
        int i, ret = 0;

        for (i = 1, str = mutable; ; i++, str = NULL)
        {
                token = strtok_r(str, sep, &saveptr);
                if (token == NULL)
                        break;
                if (handler(token) < 0) {
		        ret = PCAP_ERROR;
			break;
		}
        }

        free(mutable);
	return ret;
}


static char *
string_first_token(const char *str, const char *sep)
{
	char *end;

	if ((end = strstr(str, sep))) {
		char *ret = malloc(end - str + 1);
		strncpy(ret, str, end - str);
		ret[end - str] = '\0';
		return ret;
	}

	return strdup(str);
}


static char *
string_trim(char *str)
{
	int i = 0, j = strlen(str) - 1;

	while (isspace(str[i]) && str[i] != '\0')
		i++;
	while (j >= 0 && isspace(str[j]))
		j--;

	str[j+1] = '\0';
	return str+i;
}


static char *
pfq_get_config_file(const char *fullname)
{
	char *end, *conf;
	if (!fullname)
		return NULL;
	conf = strstr(fullname, "pfq/");
	if (conf == NULL)
		return NULL;
	conf = strdup(conf+4);
	if (end = strchr(conf, ':')) {
		*end = '\0';
	}
	return conf;
}


static char *
pfq_get_devname(const char *fullname)
{
	char *dev;
	if (!fullname)
		return NULL;
	dev = strstr(fullname, "pfq");
	if (!dev)
		return strdup(fullname);
	if (dev = strchr(dev, ':'))
		return strdup(dev+1);
	return NULL;
}


static long int
linux_if_drops(const char * if_name)
{
	char buffer[512];
	char * bufptr;
	FILE * file;
	int field_to_convert = 3, if_name_sz = strlen(if_name);
	long int dropped_pkts = 0;

	file = fopen("/proc/net/dev", "r");
	if (!file)
		return 0;

	while (!dropped_pkts && fgets( buffer, sizeof(buffer), file ))
	{
		/* 	search for 'bytes' -- if its in there, then
			that means we need to grab the fourth field. otherwise
			grab the third field. */
		if (field_to_convert != 4 && strstr(buffer, "bytes")) {

			field_to_convert = 4;
			continue;
		}

		/* find iface and make sure it actually matches -- space before the name and : after it */
		if ((bufptr = strstr(buffer, if_name)) &&
			(bufptr == buffer || *(bufptr-1) == ' ') &&
			*(bufptr + if_name_sz) == ':')
		{
			bufptr = bufptr + if_name_sz + 1;

			/* grab the nth field from it */
			while( --field_to_convert && *bufptr != '\0')
			{
				while (*bufptr != '\0' && *(bufptr++) == ' ');
				while (*bufptr != '\0' && *(bufptr++) != ' ');
			}

			/* get rid of any final spaces */
			while (*bufptr != '\0' && *bufptr == ' ') bufptr++;

			if (*bufptr != '\0')
				dropped_pkts = strtol(bufptr, NULL, 10);

			break;
		}
	}

	fclose(file);
	return dropped_pkts;
}

static int
pfq_parse_integers(int *out, size_t max, const char *in)
{
	size_t n = 0; int ret = 0;

	int store_int(const char *num) {
		if (n < max) {
			out[n++] = atoi(num);
			ret++;
		}
		return 0;
	}

	if (string_for_each_token(in, ",", store_int) < 0)
		return -1;
	return ret;
}


static size_t
pfq_count_tx_thread(struct pfq_opt const *opt)
{
	size_t n, tx = 0;
        for(n = 0; n < 4; n++) {
		if (opt->tx_thread[n] != Q_NO_KTHREAD)
			tx++;
	}
	return tx;
}


static struct pfq_opt
pfq_opt_default(pcap_t *handle)
{
	return (struct pfq_opt)
	{
		.group    = -1,
		.caplen   = handle->snapshot,
		.rx_slots = 4096,
		.tx_slots = 4096,
		.tx_fhint = 1,
		.tx_async = 0,
		.tx_queue = {-1, -1, -1, -1},
		.tx_thread= { Q_NO_KTHREAD, Q_NO_KTHREAD, Q_NO_KTHREAD, Q_NO_KTHREAD },
		.vlan     = NULL,
		.comp     = NULL
	};
}


static int
pfq_parse_env(struct pfq_opt *opt)
{
	char *var;

	if ((var = getenv("PFQ_GROUP")))
		opt->group = atoi(var);

	if ((var = getenv("PFQ_CAPLEN")))
		opt->caplen = atoi(var);

	if ((var = getenv("PFQ_RX_SLOTS")))
		opt->rx_slots = atoi(var);

	if ((var = getenv("PFQ_TX_SLOTS")))
		opt->tx_slots = atoi(var);

	if ((var = getenv("PFQ_TX_FHINT")))
		opt->tx_fhint = atoi(var);

	if ((var = getenv("PFQ_VLAN")))
		opt->vlan = var;

	if ((var = getenv("PFQ_LANG"))) {
		free(opt->comp);
		opt->comp = var;
	}

	if ((var = getenv("PFQ_TX_QUEUE"))) {
		if (pfq_parse_integers(opt->tx_queue, 4, var) < 0) {
			fprintf(stderr, "[PFQ] PFQ_TX_QUEUE parse error!\n");
			return -1;
		}
	}

	if ((var = getenv("PFQ_TX_THREAD"))) {
		if (pfq_parse_integers(opt->tx_thread, 4, var) < 0) {
			fprintf(stderr, "[PFQ] PFQ_TX_THREAD parse error!\n");
			return -1;
		}
	}

	return 0;
}


#define KEY(value) [KEY_ ## value] = # value

#define KEY_ERR			-1
#define KEY_group		0
#define KEY_caplen		1
#define KEY_rx_slots		2
#define KEY_tx_slots            3
#define KEY_tx_fhint		4
#define KEY_tx_queue		5
#define KEY_tx_thread		6
#define KEY_vlan		7


struct pfq_conf_key
{
	const char *value;
} pfq_conf_keys[] =
{
	KEY(group),
	KEY(caplen),
	KEY(rx_slots),
	KEY(tx_slots),
	KEY(tx_queue),
	KEY(tx_fhint),
	KEY(tx_thread),
	KEY(vlan)
};


static int
pfq_conf_find_key(const char *key)
{
	int n;
	for(n = 0; n < sizeof(pfq_conf_keys)/sizeof(pfq_conf_keys[0]); n++)
	{
		if (strcasecmp(pfq_conf_keys[n].value, key) == 0)
			return n;
	}
	return -1;
}

char *
str_append(char *str1, const char *str2)
{
	char *ret;
	if (str1) {
		ret = realloc(str1, strlen(str1) + strlen(str2) + 1);
		strcat(ret, str2);
	}
	else {
		ret = malloc(strlen(str2) + 1);
		strcpy(ret, str2);
	}
	return ret;
}


static int
pfq_parse_config(struct pfq_opt *opt, const char *filename)
{
        char *pfq_lang = NULL;
	char line[1024];
	FILE *file;
	int rc = 0, n;

	file = fopen(filename, "r");
	if (!file) {
		fprintf(stderr, "[PFQ] could not open '%s' file!\n", filename);
		rc = -1; goto err;
	}

	for(n = 0; fgets(line, sizeof(line), file); n++) {

		char *key = NULL, *value = NULL, *tkey;

		int ret = sscanf(line, "%m[^=]=%m[^\n]",&key, &value);
		if (ret < 0) {
			fprintf(stderr, "[PFQ] %s: parse error at: %s\n", filename, key);
			rc = -1; goto next;
		}

		if (ret == 0)
			goto next;

		tkey = string_trim(key);

		if (strcmp(tkey, "qlang") == 0) {
			if (value) {
				pfq_lang = str_append(pfq_lang, value);
				pfq_lang = str_append(pfq_lang, "\n");
			}
			continue;
		}

		if (ret == 1) {
			if (strlen(tkey) && tkey[0] != '#') {
				fprintf(stderr, "[PFQ] %s: parse error at: %s\n", filename, key);
				rc = -1;
			}
			goto next;
		}

		// ret > 1

		switch(pfq_conf_find_key(tkey))
		{
			case KEY_group:		opt->group    = atoi(value);  break;
			case KEY_caplen:	opt->caplen   = atoi(value);  break;
			case KEY_rx_slots:	opt->rx_slots = atoi(value);  break;
			case KEY_tx_slots:	opt->tx_slots = atoi(value);  break;
			case KEY_tx_fhint:	opt->tx_fhint = atoi(value);  break;
			case KEY_tx_queue:  {
				if (pfq_parse_integers(opt->tx_queue, 4, value) < 0) {
					fprintf(stderr, "[PFQ] %s: parse error at: %s\n", filename, tkey);
					rc = -1;
				}
			} break;
			case KEY_tx_thread:   {
				if (pfq_parse_integers(opt->tx_thread, 4, value) < 0) {
					fprintf(stderr, "[PFQ] %s: parse error at: %s\n", filename, tkey);
					rc = -1;
				}
			} break;
			case KEY_vlan:		opt->vlan = strdup(string_trim(value)); break;
			case KEY_ERR: {
				fprintf(stderr, "[PFQ] %s: unknown keyword '%s'\n", filename, tkey);
				rc = -1;
			} break;
			default: assert(!"[PFQ] config parser: internal error!");
		}
	next:
		free(key);
		free(value);
		if (rc == -1)
			break;
	}

	fclose(file);

	if (pfq_lang) {
		free(opt->comp);
		opt->comp = pfq_lang;
	}
err:
	return rc;
}

static int
pfq_activate_linux(pcap_t *handle)
{
	char *device = NULL, *config = NULL;
        const int maxlen = 1514;
	const int queue = Q_ANY_QUEUE;
	char *first_dev;

	handle->opt.pfq  = pfq_opt_default(handle);
	handle->linktype = DLT_EN10MB;

	device = pfq_get_devname(handle->opt.source);
	fprintf(stdout, "[PFQ] running on device %s...\n", device);

	config = pfq_get_config_file(handle->opt.source);


	if (config == NULL) {
		char *conf = getenv("PFQ_CONFIG");
		if (conf)
			config = strdup(conf);
	}

	if (config)
		fprintf(stdout, "[PFQ] configuration file %s...\n", config);

        if (config != NULL) {
		if (pfq_parse_config(&handle->opt.pfq, config) == -1) {
			snprintf(handle->errbuf, PCAP_ERRBUF_SIZE, "pfq: config error");
			return PCAP_ERROR;
		}
		free(config);
	}


	if (pfq_parse_env(&handle->opt.pfq) == -1) {
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE, "pfq: env error!");
		return PCAP_ERROR;
	}

        if (handle->opt.pfq.caplen > maxlen || handle->opt.pfq.caplen == 0) {
                fprintf(stdout, "[PFQ] capture length forced to %d\n", maxlen);
                handle->opt.pfq.caplen = maxlen;
        }

	if (handle->opt.buffer_size/handle->opt.pfq.caplen > handle->opt.pfq.rx_slots)
		handle->opt.pfq.rx_slots = handle->opt.buffer_size/handle->opt.pfq.caplen;


	fprintf(stdout, "[PFQ] buffer_size = %d caplen = %d, rx_slots = %d, tx_slots = %d, tx_fhint = %d\n",
		handle->opt.buffer_size,
		handle->opt.pfq.caplen,
		handle->opt.pfq.rx_slots,
		handle->opt.pfq.tx_slots,
		handle->opt.pfq.tx_fhint);

	handle->read_op		= pfq_read_linux;
	handle->inject_op	= pfq_inject_linux;
	handle->setfilter_op	= pfq_setfilter_linux;
	handle->setdirection_op	= pfq_setdirection_linux;
	handle->getnonblock_op	= pcap_getnonblock_fd;
	handle->setnonblock_op	= pcap_setnonblock_fd;
	handle->stats_op	= pfq_stats_linux;
	handle->cleanup_op	= pfq_cleanup_linux;
	handle->set_datalink_op	= NULL;	/* can't change data link type */


	handle->md.pfq.q	= NULL;
	handle->md.pfq.current	= NULL;
	pfq_net_queue_init(&handle->md.pfq.nq);
	handle->md.pfq.ifs_promisc = 0;

	handle->fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (handle->fd == -1) {
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE,
			 "socket: %s", pcap_strerror(errno));
		if (errno == EPERM || errno == EACCES) {
			/*
			 * You don't have permission to open the
			 * socket.
			 */
			return PCAP_ERROR_PERM_DENIED;
		} else {
			/*
			 * Other error.
			 */
			return PCAP_ERROR;
		}
	}

	/*
	 * The "any" device is a special device which causes us not
	 * to bind to a particular device and thus to look at all
	 * devices of a given group.
	 */

	/* handle promisc */

	if (handle->opt.promisc) {

		/* put all devic(es) in promisc mode */
                int n = 0;

		int set_promisc(const char *dev)
		{
			struct ifreq ifr;

			memset(&ifr, 0, sizeof(ifr));
			strncpy(ifr.ifr_name, dev, sizeof(ifr.ifr_name));
			if (ioctl(handle->fd, SIOCGIFFLAGS, &ifr) == -1) {
				snprintf(handle->errbuf, PCAP_ERRBUF_SIZE,
						"SIOCGIFFLAGS: %s", pcap_strerror(errno));
				return PCAP_ERROR;
			}
			if ((ifr.ifr_flags & IFF_PROMISC) == 0) {

				/*
				 * Promiscuous mode isn't currently on,
				 * so turn it on, and remember that
				 * we should turn it off when the
				 * pcap_t is closed.
				 */

				/*
				 * If we haven't already done so, arrange
				 * to have "pcap_close_all()" called when
				 * we exit.
				 */
				if (!pcap_do_addexit(handle)) {
					/*
					 * "atexit()" failed; don't put
					 * the interface in promiscuous
					 * mode, just give up.
					 */
					return PCAP_ERROR;
				}

				fprintf(stdout, "[PFQ] set promisc on dev %s...\n", dev);

				ifr.ifr_flags |= IFF_PROMISC;
				if (ioctl(handle->fd, SIOCSIFFLAGS, &ifr) == -1) {
					snprintf(handle->errbuf, PCAP_ERRBUF_SIZE,
							"SIOCSIFFLAGS: %s",
							pcap_strerror(errno));
					return PCAP_ERROR;
				}

				handle->md.pfq.ifs_promisc |= (1 << n);
				handle->md.must_do_on_close |= MUST_CLEAR_PROMISC;
			}

			n++;
			return 0;
		}

		if (device && strcmp(device, "any")) {
			if (string_for_each_token(device, ":", set_promisc) < 0) {
				goto fail;
			}
		}
	}

	if (device) {
		handle->md.device = strdup(device);
		if (handle->md.device == NULL) {
			snprintf(handle->errbuf, PCAP_ERRBUF_SIZE, "strdup: %s",
				 pcap_strerror(errno) );
			goto fail;
		}
	}

	/*
	 * If we're in promiscuous mode, then we probably want
	 * to see when the interface drops packets too, so get an
	 * initial count from /proc/net/dev
	 */

	if (handle->opt.promisc)
		handle->md.proc_dropped = handle->md.device ? linux_if_drops(handle->md.device) : 0;

	if (handle->opt.pfq.group != -1) {

		int bind_group(const char *dev)
		{
			fprintf(stdout, "[PFQ] binding group %d on dev %s...\n", handle->opt.pfq.group, dev);

			if (pfq_bind_group(handle->md.pfq.q, handle->opt.pfq.group, dev, queue) == -1) {
				fprintf(stderr, "[PFQ] error: %s\n", pfq_error(handle->md.pfq.q));
			}
			return 0;
		}

		handle->md.pfq.q = pfq_open_nogroup(handle->opt.pfq.caplen,
						    handle->opt.pfq.rx_slots,
						    handle->opt.pfq.tx_slots);
		if (handle->md.pfq.q == NULL) {

			snprintf(handle->errbuf, PCAP_ERRBUF_SIZE, "%s", pfq_error(handle->md.pfq.q));
			goto fail;
		}

                fprintf(stdout, "[PFQ] group = %d\n", handle->opt.pfq.group);

		if (pfq_join_group(handle->md.pfq.q,
				   handle->opt.pfq.group, Q_CLASS_DEFAULT, Q_POLICY_GROUP_SHARED) < 0) {
			fprintf(stderr, "[PFQ] error: %s\n", pfq_error(handle->md.pfq.q));
		}

		/* bind to device(es) if specified */

		if (device && strcmp(device, "any") != 0) {
			if (string_for_each_token(device, ":", bind_group) < 0)
				goto fail;
		}
	}
	else
	{
		int bind_socket(const char *dev)
		{
			fprintf(stdout, "[PFQ] binding socket on dev %s...\n", dev);

			if (pfq_bind(handle->md.pfq.q, dev, queue) == -1) {
				fprintf(stderr, "[PFQ] error: %s\n", pfq_error(handle->md.pfq.q));
			}
			return 0;
		}

		handle->md.pfq.q = pfq_open_group(Q_CLASS_DEFAULT, Q_POLICY_GROUP_SHARED,
						  handle->opt.pfq.caplen,
						  handle->opt.pfq.rx_slots,
						  handle->opt.pfq.tx_slots);
		if (handle->md.pfq.q == NULL) {
			snprintf(handle->errbuf, PCAP_ERRBUF_SIZE, "%s", pfq_error(handle->md.pfq.q));
			goto fail;
		}

		/* bind to device(es) if specified */

		if (device && strcmp(device, "any") != 0) {
			if (string_for_each_token(device, ":", bind_socket) < 0)
				goto fail;
		}
	}

        handle->opt.pfq.group = pfq_group_id(handle->md.pfq.q);
	if (handle->opt.pfq.group == -1) {
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE, "%s", pfq_error(handle->md.pfq.q));
		goto fail;
	}

	/* bind TX to device/queue */

	if (device && strcmp(device, "any"))
	{
		if ((first_dev = string_first_token(device, ":"))) {

			size_t tot, idx;

			tot = pfq_count_tx_thread(&handle->opt.pfq);
			if (tot) {
				fprintf(stdout, "[PFQ] enabling %zu Tx async on dev %s...\n", tot, first_dev);

				handle->opt.pfq.tx_async = 1;

				for(idx = 0; idx < tot; idx++)
				{
					fprintf(stdout, "[PFQ] binding Tx on %s, hw queue %d, tx-thread %d\n",
						first_dev, handle->opt.pfq.tx_queue[idx], handle->opt.pfq.tx_thread[idx]);

					if (pfq_bind_tx(handle->md.pfq.q, first_dev,
							handle->opt.pfq.tx_queue[idx],
							handle->opt.pfq.tx_thread[idx]) < 0) {
						fprintf(stderr, "[PFQ] error: %s\n", pfq_error(handle->md.pfq.q));
						goto fail;
					}
				}
			}
			else {
				fprintf(stdout, "[PFQ] enabling Tx on dev %s...\n", first_dev);
				if (pfq_bind_tx(handle->md.pfq.q, first_dev, 0, -1)) {
					fprintf(stderr, "[PFQ] error: %s\n", pfq_error(handle->md.pfq.q));
					goto fail;
				}
			}

			free(first_dev);
		}
	}

	/* set FUNCTION/computation */

	if (handle->opt.pfq.comp) {

		fprintf(stdout, "[PFQ] setting computation '%s' for group %d\n",
			handle->opt.pfq.comp, handle->opt.pfq.group);

		if (pfq_set_group_computation_from_string(handle->md.pfq.q,
							  handle->opt.pfq.group,
							  handle->opt.pfq.comp) < 0) {

			fprintf(stderr, "[PFQ] error: %s\n", pfq_error(handle->md.pfq.q));
		}
	}

	/* set vlan filters */

	if (handle->opt.pfq.vlan) {

                if (pfq_vlan_filters_enable(handle->md.pfq.q, handle->opt.pfq.group, 1) < 0) {

			fprintf(stderr, "[PFQ] error: %s\n", pfq_error(handle->md.pfq.q));
                }

		int set_vlan_filter(const char *vid_)
		{
		        int vid = atoi(vid_);

			fprintf(stdout, "[PFQ] group %d setting vlan filer id=%d\n", handle->opt.pfq.group, vid);

			if (pfq_vlan_set_filter(handle->md.pfq.q, handle->opt.pfq.group, vid)  == -1) {
				fprintf(stderr, "[PFQ] error: %s\n", pfq_error(handle->md.pfq.q));
			}
			return 0;
		}

		if (string_for_each_token(handle->opt.pfq.vlan, ",", set_vlan_filter) < 0)
                        goto fail;
        }

	/* enable timestamping */

	if (pfq_timestamping_enable(handle->md.pfq.q, 1) == -1) {
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE, "%s", pfq_error(handle->md.pfq.q));
		goto fail;
	}

	/* enable socket */

	if (pfq_enable(handle->md.pfq.q) == -1) {
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE, "%s", pfq_error(handle->md.pfq.q));
		goto fail;
	}

	/* handle->selectable_fd = pfq_get_fd(handle->md.pfq.q); */

	handle->selectable_fd = -1;
	return 0;

fail:
	pfq_cleanup_linux(handle);
	return PCAP_ERROR;
}


static int
pfq_inject_linux(pcap_t *handle, const void * buf, size_t size)
{
	int ret;

	if (handle->opt.pfq.tx_async)
		ret = pfq_send_async(handle->md.pfq.q, buf, size, 1);
	else
		ret = pfq_send(handle->md.pfq.q, buf, size, handle->opt.pfq.tx_fhint, 1);

        if (ret == -1) {
		/* snprintf(handle->errbuf, PCAP_ERRBUF_SIZE, "%s", pfq_error(handle->md.pfq.q)); */
		return PCAP_ERROR;
        }
	return ret;

}


static void
pfq_cleanup_linux(pcap_t *handle)
{
	int n = 0;
	int clear_promisc(const char *dev)
	{
		struct ifreq ifr;

		if (!(handle->md.pfq.ifs_promisc & (1 << n++)))
			return 0;

		memset(&ifr, 0, sizeof(ifr));
		strncpy(ifr.ifr_name, dev,
				sizeof(ifr.ifr_name));
		if (ioctl(handle->fd, SIOCGIFFLAGS, &ifr) == -1) {
			fprintf(stderr,
					"Can't restore interface %s flags (SIOCGIFFLAGS failed: %s).\n"
					"Please adjust manually.\n"
					"Hint: This can't happen with Linux >= 2.2.0.\n",
					dev, strerror(errno));
		} else {
			if (ifr.ifr_flags & IFF_PROMISC) {
				/*
				 * Promiscuous mode is currently on;
				 * turn it off.
				 */

				fprintf(stdout, "[PFQ] clear promisc on dev %s...\n", dev);

				ifr.ifr_flags &= ~IFF_PROMISC;
				if (ioctl(handle->fd, SIOCSIFFLAGS,
							&ifr) == -1) {
					fprintf(stderr,
							"Can't restore interface %s flags (SIOCSIFFLAGS failed: %s).\n"
							"Please adjust manually.\n"
							"Hint: This can't happen with Linux >= 2.2.0.\n",
							dev,
							strerror(errno));
				}
			}
		}

		return 0;
	}

	if (handle->md.must_do_on_close & MUST_CLEAR_PROMISC) {

		if (handle->md.device && strcmp(handle->md.device, "any") != 0) {
			string_for_each_token(handle->md.device, ":", clear_promisc);
		}
	}

	if(handle->md.pfq.q) {
		fprintf(stdout, "[PFQ] close socket.\n");
		pfq_close(handle->md.pfq.q);
		handle->md.pfq.q = NULL;
	}

	close(handle->fd);

	free(handle->md.device);
	handle->md.device = NULL;
	pcap_cleanup_live_common(handle);
}


static int
pfq_read_linux(pcap_t *handle, int max_packets, pcap_handler callback, u_char *user)
{
        int start = handle->md.packets_read;
	struct pfq_net_queue *nq = &handle->md.pfq.nq;
	pfq_iterator_t it = handle->md.pfq.current;
	int n = max_packets;

        if (it == pfq_net_queue_end(nq)) {
		if (pfq_read(handle->md.pfq.q, nq, handle->md.timeout > 0 ? handle->md.timeout * 1000 : 1000000) < 0) {
			snprintf(handle->errbuf, sizeof(handle->errbuf), "PFQ read error");
			return PCAP_ERROR;
		}
		it = handle->md.pfq.current = pfq_net_queue_begin(nq);
	}

	for(; (max_packets <= 0 || n > 0) && (it != pfq_net_queue_end(nq)); it = pfq_net_queue_next(nq, it))
	{
		struct pfq_pcap_pkthdr pcap_h;
		struct pfq_pkthdr *h;
                uint16_t vlan_tci;
		const char *pkt;

		while (!pfq_pkt_ready(nq, it)) {
			if (handle->break_loop) {
				handle->break_loop = 0;
				return PCAP_ERROR_BREAK;
			}
			pfq_yield();
		}

		h = (struct pfq_pkthdr *)pfq_pkt_header(it);

		pcap_h.ts.tv_sec  = h->tstamp.tv.sec;
		pcap_h.ts.tv_usec = h->tstamp.tv.nsec / 1000;
		pcap_h.caplen     = h->caplen;
		pcap_h.len        = h->len;

		/* extended pcap header */

		pcap_h.data.mark  = h->data.mark;
		pcap_h.data.state = h->data.state;
		pcap_h.ifindex   = h->ifindex;
		pcap_h.queue	  = h->queue;
		pcap_h.gid	  = h->gid;

		pkt = pfq_pkt_data(it);

		if ((vlan_tci = h->vlan.tci) != 0) {

			struct vlan_tag *tag;

			pkt -= VLAN_TAG_LEN;

			memmove((char *)pkt, pkt + VLAN_TAG_LEN, 2 * ETH_ALEN);

			tag = (struct vlan_tag *)(pkt + 2 * ETH_ALEN);
			tag->vlan_tpid = htons(ETH_P_8021Q);
			tag->vlan_tci  = htons(vlan_tci);

			pcap_h.len += VLAN_TAG_LEN;
		}

		callback(user, (struct pcap_pkthdr *)&pcap_h, pkt);

		handle->md.packets_read++;
		n--;
	}

	handle->md.pfq.current = it;

	if (handle->break_loop) {
		handle->break_loop = 0;
		return PCAP_ERROR_BREAK;
	}

	return handle->md.packets_read-start;
}


static int
pfq_setdirection_linux(pcap_t *handle, pcap_direction_t d)
{
        fprintf(stdout, "[PFQ] set direciton not support with PFQ.\n");
	return 0;
}


static int
pfq_stats_linux(pcap_t *handle, struct pcap_stat *stat)
{
	struct pfq_stats qstats;
	long if_dropped = 0;

	if(pfq_get_stats(handle->md.pfq.q, &qstats) < 0)
		return -1;

	if (handle->opt.promisc) {
		if_dropped = handle->md.proc_dropped;
		handle->md.proc_dropped = handle->md.device ? linux_if_drops(handle->md.device) : 0;
		handle->md.stat.ps_ifdrop += (handle->md.proc_dropped - if_dropped);
	}

	stat->ps_recv   = handle->md.packets_read;
	stat->ps_drop   = (u_int) qstats.drop;
	stat->ps_ifdrop = handle->md.stat.ps_ifdrop;

	return 0;
}

