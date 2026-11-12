// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 * Author: Tomasz Figa <t.figa@samsung.com>
 *
 * Clock driver for Exynos clock output
 */

#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm.h>

#define DRV_NAME			"exynos-clkout"

#define EXYNOS_CLKOUT_NR_CLKS		2
#define EXYNOS_CLKOUT_PARENTS		32

#define EXYNOS_PMU_DEBUG_REG		0xa00
#define EXYNOS9820_PMU_CLKOUT1_REG	0xa0c
#define EXYNOS9820_CLKOUT1_SEL_MASK	0x3f	/* [13:8], 0 = TCXO */
#define EXYNOS_CLKOUT_DISABLE_SHIFT	0
#define EXYNOS_CLKOUT_MUX_SHIFT		8
#define EXYNOS4_CLKOUT_MUX_MASK		0xf
#define EXYNOS5_CLKOUT_MUX_MASK		0x1f

struct exynos_clkout {
	struct clk_gate gate;
	struct clk_mux mux;
	spinlock_t slock;
	void __iomem *reg;
	struct device_node *np;
	u32 pmu_debug_save;
	u32 clkout1_save;
	bool has_clkout1;
	struct clk_hw_onecell_data data;
};

struct exynos_clkout_variant {
	u32 mux_mask;
	/*
	 * Exynos 9820: a second output, CLKOUT1 (stock's OSC_NFC), whose source
	 * 0 is the TCXO that is CLKOUT0's source 1. Only the TCXO is offered.
	 */
	bool has_clkout1;
};

static const struct exynos_clkout_variant exynos_clkout_exynos4 = {
	.mux_mask	= EXYNOS4_CLKOUT_MUX_MASK,
};

static const struct exynos_clkout_variant exynos_clkout_exynos5 = {
	.mux_mask	= EXYNOS5_CLKOUT_MUX_MASK,
};

static const struct exynos_clkout_variant exynos_clkout_exynos9820 = {
	.mux_mask	= EXYNOS5_CLKOUT_MUX_MASK,
	.has_clkout1	= true,
};

static const struct of_device_id exynos_clkout_ids[] = {
	{
		.compatible = "samsung,exynos3250-pmu",
		.data = &exynos_clkout_exynos4,
	}, {
		.compatible = "samsung,exynos4210-pmu",
		.data = &exynos_clkout_exynos4,
	}, {
		.compatible = "samsung,exynos4212-pmu",
		.data = &exynos_clkout_exynos4,
	}, {
		.compatible = "samsung,exynos4412-pmu",
		.data = &exynos_clkout_exynos4,
	}, {
		.compatible = "samsung,exynos5250-pmu",
		.data = &exynos_clkout_exynos5,
	}, {
		.compatible = "samsung,exynos5410-pmu",
		.data = &exynos_clkout_exynos5,
	}, {
		.compatible = "samsung,exynos5420-pmu",
		.data = &exynos_clkout_exynos5,
	}, {
		.compatible = "samsung,exynos5433-pmu",
		.data = &exynos_clkout_exynos5,
	}, {
		.compatible = "samsung,exynos9820-pmu",
		.data = &exynos_clkout_exynos9820,
	}, { }
};

/*
 * Device will be instantiated as child of PMU device without its own
 * device node.  Therefore match compatibles against parent.
 */
static int exynos_clkout_match_parent_dev(struct device *dev, u32 *mux_mask,
					  bool *has_clkout1)
{
	const struct exynos_clkout_variant *variant;
	const struct of_device_id *match;

	if (!dev->parent) {
		dev_err(dev, "not instantiated from MFD\n");
		return -EINVAL;
	}

	/*
	 * 'exynos_clkout_ids' arrays is not the ids array matched by
	 * the dev->parent driver, so of_device_get_match_data() or
	 * device_get_match_data() cannot be used here.
	 */
	match = of_match_device(exynos_clkout_ids, dev->parent);
	if (!match) {
		dev_err(dev, "cannot match parent device\n");
		return -EINVAL;
	}
	variant = match->data;

	*mux_mask = variant->mux_mask;
	*has_clkout1 = variant->has_clkout1;

	return 0;
}

