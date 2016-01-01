/***************************************************************
 *
 * (C) 2011-14 Nicola Bonelli <nicola@pfq.io>
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

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/mman.h>

#include <net/if.h>
#include <net/ethernet.h>
#include <arpa/inet.h>

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>
#include <sched.h>
#include <fcntl.h>

#include <poll.h>

#include <linux/if_ether.h>
#include <linux/pf_q.h>

#include <pfq.h>
#include <pfq-int.h>

#include "parson.h"


struct popen2
{
    pid_t child_pid;
    int   from_child, to_child;
};


/* macros */

#define ALIGN(x, a)            ALIGN_MASK(x, (__typeof__(x))(a) - 1)
#define ALIGN_MASK(x, mask)    (((x) + (mask)) & ~(mask))

#define Q_VALUE(q,value)   __builtin_choose_expr(__builtin_types_compatible_p(__typeof__(q), pfq_t *), (((pfq_t *)q)->error = NULL, (value)), \
				( __builtin_choose_expr(__builtin_types_compatible_p(__typeof__(q), pfq_t const *), (((pfq_t *)q)->error = NULL, (value)), (void)0)))

#define Q_ERROR(q,msg)	  __builtin_choose_expr(__builtin_types_compatible_p(__typeof__(q), pfq_t *), (((pfq_t *)q)->error = (msg), -1), \
				( __builtin_choose_expr(__builtin_types_compatible_p(__typeof__(q), pfq_t const *), (((pfq_t *)q)->error = (msg), -1), (void)0)))

#define Q_OK(q) Q_VALUE(q,0)


#define max(a,b) \
	({ __typeof__ (a) _a = (a); \
	   __typeof__ (b) _b = (b); \
	  _a > _b ? _a : _b; })

#define min(a,b) \
	({ __typeof__ (a) _a = (a); \
	   __typeof__ (b) _b = (b); \
	  _a < _b ? _a : _b; })


/* basic string utils */

static char *
trim_string(char *str)
{
	ptrdiff_t i = 0, j = (ptrdiff_t)strlen (str) - 1;

	while ( isspace ( str[i] ) && str[i] != '\0' )
		i++;
	while ( isspace ( str[j] ) && j >= 0 )
		j--;

	str[j+1] = '\0';
	return str+i;
}

static int
with_tokens(const char *p, const char *sep, int (*call)(char **, size_t n, va_list), ...)
{
	char *tokens[256] = { NULL };
	size_t i, n = 0;
	const char * q;
        int ret;

	/* parse string */

	while((q = strstr(p, sep)) != NULL) {
		tokens[n++] = strndup(p, (size_t)(q-p));
		p = q + strlen(sep);
	}

	tokens[n++] = strdup(p);

	/* call callback */
	{
		va_list arg_list;
		va_start(arg_list, call);
		ret = call(tokens, n, arg_list);
		va_end(arg_list);
	}

	/* free memory */

        for(i = 0; i < n; ++i)
		free(tokens[i]);

	return ret;
}

static char *
hugepages_mountpoint()
{
	FILE *mp;
	char *line = NULL, *mount_point = NULL;
	size_t len = 0;
	ssize_t read;

	mp = fopen("/proc/mounts", "r");
	if (!mp)
		return NULL;

	while((read = getline(&line, &len, mp)) != -1) {
		char mbuff[256];
		if(sscanf(line, "hugetlbfs %s", mbuff) == 1) {
			mount_point = strdup(mbuff);
			break;
		}
	}

	free (line);
	fclose (mp);
	return mount_point;
}


/* return the string error */

static __thread const char * __error;

const char *pfq_string_version = PFQ_VERSION_STRING;


const char *pfq_error(pfq_t *q)
{
        const char * p = q == NULL ? __error : q->error;
	return p == NULL ? "NULL" : p;
}


/* costructor */


pfq_t *
pfq_open(size_t caplen, size_t rx_slots, size_t tx_slots)
{
	return pfq_open_group(Q_CLASS_DEFAULT, Q_POLICY_GROUP_PRIVATE, caplen, rx_slots, tx_slots);
}


pfq_t *
pfq_open_nogroup(size_t caplen, size_t rx_slots, size_t tx_slots)
{
	return pfq_open_group(Q_CLASS_DEFAULT, Q_POLICY_GROUP_UNDEFINED, caplen, rx_slots, tx_slots);
}


