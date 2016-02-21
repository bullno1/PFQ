/***************************************************************
 *
 * (C) 2011-15 Nicola Bonelli <nicola@pfq.io>
 *             Andrea Di Pietro <andrea.dipietro@for.unipi.it>
 * 	       Loris Gazzarrini <loris.gazzarrini@iet.unipi.it>
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
#include <linux/version.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/semaphore.h>
#include <linux/rwsem.h>
#include <linux/socket.h>
#include <linux/types.h>
#include <linux/skbuff.h>
#include <linux/highmem.h>
#include <linux/ioctl.h>
#include <linux/ip.h>
#include <linux/poll.h>
#include <linux/etherdevice.h>
#include <linux/kthread.h>
#include <linux/vmalloc.h>
#include <linux/percpu.h>
#include <linux/bug.h>

#include <net/sock.h>
#ifdef CONFIG_INET
#include <net/inet_common.h>
#endif

#include <linux/pf_q.h>

#include <pragma/diagnostic_pop>

#include <pf_q-shmem.h>
#include <pf_q-proc.h>
#include <pf_q-define.h>
#include <pf_q-sockopt.h>
#include <pf_q-devmap.h>
#include <pf_q-group.h>
#include <pf_q-bitops.h>
#include <pf_q-bpf.h>
#include <pf_q-memory.h>
#include <pf_q-sock.h>
#include <pf_q-thread.h>
#include <pf_q-global.h>
#include <pf_q-vlan.h>
#include <pf_q-stats.h>
#include <pf_q-endpoint.h>
#include <pf_q-shared-queue.h>
#include <pf_q-pool.h>
#include <pf_q-transmit.h>
#include <pf_q-percpu.h>

#include <lang/engine.h>
#include <lang/symtable.h>
#include <lang/GC.h>

static struct packet_type       pfq_prot_hook;


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nicola Bonelli <nicola@pfq.io>");
MODULE_DESCRIPTION("Functional Networking Framework for Multi-core Architectures");

#ifdef PFQ_DEBUG
#pragma message "[PFQ] *** PFQ_DEBUG mode ***"
#endif
#ifdef DEBUG
#pragma message "[PFQ] *** DEBUG mode ***"
#endif

static DEFINE_SEMAPHORE(sock_sem);

void pfq_timer(unsigned long cpu);

/* send this packet to selected sockets */

static inline
void mask_to_sock_queue(unsigned long n, unsigned long mask, unsigned long long *sock_queue)
{
	unsigned long bit;
	pfq_bitwise_foreach(mask, bit,
	{
	        int index = pfq_ctz(bit);
                sock_queue[index] |= 1UL << n;
        })
}

/*
 * Find the next power of two.
 * from "Hacker's Delight, Henry S. Warren."
 */

static inline
unsigned clp2(unsigned int x)
{
        x = x - 1;
        x = x | (x >> 1);
        x = x | (x >> 2);
        x = x | (x >> 4);
        x = x | (x >> 8);
        x = x | (x >> 16);
        return x + 1;
}


/*
 * Optimized folding operation...
 */

static inline
unsigned int pfq_fold(unsigned int a, unsigned int b)
{
	unsigned int c;
	if (b == 1)
		return 0;
        c = b - 1;
        if (likely((b & c) == 0))
		return a & c;
        switch(b)
        {
        case 3:  return a % 3;
        case 5:  return a % 5;
        case 6:  return a % 6;
        case 7:  return a % 7;
        default: {
                const unsigned int p = clp2(b);
                const unsigned int r = a & (p-1);
                return r < b ? r : a % b;
            }
        }
}


static int
pfq_receive_batch(struct pfq_percpu_data *data,
		  struct pfq_percpu_sock *sock,
		  struct pfq_percpu_pool *pool,
		  struct GC_data *GC_ptr,
		  int cpu)
{
	unsigned long long sock_queue[Q_SKBUFF_BATCH];
        unsigned long group_mask, socket_mask;
	struct pfq_endpoint_info endpoints;
        struct sk_buff *skb;
	struct sk_buff __GC * buff;

        long unsigned n, bit, lb;
	size_t this_batch_len;
	struct pfq_lang_monad monad;

#ifdef PFQ_RX_PROFILE
	cycles_t start, stop;
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,9,0))
	BUILD_BUG_ON_MSG(Q_SKBUFF_BATCH > (sizeof(sock_queue[0]) << 3), "skbuff batch overflow");