static int exynos_clkout_probe(struct platform_device *pdev)
{
	const char *parent_names[EXYNOS_CLKOUT_PARENTS];
	struct clk *parents[EXYNOS_CLKOUT_PARENTS];
	struct exynos_clkout *clkout;
	int parent_count, ret, i;
	u32 mux_mask;

	clkout = devm_kzalloc(&pdev->dev,
			      struct_size(clkout, data.hws, EXYNOS_CLKOUT_NR_CLKS),
			      GFP_KERNEL);
	if (!clkout)
		return -ENOMEM;

	ret = exynos_clkout_match_parent_dev(&pdev->dev, &mux_mask,
					     &clkout->has_clkout1);
	if (ret)
		return ret;

	clkout->np = pdev->dev.of_node;
	if (!clkout->np) {
		/*
		 * pdev->dev.parent was checked by exynos_clkout_match_parent_dev()
		 * so it is not NULL.
		 */
		clkout->np = pdev->dev.parent->of_node;
	}

	platform_set_drvdata(pdev, clkout);

	spin_lock_init(&clkout->slock);

	parent_count = 0;
	for (i = 0; i < EXYNOS_CLKOUT_PARENTS; ++i) {
		char name[] = "clkoutXX";

		snprintf(name, sizeof(name), "clkout%d", i);
		parents[i] = of_clk_get_by_name(clkout->np, name);
		if (IS_ERR(parents[i])) {
			parent_names[i] = "none";
			continue;
		}

		parent_names[i] = __clk_get_name(parents[i]);
		parent_count = i + 1;
	}

	if (!parent_count)
		return -EINVAL;

	clkout->reg = of_iomap(clkout->np, 0);
	if (!clkout->reg) {
		ret = -ENODEV;
		goto clks_put;
	}

	clkout->gate.reg = clkout->reg + EXYNOS_PMU_DEBUG_REG;
	clkout->gate.bit_idx = EXYNOS_CLKOUT_DISABLE_SHIFT;
	clkout->gate.flags = CLK_GATE_SET_TO_DISABLE;
	clkout->gate.lock = &clkout->slock;

	clkout->mux.reg = clkout->reg + EXYNOS_PMU_DEBUG_REG;
	clkout->mux.mask = mux_mask;
	clkout->mux.shift = EXYNOS_CLKOUT_MUX_SHIFT;
	clkout->mux.lock = &clkout->slock;

	clkout->data.num = clkout->has_clkout1 ? 2 : 1;
	clkout->data.hws[0] = clk_hw_register_composite(NULL, "clkout",
				parent_names, parent_count, &clkout->mux.hw,
				&clk_mux_ops, NULL, NULL, &clkout->gate.hw,
				&clk_gate_ops, CLK_SET_RATE_PARENT
				| CLK_SET_RATE_NO_REPARENT);
	if (IS_ERR(clkout->data.hws[0])) {
		ret = PTR_ERR(clkout->data.hws[0]);
		goto err_unmap;
	}

	if (clkout->has_clkout1) {
		void __iomem *reg = clkout->reg + EXYNOS9820_PMU_CLKOUT1_REG;

		if (parent_count < 2 || IS_ERR(parents[1])) {
			ret = -EINVAL;
			goto err_clk_unreg;
		}
		/* the TCXO, still gated: its source field to 0 */
		writel(readl(reg) & ~(EXYNOS9820_CLKOUT1_SEL_MASK << EXYNOS_CLKOUT_MUX_SHIFT),
		       reg);
		clkout->data.hws[1] = clk_hw_register_gate(NULL, "clkout1",
				parent_names[1], CLK_SET_RATE_PARENT, reg,
				EXYNOS_CLKOUT_DISABLE_SHIFT,
				CLK_GATE_SET_TO_DISABLE, &clkout->slock);
		if (IS_ERR(clkout->data.hws[1])) {
			ret = PTR_ERR(clkout->data.hws[1]);
			goto err_clk_unreg;
		}
	}

	ret = of_clk_add_hw_provider(clkout->np, of_clk_hw_onecell_get, &clkout->data);
	if (ret)
		goto err_clk1_unreg;

	return 0;

err_clk1_unreg:
	if (clkout->has_clkout1)
		clk_hw_unregister_gate(clkout->data.hws[1]);
err_clk_unreg:
	clk_hw_unregister(clkout->data.hws[0]);
err_unmap:
	iounmap(clkout->reg);
clks_put:
	for (i = 0; i < EXYNOS_CLKOUT_PARENTS; ++i)
		if (!IS_ERR(parents[i]))
			clk_put(parents[i]);

	dev_err(&pdev->dev, "failed to register clkout clock\n");

	return ret;
}

static void exynos_clkout_remove(struct platform_device *pdev)
{
	struct exynos_clkout *clkout = platform_get_drvdata(pdev);

	of_clk_del_provider(clkout->np);
	if (clkout->has_clkout1)
		clk_hw_unregister_gate(clkout->data.hws[1]);
	clk_hw_unregister(clkout->data.hws[0]);
	iounmap(clkout->reg);
}

static int __maybe_unused exynos_clkout_suspend(struct device *dev)
{
	struct exynos_clkout *clkout = dev_get_drvdata(dev);

	clkout->pmu_debug_save = readl(clkout->reg + EXYNOS_PMU_DEBUG_REG);
	if (clkout->has_clkout1)
		clkout->clkout1_save = readl(clkout->reg + EXYNOS9820_PMU_CLKOUT1_REG);

	return 0;
}

static int __maybe_unused exynos_clkout_resume(struct device *dev)
{
	struct exynos_clkout *clkout = dev_get_drvdata(dev);

	writel(clkout->pmu_debug_save, clkout->reg + EXYNOS_PMU_DEBUG_REG);
	if (clkout->has_clkout1)
		writel(clkout->clkout1_save, clkout->reg + EXYNOS9820_PMU_CLKOUT1_REG);

	return 0;
}

static SIMPLE_DEV_PM_OPS(exynos_clkout_pm_ops, exynos_clkout_suspend,
			 exynos_clkout_resume);

static struct platform_driver exynos_clkout_driver = {
	.driver = {
		.name = DRV_NAME,
		.pm = &exynos_clkout_pm_ops,
	},
	.probe = exynos_clkout_probe,
	.remove = exynos_clkout_remove,
};
module_platform_driver(exynos_clkout_driver);

MODULE_AUTHOR("Krzysztof Kozlowski <krzk@kernel.org>");
MODULE_AUTHOR("Tomasz Figa <tomasz.figa@gmail.com>");
MODULE_DESCRIPTION("Samsung Exynos clock output driver");
MODULE_ALIAS("platform:" DRV_NAME);
MODULE_LICENSE("GPL");