pfq_t *
pfq_open_group(unsigned long class_mask, int group_policy, size_t caplen, size_t rx_slots, size_t tx_slots)
{
	int fd = socket(PF_Q, SOCK_RAW, htons(ETH_P_ALL));
	int maxlen;

	pfq_t * q;

	if (fd == -1) {
		return __error = "PFQ: module not loaded", NULL;
	}

	q = (pfq_t *) malloc(sizeof(pfq_t));
	if (q == NULL) {
		return __error = "PFQ: out of memory", NULL;
	}

	memset(q, 0, sizeof(pfq_t));

	q->fd = fd;
	q->hd = -1;
	q->id = -1;
	q->gid = -1;

        memset(&q->nq, 0, sizeof(q->nq));

	/* get id */

	q->id = PFQ_VERSION_CODE;
	socklen_t size = sizeof(q->id);

	if (getsockopt(fd, PF_Q, Q_SO_GET_ID, &q->id, &size) == -1) {
		return __error = "PFQ: get id error", free(q), NULL;
	}

	/* set rx queue slots */
	if (setsockopt(fd, PF_Q, Q_SO_SET_RX_SLOTS, &rx_slots, sizeof(rx_slots)) == -1) {
		return __error = "PFQ: set Rx slots error", free(q), NULL;
	}

	q->rx_slots = rx_slots;

	/* set caplen */
	if (setsockopt(fd, PF_Q, Q_SO_SET_RX_CAPLEN, &caplen, sizeof(caplen)) == -1) {
		return __error = "PFQ: set Rx caplen error", free(q), NULL;
	}

	q->rx_slot_size = ALIGN(sizeof(struct pfq_pkthdr) + caplen, 8);

	/* set Tx queue slots */
	if (setsockopt(fd, PF_Q, Q_SO_SET_TX_SLOTS, &tx_slots, sizeof(tx_slots)) == -1) {
		return __error = "PFQ: set Tx slots error", free(q), NULL;
	}

        /* get maxlen */

	size = sizeof(maxlen);
        if (getsockopt(fd, PF_Q, Q_SO_GET_TX_MAXLEN, &maxlen, &size) == -1)
        {
		return __error = "PFQ: get Tx maxlen error", free(q), NULL;
        }

	q->tx_slots = tx_slots;
	q->tx_slot_size = ALIGN(sizeof(struct pfq_pkthdr) + (size_t)maxlen, 8);


	if (group_policy != Q_POLICY_GROUP_UNDEFINED)
	{
		q->gid = pfq_join_group(q, Q_ANY_GROUP, class_mask, group_policy);
		if (q->gid == -1) {
			return __error = q->error, free(q), NULL;
		}
	}

	return __error = NULL, q;
}


int pfq_close(pfq_t *q)
{
	if (q->fd != -1)
	{
		if (q->shm_addr)
			pfq_disable(q);

		if (close(q->fd) < 0)
			return Q_ERROR(q, "PFQ: close error");

		if (q->hd != -1)
			close(q->hd);

		free(q);
                return Q_OK(q);
	}

	free(q);
	return __error = "PFQ: socket not open", -1;
}


int
pfq_enable(pfq_t *q)
{
	size_t tot_mem; socklen_t size = sizeof(tot_mem);
	char filename[256];
        char *hugepages, *env;

	if (q->shm_addr != MAP_FAILED &&
	    q->shm_addr != NULL) {
		return Q_ERROR(q, "PFQ: queue already enabled");
	}

	if (getsockopt(q->fd, PF_Q, Q_SO_GET_SHMEM_SIZE, &tot_mem, &size) == -1) {
		return Q_ERROR(q, "PFQ: queue memory error");
	}

	env = getenv("PFQ_HUGEPAGES");
	hugepages = hugepages_mountpoint();

	if (hugepages &&
	    !getenv("PFQ_NO_HUGEPAGES") &&
	    (env == NULL || atoi(env) != 0) )
	{
		/* HugePages */
		fprintf(stdout, "[PFQ] using HugePages...\n");

		snprintf(filename, 256, "%s/pfq.%d", hugepages, q->id);
		free (hugepages);

		q->hd = open(filename, O_CREAT | O_RDWR, 0755);
		if (q->hd == -1)
			return Q_ERROR(q, "PFQ: couldn't open a HugePages descriptor");

		q->shm_addr = mmap(NULL, tot_mem, PROT_READ|PROT_WRITE, MAP_SHARED, q->hd, 0);
		if (q->shm_addr == MAP_FAILED)
			return Q_ERROR(q, "PFQ: couldn't mmap HugePages");

		if(setsockopt(q->fd, PF_Q, Q_SO_ENABLE, &q->shm_addr, sizeof(q->shm_addr)) == -1)
			return Q_ERROR(q, "PFQ: socket enable (HugePages)");
	}
	else {
		/* Standard pages (4K) */

		void * null = NULL;
		fprintf(stdout, "[PFQ] using 4k-Pages...\n");
		if(setsockopt(q->fd, PF_Q, Q_SO_ENABLE, &null, sizeof(null)) == -1)
			return Q_ERROR(q, "PFQ: socket enable");

		q->shm_addr = mmap(NULL, tot_mem, PROT_READ|PROT_WRITE, MAP_SHARED, q->fd, 0);
		if (q->shm_addr == MAP_FAILED)
			return Q_ERROR(q, "PFQ: socket enable (memory map)");
	}

	q->shm_size = tot_mem;

	q->rx_queue_addr = (char *)(q->shm_addr) + sizeof(struct pfq_shared_queue);
	q->rx_queue_size = q->rx_slots * q->rx_slot_size;

	q->tx_queue_addr = (char *)(q->shm_addr) + sizeof(struct pfq_shared_queue) + q->rx_queue_size * 2;
	q->tx_queue_size = q->tx_slots * q->tx_slot_size;

	return Q_OK(q);
}


int
pfq_disable(pfq_t *q)
{
	if (q->fd == -1)
		return Q_ERROR(q, "PFQ: socket not open");

	if (q->shm_addr != MAP_FAILED) {

		if (munmap(q->shm_addr,q->shm_size) == -1)
			return Q_ERROR(q, "PFQ: munmap error");

		if (q->hd != -1) {
			char filename[256];
			char *hugepages = hugepages_mountpoint();
			if (hugepages) {
				snprintf(filename, 256, "%s/pfq.%d", hugepages, q->fd);
				unlink(filename);
				free(hugepages);
			}
		}
	}

	q->shm_addr = NULL;
	q->shm_size = 0;

	if(setsockopt(q->fd, PF_Q, Q_SO_DISABLE, NULL, 0) == -1) {
		return Q_ERROR(q, "PFQ: socket disable");
	}
	return Q_OK(q);
}


int
pfq_is_enabled(pfq_t const *q)
{
	if (q->fd != -1)
	{
		int ret; socklen_t size = sizeof(ret);
		if (getsockopt(q->fd, PF_Q, Q_SO_GET_STATUS, &ret, &size) == -1) {
			return Q_ERROR(q, "PFQ: get status error");
		}
		return Q_VALUE(q, ret);
	}
	return Q_OK(q);
}