#endif

	this_batch_len = GC_size(GC_ptr);

	__sparse_add(&global_stats, recv, this_batch_len, cpu);

	/* cleanup sock_queue... */

        memset(sock_queue, 0, sizeof(sock_queue));
	group_mask = 0;

#ifdef PFQ_RX_PROFILE
	start = get_cycles();
#endif

        /* setup all the skbs collected */

	for_each_skbuff(SKBUFF_QUEUE_ADDR(GC_ptr->pool), skb, n)
        {
		uint16_t queue = skb_rx_queue_recorded(skb) ? skb_get_rx_queue(skb) : 0;
		unsigned long local_group_mask = pfq_devmap_get_groups(skb->dev->ifindex, queue);
		group_mask |= local_group_mask;
		PFQ_CB(skb)->group_mask = local_group_mask;
		PFQ_CB(skb)->monad = &monad;
		PFQ_CB(skb)->counter = data->counter++;
	}

        /* process all groups enabled for this batch */

	pfq_bitwise_foreach(group_mask, bit,
	{
		pfq_gid_t gid = { pfq_ctz(bit) };

		struct pfq_group * this_group = pfq_get_group(gid);
		bool bf_filt_enabled = atomic_long_read(&this_group->bp_filter);
		bool vlan_filt_enabled = pfq_vlan_filters_enabled(gid);
		struct GC_skbuff_batch refs = { len:0 };

		socket_mask = 0;

		for_each_skbuff_upto(this_batch_len, &GC_ptr->pool, buff, n)
		{
			struct pfq_lang_computation_tree *prg;
			unsigned long sock_mask = 0;

			/* skip this packet for this group ? */

			if ((PFQ_CB(buff)->group_mask & bit) == 0) {
				refs.queue[refs.len++] = NULL;
				continue;
			}

			/* increment counter for this group */

			__sparse_inc(this_group->stats, recv, cpu);

			/* check if bp filter is enabled */

			if (bf_filt_enabled) {
				struct sk_filter *bpf = (struct sk_filter *)atomic_long_read(&this_group->bp_filter);

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3,15,0))
				if (bpf && !sk_run_filter(buff, bpf->insns))
#else
				if (bpf && !SK_RUN_FILTER(bpf, PFQ_SKB(buff)))
#endif
				{
					__sparse_inc(this_group->stats, drop, cpu);
					refs.queue[refs.len++] = NULL;
					continue;
				}
			}

			/* check vlan filter */

			if (vlan_filt_enabled) {
				if (!pfq_check_group_vlan_filter(gid, buff->vlan_tci & ~VLAN_TAG_PRESENT)) {
					__sparse_inc(this_group->stats, drop, cpu);
					refs.queue[refs.len++] = NULL;
					continue;
				}
			}

			/* evaluate the computation of the current group */

			PFQ_CB(buff)->state = 0;

			prg = (struct pfq_lang_computation_tree *)atomic_long_read(&this_group->comp);
			if (prg) {
				unsigned long cbit, eligible_mask = 0;
				size_t to_kernel = PFQ_CB(buff)->log->to_kernel;
				size_t num_fwd = PFQ_CB(buff)->log->num_devs;

				/* setup monad for this computation */

				monad.fanout.class_mask = Q_CLASS_DEFAULT;
				monad.fanout.type = fanout_copy;
				monad.group = this_group;
                                monad.state = 0;

				/* run the functional program */

				buff = pfq_lang_run(buff, prg).skb;
				if (buff == NULL) {
					__sparse_inc(this_group->stats, drop, cpu);
					refs.queue[refs.len++] = NULL;
					continue;
				}

				/* park the monad state */

				PFQ_CB(buff)->state = monad.state;

				/* update stats */

                                __sparse_add(this_group->stats, frwd, PFQ_CB(buff)->log->num_devs -num_fwd, cpu);
                                __sparse_add(this_group->stats, kern, PFQ_CB(buff)->log->to_kernel -to_kernel, cpu);

				/* skip the packet? */

				if (is_drop(monad.fanout)) {
					__sparse_inc(this_group->stats, drop, cpu);
					refs.queue[refs.len++] = NULL;
					continue;
				}

				/* save a reference to the current packet */

				refs.queue[refs.len++] = buff;

				/* compute the eligible mask of sockets enabled for this packet... */

				pfq_bitwise_foreach(monad.fanout.class_mask, cbit,
				{
					int class = pfq_ctz(cbit);
					eligible_mask |= atomic_long_read(&this_group->sock_id[class]);
				})

				/* logical dependency: when sock_masks of a
				 * given group is modified, it is necessary to
				 * invalidate the per-cpu sock->eligible_mask cache */

				if (is_steering(monad.fanout)) { /* cache the number of sockets in the mask */

					if (eligible_mask != sock->eligible_mask) {
						unsigned long ebit;
						sock->eligible_mask = eligible_mask;
						sock->cnt = 0;
						pfq_bitwise_foreach(eligible_mask, ebit,
						{
							pfq_id_t id = pfq_ctz(ebit);
							struct pfq_sock * so = pfq_get_sock_by_id(id);
                                                        int i;

							/* max weight = Q_MAX_SOCK_MASK / Q_MAX_ID */

							for(i = 0; i < so->weight; ++i)
								sock->mask[sock->cnt++] = ebit;
						})
					}

					if (likely(sock->cnt)) {
						unsigned int hash = monad.fanout.hash;
						unsigned int h = hash ^ (hash >> 8) ^ (hash >> 16) ^ (hash >> 24);
						sock_mask |= sock->mask[pfq_fold(h, sock->cnt)];
					}
				}
				else {  /* clone or continue ... */

					sock_mask |= eligible_mask;
				}
			}
			else {
				/* save a reference to the current packet */
				refs.queue[refs.len++] = buff;
				sock_mask |= atomic_long_read(&this_group->sock_id[0]);
			}

			mask_to_sock_queue(n, sock_mask, sock_queue);
			socket_mask |= sock_mask;
		}

		/* copy payloads to endpoints... */

		pfq_bitwise_foreach(socket_mask, lb,
		{
			pfq_id_t id = pfq_ctz(lb);
			struct pfq_sock * so = pfq_get_sock_by_id(id);
			copy_to_endpoint_skbs(so, SKBUFF_GC_QUEUE_ADDR(refs), sock_queue[(int __force)id], cpu, gid);
		})
	})

	/* forward skbs to network devices */

	GC_get_lazy_endpoints(GC_ptr, &endpoints);

	if (endpoints.cnt_total)
	{
		size_t total = pfq_skb_queue_lazy_xmit_run(SKBUFF_GC_QUEUE_ADDR(GC_ptr->pool), &endpoints);

		__sparse_add(&global_stats, frwd, total, cpu);
		__sparse_add(&global_stats, disc, endpoints.cnt_total - total, cpu);
	}

	/* forward skbs to kernel or to the pool */

	for_each_skbuff(SKBUFF_QUEUE_ADDR(GC_ptr->pool), skb, n)
	{
		struct pfq_cb *cb = PFQ_CB(skb);

		/* send a copy of this skb to the kernel */

		if (cb->direct && fwd_to_kernel(skb)) {
		        __sparse_inc(&global_stats, kern, cpu);
			skb_pull(skb, skb->mac_len);
			skb->peeked = capture_incoming;
			netif_receive_skb(skb);
		}
		else {
			pfq_kfree_skb_pool(skb, &pool->rx_pool);
		}
	}

	/* reset the GC */

	GC_reset(GC_ptr);

