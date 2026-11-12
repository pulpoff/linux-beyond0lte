// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Copyright (C) 2013 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

#define pr_fmt(fmt) "psci: " fmt

#include <linux/init.h>
#include <linux/of.h>
#include <linux/smp.h>
#include <linux/delay.h>
#include <linux/psci.h>
#include <linux/mm.h>

#include <uapi/linux/psci.h>

#include <asm/cpu_ops.h>
#include <asm/errno.h>
#include <asm/smp_plat.h>
#include <asm/topology.h>

/*
 * Samsung's EL3 on the Exynos9820 reads CPU_OFF's power state: stock passes
 * affinity level 1 for the last CPU of a cluster to have the cluster taken
 * down as well. Passed level 0, the cluster stays up as far as the firmware
 * knows; a system sleep then powers it off underneath, and after the wake the
 * second CPU of the big cluster hung coming up (2026-11-08).
 */
static bool psci_off_cluster_level;

/*
 * No other CPU of @cpu's power cluster online: MPIDR affinity level 1, as the
 * firmware counts. Not the scheduler's clusters: the 9820's little and middle
 * cores are two of those but one DSU, and the firmware refuses a cluster-level
 * CPU_OFF for the last middle core (-EINVAL, the kernel's BUG in cpu_die).
 */
static bool psci_cluster_last(unsigned int cpu)
{
	u64 cluster = MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 1);
	unsigned int other;

	for_each_online_cpu(other)
		if (other != cpu && MPIDR_AFFINITY_LEVEL(cpu_logical_map(other), 1) == cluster)
			return false;
	return true;
}

static int __init cpu_psci_cpu_init(unsigned int cpu)
{
	if (of_machine_is_compatible("samsung,exynos9820"))
		psci_off_cluster_level = true;
	return 0;
}

static int __init cpu_psci_cpu_prepare(unsigned int cpu)
{
	if (!psci_ops.cpu_on) {
		pr_err("no cpu_on method, not booting CPU%d\n", cpu);
		return -ENODEV;
	}

	return 0;
}

static int cpu_psci_cpu_boot(unsigned int cpu)
{
	phys_addr_t pa_secondary_entry = __pa_symbol(secondary_entry);
	int err = psci_ops.cpu_on(cpu_logical_map(cpu), pa_secondary_entry);
	if (err && err != -EPERM)
		pr_err("failed to boot CPU%d (%d)\n", cpu, err);

	return err;
}

#ifdef CONFIG_HOTPLUG_CPU
static bool cpu_psci_cpu_can_disable(unsigned int cpu)
{
	return !psci_tos_resident_on(cpu);
}

static int cpu_psci_cpu_disable(unsigned int cpu)
{
	/* Fail early if we don't have CPU_OFF support */
	if (!psci_ops.cpu_off)
		return -EOPNOTSUPP;

	/* Trusted OS will deny CPU_OFF */
	if (psci_tos_resident_on(cpu))
		return -EPERM;

	return 0;
}

static void cpu_psci_cpu_die(unsigned int cpu)
{
	/*
	 * There are no known implementations of PSCI actually using the
	 * power state field, pass a sensible default for now.
	 */
	u32 state = PSCI_POWER_STATE_TYPE_POWER_DOWN <<
		    PSCI_0_2_POWER_STATE_TYPE_SHIFT;

	if (psci_off_cluster_level && psci_cluster_last(cpu))
		state |= 1 << PSCI_0_2_POWER_STATE_AFFL_SHIFT;

	psci_ops.cpu_off(state);
}

static int cpu_psci_cpu_kill(unsigned int cpu)
{
	int err;
	unsigned long start, end;

	if (!psci_ops.affinity_info)
		return 0;
	/*
	 * cpu_kill could race with cpu_die and we can
	 * potentially end up declaring this cpu undead
	 * while it is dying. So, try again a few times.
	 */

	start = jiffies;
	end = start + msecs_to_jiffies(100);
	do {
		err = psci_ops.affinity_info(cpu_logical_map(cpu), 0);
		if (err == PSCI_0_2_AFFINITY_LEVEL_OFF) {
			pr_info("CPU%d killed (polled %d ms)\n", cpu,
				jiffies_to_msecs(jiffies - start));
			return 0;
		}

		usleep_range(100, 1000);
	} while (time_before(jiffies, end));

	pr_warn("CPU%d may not have shut down cleanly (AFFINITY_INFO reports %d)\n",
			cpu, err);
	return -ETIMEDOUT;
}
#endif

const struct cpu_operations cpu_psci_ops = {
	.name		= "psci",
	.cpu_init	= cpu_psci_cpu_init,
	.cpu_prepare	= cpu_psci_cpu_prepare,
	.cpu_boot	= cpu_psci_cpu_boot,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_can_disable = cpu_psci_cpu_can_disable,
	.cpu_disable	= cpu_psci_cpu_disable,
	.cpu_die	= cpu_psci_cpu_die,
	.cpu_kill	= cpu_psci_cpu_kill,
#endif
};

