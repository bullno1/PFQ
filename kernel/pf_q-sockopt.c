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
#include <linux/version.h>
#include <linux/kthread.h>
#include <linux/pf_q.h>

#include <pragma/diagnostic_pop>

#include <pf_q-stats.h>
#include <pf_q-netdev.h>
#include <pf_q-transmit.h>
#include <pf_q-thread.h>
#include <pf_q-global.h>
#include <pf_q-sock.h>
#include <pf_q-group.h>
#include <pf_q-memory.h>
#include <pf_q-devmap.h>
#include <pf_q-thread.h>
#include <pf_q-sockopt.h>
#include <pf_q-endpoint.h>
#include <pf_q-shared-queue.h>
#include <pf_q-printk.h>
#include <pf_q-netdev.h>

#include <lang/engine.h>
#include <lang/symtable.h>
#include <lang/printk.h>

int pfq_getsockopt(struct socket *sock,
                int level, int optname,
                char __user * optval, int __user * optlen)
{
        struct pfq_sock *so = pfq_sk(sock->sk);
        int len;

        if (so == NULL)
                return -EFAULT;

        if (get_user(len, optlen))
                return -EFAULT;

        if (len < 0)
                return -EINVAL;

        switch(optname)
        {

        case Q_SO_GROUP_JOIN:
        {
                struct pfq_group_join group;

                if (len != sizeof(group))
                        return -EINVAL;

                if (copy_from_user(&group, optval, sizeof(group)))
                        return -EFAULT;

                if (group.class_mask == 0) {
                        printk(KERN_INFO "[PFQ|%d] join group error: bad class_mask (%lx)!\n",
                               so->id, group.class_mask);
                        return -EINVAL;
                }

                if (group.gid == Q_ANY_GROUP) {

                        group.gid = pfq_join_free_group(so->id, group.class_mask, group.policy);
                        if (group.gid < 0)
                                return -EFAULT;
                        if (copy_to_user(optval, &group, len))
                                return -EFAULT;
                }
                else {
			pfq_gid_t gid = (__force pfq_gid_t)group.gid;

			if (!pfq_get_group(gid)) {
				printk(KERN_INFO "[PFQ|%d] join group error: invalid group id %d!\n",
				       so->id, gid);
				return -EFAULT;
			}

                        if (pfq_join_group(gid, so->id, group.class_mask, group.policy) < 0) {
                                printk(KERN_INFO "[PFQ|%d] join group error: permission denied (gid=%d)!\n",
                                       so->id, group.gid);
                                return -EACCES;
                        }
                }

                pr_devel("[PFQ|%d] join group: gid=%d class_mask=%lx policy=%d\n",
				so->id, group.gid, group.class_mask, group.policy);
        } break;

        case Q_SO_GET_ID:
        {
		int ver;

                if (len != sizeof(so->id))
                        return -EINVAL;

                if (copy_from_user(&ver, optval, sizeof(ver)))
                        return -EFAULT;

		if (ver != PFQ_VERSION_CODE) {
			printk(KERN_INFO "[PFQ] version mismatch: kernel version %d.%d.%d, library version = %d.%d.%d!\n",
			       PFQ_MAJOR(PFQ_VERSION_CODE),
			       PFQ_MINOR(PFQ_VERSION_CODE),
			       PFQ_PATCHLEVEL(PFQ_VERSION_CODE),
			       PFQ_MAJOR(ver),
			       PFQ_MINOR(ver),
			       PFQ_PATCHLEVEL(ver));

			return -EPERM;
		}

                if (copy_to_user(optval, &so->id, sizeof(so->id)))
                        return -EFAULT;
        } break;

        case Q_SO_GET_STATUS:
        {
                int enabled;
                if (len != sizeof(int))
                        return -EINVAL;

                enabled = so->shmem.addr == NULL ? 0 : 1;

                if (copy_to_user(optval, &enabled, sizeof(enabled)))
                        return -EFAULT;

        } break;

        case Q_SO_GET_STATS:
        {
                struct pfq_stats stat;
                if (len != sizeof(struct pfq_stats))
                        return -EINVAL;

		pfq_kernel_stats_read(so->stats, &stat);

                if (copy_to_user(optval, &stat, sizeof(stat)))
                        return -EFAULT;
        } break;

        case Q_SO_GET_RX_TSTAMP:
        {
                if (len != sizeof(so->opt.tstamp))
                        return -EINVAL;
                if (copy_to_user(optval, &so->opt.tstamp, sizeof(so->opt.tstamp)))
                        return -EFAULT;
        } break;

        case Q_SO_GET_SHMEM_SIZE:
	{
		size_t size = pfq_shared_memory_size(so);

                if (len != sizeof(size))
                        return -EINVAL;

                if (copy_to_user(optval, &size, sizeof(size)))
                        return -EFAULT;
        } break;

        case Q_SO_GET_RX_CAPLEN:
        {
                if (len != sizeof(so->opt.caplen))
                        return -EINVAL;
                if (copy_to_user(optval, &so->opt.caplen, sizeof(so->opt.caplen)))
                        return -EFAULT;
        } break;

        case Q_SO_GET_TX_MAXLEN:
        {
                if (len != sizeof(xmit_slot_size))
                        return -EINVAL;
                if (copy_to_user(optval, &xmit_slot_size, sizeof(xmit_slot_size)))
                        return -EFAULT;
        } break;

        case Q_SO_GET_RX_SLOTS:
        {
                if (len != sizeof(so->opt.rx_queue_len))
                        return -EINVAL;
                if (copy_to_user(optval, &so->opt.rx_queue_len, sizeof(so->opt.rx_queue_len)))
                        return -EFAULT;
        } break;

        case Q_SO_GET_TX_SLOTS:
        {
                if (len != sizeof(so->opt.tx_queue_len))
                        return -EINVAL;
                if (copy_to_user(optval, &so->opt.tx_queue_len, sizeof(so->opt.tx_queue_len)))
                        return -EFAULT;
        } break;

        case Q_SO_GET_GROUPS:
        {
                unsigned long grps;
                if(len != sizeof(unsigned long))
                        return -EINVAL;
                grps = pfq_get_groups(so->id);
                if (copy_to_user(optval, &grps, sizeof(grps)))
                        return -EFAULT;
        } break;

        case Q_SO_GET_GROUP_STATS:
        {
                struct pfq_group *group;
                struct pfq_stats stat;
                pfq_gid_t gid;

                if (len != sizeof(stat))
                        return -EINVAL;

                if (copy_from_user(&stat, optval, sizeof(stat)))
                        return -EFAULT;

                gid = (__force pfq_gid_t)stat.recv;

                group = pfq_get_group(gid);
                if (group == NULL) {
                        printk(KERN_INFO "[PFQ|%d] group error: invalid group id %d!\n", so->id, gid);
                        return -EFAULT;
                }

		if (pfq_group_is_free(gid)) {
                        printk(KERN_INFO "[PFQ|%d] group stats error: gid=%d is a free group!\n",
                               so->id, gid);
                        return -EACCES;
		}

                if (!pfq_group_access(gid, so->id)) {
                        printk(KERN_INFO "[PFQ|%d] group stats error: gid=%d permission denied!\n",
                               so->id, gid);
                        return -EACCES;
                }

		pfq_kernel_stats_read(group->stats, &stat);

                if (copy_to_user(optval, &stat, sizeof(stat)))
                        return -EFAULT;
        } break;

        case Q_SO_GET_GROUP_COUNTERS:
        {
                struct pfq_group *group;
                struct pfq_counters cs;
                pfq_gid_t gid;
                int i;

                if (len != sizeof(cs))
                        return -EINVAL;

                if (copy_from_user(&cs, optval, sizeof(cs)))
                        return -EFAULT;

                gid = (__force pfq_gid_t)cs.counter[0];

                group = pfq_get_group(gid);
                if (group == NULL) {
                        printk(KERN_INFO "[PFQ|%d] group error: invalid group id %d!\n", so->id, gid);
                        return -EFAULT;
                }

                /* check whether the group is joinable.. */

                if (!pfq_group_policy_access(gid, so->id, Q_POLICY_GROUP_UNDEFINED)) {
                        printk(KERN_INFO "[PFQ|%d] group error: permission denied (gid=%d)!\n",
                               so->id, gid);
                        return -EACCES;
                }

                for(i = 0; i < Q_MAX_COUNTERS; i++)
                {
                        cs.counter[i] = sparse_read(group->counters, value[i]);
                }

                if (copy_to_user(optval, &cs, sizeof(cs)))
                        return -EFAULT;
        } break;

        case Q_SO_GET_WEIGHT:
        {
                if (len != sizeof(so->weight))
                        return -EINVAL;

                if (copy_to_user(optval, &so->weight, sizeof(so->weight)))
                        return -EFAULT;
        } break;

        default:
                return -EFAULT;
        }

        return 0;
}