#ifdef PFQ_RX_PROFILE
	stop = get_cycles();
	if (printk_ratelimit())
		printk(KERN_INFO "[PFQ] Rx profile: %llu_tsc.\n", (stop-start)/batch_len);
#endif
        return 0;
}


static int
pfq_receive(struct napi_struct *napi, struct sk_buff * skb, int direct)
{
	struct pfq_percpu_data * data;
	int cpu;

	/* if no socket is open drop the packet */

	if (unlikely(pfq_get_sock_count() == 0)) {
		sparse_inc(&memory_stats, os_free);
		kfree_skb(skb);
		return 0;
	}

        cpu = smp_processor_id();
	data = per_cpu_ptr(percpu_data, cpu);

	if (likely(skb))
	{
		struct sk_buff __GC * buff;

		/* if required, timestamp the packet now */

		if (skb->tstamp.tv64 == 0)
			__net_timestamp(skb);

		/* if vlan header is present, remove it */

		if (vl_untag && skb->protocol == cpu_to_be16(ETH_P_8021Q)) {
			skb = pfq_vlan_untag(skb);
			if (unlikely(!skb)) {
				__sparse_inc(&global_stats, lost, cpu);
				return -1;
			}
		}

		skb_reset_mac_len(skb);

		/* push the mac header: reset skb->data to the beginning of the packet */

		if (likely(skb->pkt_type != PACKET_OUTGOING))
		    skb_push(skb, skb->mac_len);

		/* pass the ownership of this skb to the garbage collector */

		buff = GC_make_buff(data->GC, skb);
		if (buff == NULL) {
			if (printk_ratelimit())
				printk(KERN_INFO "[PFQ] GC: memory exhausted!\n");
			__sparse_inc(&global_stats, lost, cpu);
			__sparse_inc(&memory_stats, os_free, cpu);
			kfree_skb(skb);
			return 0;
		}

		PFQ_CB(buff)->direct = direct;

		if ((GC_size(data->GC) < (size_t)capt_batch_len) &&
		     (ktime_to_ns(ktime_sub(skb_get_ktime(PFQ_SKB(buff)), data->last_rx)) < 1000000))
		{
			return 0;
		}

		data->last_rx = skb_get_ktime(PFQ_SKB(buff));
	}
	else {
                if (GC_size(data->GC) == 0)
		{
			return 0;
		}
	}

	return pfq_receive_batch(data,
				 per_cpu_ptr(percpu_sock, cpu),
				 per_cpu_ptr(percpu_pool, cpu),
				 data->GC, cpu);
}