int
pfq_timestamping_enable(pfq_t *q, int value)
{
	if (setsockopt(q->fd, PF_Q, Q_SO_SET_RX_TSTAMP, &value, sizeof(value)) == -1) {
		return Q_ERROR(q, "PFQ: set timestamp mode");
	}
	return Q_OK(q);
}


int
pfq_is_timestamping_enabled(pfq_t const *q)
{
	int ret; socklen_t size = sizeof(int);

	if (getsockopt(q->fd, PF_Q, Q_SO_GET_RX_TSTAMP, &ret, &size) == -1) {
	        return Q_ERROR(q, "PFQ: get timestamp mode");
	}
	return Q_VALUE(q, ret);
}


int
pfq_set_weight(pfq_t *q, int value)
{
	if (setsockopt(q->fd, PF_Q, Q_SO_SET_WEIGHT, &value, sizeof(value)) == -1) {
		return Q_ERROR(q, "PFQ: set socket weight");
	}
	return Q_OK(q);
}


int
pfq_get_weight(pfq_t const *q)
{
	int ret; socklen_t size = sizeof(ret);

	if (getsockopt(q->fd, PF_Q, Q_SO_GET_WEIGHT, &ret, &size) == -1) {
	        return Q_ERROR(q, "PFQ: get socket weight");
	}
	return Q_VALUE(q, ret);
}

int
pfq_ifindex(pfq_t const *q, const char *dev)
{
	struct ifreq ifreq_io;

	memset(&ifreq_io, 0, sizeof(struct ifreq));
	strncpy(ifreq_io.ifr_name, dev, IFNAMSIZ);
	if (ioctl(q->fd, SIOCGIFINDEX, &ifreq_io) == -1) {
		return Q_ERROR(q, "PFQ: ioctl get ifindex error");
	}
	return Q_VALUE(q, ifreq_io.ifr_ifindex);
}


int
pfq_set_promisc(pfq_t const *q, const char *dev, int value)
{
	struct ifreq ifreq_io;

	memset(&ifreq_io, 0, sizeof(struct ifreq));
	strncpy(ifreq_io.ifr_name, dev, IFNAMSIZ);

	if(ioctl(q->fd, SIOCGIFFLAGS, &ifreq_io) == -1) {
		return Q_ERROR(q, "PFQ: ioctl getflags error");
	}

	if (value)
		ifreq_io.ifr_flags |= IFF_PROMISC;
	else
		ifreq_io.ifr_flags &= ~IFF_PROMISC;

	if(ioctl(q->fd, SIOCSIFFLAGS, &ifreq_io) == -1) {
		return Q_ERROR(q, "PFQ: ioctl setflags error");
	}
	return Q_OK(q);
}


int
pfq_set_caplen(pfq_t *q, size_t value)
{
	int enabled = pfq_is_enabled(q);
	if (enabled == 1) {
		return Q_ERROR(q, "PFQ: enabled (caplen could not be set)");
	}

	if (setsockopt(q->fd, PF_Q, Q_SO_SET_RX_CAPLEN, &value, sizeof(value)) == -1) {
		return Q_ERROR(q, "PFQ: set caplen error");
	}

	q->rx_slot_size = ALIGN(sizeof(struct pfq_pkthdr) + value, 8);
	return Q_OK(q);
}


ssize_t
pfq_get_caplen(pfq_t const *q)
{
	size_t ret; socklen_t size = sizeof(ret);

	if (getsockopt(q->fd, PF_Q, Q_SO_GET_RX_CAPLEN, &ret, &size) == -1) {
		return Q_ERROR(q, "PFQ: get caplen error");
	}
	return Q_VALUE(q, (ssize_t)ret);
}


ssize_t
pfq_get_maxlen(pfq_t const *q)
{
	int ret; socklen_t size = sizeof(ret);

	if (getsockopt(q->fd, PF_Q, Q_SO_GET_TX_MAXLEN, &ret, &size) == -1) {
		return Q_ERROR(q, "PFQ: get maxlen error");
	}
	return Q_VALUE(q, (ssize_t)ret);
}


int
pfq_set_rx_slots(pfq_t *q, size_t value)
{
	int enabled = pfq_is_enabled(q);
	if (enabled == 1) {
		return Q_ERROR(q, "PFQ: enabled (slots could not be set)");
	}
	if (setsockopt(q->fd, PF_Q, Q_SO_SET_RX_SLOTS, &value, sizeof(value)) == -1) {
		return Q_ERROR(q, "PFQ: set Rx slots error");
	}

	q->rx_slots = value;
	return Q_OK(q);
}


size_t
pfq_get_rx_slots(pfq_t const *q)
{
	return q->rx_slots;
}


int
pfq_set_tx_slots(pfq_t *q, size_t value)
{
	int enabled = pfq_is_enabled(q);
	if (enabled == 1) {
		return Q_ERROR(q, "PFQ: enabled (Tx slots could not be set)");
	}
	if (setsockopt(q->fd, PF_Q, Q_SO_SET_TX_SLOTS, &value, sizeof(value)) == -1) {
		return Q_ERROR(q, "PFQ: set Tx slots error");
	}

	q->tx_slots = value;
	return Q_OK(q);
}


size_t
pfq_get_tx_slots(pfq_t const *q)
{
	return q->tx_slots;
}


size_t
pfq_get_rx_slot_size(pfq_t const *q)
{
	return q->rx_slot_size;
}