int pfq_setsockopt(struct socket *sock,
                int level, int optname,
                char __user * optval,
#if(LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,31))
                unsigned
#endif
                int optlen)
{
        struct pfq_sock *so = pfq_sk(sock->sk);

        bool found = true;

        if (so == NULL)
                return -EINVAL;

        switch(optname)
        {
        case Q_SO_ENABLE:
	{
		unsigned long addr;
		int err = 0;

                if (optlen != sizeof(addr))
                        return -EINVAL;

                if (copy_from_user(&addr, optval, optlen))
                        return -EFAULT;

                err = pfq_shared_queue_enable(so, addr);
                if (err < 0) {
                        printk(KERN_INFO "[PFQ|%d] enable error!\n", so->id);
                        return err;
                }

		return 0;

	} break;

	case Q_SO_DISABLE:
	{
		int err = 0;

		pfq_sock_tx_unbind(so);

		msleep(Q_GRACE_PERIOD);

                err = pfq_shared_queue_disable(so);
                if (err < 0) {
                        printk(KERN_INFO "[PFQ|%d] disable error!\n", so->id);
                        return err;
                }

	} break;

        case Q_SO_GROUP_BIND:
        {
                struct pfq_binding bind;
		pfq_gid_t gid;

                if (optlen != sizeof(struct pfq_binding))
                        return -EINVAL;

                if (copy_from_user(&bind, optval, optlen))
                        return -EFAULT;

		gid = (__force pfq_gid_t)bind.gid;

                if (!pfq_has_joined_group(gid, so->id)) {
                        printk(KERN_INFO "[PFQ|%d] add bind: gid=%d not joined!\n", so->id, bind.gid);
			return -EACCES;
		}

#ifdef PFQ_DEBUG
		{
			int ref = netdev_refcnt_read_by_index(sock_net(&so->sk), bind.ifindex);
			printk(KERN_INFO "[PFQ] GROUP_BIND: dev_get_by_index: ifindex=%d ref=%d\n", bind.ifindex, ref);
		}
#endif

		rcu_read_lock();
                if (!dev_get_by_index_rcu(sock_net(&so->sk), bind.ifindex)) {
                        printk(KERN_INFO "[PFQ|%d] bind: invalid ifindex=%d!\n", so->id, bind.ifindex);
                        rcu_read_unlock();
                        return -EACCES;
                }
                rcu_read_unlock();

                pfq_devmap_update(map_set, bind.ifindex, bind.qindex, gid);

                pr_devel("[PFQ|%d] group id=%d bind: device ifindex=%d qindex=%d\n",
					so->id, bind.gid, bind.ifindex, bind.qindex);

        } break;

        case Q_SO_GROUP_UNBIND:
        {
                struct pfq_binding bind;
		pfq_gid_t gid;

                if (optlen != sizeof(struct pfq_binding))
                        return -EINVAL;

                if (copy_from_user(&bind, optval, optlen))
                        return -EFAULT;

		gid = (__force pfq_gid_t)bind.gid;

		if (!pfq_has_joined_group(gid, so->id)) {
                        printk(KERN_INFO "[PFQ|%d] group id=%d unbind: gid=%d not joined!\n", so->id, gid, bind.gid);
			return -EACCES;
		}

#ifdef PFQ_DEBUG
		{
			int ref = netdev_refcnt_read_by_index(sock_net(&so->sk), bind.ifindex);
			printk(KERN_INFO "[PFQ] GROUP_UNBIND: dev_put_by_index: ifindex=%d ref=%d\n", bind.ifindex, ref);
		}
#endif

                pfq_devmap_update(map_reset, bind.ifindex, bind.qindex, gid);

                pr_devel("[PFQ|%d] group id=%d unbind: device ifindex=%d qindex=%d\n",
					so->id, gid, bind.ifindex, bind.qindex);

        } break;

        case Q_SO_EGRESS_BIND:
        {
                struct pfq_binding bind;

                if (optlen != sizeof(bind))
                        return -EINVAL;
                if (copy_from_user(&bind, optval, optlen))
                        return -EFAULT;

		rcu_read_lock();
                if (!dev_get_by_index_rcu(sock_net(&so->sk), bind.ifindex)) {
                        printk(KERN_INFO "[PFQ|%d] egress bind: invalid ifindex=%d\n", so->id, bind.ifindex);
                        rcu_read_unlock();
                        return -EPERM;
                }
                rcu_read_unlock();

                if (bind.qindex < -1) {
                        printk(KERN_INFO "[PFQ|%d] egress bind: invalid qindex=%d\n", so->id, bind.qindex);
                        return -EPERM;
                }

		so->egress_type  = pfq_endpoint_device;
                so->egress_index = bind.ifindex;
                so->egress_queue = bind.qindex;

                pr_devel("[PFQ|%d] egress bind: device ifindex=%d qindex=%d\n",
			 so->id, so->egress_index, so->egress_queue);

        } break;

        case Q_SO_EGRESS_UNBIND:
        {
		so->egress_type  = pfq_endpoint_socket;
                so->egress_index = 0;
                so->egress_queue = 0;

                pr_devel("[PFQ|%d] egress unbind.\n", so->id);

        } break;

        case Q_SO_SET_RX_TSTAMP:
        {
                int tstamp;
                if (optlen != sizeof(so->opt.tstamp))
                        return -EINVAL;

                if (copy_from_user(&tstamp, optval, optlen))
                        return -EFAULT;

                tstamp = tstamp ? 1 : 0;
                so->opt.tstamp = tstamp;

                pr_devel("[PFQ|%d] timestamp enabled.\n", so->id);
        } break;

        case Q_SO_SET_RX_CAPLEN:
        {
                typeof(so->opt.caplen) caplen;

                if (optlen != sizeof(caplen))
                        return -EINVAL;
                if (copy_from_user(&caplen, optval, optlen))
                        return -EFAULT;

                if (caplen > (size_t)capt_slot_size) {
                        printk(KERN_INFO "[PFQ|%d] invalid caplen=%zu (max %d)\n", so->id, caplen, capt_slot_size);
                        return -EPERM;
                }

                so->opt.caplen = caplen;
                so->opt.rx_slot_size = Q_SHARED_QUEUE_SLOT_SIZE(so->opt.caplen);

                pr_devel("[PFQ|%d] caplen=%zu, slot_size=%zu\n",
                                so->id, so->opt.caplen, so->opt.rx_slot_size);
        } break;

        case Q_SO_SET_RX_SLOTS:
        {
                typeof(so->opt.rx_queue_len) slots;

                if (optlen != sizeof(slots))
                        return -EINVAL;

                if (copy_from_user(&slots, optval, optlen))
                        return -EFAULT;

                if (slots > Q_MAX_SOCKQUEUE_LEN) {
                        printk(KERN_INFO "[PFQ|%d] invalid Rx slots=%zu (max %d)\n",
                               so->id, slots, Q_MAX_SOCKQUEUE_LEN);
                        return -EPERM;
                }

                so->opt.rx_queue_len = slots;

                pr_devel("[PFQ|%d] rx_queue slots=%zu\n", so->id, so->opt.rx_queue_len);
        } break;

        case Q_SO_SET_TX_SLOTS:
        {
                typeof (so->opt.tx_queue_len) slots;

                if (optlen != sizeof(slots))
                        return -EINVAL;
                if (copy_from_user(&slots, optval, optlen))
                        return -EFAULT;

                if (slots > Q_MAX_SOCKQUEUE_LEN) {
                        printk(KERN_INFO "[PFQ|%d] invalid Tx slots=%zu (max %d)\n",
                               so->id, slots, Q_MAX_SOCKQUEUE_LEN);
                        return -EPERM;
                }

                so->opt.tx_queue_len = slots;

                pr_devel("[PFQ|%d] tx_queue slots=%zu\n", so->id, so->opt.tx_queue_len);
        } break;

        case Q_SO_SET_WEIGHT:
        {
                int weight;

                if (optlen != sizeof(so->weight))
                        return -EINVAL;

                if (copy_from_user(&weight, optval, optlen))
                        return -EFAULT;

		if (weight < 1 || weight > (Q_MAX_SOCK_MASK/Q_MAX_ID)) {
                        printk(KERN_INFO "[PFQ|%d] weight=%d: invalid range (min 1, max %d)\n", so->id, weight,
                               Q_MAX_SOCK_MASK/Q_MAX_ID);
                        return -EPERM;
		}

                so->weight = weight;

		/* invalidate per-cpu sock mask cache */

		pfq_invalidate_percpu_eligible_mask(so->id);

                pr_devel("[PFQ|%d] new weight set to %d.\n", so->id, weight);

        } break;

        case Q_SO_GROUP_LEAVE:
        {
                pfq_gid_t gid;

                if (optlen != sizeof(gid))
                        return -EINVAL;

                if (copy_from_user(&gid, optval, optlen))
                        return -EFAULT;

                if (pfq_leave_group(gid, so->id) < 0)
                        return -EFAULT;

                pr_devel("[PFQ|%d] group id=%d left.\n", so->id, gid);

        } break;

        case Q_SO_GROUP_FPROG:
        {
                struct pfq_fprog fprog;
		pfq_gid_t gid;

                if (optlen != sizeof(fprog))
                        return -EINVAL;

                if (copy_from_user(&fprog, optval, optlen))
                        return -EFAULT;

		gid = (__force pfq_gid_t)fprog.gid;

		if (!pfq_has_joined_group(gid, so->id)) {
			/* don't set the first and return */
			return 0;
		}

                if (fprog.fcode.len > 0) {  /* set the filter */

                        struct sk_filter *filter;

			if (fprog.fcode.len == 1) {
				struct sock_filter tmp;

				/* get the first filter */
				if (copy_from_user(&tmp, fprog.fcode.filter, sizeof(tmp)))
					return -EFAULT;

				/* check whether the first filter is a dummy BPF_RET */
				if (BPF_CLASS(tmp.code) == BPF_RET) {
					pr_devel("[PFQ|%d] fprog: BPF_RET optimized out!\n", so->id);
					return 0;
				}
			}

                        filter = pfq_alloc_sk_filter(&fprog.fcode);
                        if (filter == NULL) {
                                printk(KERN_INFO "[PFQ|%d] fprog error: alloc_sk_filter for gid=%d\n",
                                       so->id, fprog.gid);
                                return -EINVAL;
                        }

                        pfq_set_group_filter(gid, filter);

                        pr_devel("[PFQ|%d] fprog: gid=%d (fprog len %d bytes)\n",
				 so->id, fprog.gid, fprog.fcode.len);
                }
                else {
			/* reset the filter */
                        pfq_set_group_filter(gid, NULL);
                        pr_devel("[PFQ|%d] fprog: gid=%d (resetting filter)\n", so->id, fprog.gid);
                }

        } break;

        case Q_SO_GROUP_VLAN_FILT_TOGGLE:
        {
                struct pfq_vlan_toggle vlan;
                pfq_gid_t gid;

                if (optlen != sizeof(vlan))
                        return -EINVAL;

                if (copy_from_user(&vlan, optval, optlen))
                        return -EFAULT;

		gid = (__force pfq_gid_t)vlan.gid;

		if (!pfq_has_joined_group(gid, so->id)) {
                        printk(KERN_INFO "[PFQ|%d] vlan filter toggle: gid=%d not joined!\n", so->id, vlan.gid);
			return -EACCES;
		}

                pfq_toggle_group_vlan_filters(gid, vlan.toggle);
                pr_devel("[PFQ|%d] vlan filters %s for gid=%d\n",
			 so->id, (vlan.toggle ? "enabled" : "disabled"), vlan.gid);

        } break;

        case Q_SO_GROUP_VLAN_FILT:
        {
                struct pfq_vlan_toggle filt;
                pfq_gid_t gid;

                if (optlen != sizeof(filt))
                        return -EINVAL;

                if (copy_from_user(&filt, optval, optlen))
                        return -EFAULT;

		gid = (__force pfq_gid_t)filt.gid;

		if (!pfq_has_joined_group(gid, so->id)) {
                        printk(KERN_INFO "[PFQ|%d] vlan filter: gid=%d not joined!\n", so->id, filt.gid);
			return -EACCES;
		}

                if (filt.vid < -1 || filt.vid > 4094) {
                        printk(KERN_INFO "[PFQ|%d] vlan error: invalid vid=%d for gid=%d!\n",
                               so->id, filt.vid, filt.gid);
                        return -EINVAL;
                }

                if (!pfq_vlan_filters_enabled(gid)) {
                        printk(KERN_INFO "[PFQ|%d] vlan error: vlan filters disabled for gid=%d!\n",
                               so->id, filt.gid);
                        return -EPERM;
                }

                if (filt.vid  == -1) { /* any */
                        int i;
                        for(i = 1; i < 4095; i++)
			{
                                pfq_set_group_vlan_filter(gid, filt.toggle, i);
			}
                }
                else  {
                        pfq_set_group_vlan_filter(gid, filt.toggle, filt.vid);
		}

                pr_devel("[PFQ|%d] vlan filter vid %d set for gid=%d\n", so->id, filt.vid, filt.gid);
        } break;

        case Q_SO_TX_BIND:
        {
                struct pfq_binding bind;

                if (optlen != sizeof(bind))
                        return -EINVAL;

                if (copy_from_user(&bind, optval, optlen))
                        return -EFAULT;

		if (bind.tid < -1) {
			printk(KERN_INFO "[PFQ|%d] Tx thread: invalid thread index (%d)!\n", so->id, bind.tid);
			return -EPERM;
		}

		if (bind.tid >= 0 &&
		    so->opt.tx_num_async_queues >= Q_MAX_TX_QUEUES) {
			printk(KERN_INFO "[PFQ|%d] Tx thread: max number of sock queues exceeded!\n", so->id);
			return -EPERM;
		}

                if (bind.qindex < -1) {
                        printk(KERN_INFO "[PFQ|%d] Tx thread: invalid hw queue (%d)\n", so->id, bind.qindex);
                        return -EPERM;
                }

		/* get device */

		rcu_read_lock();
		if (bind.ifindex != -1 && !dev_get_by_index_rcu(sock_net(&so->sk), bind.ifindex)) {
			printk(KERN_INFO "[PFQ|%d] Tx thread: invalid ifindex=%d\n", so->id, bind.ifindex);
			rcu_read_unlock();
			return -EPERM;
		}
		rcu_read_unlock();

		/* update the socket queue information */

		if (bind.tid >= 0) /* async queues */
		{
			int err = pfq_sock_tx_bind(so, bind.tid, bind.ifindex, bind.qindex);
			if (err < 0) {
				return err;
			}

			pr_devel("[PFQ|%d] Tx[%d] bind: if_index=%d qindex=%d\n", so->id, bind.tid, bind.ifindex, bind.qindex);
		}
		else /* sync queue */
		{
			so->opt.txq.def_ifindex = bind.ifindex;
			so->opt.txq.def_queue = bind.qindex;
			pr_devel("[PFQ|%d] Tx bind: if_index=%d qindex=%d\n", so->id,
				so->opt.txq.def_ifindex,
				so->opt.txq.def_queue);
		}

        } break;

	case Q_SO_TX_UNBIND:
	{
		pfq_sock_tx_unbind(so);
        } break;

        case Q_SO_TX_QUEUE:
        {
		int queue;

		if (optlen != sizeof(queue))
			return -EINVAL;

		if (copy_from_user(&queue, optval, optlen))
			return -EFAULT;

		if (pfq_get_tx_queue(&so->opt, -1) == NULL) {
			printk(KERN_INFO "[PFQ|%d] Tx queue: socket not enabled!\n", so->id);
			return -EPERM;
		}

		if (queue == 0) { /* transmit Tx queue */
			atomic_t stop = {0};
			tx_ret tx = pfq_sk_queue_xmit(so, -1, Q_NO_KTHREAD, NUMA_NO_NODE, &stop);

			sparse_add(so->stats, sent, tx.ok);
			sparse_add(so->stats, fail, tx.fail);
			sparse_add(&global_stats, sent, tx.ok);
			sparse_add(&global_stats, fail, tx.fail);

			return 0;
		}

		printk(KERN_INFO "[PFQ|%d] Tx queue: bad queue %d!\n", so->id, queue);
		return -EPERM;

        } break;

        case Q_SO_GROUP_FUNCTION:
        {
                struct pfq_lang_computation_descr *descr = NULL;
                struct pfq_lang_computation_tree *comp = NULL;
                struct pfq_group_computation tmp;
                size_t psize, ucsize;
                void *context = NULL;
                pfq_gid_t gid;

                int err = 0;

                if (optlen != sizeof(tmp))
                        return -EINVAL;

                if (copy_from_user(&tmp, optval, optlen))
                        return -EFAULT;

		gid = (__force pfq_gid_t)tmp.gid;

		if (!pfq_has_joined_group(gid, so->id)) {
                        printk(KERN_INFO "[PFQ|%d] computation: gid=%d not joined!\n", so->id, tmp.gid);
			return -EACCES;
		}

                if (copy_from_user(&psize, tmp.prog, sizeof(size_t)))
                        return -EFAULT;

                pr_devel("[PFQ|%d] computation size: %zu\n", so->id, psize);

                ucsize = sizeof(size_t) * 2 + psize * sizeof(struct pfq_lang_functional_descr);

                descr = kmalloc(ucsize, GFP_KERNEL);
                if (descr == NULL) {
                        printk(KERN_INFO "[PFQ|%d] computation: out of memory!\n", so->id);
                        return -ENOMEM;
                }

                if (copy_from_user(descr, tmp.prog, ucsize)) {
                        printk(KERN_INFO "[PFQ|%d] computation: copy_from_user error!\n", so->id);
                        err = -EFAULT;
                        goto error;
                }

                /* print computation */

                pr_devel_computation_descr(descr);


		/* perform computation sanity check */

		if (pfq_lang_check_computation_descr(descr) < 0) {
                        printk(KERN_INFO "[PFQ|%d] computation: invalid expression!\n", so->id);
                        err = -EFAULT;
                        goto error;
		}

                /* allocate context */

                context = pfq_lang_context_alloc(descr);
                if (context == NULL) {
                        printk(KERN_INFO "[PFQ|%d] computation: alloc error!\n", so->id);
                        err = -EFAULT;
                        goto error;
                }

                /* allocate a pfq_lang_computation_tree */

                comp = pfq_lang_computation_alloc(descr);
                if (comp == NULL) {
                        printk(KERN_INFO "[PFQ|%d] computation: alloc error!\n", so->id);
                        err = -EFAULT;
                        goto error;
                }

                /* link functions */

                if (pfq_lang_computation_rtlink(descr, comp, context) < 0) {
                        printk(KERN_INFO "[PFQ|%d] computation aborted!", so->id);
                        err = -EPERM;
                        goto error;
                }

		/* print executable tree data structure */

		pr_devel_computation_tree(comp);

		/* run init functions */

		if (pfq_lang_computation_init(comp) < 0) {
                        printk(KERN_INFO "[PFQ|%d] computation: initialization aborted!", so->id);
                        pfq_lang_computation_destruct(comp);
                        err = -EPERM;
                        goto error;
		}

                /* enable functional program */

                if (pfq_set_group_prog(gid, comp, context) < 0) {
                        printk(KERN_INFO "[PFQ|%d] computation: set program error!\n", so->id);
                        err = -EPERM;
                        goto error;
                }

		kfree(descr);
                return 0;

	error:  kfree(comp);
		kfree(context);
		kfree(descr);
		return err;

        } break;

        default:
        {
                found = false;
        } break;

        }

        return found ? 0 : sock_setsockopt(sock, level, optname, optval, optlen);
}