/* simple packet HANDLER */

static int
pfq_packet_rcv
(
    struct sk_buff *skb, struct net_device *dev,
    struct packet_type *pt
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,16))
    ,struct net_device *orig_dev
#endif
    )
{
	if (skb->pkt_type == PACKET_LOOPBACK)
		goto out;

	if (skb->peeked) {
		skb->peeked = 0;
		goto out;
	}

	skb = skb_share_check(skb, GFP_ATOMIC);
	if (unlikely(!skb))
		return 0;

        switch(skb->pkt_type)
        {
            case PACKET_OUTGOING: {
                if (!capture_outgoing)
                        goto out;

                skb->mac_len = ETH_HLEN;
            } break;

            default:
		if (!capture_incoming)
			goto out;
        }

        return pfq_receive(NULL, skb, 0);
out:
	sparse_inc(&memory_stats, os_free);
	kfree_skb(skb);
	return 0;
}


void pfq_timer(unsigned long cpu)
{
	struct pfq_percpu_data *data;

	pfq_receive(NULL, NULL, 0);

	data = per_cpu_ptr(percpu_data, cpu);
	mod_timer_pinned(&data->timer, jiffies + msecs_to_jiffies(100));
}


static int
pfq_release(struct socket *sock)
{
        struct sock * sk = sock->sk;
        struct pfq_sock *so;
        pfq_id_t id;
        int total = 0;

	if (!sk)
		return 0;

        so = pfq_sk(sk);
        id = so->id;

        /* unbind AX threads */

        pr_devel("[PFQ|%d] unbinding devs and Tx threads...\n", id);

	pfq_sock_tx_unbind(so);

        pr_devel("[PFQ|%d] releasing socket...\n", id);

        pfq_leave_all_groups(so->id);
        pfq_release_sock_id(so->id);

	msleep(Q_GRACE_PERIOD);

        if (so->shmem.addr) {
		pr_devel("[PFQ|%d] freeing shared memory...\n", id);
                pfq_shared_queue_disable(so);
	}

        down(&sock_sem);

        /* purge both batch and recycle queues if no socket is open */

        if (pfq_get_sock_count() == 0)
                total += pfq_percpu_destruct();

        up (&sock_sem);

        if (total)
                printk(KERN_INFO "[PFQ|%d] cleanup: %d skb purged.\n", id, total);

        sock_orphan(sk);
	sock->sk = NULL;
	sock_put(sk);

        up_read(&symtable_sem);

	pr_devel("[PFQ|%d] socket closed.\n", id);
        return 0;
}