int
pfq_bind_group(pfq_t *q, int gid, const char *dev, int queue)
{
	struct pfq_binding b;
	int index;

	if (strcmp(dev, "any")==0) {
		index = Q_ANY_DEVICE;
	}
	else {
		index = pfq_ifindex(q, dev);
		if (index == -1) {
			return Q_ERROR(q, "PFQ: bind_group: device not found");
		}
	}

	b.gid     = gid;
	b.ifindex = index;
	b.qindex  = queue;

	if (setsockopt(q->fd, PF_Q, Q_SO_GROUP_BIND, &b, sizeof(b)) == -1) {
		return Q_ERROR(q, "PFQ: bind error");
	}
	return Q_OK(q);
}


int
pfq_bind(pfq_t *q, const char *dev, int queue)
{
	int gid = q->gid;
	if (gid < 0) {
		return Q_ERROR(q, "PFQ: default group undefined");
	}
	return pfq_bind_group(q, gid, dev, queue);
}


int
pfq_egress_bind(pfq_t *q, const char *dev, int queue)
{
	struct pfq_binding b;

	int index;
	if (strcmp(dev, "any")==0) {
		index = Q_ANY_DEVICE;
	}
	else {
		index = pfq_ifindex(q, dev);
		if (index == -1) {
			return Q_ERROR(q, "PFQ: egress_bind: device not found");
		}
	}

	b.gid = 0;
	b.ifindex = index;
	b.qindex = queue;

        if (setsockopt(q->fd, PF_Q, Q_SO_EGRESS_BIND, &b, sizeof(b)) == -1)
		return Q_ERROR(q, "PFQ: egress bind error");

	return Q_OK(q);
}

int
pfq_egress_unbind(pfq_t *q)
{
        if (setsockopt(q->fd, PF_Q, Q_SO_EGRESS_UNBIND, 0, 0) == -1)
		return Q_ERROR(q, "PFQ: egress unbind error");

	return Q_OK(q);
}


int
pfq_unbind_group(pfq_t *q, int gid, const char *dev, int queue) /* Q_ANY_QUEUE */
{
	struct pfq_binding b;

	int index;
	if (strcmp(dev, "any")==0) {
		index = Q_ANY_DEVICE;
	}
	else {
		index = pfq_ifindex(q, dev);
		if (index == -1) {
			return Q_ERROR(q, "PFQ: unbind_group: device not found");
		}
	}

	b.gid = gid;
	b.ifindex = index;
	b.qindex = queue;

	if (setsockopt(q->fd, PF_Q, Q_SO_GROUP_UNBIND, &b, sizeof(b)) == -1) {
		return Q_ERROR(q, "PFQ: unbind error");
	}
	return Q_OK(q);
}


int
pfq_unbind(pfq_t *q, const char *dev, int queue)
{
	int gid = q->gid;
	if (gid < 0) {
		return Q_ERROR(q, "PFQ: default group undefined");
	}
	return pfq_unbind_group(q, gid, dev, queue);
}


int
pfq_groups_mask(pfq_t const *q, unsigned long *_mask)
{
	unsigned long mask; socklen_t size = sizeof(mask);

	if (getsockopt(q->fd, PF_Q, Q_SO_GET_GROUPS, &mask, &size) == -1) {
		return Q_ERROR(q, "PFQ: get groups error");
	}
	*_mask = mask;
	return Q_OK(q);
}


static int
popen2(const char *command, struct popen2 *childinfo)
{
	int pipe_stdin[2], pipe_stdout[2];
	pid_t p;

	if(pipe(pipe_stdin))
		return -1;

	if(pipe(pipe_stdout)) {
		close(pipe_stdin[0]);
		close(pipe_stdin[1]);
		return -1;
	}

	p = fork();
	if(p < 0)
		return p; /* Fork failed */

	if(p == 0) {
		close(pipe_stdin[1]);
		dup2(pipe_stdin[0], 0);
		close(pipe_stdout[0]);
		dup2(pipe_stdout[1], 1);
		if (execl("/bin/sh", "sh", "-c", command, NULL) < 0)
			return -1;
		exit(1);
	}

	close(pipe_stdin[0]);
	close(pipe_stdout[1]);

	childinfo->child_pid = p;
	childinfo->to_child = pipe_stdin[1];
	childinfo->from_child = pipe_stdout[0];
	return 0;
}


int
pfq_set_group_computation(pfq_t *q, int gid, struct pfq_lang_computation_descr *comp)
{
        struct pfq_group_computation p = { gid, comp };

        if (setsockopt(q->fd, PF_Q, Q_SO_GROUP_FUNCTION, &p, sizeof(p)) == -1) {
		return Q_ERROR(q, "PFQ: group computation error");
        }

	return Q_OK(q);
}


#define PFQ_ALLOCA(type, value) ({		\
	void * new = alloca(sizeof(type));      \
	*(type *)new = value;			\
	new;					\
})

