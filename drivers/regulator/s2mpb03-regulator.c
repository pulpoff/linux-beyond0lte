// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung S2MPB03 camera PMIC: seven LDOs, the rails of the Galaxy S10e's
 * main camera (S5K2L4) and its OIS. Register map from Samsung's downstream
 * s2mpb03 driver.
 *
 * Rails without a node under "regulators" get no constraints, so the core
 * never switches them.
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>

#define S2MPB03_REG_PMIC_ID	0x00
#define S2MPB03_REG_L1CTRL	0x03
#define S2MPB03_REG_LDO_SLEW2	0x0b

#define S2MPB03_LDO_VSEL_MASK	0x3f
#define S2MPB03_LDO_ENABLE	BIT(7)

enum {
	S2MPB03_LDO1, S2MPB03_LDO2, S2MPB03_LDO3, S2MPB03_LDO4, S2MPB03_LDO5,
	S2MPB03_LDO6, S2MPB03_LDO7,
	S2MPB03_NUM,
};

static const struct regulator_ops s2mpb03_ldo_ops = {
	.list_voltage		= regulator_list_voltage_linear,
	.map_voltage		= regulator_map_voltage_linear,
	.get_voltage_sel	= regulator_get_voltage_sel_regmap,
	.set_voltage_sel	= regulator_set_voltage_sel_regmap,
	.enable			= regulator_enable_regmap,
	.disable		= regulator_disable_regmap,
	.is_enabled		= regulator_is_enabled_regmap,
};

#define S2MPB03_LDO(_n, _min, _step) {					\
	.name		= "ldo" #_n,					\
	.of_match	= of_match_ptr("ldo" #_n),			\
	.regulators_node = of_match_ptr("regulators"),			\
	.id		= S2MPB03_LDO##_n,				\
	.ops		= &s2mpb03_ldo_ops,				\
	.type		= REGULATOR_VOLTAGE,				\
	.owner		= THIS_MODULE,					\
	.min_uV		= _min,						\
	.uV_step	= _step,					\
	.n_voltages	= S2MPB03_LDO_VSEL_MASK + 1,			\
	.vsel_reg	= S2MPB03_REG_L1CTRL + (_n) - 1,		\
	.vsel_mask	= S2MPB03_LDO_VSEL_MASK,			\
	.enable_reg	= S2MPB03_REG_L1CTRL + (_n) - 1,		\
	.enable_mask	= S2MPB03_LDO_ENABLE,				\
	.enable_time	= 150,						\
}

static const struct regulator_desc s2mpb03_regulators[S2MPB03_NUM] = {
	S2MPB03_LDO(1, 700000, 12500), S2MPB03_LDO(2, 700000, 12500),
	S2MPB03_LDO(3, 700000, 25000), S2MPB03_LDO(4, 700000, 12500),
	S2MPB03_LDO(5, 1800000, 25000), S2MPB03_LDO(6, 1800000, 25000),
	S2MPB03_LDO(7, 1800000, 25000),
};

static const struct regmap_config s2mpb03_regmap = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= S2MPB03_REG_LDO_SLEW2,
};

static int s2mpb03_probe(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev;
	struct regulator_config cfg = { .dev = dev };
	struct regmap *regmap;
	unsigned int id;
	int i, ret;

	regmap = devm_regmap_init_i2c(i2c, &s2mpb03_regmap);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);
	ret = regmap_read(regmap, S2MPB03_REG_PMIC_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "no answer\n");
	dev_info(dev, "S2MPB03 rev %#x\n", id);

	cfg.regmap = regmap;
	for (i = 0; i < S2MPB03_NUM; i++) {
		struct regulator_dev *rdev;

		rdev = devm_regulator_register(dev, &s2mpb03_regulators[i], &cfg);
		if (IS_ERR(rdev))
			return dev_err_probe(dev, PTR_ERR(rdev), "%s\n",
					     s2mpb03_regulators[i].name);
	}
	return 0;
}

static const struct of_device_id s2mpb03_of_match[] = {
	{ .compatible = "samsung,s2mpb03" },
	{ }
};
MODULE_DEVICE_TABLE(of, s2mpb03_of_match);

static const struct i2c_device_id s2mpb03_id[] = {
	{ "s2mpb03" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, s2mpb03_id);

static struct i2c_driver s2mpb03_driver = {
	.driver = {
		.name = "s2mpb03",
		.of_match_table = s2mpb03_of_match,
	},
	.probe = s2mpb03_probe,
	.id_table = s2mpb03_id,
};
module_i2c_driver(s2mpb03_driver);

MODULE_DESCRIPTION("Samsung S2MPB03 camera PMIC regulators");
MODULE_LICENSE("GPL");