static unsigned int
pfq_poll(struct file *file, struct socket *sock, poll_table * wait)
{
        struct sock *sk = sock->sk;
        struct pfq_sock *so = pfq_sk(sk);
        unsigned int mask = 0;

	poll_wait(file, &so->opt.waitqueue, wait);

        if(!pfq_get_rx_queue(&so->opt))
                return mask;

        if (pfq_mpsc_queue_len(so) > 0)
                mask |= POLLIN | POLLRDNORM;

        return mask;
}

static
int pfq_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
        switch (cmd) {
#ifdef CONFIG_INET
        case SIOCGIFFLAGS:
        case SIOCSIFFLAGS:
        case SIOCGIFCONF:
        case SIOCGIFMETRIC:
        case SIOCSIFMETRIC:
        case SIOCGIFMEM:
        case SIOCSIFMEM:
        case SIOCGIFMTU:
        case SIOCSIFMTU:
        case SIOCSIFLINK:
        case SIOCGIFHWADDR:
        case SIOCSIFHWADDR:
        case SIOCSIFMAP:
        case SIOCGIFMAP:
        case SIOCSIFSLAVE:
        case SIOCGIFSLAVE:
        case SIOCGIFINDEX:
        case SIOCGIFNAME:
        case SIOCGIFCOUNT:
        case SIOCSIFHWBROADCAST:
            return(inet_dgram_ops.ioctl(sock, cmd, arg));
#endif
        default:
            return -ENOIOCTLCMD;
        }

        return 0;
}

static int pfq_netdev_notifier(struct notifier_block *this, unsigned long info,
			       void *data)
{
	struct net_device *dev = netdev_notifier_info_to_dev(data);

	if (dev) {
                const char *kind = "NETDEV_UNKNOWN";
		BUG_ON(dev->ifindex >= Q_MAX_DEVICE);

		switch(info)
		{
			case NETDEV_UP			: kind = "NETDEV_UP"; break;
			case NETDEV_DOWN		: kind = "NETDEV_DOWN"; break;
			case NETDEV_REBOOT		: kind = "NETDEV_REBOOT"; break;
			case NETDEV_CHANGE		: kind = "NETDEV_CHANGE"; break;
			case NETDEV_REGISTER		: kind = "NETDEV_REGISTER"; break;
			case NETDEV_UNREGISTER		: kind = "NETDEV_UNREGISTER"; break;
			case NETDEV_CHANGEMTU		: kind = "NETDEV_CHANGEMTU"; break;
			case NETDEV_CHANGEADDR		: kind = "NETDEV_CHANGEADDR"; break;
			case NETDEV_GOING_DOWN		: kind = "NETDEV_GOING_DOWN"; break;
			case NETDEV_CHANGENAME		: kind = "NETDEV_CHANGENAME"; break;
			case NETDEV_FEAT_CHANGE		: kind = "NETDEV_FEAT_CHANGE"; break;
			case NETDEV_BONDING_FAILOVER	: kind = "NETDEV_BONDING_FAILOVER"; break;
			case NETDEV_PRE_UP		: kind = "NETDEV_PRE_UP"; break;
			case NETDEV_PRE_TYPE_CHANGE	: kind = "NETDEV_PRE_TYPE_CHANGE"; break;
			case NETDEV_POST_TYPE_CHANGE	: kind = "NETDEV_POST_TYPE_CHANGE"; break;
			case NETDEV_POST_INIT		: kind = "NETDEV_POST_INIT"; break;
			case NETDEV_UNREGISTER_FINAL	: kind = "NETDEV_UNREGISTER_FINAL"; break;
			case NETDEV_RELEASE		: kind = "NETDEV_RELEASE"; break;
			case NETDEV_NOTIFY_PEERS	: kind = "NETDEV_NOTIFY_PEERS"; break;
			case NETDEV_JOIN		: kind = "NETDEV_JOIN"; break;
			case NETDEV_CHANGEUPPER		: kind = "NETDEV_CHANGEUPPER"; break;
			case NETDEV_RESEND_IGMP		: kind = "NETDEV_RESEND_IGMP"; break;
			case NETDEV_PRECHANGEMTU	: kind = "NETDEV_PRECHANGEMTU"; break;
		}

		pr_devel("[PFQ] %s: device %s, ifindex %d\n", kind, dev->name, dev->ifindex);
		return NOTIFY_OK;
	}

	return NOTIFY_DONE;
}