int
pfq_set_group_computation_from_json(pfq_t *q, int gid, const char *input)
{
	JSON_Value * root = json_parse_string(input);
	JSON_Array * funs;
	struct pfq_lang_computation_descr *prog;
	size_t n;

	printf("%s\n", input);

	if (json_value_get_type(root) != JSONArray) {
		json_value_free(root);
		return Q_ERROR(q, "PFQ: computation: JSON parse error");
	}

	funs = json_value_get_array(root);
	prog = alloca(sizeof(size_t) * 2 + sizeof(struct pfq_lang_functional_descr) * json_array_get_count(funs));
        if (!prog) {
		json_value_free(root);
		return Q_ERROR(q, "PFQ: computation: JSON alloca!");
	}

	prog->entry_point = 0;
	prog->size = json_array_get_count(funs);

	for(n = 0; n < json_array_get_count(funs); ++n)
	{
		JSON_Object *fun = json_array_get_object(funs, n);
		JSON_Array  *args;
		size_t i;

		prog->fun[n].symbol = json_object_get_string   (fun, "funSymbol");
                prog->fun[n].next = (int)json_object_get_number(fun, "funLink");

		args   = json_object_get_array(fun, "funArgs");

		if (!args) {
			json_value_free(root);
			return Q_ERROR(q, "PFQ: computation: JSON funArgs missing!");
		}

		for(i = 0; i < json_array_get_count(args); ++i)
		{
			JSON_Object *arg = json_array_get_object(args, i);
			const char *type;
			type = json_object_get_string(arg, "argType");
			if (!type) {
				json_value_free(root);
				return Q_ERROR(q, "PFQ: computation: JSON argType missing!");
			}

			if (strcmp(type, "CInt") == 0)
			{
				JSON_Value * value = json_object_get_value(arg, "argValue");
				if (!value) {
					json_value_free(root);
					return Q_ERROR(q, "PFQ: computation: JSON argValue missing!");
				}

				prog->fun[n].arg[i].addr  = PFQ_ALLOCA(int, (int)json_value_get_number(value));
				prog->fun[n].arg[i].size  = sizeof(int);
				prog->fun[n].arg[i].nelem = -1;
			}
			else if (strcmp(type, "Int64") == 0)
			{
				JSON_Value * value = json_object_get_value(arg, "argValue");
				if (!value) {
					json_value_free(root);
					return Q_ERROR(q, "PFQ: computation: JSON argValue missing!");
				}

				prog->fun[n].arg[i].addr  = PFQ_ALLOCA(int64_t, (int64_t)json_value_get_number(value));
				prog->fun[n].arg[i].size  = sizeof(int64_t);
				prog->fun[n].arg[i].nelem = -1;
			}
			else if (strcmp(type, "Int32") == 0)
			{
				JSON_Value * value = json_object_get_value(arg, "argValue");
				if (!value) {
					json_value_free(root);
					return Q_ERROR(q, "PFQ: computation: JSON argValue missing!");
				}

				prog->fun[n].arg[i].addr  = PFQ_ALLOCA(int32_t, (int32_t)json_value_get_number(value));
				prog->fun[n].arg[i].size  = sizeof(int32_t);
				prog->fun[n].arg[i].nelem = -1;
			}
			else if (strcmp(type, "Int16") == 0)
			{
				JSON_Value * value = json_object_get_value(arg, "argValue");
				if (!value) {
					json_value_free(root);
					return Q_ERROR(q, "PFQ: computation: JSON argValue missing!");
				}

				prog->fun[n].arg[i].addr  = PFQ_ALLOCA(int16_t, (int16_t)json_value_get_number(value));
				prog->fun[n].arg[i].size  = sizeof(int16_t);
				prog->fun[n].arg[i].nelem = -1;
			}
			else if (strcmp(type, "Int8") == 0)
			{
				JSON_Value * value = json_object_get_value(arg, "argValue");
				if (!value) {
					json_value_free(root);
					return Q_ERROR(q, "PFQ: computation: JSON argValue missing!");
				}

				prog->fun[n].arg[i].addr  = PFQ_ALLOCA(int8_t, (int8_t)json_value_get_number(value));
				prog->fun[n].arg[i].size  = sizeof(int8_t);
				prog->fun[n].arg[i].nelem = -1;
			}
			else if (strcmp(type, "Word64") == 0)
			{
				JSON_Value * value = json_object_get_value(arg, "argValue");
				if (!value) {
					json_value_free(root);
					return Q_ERROR(q, "PFQ: computation: JSON argValue missing!");
				}

				prog->fun[n].arg[i].addr  = PFQ_ALLOCA(uint64_t, (uint64_t)json_value_get_number(value));
				prog->fun[n].arg[i].size  = sizeof(uint64_t);
				prog->fun[n].arg[i].nelem = -1;
			}
			else if (strcmp(type, "Word32") == 0)
			{
				JSON_Value * value = json_object_get_value(arg, "argValue");
				if (!value) {
					json_value_free(root);
					return Q_ERROR(q, "PFQ: computation: JSON argValue missing!");
				}

				prog->fun[n].arg[i].addr  = PFQ_ALLOCA(uint32_t, (uint32_t)json_value_get_number(value));
				prog->fun[n].arg[i].size  = sizeof(uint32_t);
				prog->fun[n].arg[i].nelem = -1;
			}
			else if (strcmp(type, "Word16") == 0)
			{
				JSON_Value * value = json_object_get_value(arg, "argValue");
				if (!value) {
					json_value_free(root);
					return Q_ERROR(q, "PFQ: computation: JSON argValue missing!");
				}

				prog->fun[n].arg[i].addr  = PFQ_ALLOCA(uint16_t, (uint16_t)json_value_get_number(value));
				prog->fun[n].arg[i].size  = sizeof(uint16_t);
				prog->fun[n].arg[i].nelem = -1;
			}
			else if (strcmp(type, "Word8") == 0)
			{
				JSON_Value * value = json_object_get_value(arg, "argValue");
				if (!value) {
					json_value_free(root);
					return Q_ERROR(q, "PFQ: computation: JSON argValue missing!");
				}

				prog->fun[n].arg[i].addr  = PFQ_ALLOCA(uint8_t, (uint8_t)json_value_get_number(value));
				prog->fun[n].arg[i].size  = sizeof(uint8_t);
				prog->fun[n].arg[i].nelem = -1;
			}
			else if (strcmp(type, "Fun") == 0)
			{
				JSON_Value * value = json_object_get_value(arg, "argValue");
				if (!value) {
					json_value_free(root);
					return Q_ERROR(q, "PFQ: computation: JSON argValue missing!");
				}

				prog->fun[n].arg[i].addr  = NULL;
				prog->fun[n].arg[i].size  = (size_t)json_value_get_number(value);
				prog->fun[n].arg[i].nelem = -1;
			}
			else if (strcmp(type, "String") == 0)
			{
				JSON_Value * value = json_object_get_value(arg, "argValue");
				if (!value) {
					json_value_free(root);
					return Q_ERROR(q, "PFQ: computation: JSON argValue missing!");
				}

				prog->fun[n].arg[i].addr  = json_value_get_string(value);
				prog->fun[n].arg[i].size  = 0;
				prog->fun[n].arg[i].nelem = -1;
			}
			else if (strcmp(type, "IPv4") == 0)
			{
				JSON_Object * obj = json_object_get_object(arg, "argValue");
				JSON_Value  * value;
				if (!obj) {
					json_value_free(root);
					return Q_ERROR(q, "PFQ: computation: JSON argValue missing!");
				}

				value = json_object_get_value(obj, "getHostAddress");
                                if (!value) {
					json_value_free(root);
					return Q_ERROR(q, "PFQ: computation: JSON IPv4 internal error!");
				}

				prog->fun[n].arg[i].addr  = PFQ_ALLOCA(uint32_t, (uint32_t)json_value_get_number(value));
				prog->fun[n].arg[i].size  = sizeof(uint32_t);
				prog->fun[n].arg[i].nelem = -1;
			}
			else if (strlen(type) == 0)
			{
				prog->fun[n].arg[i].addr  = NULL;
				prog->fun[n].arg[i].size  = 0;
				prog->fun[n].arg[i].nelem = 0;
			}
			else {
				static __thread char *e = NULL; free(e);
				asprintf(&e, "PFQ: computation: JSON unknown argType: %s!", type);
				json_value_free(root);
				return Q_ERROR(q, e);
			}
		}

		printf("%.10s -> %d\n", prog->fun[n].symbol, prog->fun[n].next);
	}

	return pfq_set_group_computation(q, gid, prog);
}

