// SPDX-License-Identifier: GPL-2.0
//
// Stage D probe for the S10e audio block: power on pd_aud through genpd
// (which now issues the TZPC restore SMC), enable the CMU_TOP AUD clock, and
// read back a few CMU_AUD and ABOX registers to prove the block is reachable.
// Nothing is written. Keep this module blacklisted and load it by hand.

#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

/*
 * 0: power on only (genpd + SMC), no register access
 * 1: also enable the clock and read CMU_AUD
 * 2: also read the AUD sysreg
 * 3: also read ABOX SFR
 */
static int stage;
module_param(stage, int, 0444);

/* issue the TZPC restore SMC directly, for a domain left on by the bootloader */
static bool smc;
module_param(smc, bool, 0444);

/* stage 9: after power-on and the clock, read just this one physical address */
static ulong phys;
module_param(phys, ulong, 0444);

struct aud_probe {
	struct clk *clk;
};

static const struct {
	const char *res;
	unsigned int off;
	const char *name;
} regs[] = {
	{ "cmu",    0x0000, "PLL_LOCKTIME_PLL_AUD" },
	{ "cmu",    0x0100, "PLL_CON0_MUX_CLKCMU_AUD_CPU_USER" },
	{ "cmu",    0x0120, "PLL_CON0_PLL_AUD0" },
	{ "cmu",    0x0124, "PLL_CON1_PLL_AUD0" },
	{ "cmu",    0x0140, "PLL_CON0_PLL_AUD1" },
	{ "cmu",    0x1000, "MUX_CLK_AUD_CPU" },
	{ "cmu",    0x100c, "MUX_CLK_AUD_UAIF0" },
	{ "cmu",    0x1010, "MUX_CLK_AUD_UAIF1" },
	{ "cmu",    0x1800, "DIV_CLK_AUD_AUDIF" },
	{ "cmu",    0x1804, "DIV_CLK_AUD_BUS" },
	{ "cmu",    0x182c, "DIV_CLK_AUD_UAIF0" },
	{ "cmu",    0x1830, "DIV_CLK_AUD_UAIF1" },
	{ "sfr",    0x0000, "ABOX_IP_INDEX" },
	{ "sfr",    0x0004, "ABOX_VERSION" },
	{ "sfr",    0x0010, "ABOX_SYSPOWER_CTRL" },
	{ "sfr",    0x0014, "ABOX_SYSPOWER_STATUS" },
	{ "sfr",    0x0038, "ABOX_QCHANNEL_DISABLE" },
	{ "sfr",    0x0510, "ABOX_UAIF1_CTRL0" },
	{ "sfr",    0x051c, "ABOX_UAIF1_STATUS" },
	{ "sysreg", 0x032c, "ABOX_SYSREG_MISC_CON" },
};

static int aud_probe_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	void __iomem *base[3];
	static const char * const names[] = { "cmu", "sfr", "sysreg" };
	struct aud_probe *ap;
	int i, j, ret;

	ap = devm_kzalloc(dev, sizeof(*ap), GFP_KERNEL);
	if (!ap)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		base[i] = devm_platform_ioremap_resource_byname(pdev, names[i]);
		if (IS_ERR(base[i]))
			return PTR_ERR(base[i]);
	}

	ap->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(ap->clk))
		return dev_err_probe(dev, PTR_ERR(ap->clk), "no clock\n");

	devm_pm_runtime_enable(dev);
	dev_info(dev, "powering on pd_aud\n");
	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return dev_err_probe(dev, ret, "pd_aud power-on failed\n");
	dev_info(dev, "pd_aud on, TZPC restored (stage %d)\n", stage);
	if (stage < 1) {
		platform_set_drvdata(pdev, ap);
		return 0;
	}

	ret = clk_prepare_enable(ap->clk);
	if (ret) {
		pm_runtime_put(dev);
		return dev_err_probe(dev, ret, "clock enable failed\n");
	}
	dev_info(dev, "aud clock %lu Hz\n", clk_get_rate(ap->clk));

	if (smc) {
		struct arm_smccc_res res;

		dev_info(dev, "TZPC restore SMC\n");
		msleep(500);
		arm_smccc_smc(0x82000410, 1, 0x18c62000, 2, 0, 0, 0, 0, &res);
		dev_info(dev, "TZPC restore SMC returned %ld\n", (long)res.a0);
		msleep(500);
	}

	if (stage == 9 && phys) {
		void __iomem *p = devm_ioremap(dev, phys & ~0xfffUL, 0x1000);

		if (!p)
			return -ENOMEM;
		dev_info(dev, "reading 0x%08lx\n", phys);
		msleep(500);
		dev_info(dev, "0x%08lx = 0x%08x\n", phys,
			 readl(p + (phys & 0xfff)));
		platform_set_drvdata(pdev, ap);
		return 0;
	}

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		if (stage < 2 && strcmp(regs[i].res, "cmu"))
			continue;
		if (stage < 3 && !strcmp(regs[i].res, "sfr"))
			continue;
		for (j = 0; strcmp(names[j], regs[i].res); j++)
			;
		if (strcmp(regs[i].res, "cmu")) {
			dev_info(dev, "reading %s +0x%04x\n", regs[i].res,
				 regs[i].off);
			msleep(500);
		}
		dev_info(dev, "%-6s +0x%04x %-34s = 0x%08x\n", regs[i].res,
			 regs[i].off, regs[i].name,
			 readl(base[j] + regs[i].off));
	}

	platform_set_drvdata(pdev, ap);
	return 0;
}

static void aud_probe_remove(struct platform_device *pdev)
{
	struct aud_probe *ap = platform_get_drvdata(pdev);

	if (stage >= 1)
		clk_disable_unprepare(ap->clk);
	pm_runtime_put_sync(&pdev->dev);
}

static const struct of_device_id aud_probe_of_match[] = {
	{ .compatible = "samsung,s10e-aud-probe" },
	{ }
};
MODULE_DEVICE_TABLE(of, aud_probe_of_match);

static struct platform_driver aud_probe_driver = {
	.driver = {
		.name = "s10e-aud-probe",
		.of_match_table = aud_probe_of_match,
	},
	.probe = aud_probe_probe,
	.remove = aud_probe_remove,
};
module_platform_driver(aud_probe_driver);

MODULE_DESCRIPTION("S10e audio block power-on probe");
MODULE_LICENSE("GPL");