static
void pfq_register_device_handler(void)
{
        if (capture_incoming || capture_outgoing) {
                pfq_prot_hook.func = pfq_packet_rcv;
                pfq_prot_hook.type = __constant_htons(ETH_P_ALL);
                dev_add_pack(&pfq_prot_hook);
        }
}


static
void unregister_device_handler(void)
{
        if (capture_incoming || capture_outgoing) {
                dev_remove_pack(&pfq_prot_hook); /* Remove protocol hook */
        }
}


static struct proto_ops pfq_ops =
{
	.family = PF_Q,
        .owner = THIS_MODULE,

        /* Operations that make no sense on queue sockets. */
        .connect    = sock_no_connect,
        .socketpair = sock_no_socketpair,
        .accept     = sock_no_accept,
        .getname    = sock_no_getname,
        .listen     = sock_no_listen,
        .shutdown   = sock_no_shutdown,
        .sendpage   = sock_no_sendpage,

        /* Now the operations that really occur. */
        .release    = pfq_release,
        .bind       = sock_no_bind,
        .mmap       = pfq_mmap,
        .poll       = pfq_poll,
        .setsockopt = pfq_setsockopt,
        .getsockopt = pfq_getsockopt,
        .ioctl      = pfq_ioctl,
        .recvmsg    = sock_no_recvmsg,
        .sendmsg    = sock_no_sendmsg
};


static struct proto pfq_proto =
{
	.name  = "PFQ",
        .owner = THIS_MODULE,
        .obj_size = sizeof(struct pfq_sock)
};


static int
pfq_create(
#if(LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,24))
    struct net *net,
#endif
    struct socket *sock, int protocol
#if(LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,33))
    , int kern
#endif
    )
{
        struct pfq_sock *so;
        struct sock *sk;
	int id;

        /* security and sanity check */

        if (!capable(CAP_NET_ADMIN))
                return -EPERM;
        if (sock->type != SOCK_RAW)
                return -ESOCKTNOSUPPORT;
        if (protocol != __constant_htons(ETH_P_ALL))
                return -EPROTONOSUPPORT;

        sock->state = SS_UNCONNECTED;

	sk = sk_alloc(  net
		     ,  PF_INET
		     ,  GFP_KERNEL
		     ,  &pfq_proto
#if(LINUX_VERSION_CODE >= KERNEL_VERSION(4,2,0))
		     ,  kern
#endif
		     );

        if (sk == NULL) {
                printk(KERN_WARNING "[PFQ] error: pfq_sock_init: could not allocate a socket!\n");
                return -ENOMEM;
        }

        sock->ops = &pfq_ops;

        /* initialize the socket */

        sock_init_data(sock,sk);

        so = pfq_sk(sk);

        /* get a unique id for this sock */

        id = pfq_get_free_id(so);
        if (id == -1) {
                printk(KERN_WARNING "[PFQ] error: pfq_sock_init: resource exhausted!\n");
                sk_free(sk);
                return -EBUSY;
        }

        down(&sock_sem);

        /* initialize sock */

	if (pfq_sock_init(so, id) < 0) {
                printk(KERN_WARNING "[PFQ] error: pfq_sock_init: no memory!\n");
		sk_free(sk);
		up(&sock_sem);
		return -EINVAL;
	}

        /* initialize sock opt */

        pfq_sock_opt_init(&so->opt, capt_slot_size, xmit_slot_size);

        /* initialize socket */

        sk->sk_family   = PF_Q;
        sk->sk_destruct = pfq_sock_destruct;

        sk_refcnt_debug_inc(sk);

        up (&sock_sem);

        down_read(&symtable_sem);
        return 0;
}