int
pfq_set_group_computation_from_string(pfq_t *q, int gid, const char *comp)
{
	ssize_t chunk, size = 0, max_size = 4096;
	struct popen2 p;
	char *page;
	int status;

	page = malloc((size_t)max_size);
	if (!page)
		return Q_ERROR(q, "PFQ: computation_from_string: memory error");

	if (popen2("qlang --json", &p) < 0) {
		free(page);
		return Q_ERROR(q, "PFQ: computation_from_string: popen2 error");
	}

	if (write(p.to_child, comp, strlen(comp)) < 0) {
		free(page);
		return Q_ERROR(q, "PFQ: computation_from_string: write error");
	}

	close(p.to_child);

	if (waitpid(p.child_pid, &status, 0) < 0) {
		free(page);
		return Q_ERROR(q, "PFQ: computation_from_string: waitpid error");
	}

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		free(page);
		return Q_ERROR(q, "PFQ: computation_from_string: qlang compiler error!");
	}

	while((chunk=read(p.from_child, page + size, (size_t)(max_size -size))) > 0)
	{
		size += chunk;
		if (size == max_size) {
			max_size += 4096;
			page = realloc(page, (size_t)max_size);
			if (!page)
				return Q_ERROR(q, "PFQ: set_group_computation_from_string: realloc");
		}
	}

	close(p.from_child);
	*(page+size) = '\0';

	status = pfq_set_group_computation_from_json(q, gid, page);

	free(page);
	return status;
}


int
pfq_group_fprog(pfq_t *q, int gid, struct sock_fprog *f)
{
	struct pfq_fprog fprog;

	fprog.gid = gid;
	if (f != NULL)
	{
		fprog.fcode.len = f->len;
		fprog.fcode.filter = f->filter;
	}
	else
	{
		fprog.fcode.len = 0;
		fprog.fcode.filter = NULL;
	}

        if (setsockopt(q->fd, PF_Q, Q_SO_GROUP_FPROG, &fprog, sizeof(fprog)) == -1) {
		return Q_ERROR(q, "PFQ: set group fprog error");
	}

	return Q_OK(q);
}


int
pfq_group_fprog_reset(pfq_t *q, int gid)
{
	struct sock_fprog null = { 0, NULL };

	return pfq_group_fprog(q, gid, &null);
}


int
pfq_join_group(pfq_t *q, int gid, unsigned long class_mask, int group_policy)
{
	if (group_policy == Q_POLICY_GROUP_UNDEFINED) {
		return Q_ERROR(q, "PFQ: join with undefined policy!");
	}

	struct pfq_group_join group = { gid, group_policy, class_mask };

	socklen_t size = sizeof(group);
	if (getsockopt(q->fd, PF_Q, Q_SO_GROUP_JOIN, &group, &size) == -1) {
	        return Q_ERROR(q, "PFQ: join group error");
	}

        if (q->gid == -1)
                q->gid = group.gid;

	return Q_VALUE(q, group.gid);
}


int
pfq_leave_group(pfq_t *q, int gid)
{
	if (setsockopt(q->fd, PF_Q, Q_SO_GROUP_LEAVE, &gid, sizeof(gid)) == -1) {
	        return Q_ERROR(q, "PFQ: leave group error");
	}
	if (q->gid == gid)
	        q->gid = -1;

	return Q_OK(q);
}


