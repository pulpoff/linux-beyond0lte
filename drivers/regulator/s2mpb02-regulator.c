// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung S2MPB02 camera PMIC: 18 LDOs, two bucks and a buck-boost (the flash
 * LED driver in the same chip is not handled here). Register map from
 * Samsung's downstream s2mpb02 driver.
 *
 * Rails without a node under "regulators" get no constraints, so the core
 * never switches them; the always-on grip sensor rail stays as S-Boot left it.
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>

#define S2MPB02_REG_ID		0x00
#define S2MPB02_REG_B1CTRL1	0x0b
#define S2MPB02_REG_B1CTRL2	0x0c
#define S2MPB02_REG_B2CTRL1	0x0d
#define S2MPB02_REG_B2CTRL2	0x0e
#define S2MPB02_REG_BB1CTRL1	0x0f
#define S2MPB02_REG_BB1CTRL2	0x10
#define S2MPB02_REG_L1CTRL	0x1e
#define S2MPB02_REG_LDO_DSCH3	0x32

#define S2MPB02_LDO_VSEL_MASK	0x7f
#define S2MPB02_LDO_ENABLE	BIT(7)
#define S2MPB02_BUCK_ENABLE	GENMASK(7, 6)

enum {
	S2MPB02_LDO1, S2MPB02_LDO2, S2MPB02_LDO3, S2MPB02_LDO4, S2MPB02_LDO5,
	S2MPB02_LDO6, S2MPB02_LDO7, S2MPB02_LDO8, S2MPB02_LDO9, S2MPB02_LDO10,
	S2MPB02_LDO11, S2MPB02_LDO12, S2MPB02_LDO13, S2MPB02_LDO14, S2MPB02_LDO15,
	S2MPB02_LDO16, S2MPB02_LDO17, S2MPB02_LDO18,
	S2MPB02_BUCK1, S2MPB02_BUCK2, S2MPB02_BB1,
	S2MPB02_NUM,
};

static const struct regulator_ops s2mpb02_ldo_ops = {
	.list_voltage		= regulator_list_voltage_linear,
	.map_voltage		= regulator_map_voltage_linear,
	.get_voltage_sel	= regulator_get_voltage_sel_regmap,
	.set_voltage_sel	= regulator_set_voltage_sel_regmap,
	.enable			= regulator_enable_regmap,
	.disable		= regulator_disable_regmap,
	.is_enabled		= regulator_is_enabled_regmap,
};

static const struct regulator_ops s2mpb02_buck_ops = {
	.list_voltage		= regulator_list_voltage_linear,
	.map_voltage		= regulator_map_voltage_linear,
	.get_voltage_sel	= regulator_get_voltage_sel_regmap,
	.set_voltage_sel	= regulator_set_voltage_sel_regmap,
	.set_voltage_time_sel	= regulator_set_voltage_time_sel,
	.enable			= regulator_enable_regmap,
	.disable		= regulator_disable_regmap,
	.is_enabled		= regulator_is_enabled_regmap,
};

#define S2MPB02_LDO(_n, _step) {					\
	.name		= "ldo" #_n,					\
	.of_match	= of_match_ptr("ldo" #_n),			\
	.regulators_node = of_match_ptr("regulators"),			\
	.id		= S2MPB02_LDO##_n,				\
	.ops		= &s2mpb02_ldo_ops,				\
	.type		= REGULATOR_VOLTAGE,				\
	.owner		= THIS_MODULE,					\
	.min_uV		= 600000,					\
	.uV_step	= _step,					\
	.n_voltages	= S2MPB02_LDO_VSEL_MASK + 1,			\
	.vsel_reg	= S2MPB02_REG_L1CTRL + (_n) - 1,		\
	.vsel_mask	= S2MPB02_LDO_VSEL_MASK,			\
	.enable_reg	= S2MPB02_REG_L1CTRL + (_n) - 1,		\
	.enable_mask	= S2MPB02_LDO_ENABLE,				\
	.enable_time	= 180,						\
}