static struct net_proto_family pfq_family_ops =
{
	.family = PF_Q,
        .create = pfq_create,
        .owner = THIS_MODULE,
};



static struct notifier_block pfq_netdev_notifier_block =
{
	.notifier_call = pfq_netdev_notifier
};



static int
check_tx_threads_affinity(void)
{
	int i, j;

	for(i=0; i < tx_thread_nr; ++i)
	{
		if (tx_affinity[i] < 0 || tx_affinity[i] >= num_online_cpus())
		{
			printk(KERN_INFO "[PFQ] error: Tx thread bad affinity on cpu:%d!\n", tx_affinity[i]);
			return -EFAULT;
		}
	}

	for(i=0; i < tx_thread_nr-1; ++i)
	for(j=i+1; j < tx_thread_nr; ++j)
	{
		if (tx_affinity[i] == tx_affinity[j])
		{
			printk(KERN_INFO "[PFQ] error: Tx thread affinity for cpu:%d already in use!\n", tx_affinity[i]);
			return -EFAULT;
		}
	}

	return 0;
}


static int __init pfq_init_module(void)
{
        int err = -EFAULT;

        printk(KERN_INFO "[PFQ] loading...\n");

	/* check options */

        if (capt_batch_len <= 0 || capt_batch_len > Q_SKBUFF_BATCH) {
                printk(KERN_INFO "[PFQ] capt_batch_len=%d not allowed: valid range (0,%d]!\n",
                       capt_batch_len, Q_SKBUFF_BATCH);
                return -EFAULT;
        }

        if (xmit_batch_len <= 0 || xmit_batch_len > (Q_SKBUFF_BATCH*4)) {
                printk(KERN_INFO "[PFQ] xmit_batch_len=%d not allowed: valid range (0,%d]!\n",
                       xmit_batch_len, Q_SKBUFF_BATCH * 4);
                return -EFAULT;
        }

	if (skb_pool_size > Q_MAX_POOL_SIZE) {
                printk(KERN_INFO "[PFQ] skb_pool_size=%d not allowed: valid range [0,%d]!\n",
                       skb_pool_size, Q_MAX_POOL_SIZE);
		return -EFAULT;
	}

	/* initialize data structures ... */

	err = pfq_groups_init();
	if (err < 0)
		goto err;

	/* initialization */

	err = pfq_percpu_alloc();
	if (err < 0)
		goto err;

	err = pfq_percpu_init();
	if (err < 0)
		goto err1;

	err = pfq_proc_init();
	if (err < 0)
		goto err2;

        /* register PFQ sniffer protocol */

        err = proto_register(&pfq_proto, 0);
        if (err < 0)
		goto err3;

	/* register the pfq socket */

        err = sock_register(&pfq_family_ops);
        if (err < 0)
		goto err4;

#ifdef PFQ_USE_SKB_POOL
	err = pfq_skb_pool_init_all();
        if (err < 0) {
		pfq_skb_pool_free_all();
		goto err5;
	}
        printk(KERN_INFO "[PFQ] skb pool initialized.\n");
#endif

	/* register pfq-lang default functions */
	pfq_lang_symtable_init();

        /* finally register the basic device handler */
        pfq_register_device_handler();

	/* register netdev notifier */
        register_netdevice_notifier(&pfq_netdev_notifier_block);

	/* start Tx threads for asynchronous transmission */
	if (tx_thread_nr)
	{
		if ((err = check_tx_threads_affinity()) < 0)
			goto err6;

		if ((err = pfq_start_all_tx_threads()) < 0)
			goto err7;
	}

	/* ensure each device has ifindex < Q_MAX_DEVICE */
	{
		struct net_device *dev;
		for_each_netdev(&init_net, dev)
			BUG_ON(dev->ifindex >= Q_MAX_DEVICE);
	}

        printk(KERN_INFO "[PFQ] version %d.%d.%d ready!\n",
               PFQ_MAJOR(PFQ_VERSION_CODE),
               PFQ_MINOR(PFQ_VERSION_CODE),
               PFQ_PATCHLEVEL(PFQ_VERSION_CODE));

        return 0;

err7:
	pfq_stop_all_tx_threads();
err6:
	unregister_netdevice_notifier(&pfq_netdev_notifier_block);
	unregister_device_handler();
err5:
        sock_unregister(PF_Q);
err4:
        proto_unregister(&pfq_proto);
err3:
	pfq_proc_destruct();
err2:
	pfq_percpu_destruct();
err1:
	pfq_percpu_free();
err:
	return err < 0 ? err : -EFAULT;
}