int
pfq_poll(pfq_t *q, long int microseconds /* = -1 -> infinite */)
{
	struct timespec timeout;
	struct pollfd fd = {q->fd, POLLIN, 0 };
        int ret;

	if (q->fd == -1) {
		return Q_ERROR(q, "PFQ: socket not open");
	}

	if (microseconds >= 0) {
		timeout.tv_sec  = microseconds/1000000;
		timeout.tv_nsec = (microseconds%1000000) * 1000;
	}

	ret = ppoll(&fd, 1, microseconds < 0 ? NULL : &timeout, NULL);
	if (ret < 0 && errno != EINTR) {
	    return Q_ERROR(q, "PFQ: ppoll error");
	}
	return Q_OK(q);
}


int
pfq_get_stats(pfq_t const *q, struct pfq_stats *stats)
{
	socklen_t size = sizeof(struct pfq_stats);
	if (getsockopt(q->fd, PF_Q, Q_SO_GET_STATS, stats, &size) == -1) {
		return Q_ERROR(q, "PFQ: get stats error");
	}
	return Q_OK(q);
}


int
pfq_get_group_stats(pfq_t const *q, int gid, struct pfq_stats *stats)
{
	socklen_t size = sizeof(struct pfq_stats);

	stats->recv = (unsigned int)gid;
	if (getsockopt(q->fd, PF_Q, Q_SO_GET_GROUP_STATS, stats, &size) == -1) {
		return Q_ERROR(q, "PFQ: get group stats error");
	}
	return Q_OK(q);
}


int
pfq_get_group_counters(pfq_t const *q, int gid, struct pfq_counters *cs)
{
	socklen_t size = sizeof(struct pfq_counters);

	cs->counter[0] = (unsigned int)gid;

	if (getsockopt(q->fd, PF_Q, Q_SO_GET_GROUP_COUNTERS, cs, &size) == -1) {
		return Q_ERROR(q, "PFQ: get group counters error");
	}
	return Q_OK(q);
}


int
pfq_vlan_filters_enable(pfq_t *q, int gid, int toggle)
{
        struct pfq_vlan_toggle value = { gid, 0, toggle };

        if (setsockopt(q->fd, PF_Q, Q_SO_GROUP_VLAN_FILT_TOGGLE, &value, sizeof(value)) == -1) {
	        return Q_ERROR(q, "PFQ: vlan filters");
        }

        return Q_OK(q);
}

int
pfq_vlan_set_filter(pfq_t *q, int gid, int vid)
{
        struct pfq_vlan_toggle value = { gid, vid, 1 };

        if (setsockopt(q->fd, PF_Q, Q_SO_GROUP_VLAN_FILT, &value, sizeof(value)) == -1) {
	        return Q_ERROR(q, "PFQ: vlan set filter");
        }

        return Q_OK(q);
}

int pfq_vlan_reset_filter(pfq_t *q, int gid, int vid)
{
        struct pfq_vlan_toggle value = { gid, vid, 0 };

        if (setsockopt(q->fd, PF_Q, Q_SO_GROUP_VLAN_FILT, &value, sizeof(value)) == -1) {
	        return Q_ERROR(q, "PFQ: vlan reset filter");
        }

        return Q_OK(q);
}


int
pfq_read(pfq_t *q, struct pfq_net_queue *nq, long int microseconds)
{
	struct pfq_shared_queue * qd;
	unsigned int index, data;

        if (q->shm_addr == NULL) {
		return Q_ERROR(q, "PFQ: read: socket not enabled");
	}

	qd = (struct pfq_shared_queue *)(q->shm_addr);

	data = __atomic_load_n(&qd->rx.data, __ATOMIC_RELAXED);
	index = Q_SHARED_QUEUE_INDEX(data);

        /* at wrap-around reset Rx slots... */

        if (((index+1) & 0xfe)== 0)
        {
            char * raw = (char *)(q->rx_queue_addr) + ((index+1) & 1) * q->rx_queue_size;
            char * end = raw + q->rx_queue_size;
            const uint8_t rst = index & 1;
            for(; raw < end; raw += q->rx_slot_size)
                ((struct pfq_pkthdr *)raw)->commit = rst;
        }

	if (Q_SHARED_QUEUE_LEN(data) == 0) {
#ifdef PFQ_USE_POLL
		if (pfq_poll(q, microseconds) < 0)
			return Q_ERROR(q, "PFQ: poll error");
#else
		(void)microseconds;
		nq->len = 0;
		return Q_VALUE(q, (int)0);
#endif
	}

	/* swap the queue... */

        data = __atomic_exchange_n(&qd->rx.data, (unsigned int)((index+1) << 24), __ATOMIC_RELAXED);

	size_t queue_len = min(Q_SHARED_QUEUE_LEN(data), q->rx_slots);

	nq->queue = (char *)(q->rx_queue_addr) + (index & 1) * q->rx_queue_size;
	nq->index = index;
	nq->len = queue_len;
        nq->slot_size = q->rx_slot_size;

	return Q_VALUE(q, (int)queue_len);
}


int
pfq_recv(pfq_t *q, void *buf, size_t buflen, struct pfq_net_queue *nq, long int microseconds)
{
	if (buflen < (q->rx_slots * q->rx_slot_size)) {
		return Q_ERROR(q, "PFQ: buffer too small");
	}

	if (pfq_read(q, nq, microseconds) < 0)
		return -1;

	memcpy(buf, nq->queue, q->rx_slot_size * nq->len);
	return Q_OK(q);
}


