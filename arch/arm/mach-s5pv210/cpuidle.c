/*
 * arch/arm/mach-s5pv210/cpuidle.c
 *
 * Copyright (c) Samsung Electronics Co. Ltd
 * Copyright (c) 2012 - Will Tisdale <willtisdale@gmail.com>
 *
 * CPU idle driver for S5PV210
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <asm/proc-fns.h>
#include <asm/cacheflush.h>
#include <linux/dma-mapping.h>

#include <mach/map.h>
#include <mach/regs-irq.h>
#include <mach/regs-clock.h>
#include <plat/pm.h>
#include <plat/devs.h>
#include <linux/cpuidle.h>

#ifdef CONFIG_S5P_IDLE2
#include <mach/idle2.h>
#include <linux/suspend.h>
#include <linux/workqueue.h>
#endif /* CONFIG_S5P_IDLE2 */

inline static void s5p_enter_idle(void)
{
	unsigned long tmp;

	tmp = __raw_readl(S5P_IDLE_CFG);
	tmp &= ~((3<<30)|(3<<28)|(1<<0));
	tmp |= ((2<<30)|(2<<28));
	__raw_writel(tmp, S5P_IDLE_CFG);

	tmp = __raw_readl(S5P_PWR_CFG);
	tmp &= S5P_CFG_WFI_CLEAN;
	__raw_writel(tmp, S5P_PWR_CFG);

	cpu_do_idle();
}

/* Actual code that puts the SoC in different idle states */
inline static int s5p_enter_idle_normal(struct cpuidle_device *device,
				struct cpuidle_state *state)
{
	struct timeval before, after;
	int idle_time;

	local_irq_disable();
	do_gettimeofday(&before);

	s5p_enter_idle();

	do_gettimeofday(&after);
	local_irq_enable();
	idle_time = (after.tv_sec - before.tv_sec) * USEC_PER_SEC +
			(after.tv_usec - before.tv_usec);
	return idle_time;
}

static DEFINE_PER_CPU(struct cpuidle_device, s5p_cpuidle_device);

static struct cpuidle_driver s5p_idle_driver = {
	.name =         "s5p_idle",
	.owner =        THIS_MODULE,
};

#ifdef CONFIG_S5P_IDLE2
inline static int s5p_enter_idle_idle2(struct cpuidle_device *device,
				struct cpuidle_state *state)
{
	struct timeval before, after;
	int idle_time;

	/* Reset flags */
	idle2_flags &= ~FORCE_C1_IDLE;
	idle2_flags &= ~FORCE_C2_IDLE;

	/* Check for reasons to disable DEEP-IDLE */
	if (unlikely(idle2_flags & DISABLED_BY_SUSPEND)) {
		idle2_flags |= FORCE_C1_IDLE;
		pr_debug("%s: Setting FORCE_C1_IDLE\n", __func__);
		goto begin_enter_state;
	}

	/* Check for reasons to disable DEEP-IDLE TOP block off */
	if (unlikely((idle2_flags & NEEDS_TOPON)
		|| (!(idle2_flags & EARLYSUSPEND_ACTIVE))
		|| (idle2_flags & EXTERNAL_ACTIVE))) {
		idle2_flags |= FORCE_C2_IDLE;
		pr_debug("%s: Setting FORCE_C2_IDLE\n", __func__);
		goto begin_enter_state;
	}

	/* Default for DEEP-IDLE TOP block on is to fall through to here */

begin_enter_state:
	do_gettimeofday(&before);

	local_irq_disable();
	local_fiq_disable();

	if (unlikely(s5p_vic_interrupt_pending() || need_resched())) {
		pr_debug("%s: Bailing before entry!\n", __func__);
		goto return_idle_time;
	}

	if (unlikely((idle2_flags & FORCE_C1_IDLE) || enter_idle2_check()))
		goto enter_C1_state;

	if (unlikely(idle2_flags & FORCE_C2_IDLE)) {
		goto enter_C2_state;
	}

