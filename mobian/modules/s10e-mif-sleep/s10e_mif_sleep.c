// SPDX-License-Identifier: GPL-2.0
/*
 * s10e_mif_sleep - MIF (DRAM) at its lowest level while the Exynos 9820 sleeps.
 *
 * s2idle keeps the DRAM controller where mainline put it at boot, 2093 MHz:
 * the meter reads ~130 mW on buck1m there against ~22 mW at 421 MHz, and the
 * DRAM I/O rail (ldo4m) drops ~19 mA too, on a sleep that draws ~1 W in all.
 * Nothing scans out or computes while asleep (s10e-sleepd sleeps only with
 * the screen saver up), so ask ACPM for sleep_mhz just before the tasks
 * freeze and give the old rate back once they run again.
 */
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/suspend.h>

#define MHZ		1000000UL
#define ACPM_MIF	0

static unsigned int sleep_mhz = 421;
module_param(sleep_mhz, uint, 0644);
MODULE_PARM_DESC(sleep_mhz, "MIF while asleep, in MHz (0 = leave it alone)");

static unsigned long nr_sleeps, nr_failed;

static int stats_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%lu sleeps at low MIF, %lu failed\n", nr_sleeps, nr_failed);
}

static const struct kernel_param_ops stats_ops = {
	.get = stats_get,
};
module_param_cb(stats, &stats_ops, NULL, 0444);

static struct clk *mif;
static unsigned long awake_rate;	/* to give back, 0 = nothing was changed */

static int mif_sleep_pm(struct notifier_block *nb, unsigned long event, void *data)
{
	unsigned int mhz = READ_ONCE(sleep_mhz);
	int ret;

	switch (event) {
	case PM_SUSPEND_PREPARE:
		awake_rate = 0;
		if (!mhz)
			break;
		awake_rate = clk_get_rate(mif);
		if (awake_rate <= mhz * MHZ) {
			awake_rate = 0;
			break;
		}
		ret = clk_set_rate(mif, mhz * MHZ);
		if (ret) {
			pr_warn_ratelimited("s10e_mif_sleep: %u MHz: %d\n", mhz, ret);
			awake_rate = 0;
			nr_failed++;
		} else {
			nr_sleeps++;
		}
		break;
	case PM_POST_SUSPEND:
		if (awake_rate) {
			ret = clk_set_rate(mif, awake_rate);
			if (ret)
				pr_warn_ratelimited("s10e_mif_sleep: back to %lu MHz: %d\n",
						    awake_rate / MHZ, ret);
			awake_rate = 0;
		}
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block mif_sleep_nb = {
	.notifier_call = mif_sleep_pm,
};

static int __init mif_sleep_init(void)
{
	struct of_phandle_args spec = { .args_count = 1, .args = { ACPM_MIF } };
	int ret;

	spec.np = of_find_compatible_node(NULL, NULL, "samsung,exynos9820-acpm-ipc");
	if (!spec.np)
		return -ENODEV;
	mif = of_clk_get_from_provider(&spec);
	of_node_put(spec.np);
	if (IS_ERR(mif))
		return PTR_ERR(mif);

	ret = register_pm_notifier(&mif_sleep_nb);
	if (ret) {
		clk_put(mif);
		return ret;
	}
	pr_info("s10e_mif_sleep: MIF %lu MHz awake, %u MHz asleep\n",
		clk_get_rate(mif) / MHZ, sleep_mhz);
	return 0;
}

static void __exit mif_sleep_exit(void)
{
	unregister_pm_notifier(&mif_sleep_nb);
	clk_put(mif);
}

module_init(mif_sleep_init);
module_exit(mif_sleep_exit);
MODULE_DESCRIPTION("Exynos 9820: lowest MIF level during s2idle");
MODULE_LICENSE("GPL");