#define S2MPB02_BUCK(_name, _id, _min, _step, _ctrl1, _ctrl2, _time) {	\
	.name		= _name,					\
	.of_match	= of_match_ptr(_name),				\
	.regulators_node = of_match_ptr("regulators"),			\
	.id		= _id,						\
	.ops		= &s2mpb02_buck_ops,				\
	.type		= REGULATOR_VOLTAGE,				\
	.owner		= THIS_MODULE,					\
	.min_uV		= _min,						\
	.uV_step	= _step,					\
	.n_voltages	= 256,						\
	.ramp_delay	= 12000,					\
	.vsel_reg	= _ctrl2,					\
	.vsel_mask	= 0xff,						\
	.enable_reg	= _ctrl1,					\
	.enable_mask	= S2MPB02_BUCK_ENABLE,				\
	.enable_time	= _time,					\
}

static const struct regulator_desc s2mpb02_regulators[S2MPB02_NUM] = {
	S2MPB02_LDO(1, 12500), S2MPB02_LDO(2, 12500), S2MPB02_LDO(3, 12500),
	S2MPB02_LDO(4, 12500), S2MPB02_LDO(5, 12500),
	S2MPB02_LDO(6, 25000), S2MPB02_LDO(7, 25000), S2MPB02_LDO(8, 25000),
	S2MPB02_LDO(9, 25000), S2MPB02_LDO(10, 25000), S2MPB02_LDO(11, 25000),
	S2MPB02_LDO(12, 25000), S2MPB02_LDO(13, 25000), S2MPB02_LDO(14, 25000),
	S2MPB02_LDO(15, 25000), S2MPB02_LDO(16, 25000), S2MPB02_LDO(17, 25000),
	S2MPB02_LDO(18, 25000),
	S2MPB02_BUCK("buck1", S2MPB02_BUCK1, 400000, 6250,
		     S2MPB02_REG_B1CTRL1, S2MPB02_REG_B1CTRL2, 100),
	S2MPB02_BUCK("buck2", S2MPB02_BUCK2, 400000, 6250,
		     S2MPB02_REG_B2CTRL1, S2MPB02_REG_B2CTRL2, 100),
	S2MPB02_BUCK("bb1", S2MPB02_BB1, 2600000, 12500,
		     S2MPB02_REG_BB1CTRL1, S2MPB02_REG_BB1CTRL2, 210),
};

static const struct regmap_config s2mpb02_regmap = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= S2MPB02_REG_LDO_DSCH3,
};

static int s2mpb02_probe(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev;
	struct regulator_config cfg = { .dev = dev };
	struct regmap *regmap;
	unsigned int id;
	int i, ret;

	regmap = devm_regmap_init_i2c(i2c, &s2mpb02_regmap);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);
	ret = regmap_read(regmap, S2MPB02_REG_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "no answer\n");
	dev_info(dev, "S2MPB02 rev %#x\n", id);

	cfg.regmap = regmap;
	for (i = 0; i < S2MPB02_NUM; i++) {
		struct regulator_dev *rdev;

		rdev = devm_regulator_register(dev, &s2mpb02_regulators[i], &cfg);
		if (IS_ERR(rdev))
			return dev_err_probe(dev, PTR_ERR(rdev), "%s\n",
					     s2mpb02_regulators[i].name);
	}
	return 0;
}

static const struct of_device_id s2mpb02_of_match[] = {
	{ .compatible = "samsung,s2mpb02" },
	{ }
};
MODULE_DEVICE_TABLE(of, s2mpb02_of_match);

static const struct i2c_device_id s2mpb02_id[] = {
	{ "s2mpb02" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, s2mpb02_id);

static struct i2c_driver s2mpb02_driver = {
	.driver = {
		.name = "s2mpb02",
		.of_match_table = s2mpb02_of_match,
	},
	.probe = s2mpb02_probe,
	.id_table = s2mpb02_id,
};
module_i2c_driver(s2mpb02_driver);

MODULE_DESCRIPTION("Samsung S2MPB02 camera PMIC regulators");
MODULE_LICENSE("GPL");