	if (likely(!(idle2_flags & FORCE_C2_IDLE)
		&& (!(idle2_flags & FORCE_C1_IDLE))))
		goto enter_C3_state;

	/*
	 * If somehow we fall through to here, we will end up in
	 * C1, which is the safe state, but warn anyway, because
	 * we shouldn't have got here.
	 */
	WARN_ON(0);

enter_C1_state:
	s5p_enter_idle();
	goto return_idle_time;

enter_C2_state:
	s5p_enter_idle2(true);
	goto return_idle_time;

enter_C3_state:
	s5p_enter_idle2(false);
	goto return_idle_time;

return_idle_time:
	do_gettimeofday(&after);
	local_irq_enable();
	local_fiq_enable();

	/*
	 * TODO: Fix the stat reporting or write a simple stat exporter
	 * the current stats are utterly useless other than to see if
	 * cpuidle is working or not. :(
	 */
	idle_time = (after.tv_sec - before.tv_sec) * USEC_PER_SEC +
			(after.tv_usec - before.tv_usec);
	return idle_time;
}

void earlysuspend_active_fn(bool flag)
{
	if (flag)
		idle2_flags |= EARLYSUSPEND_ACTIVE;
	else
		idle2_flags &= ~EARLYSUSPEND_ACTIVE;
	pr_info("earlysuspend_active: %d\n", flag);
}

static void external_active_fn(bool flag)
{
	if (flag)
		idle2_flags |= EXTERNAL_ACTIVE;
	else
		idle2_flags &= ~EXTERNAL_ACTIVE;
	pr_info("external_active: %d\n", flag);
}

static void needs_topon_fn(bool flag)
{
	if (flag)
		idle2_flags |= NEEDS_TOPON;
	else
		idle2_flags &= ~NEEDS_TOPON;
	pr_info("needs_topon: %d\n", flag);
}

static void idle2_external_active_work_fn(struct work_struct *work)
{
	cancel_delayed_work_sync(&idle2_external_inactive_work);
	external_active_fn(true);
}

static void idle2_external_inactive_work_fn(struct work_struct *work)
{
	external_active_fn(false);
}

static void idle2_enable_topon_work_fn(struct work_struct *work)
{
	cancel_delayed_work_sync(&idle2_cancel_topon_work);
	needs_topon_fn(true);
}

static void idle2_cancel_topon_work_fn(struct work_struct *work)
{
	needs_topon_fn(false);
}

void idle2_external_active(void)
{
	if ((idle2_flags & WORK_INITIALISED)
		&& ((!(idle2_flags & EXTERNAL_ACTIVE))
		|| (idle2_flags & INACTIVE_PENDING))) {
		queue_work(idle2_wq, &idle2_external_active_work);
		idle2_flags &= ~INACTIVE_PENDING;
		pr_debug("inactive_pending: false\n");
	}
}

void idle2_external_inactive(unsigned long delay)
{
	if ((idle2_flags & WORK_INITIALISED)
		&& (idle2_flags & EXTERNAL_ACTIVE)
		&& (!(idle2_flags & INACTIVE_PENDING))) {
		idle2_flags |= INACTIVE_PENDING;
		queue_delayed_work(idle2_wq, &idle2_external_inactive_work, delay);
		pr_debug("inactive_pending: true\n");
	}
}

void idle2_needs_topon(void)
{
	if ((idle2_flags & WORK_INITIALISED)
		&& (!(idle2_flags & NEEDS_TOPON)
		|| (idle2_flags & TOPON_CANCEL_PENDING))) {
		queue_work(idle2_wq, &idle2_enable_topon_work);
		idle2_flags &= ~TOPON_CANCEL_PENDING;
		pr_debug("topon_cancel_pending: false\n");
	}
}