int
pfq_dispatch(pfq_t *q, pfq_handler_t cb, long int microseconds, char *user)
{
	pfq_iterator_t it, it_end;
	int n = 0;

	if (pfq_read(q, &q->nq, microseconds) < 0)
		return -1;

	it = pfq_net_queue_begin(&q->nq);
	it_end = pfq_net_queue_end(&q->nq);

	for(; it != it_end; it = pfq_net_queue_next(&q->nq, it))
	{
		while (!pfq_pkt_ready(&q->nq, it))
			pfq_yield();

		cb(user, pfq_pkt_header(it), pfq_pkt_data(it));
		n++;
	}
        return Q_VALUE(q, n);
}


int
pfq_bind_tx(pfq_t *q, const char *dev, int queue, int tid)
{
	struct pfq_binding b;
        int ifindex;

        ifindex = pfq_ifindex(q, dev);
        if (ifindex == -1)
		return Q_ERROR(q, "PFQ: device not found");

	b = (struct pfq_binding){ {tid}, ifindex, queue };

        if (setsockopt(q->fd, PF_Q, Q_SO_TX_BIND, &b, sizeof(b)) == -1)
		return Q_ERROR(q, "PFQ: Tx bind error");

	if (tid != Q_NO_KTHREAD)
		q->tx_num_async++;

	return Q_OK(q);
}


int
pfq_unbind_tx(pfq_t *q)
{
        if (setsockopt(q->fd, PF_Q, Q_SO_TX_UNBIND, NULL, 0) == -1)
		return Q_ERROR(q, "PFQ: Tx unbind error");

	q->tx_num_async = 0;
	return Q_OK(q);
}


int
pfq_send_raw(pfq_t *q, const void *buf, size_t len, int ifindex, int qindex, uint64_t nsec,
	     unsigned int copies, int async, int queue)
{
        struct pfq_shared_queue *sh_queue = (struct pfq_shared_queue *)(q->shm_addr);
        struct pfq_tx_queue *tx;
        unsigned int index;
        size_t slot_size;
        char *base_addr;
        ptrdiff_t offset;
        int tss;

	if (unlikely(q->shm_addr == NULL))
		return Q_ERROR(q, "PFQ: send_deferred: socket not enabled");

	if (async) {
		if (unlikely(q->tx_num_async == 0))
			return Q_ERROR(q, "PFQ: send_deferred: socket not bound to async thread");

		tss = (int)pfq_fold((queue == Q_ANY_QUEUE ? pfq_symmetric_hash(buf) : (unsigned int)queue),
										      (unsigned int)q->tx_num_async);

		tx = (struct pfq_tx_queue *)&sh_queue->tx_async[tss];
	}
	else {
		tss = -1;
		tx = (struct pfq_tx_queue *)&sh_queue->tx;
	}

	index = __atomic_load_n(&tx->cons.index, __ATOMIC_RELAXED);
	if (index != __atomic_load_n(&tx->prod.index, __ATOMIC_RELAXED))
	{
                __atomic_store_n(&tx->prod.index, index, __ATOMIC_RELAXED);
                __atomic_store_n((index & 1) ? &tx->prod.off1 : &tx->prod.off0, 0, __ATOMIC_RELAXED);
	}


	base_addr = q->tx_queue_addr + q->tx_queue_size * (size_t)(2 * (1+tss) + (index & 1 ? 1 : 0));

        offset = __atomic_load_n((index & 1) ? &tx->prod.off1 : &tx->prod.off0, __ATOMIC_RELAXED);

	len = min(len, q->tx_slot_size - sizeof(struct pfq_pkthdr));

	slot_size = sizeof(struct pfq_pkthdr) + ALIGN(len, 8);

	if (((size_t)(offset) + slot_size) < q->tx_queue_size)
	{
		struct pfq_pkthdr *hdr = (struct pfq_pkthdr *)(base_addr + offset);
		hdr->tstamp.tv64 = nsec;
		hdr->caplen = (uint16_t)len;
		hdr->data.copies = copies;
                hdr->ifindex = ifindex;
                hdr->queue = (uint8_t)qindex;
		memcpy(hdr+1, buf, len);

                __atomic_store_n((index & 1) ? &tx->prod.off1 : &tx->prod.off0,
			offset + (ptrdiff_t)slot_size, __ATOMIC_RELEASE);


		return Q_VALUE(q, (int)len);
	}

	return Q_VALUE(q, -1);
}

int
pfq_send(pfq_t *q, const void *ptr, size_t len, size_t fhint, unsigned int copies)
{
	int ret = pfq_send_raw(q, ptr, len, 0, 0, 0, copies, 0, Q_ANY_QUEUE);
	if (++q->tx_attempt == fhint) {
		q->tx_attempt = 0;
		pfq_transmit_queue(q, 0);
	}
	return ret;
}

int
pfq_send_to(pfq_t *q, const void *ptr, size_t len, int ifindex, int qindex, size_t fhint, unsigned int copies)
{
	int ret = pfq_send_raw(q, ptr, len, ifindex, qindex, 0, copies, 0, Q_ANY_QUEUE);
	if (++q->tx_attempt == fhint) {
		q->tx_attempt = 0;
		pfq_transmit_queue(q, 0);
	}
	return ret;
}

int
pfq_transmit_queue(pfq_t *q, int queue)
{
        if (setsockopt(q->fd, PF_Q, Q_SO_TX_QUEUE, &queue, sizeof(queue)) == -1)
		return Q_ERROR(q, "PFQ: Tx queue");
        return Q_OK(q);
}


size_t
pfq_mem_size(pfq_t const *q)
{
	return q->shm_size;
}


const void *
pfq_mem_addr(pfq_t const *q)
{
	return q->shm_addr;
}


int
pfq_id(pfq_t *q)
{
	return q->id;
}


int
pfq_group_id(pfq_t *q)
{
	return q->gid;
}


int pfq_get_fd(pfq_t const *q)
{
	return q->fd;
}