static void __exit pfq_exit_module(void)
{
        int total = 0;

	/* stop Tx threads */
	pfq_stop_all_tx_threads();

#ifdef PFQ_USE_SKB_POOL
        pfq_skb_pool_enable(false);
#endif
	/* unregister netdevice notifier */
        unregister_netdevice_notifier(&pfq_netdev_notifier_block);

        /* unregister the basic device handler */
        unregister_device_handler();

        /* unregister the pfq socket */
        sock_unregister(PF_Q);

        /* unregister the pfq protocol */
        proto_unregister(&pfq_proto);

        /* disable direct capture */
        pfq_devmap_monitor_reset();

        /* wait grace period */
        msleep(Q_GRACE_PERIOD);

        /* free per CPU data */
        total += pfq_percpu_destruct();

#ifdef PFQ_USE_SKB_POOL
        total += pfq_skb_pool_free_all();
	sparse_add(&memory_stats, pool_pop, total);
#endif
        if (total)
                printk(KERN_INFO "[PFQ] %d skbuff freed.\n", total);

        /* free per-cpu data */
        pfq_percpu_free();

	/* free symbol table of pfq-lang functions */
	pfq_lang_symtable_free();

	pfq_proc_destruct();

	pfq_groups_destruct();

        printk(KERN_INFO "[PFQ] unloaded.\n");
}


/* pfq direct capture drivers support */

static inline
int pfq_direct_capture(const struct sk_buff *skb)
{
        return pfq_devmap_monitor_get(skb->dev->ifindex);
}


static inline
int pfq_normalize_skb(struct sk_buff *skb)
{
        skb_reset_network_header(skb);
	skb_reset_transport_header(skb);

#ifdef PFQ_USE_SKB_LINEARIZE
	if(skb_linearize(skb) < 0) {
		__kfree_skb(skb);
		return -1;
	}
#endif
	return 0;
}


static int
pfq_netif_receive_skb(struct sk_buff *skb)
{
        if (likely(pfq_direct_capture(skb))) {

		if (pfq_normalize_skb(skb) < 0)
			return NET_RX_DROP;

		pfq_receive(NULL, skb, 2);
		return NET_RX_SUCCESS;
	}

	return netif_receive_skb(skb);
}


static int
pfq_netif_rx(struct sk_buff *skb)
{
        if (likely(pfq_direct_capture(skb))) {

		if (pfq_normalize_skb(skb) < 0)
			return NET_RX_DROP;

		pfq_receive(NULL, skb, 1);
		return NET_RX_SUCCESS;
	}

	return netif_rx(skb);
}


static gro_result_t
pfq_gro_receive(struct napi_struct *napi, struct sk_buff *skb)
{
        if (likely(pfq_direct_capture(skb))) {

		if (pfq_normalize_skb(skb) < 0)
			return GRO_DROP;

                pfq_receive(napi, skb, 3);
                return GRO_NORMAL;
        }

        return napi_gro_receive(napi,skb);
}


EXPORT_SYMBOL_GPL(pfq_netif_rx);
EXPORT_SYMBOL_GPL(pfq_netif_receive_skb);
EXPORT_SYMBOL_GPL(pfq_gro_receive);

EXPORT_SYMBOL(pfq_lang_symtable_register_functions);
EXPORT_SYMBOL(pfq_lang_symtable_unregister_functions);

module_init(pfq_init_module);
module_exit(pfq_exit_module);
