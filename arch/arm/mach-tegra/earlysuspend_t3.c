/* Copyright (c) 2012, Will Tisdale <willtisdale@gmail.com>. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

/*
 * Driver to switch Tegra3 SoC to LP cluster only whilst screen off.
 */

#include <linux/kernel.h>
#include <linux/cpu.h>
#include <linux/earlysuspend.h>
#include <linux/spinlock.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/pm_qos_params.h>
#include <linux/delay.h>
#include <linux/workqueue.h>

#include "sleep.h"
#include "pm.h"
#include "clock.h"

static DEFINE_SPINLOCK(earlysuspend_t3_lock);

extern void disable_auto_hotplug(void);
extern void enable_auto_hotplug(void);
extern void hp_stats_update(unsigned int cpu, bool up);

struct pm_qos_request_list single_core_req;

struct delayed_work earlysuspend_t3_lp_switch_work;

static int earlysuspend_t3_lp_switch(bool enabled)
{
	int err = 0;
	unsigned int flags = 0;
	struct clk *cpu_clk = tegra_get_clock_by_name("cpu");
	struct clk *cpu_lp_clk = tegra_get_clock_by_name("cpu_lp");

	if(likely(num_online_cpus() == 1)) {
		disable_auto_hotplug();

		if (enabled) {
			if (is_lp_cluster()) {
				printk("%s: LP cluster currently active\n", __func__);
				return 0;
			}

			spin_lock(&earlysuspend_t3_lock);
			flags &= ~TEGRA_POWER_CLUSTER_MASK;
			flags |= TEGRA_POWER_CLUSTER_LP;
			printk("%s: Switching to LP cluster\n", __func__);
			tegra_cluster_switch_set_parameters(0, flags);
			spin_unlock(&earlysuspend_t3_lock);

			err = clk_set_parent(cpu_clk, cpu_lp_clk);
			if (unlikely(err))
				printk(KERN_WARNING "%s: Request failed %d\n",
								__func__, err);
			else {
				hp_stats_update(CONFIG_NR_CPUS, true);
				hp_stats_update(0, false);
			}
		}
	} else
		WARN(1, KERN_ERR "%s: Failed switch to LP: num_online_cpus(): %d\n"
						, __func__, num_online_cpus());

	return err;
}

static void earlysuspend_t3_lp_force(struct work_struct *work)
{
	earlysuspend_t3_lp_switch(true);
}

static void earlysuspend_t3_early_suspend(struct early_suspend *handler)
{
	/*
	 * Use the PM QoS API to disable secondary CPUs.
	 * Assuming we are not already using the LP cluster, schedule
	 * work with a 10 second delay to allow the QoS request time to
	 * disable the online CPUs using tegra automatic hotplugging
	 * Assuming the system is lightly loaded, the LP cluster should
	 * already be in use and the work won't be scheduled.
	 */
	pm_qos_add_request(&single_core_req, PM_QOS_MAX_ONLINE_CPUS, 1);
	if (is_lp_cluster()) {
		disable_auto_hotplug();
		if (unlikely(!is_lp_cluster())) {
			WARN(1, KERN_ERR "%s: Something is messing with clusters \
				  with hotplugging disabled\n", __func__);
			return;
		}
		printk("%s: LP cluster currently active\n", __func__);
		return;
	}
	schedule_delayed_work_on(0, &earlysuspend_t3_lp_switch_work, HZ * 10);
}

static void earlysuspend_t3_late_resume(struct early_suspend *handler)
{
	bool ret;

	ret = cancel_delayed_work_sync(&earlysuspend_t3_lp_switch_work);
	if (ret)
		printk("%s: Cancelled delayed work\n", __func__);

	pm_qos_remove_request(&single_core_req);
	enable_auto_hotplug();
}

static struct early_suspend earlysuspend_t3_suspend = {
	.suspend = earlysuspend_t3_early_suspend,
	.resume = earlysuspend_t3_late_resume,
};

static int __init earlysuspend_t3_init(void)
{
	INIT_DELAYED_WORK_DEFERRABLE(&earlysuspend_t3_lp_switch_work,
					earlysuspend_t3_lp_force);
	register_early_suspend(&earlysuspend_t3_suspend);
	return 0;
}

late_initcall(earlysuspend_t3_init);
