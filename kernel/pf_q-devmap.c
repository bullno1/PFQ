/***************************************************************
 *
 * (C) 2011-15 Nicola Bonelli <nicola@pfq.io>
 *             Andrea Di Pietro <andrea.dipietro@for.unipi.it>
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
#include <linux/mutex.h>

#include <pragma/diagnostic_pop>

#include <pf_q-devmap.h>
#include <pf_q-group.h>


static DEFINE_MUTEX(devmap_lock);

atomic_long_t   pfq_devmap [Q_MAX_DEVICE][Q_MAX_HW_QUEUE];
atomic_t        pfq_devmap_monitor [Q_MAX_DEVICE];


void pfq_devmap_monitor_update(void)
{
    int i,j;
    for(i=0; i < Q_MAX_DEVICE; ++i)
    {
        unsigned long val = 0;
        for(j=0; j < Q_MAX_HW_QUEUE; ++j)
        {
            val |= atomic_long_read(&pfq_devmap[i][j]);
        }

        atomic_set(&pfq_devmap_monitor[i], val ? 1 : 0);
    }
}


int pfq_devmap_update(int action, int index, int queue, pfq_gid_t gid)
{
    int n = 0, i,q;

    if (unlikely((__force int)gid >= Q_MAX_GID ||
		 (__force int)gid < 0)) {
        pr_devel("[PF_Q] devmap_update: bad gid (%u)\n",gid);
        return 0;
    }

    mutex_lock(&devmap_lock);

    for(i=0; i < Q_MAX_DEVICE; ++i)
    {
        for(q=0; q < Q_MAX_HW_QUEUE; ++q)
        {
            unsigned long tmp;

            if (!pfq_devmap_equal(i, q, index, queue))
                continue;

            /* map_set... */
            if (action == map_set) {

                tmp = atomic_long_read(&pfq_devmap[i][q]);
                tmp |= 1L << (__force int)gid;
                atomic_long_set(&pfq_devmap[i][q], tmp);
                n++;
                continue;
            }

            /* map_reset */
            tmp = atomic_long_read(&pfq_devmap[i][q]);
            if (tmp & (1L << (__force int)gid)) {
                tmp &= ~(1L << (__force int)gid);
                atomic_long_set(&pfq_devmap[i][q], tmp);
                n++;
                continue;
            }
        }
    }

    /* update capture monitor filter... */

    pfq_devmap_monitor_update();

    mutex_unlock(&devmap_lock);
    return n;
}