void idle2_cancel_topon(unsigned long delay)
{
	if ((idle2_flags & WORK_INITIALISED)
		&& (idle2_flags & NEEDS_TOPON)
		&& (!(idle2_flags & TOPON_CANCEL_PENDING))) {
		idle2_flags |= TOPON_CANCEL_PENDING;
		queue_delayed_work(idle2_wq, &idle2_cancel_topon_work, delay);
		pr_debug("topon_cancel_pending: true\n");
	}
}

static int idle2_pm_notify(struct notifier_block *nb,
	unsigned long event, void *dummy)
{
	if (event == PM_SUSPEND_PREPARE) {
		idle2_flags |= DISABLED_BY_SUSPEND;
		pr_info("%s: IDLE2 disabled\n", __func__);
	}
	else if (event == PM_POST_SUSPEND) {
		idle2_flags &= ~DISABLED_BY_SUSPEND;
		pr_info("%s: IDLE2 enabled\n", __func__);
	}
	return NOTIFY_OK;
}

static struct notifier_block idle2_pm_notifier = {
	.notifier_call = idle2_pm_notify,
};

#endif /* CONFIG_S5P_IDLE2 */

static void idle2_enable_hlt(struct work_struct *work)
{
        enable_hlt();
        pr_info("%s: Enabling CPUidle\n", __func__);
}
static DECLARE_DELAYED_WORK(idle2_enable_hlt_work, idle2_enable_hlt);

/* Initialize CPU idle by registering the idle states */
static int s5p_init_cpuidle(void)
{
	struct cpuidle_device *device;

	disable_hlt();
        schedule_delayed_work(&idle2_enable_hlt_work, 30 * HZ);
        pr_info("%s: Disabling CPUidle\n", __func__);

#ifdef CONFIG_S5P_IDLE2
	idle2_wq = create_singlethread_workqueue("idle2_workqueue");
	BUG_ON(!idle2_wq);
	INIT_WORK(&idle2_external_active_work, idle2_external_active_work_fn);
	INIT_DELAYED_WORK(&idle2_external_inactive_work, idle2_external_inactive_work_fn);
	INIT_WORK(&idle2_enable_topon_work, idle2_enable_topon_work_fn);
	INIT_DELAYED_WORK(&idle2_cancel_topon_work, idle2_cancel_topon_work_fn);
	idle2_flags |= WORK_INITIALISED;
#endif /* CONFIG_S5P_IDLE2 */

	cpuidle_register_driver(&s5p_idle_driver);

	device = &per_cpu(s5p_cpuidle_device, smp_processor_id());
	device->state_count = 0;

#ifndef CONFIG_S5P_IDLE2
	/* Wait for interrupt state */
	device->states[0].enter = s5p_enter_idle_normal;
	device->states[0].exit_latency = 1;	/* uS */
	device->states[0].target_residency = 10000;
	device->states[0].flags = CPUIDLE_FLAG_TIME_VALID;
	strcpy(device->states[0].name, "IDLE");
	strcpy(device->states[0].desc, "ARM clock gating - WFI");
	device->state_count++;
#else
	/* Deep-Idle top OFF Wait for interrupt state */
	device->states[0].enter = s5p_enter_idle_idle2;
	device->states[0].exit_latency = 1;
	device->states[0].target_residency = 5000;
	device->states[0].flags = CPUIDLE_FLAG_TIME_VALID |
					CPUIDLE_FLAG_CHECK_BM;
	strcpy(device->states[0].name, "IDLE2");
	strcpy(device->states[0].desc, "ARM (TOP/SUB) Power gating - WFI");
	device->state_count++;
#endif

	if (cpuidle_register_device(device)) {
		pr_err("s5p_init_cpuidle: Failed registering\n");
		BUG();
		return -EIO;
	}
#ifdef CONFIG_S5P_IDLE2
	pr_info("cpuidle: IDLE2 support enabled - version 0.310 by <willtisdale@gmail.com>\n");

	register_pm_notifier(&idle2_pm_notifier);

	return s5p_init_remap();
#else /* CONFIG_S5P_IDLE2 */
	return 0;
#endif
}

device_initcall(s5p_init_cpuidle);
