// SPDX-License-Identifier: GPL-2.0
/*
 * Based on max77650-charger.c
 *
 * Copyright (C) 2025 Dzmitry Sankouski <dsankouski@gmail.org>
 *
 * Battery charger driver for MAXIM 77705 charger/power-supply.
 */

#include <linux/devm-helpers.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/mfd/max77693-common.h>
#include <linux/mfd/max77705-private.h>
#include <linux/power/max77705_charger.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>

static const char *max77705_charger_model		= "max77705";
static const char *max77705_charger_manufacturer	= "Maxim Integrated";

static const struct regmap_config max77705_chg_regmap_config = {
	.reg_base = MAX77705_CHG_REG_BASE,
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MAX77705_CHG_REG_SAFEOUT_CTRL,
};

static enum power_supply_property max77705_charger_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
};

/*
 * The charger regulates its input voltage in hardware (VCHGIN_REG); this
 * only reports it. Stepping CHGIN_LIM down from here fights the CCIC,
 * which sets the input current itself under a PD contract (AUTOIBUS):
 * AICL_OK then stays clear and the limit was walked to its floor while
 * the input current was unchanged. With the OTG boost running, AICL is
 * the boost's own VBUS on CHGIN and never settles at all.
 */
static irqreturn_t max77705_aicl_irq(int irq, void *irq_drv_data)
{
	struct max77705_charger_data *chg = irq_drv_data;

	power_supply_changed(chg->psy_chg);

	return IRQ_HANDLED;
}

static irqreturn_t max77705_chgin_irq(int irq, void *irq_drv_data)
{
	struct max77705_charger_data *chg = irq_drv_data;

	queue_work(chg->wqueue, &chg->chgin_work);

	return IRQ_HANDLED;
}

static irqreturn_t max77705_wcin_irq(int irq, void *irq_drv_data)
{
	struct max77705_charger_data *chg = irq_drv_data;

	mod_delayed_work(chg->wqueue, &chg->wcin_work, 0);

	return IRQ_HANDLED;
}

static const struct regmap_irq max77705_charger_irqs[] = {
	REGMAP_IRQ_REG_LINE(MAX77705_BYP_I, BITS_PER_BYTE),
	REGMAP_IRQ_REG_LINE(MAX77705_INP_LIMIT_I, BITS_PER_BYTE),
	REGMAP_IRQ_REG_LINE(MAX77705_BATP_I, BITS_PER_BYTE),
	REGMAP_IRQ_REG_LINE(MAX77705_BAT_I, BITS_PER_BYTE),
	REGMAP_IRQ_REG_LINE(MAX77705_CHG_I, BITS_PER_BYTE),
	REGMAP_IRQ_REG_LINE(MAX77705_WCIN_I, BITS_PER_BYTE),
	REGMAP_IRQ_REG_LINE(MAX77705_CHGIN_I, BITS_PER_BYTE),
	REGMAP_IRQ_REG_LINE(MAX77705_AICL_I, BITS_PER_BYTE),
};

static const struct regmap_irq_chip max77705_charger_irq_chip = {
	.name			= "max77705-charger",
	.status_base		= MAX77705_CHG_REG_INT,
	.mask_base		= MAX77705_CHG_REG_INT_MASK,
	.num_regs		= 1,
	.irqs			= max77705_charger_irqs,
	.num_irqs		= ARRAY_SIZE(max77705_charger_irqs),
};

/*
 * OTG VBUS: the boost and OTG bits are added to the current mode nibble
 * (buck + charge 0x5 -> 0xf, buck 0x4 -> 0xe) and removed again, so the
 * system buck and charging state are kept. The boost drives the pins
 * CHGIN is fed from; it is refused while a charger input is present.
 */
static int max77705_otg_enable(struct regulator_dev *rdev)
{
	struct max77705_charger_data *chg = rdev_get_drvdata(rdev);
	unsigned int val;
	int err;

	err = regmap_read(chg->regmap, MAX77705_CHG_REG_INT_OK, &val);
	if (err)
		return err;

	if (val & MAX77705_CHGIN_OK)
		return -EBUSY;

	return regmap_field_update_bits(chg->rfield[MAX77705_MODE],
					MAX77705_OTG_CTRL, MAX77705_OTG_CTRL);
}

static int max77705_otg_disable(struct regulator_dev *rdev)
{
	struct max77705_charger_data *chg = rdev_get_drvdata(rdev);

	return regmap_field_update_bits(chg->rfield[MAX77705_MODE],
					MAX77705_OTG_CTRL, 0);
}

static int max77705_otg_is_enabled(struct regulator_dev *rdev)
{
	struct max77705_charger_data *chg = rdev_get_drvdata(rdev);
	unsigned int mode;
	int err;

	err = regmap_field_read(chg->rfield[MAX77705_MODE], &mode);
	if (err)
		return err;

	return (mode & MAX77705_OTG_CTRL) == MAX77705_OTG_CTRL;
}

static const struct regulator_ops max77705_otg_ops = {
	.enable		= max77705_otg_enable,
	.disable	= max77705_otg_disable,
	.is_enabled	= max77705_otg_is_enabled,
};

static const struct regulator_desc max77705_otg_desc = {
	.name		= "usb-otg-vbus",
	.of_match	= "usb-otg-vbus",
	.type		= REGULATOR_VOLTAGE,
	.owner		= THIS_MODULE,
	.ops		= &max77705_otg_ops,
	.fixed_uV	= 5000000,
	.n_voltages	= 1,
};

static int max77705_charger_enable(struct max77705_charger_data *chg)
{
	int rv;

	rv = regmap_field_write(chg->rfield[MAX77705_CHG_EN], 1);
	if (rv)
		dev_err(chg->dev, "unable to enable the charger: %d\n", rv);

	return rv;
}

static void max77705_charger_disable(void *data)
{
	struct max77705_charger_data *chg = data;
	int rv;

	/* no boost without an owner once the regulator is gone */
	regmap_field_update_bits(chg->rfield[MAX77705_MODE], MAX77705_OTG_CTRL, 0);

	rv = regmap_field_write(chg->rfield[MAX77705_CHG_EN], MAX77705_CHG_DISABLE);
	if (rv)
		dev_err(chg->dev, "unable to disable the charger: %d\n", rv);
}

static int max77705_get_online(struct regmap *regmap, int *val)
{
	unsigned int data;
	int ret;

	ret = regmap_read(regmap, MAX77705_CHG_REG_INT_OK, &data);
	if (ret < 0)
		return ret;

	*val = !!(data & (MAX77705_CHGIN_OK | MAX77705_WCIN_OK));

	return 0;
}

static int max77705_set_integer(struct max77705_charger_data *chg, enum max77705_field_idx fidx,
				unsigned int clamp_min, unsigned int clamp_max,
				unsigned int div, int val)
{
	unsigned int regval;

	regval = clamp_val(val, clamp_min, clamp_max) / div;

	return regmap_field_write(chg->rfield[fidx], regval);
}

static int max77705_check_battery(struct max77705_charger_data *chg, int *val)
{
	unsigned int reg_data;
	unsigned int reg_data2;
	struct regmap *regmap = chg->regmap;

	regmap_read(regmap, MAX77705_CHG_REG_INT_OK, &reg_data);

	dev_dbg(chg->dev, "CHG_INT_OK(0x%x)\n", reg_data);

	regmap_read(regmap, MAX77705_CHG_REG_DETAILS_00, &reg_data2);

	dev_dbg(chg->dev, "CHG_DETAILS00(0x%x)\n", reg_data2);

	if ((reg_data & MAX77705_BATP_OK) || !(reg_data2 & MAX77705_BATP_DTLS))
		*val = true;
	else
		*val = false;

	return 0;
}

static int max77705_get_charge_type(struct max77705_charger_data *chg, int *val)
{
	struct regmap *regmap = chg->regmap;
	unsigned int reg_data, chg_en;

	regmap_field_read(chg->rfield[MAX77705_CHG_EN], &chg_en);
	if (!chg_en) {
		*val = POWER_SUPPLY_CHARGE_TYPE_NONE;
		return 0;
	}

	regmap_read(regmap, MAX77705_CHG_REG_DETAILS_01, &reg_data);
	reg_data &= MAX77705_CHG_DTLS;

	switch (reg_data) {
	case 0x0:
	case MAX77705_CHARGER_CONSTANT_CURRENT:
	case MAX77705_CHARGER_CONSTANT_VOLTAGE:
		*val = POWER_SUPPLY_CHARGE_TYPE_FAST;
		return 0;
	default:
		*val = POWER_SUPPLY_CHARGE_TYPE_NONE;
		return 0;
	}

	return 0;
}

static int max77705_get_status(struct max77705_charger_data *chg, int *val)
{
	struct regmap *regmap = chg->regmap;
	unsigned int reg_data, chg_en;

	regmap_field_read(chg->rfield[MAX77705_CHG_EN], &chg_en);
	if (!chg_en) {
		*val = POWER_SUPPLY_CHARGE_TYPE_NONE;
		return 0;
	}

	regmap_read(regmap, MAX77705_CHG_REG_DETAILS_01, &reg_data);
	reg_data &= MAX77705_CHG_DTLS;

	switch (reg_data) {
	case 0x0:
	case MAX77705_CHARGER_CONSTANT_CURRENT:
	case MAX77705_CHARGER_CONSTANT_VOLTAGE:
		*val = POWER_SUPPLY_STATUS_CHARGING;
		return 0;
	case MAX77705_CHARGER_END_OF_CHARGE:
	case MAX77705_CHARGER_DONE:
		*val = POWER_SUPPLY_STATUS_FULL;
		return 0;
	/* those values hard coded as in vendor kernel, because of */
	/* failure to determine it's actual meaning. */
	case 0x05:
	case 0x06:
	case 0x07:
		*val = POWER_SUPPLY_STATUS_NOT_CHARGING;
		return 0;
	case 0x08:
	case 0xA:
	case 0xB:
		*val = POWER_SUPPLY_STATUS_DISCHARGING;
		return 0;
	default:
		*val = POWER_SUPPLY_STATUS_UNKNOWN;
		return 0;
	}

	return 0;
}

static int max77705_get_vbus_state(struct regmap *regmap, int *value)
{
	int ret;
	unsigned int charge_dtls;

	ret = regmap_read(regmap, MAX77705_CHG_REG_DETAILS_00, &charge_dtls);
	if (ret)
		return ret;

	charge_dtls = ((charge_dtls & MAX77705_CHGIN_DTLS) >>
			MAX77705_CHGIN_DTLS_SHIFT);

	switch (charge_dtls) {
	case 0x00:
		*value = POWER_SUPPLY_HEALTH_UNDERVOLTAGE;
		break;
	case 0x01:
		*value = POWER_SUPPLY_HEALTH_UNDERVOLTAGE;
		break;
	case 0x02:
		*value = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
		break;
	case 0x03:
		*value = POWER_SUPPLY_HEALTH_GOOD;
		break;
	default:
		return 0;
	}
	return 0;
}

static int max77705_get_battery_health(struct max77705_charger_data *chg,
					int *value)
{
	struct regmap *regmap = chg->regmap;
	unsigned int bat_dtls;

	regmap_read(regmap, MAX77705_CHG_REG_DETAILS_01, &bat_dtls);
	bat_dtls = ((bat_dtls & MAX77705_BAT_DTLS) >> MAX77705_BAT_DTLS_SHIFT);

	switch (bat_dtls) {
	case MAX77705_BATTERY_NOBAT:
		dev_dbg(chg->dev, "%s: No battery and the chg is suspended\n",
			__func__);
		*value = POWER_SUPPLY_HEALTH_NO_BATTERY;
		break;
	case MAX77705_BATTERY_PREQUALIFICATION:
		dev_dbg(chg->dev, "%s: battery is okay but its voltage is low(~VPQLB)\n",
			__func__);
		break;
	case MAX77705_BATTERY_DEAD:
		dev_dbg(chg->dev, "%s: battery dead\n", __func__);
		*value = POWER_SUPPLY_HEALTH_DEAD;
		break;
	case MAX77705_BATTERY_GOOD:
	case MAX77705_BATTERY_LOWVOLTAGE:
		*value = POWER_SUPPLY_HEALTH_GOOD;
		break;
	case MAX77705_BATTERY_OVERVOLTAGE:
		dev_dbg(chg->dev, "%s: battery ovp\n", __func__);
		*value = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
		break;
	default:
		dev_dbg(chg->dev, "%s: battery unknown\n", __func__);
		*value = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
		break;
	}

	return 0;
}

static int max77705_get_health(struct max77705_charger_data *chg, int *val)
{
	struct regmap *regmap = chg->regmap;
	unsigned int ok;
	int ret;

	/* the DETAILS_00 input state is CHGIN's; a wireless input has none */
	ret = regmap_read(regmap, MAX77705_CHG_REG_INT_OK, &ok);
	if (ret)
		return ret;
	if (ok & MAX77705_CHGIN_OK) {
		ret = max77705_get_vbus_state(regmap, val);
		if (ret || (*val != POWER_SUPPLY_HEALTH_GOOD))
			return ret;
	}
	return max77705_get_battery_health(chg, val);
}

static int max77705_get_input_current(struct max77705_charger_data *chg,
					int *val)
{
	unsigned int reg_data;
	int get_current = 0;

	regmap_field_read(chg->rfield[MAX77705_CHG_CHGIN_LIM], &reg_data);

	if (reg_data <= 3)
		get_current = MAX77705_CURRENT_CHGIN_MIN;
	else
		get_current = (reg_data + 1) * MAX77705_CURRENT_CHGIN_STEP;

	*val = get_current;

	return 0;
}

/*
 * The fast-charge ceiling: the battery's constant-charge-current-max when
 * it gives one, and never past the 6-bit CHG_CC field (63 x 50 mA).
 */
static int max77705_charge_current_max(struct max77705_charger_data *chg)
{
	int max = 63 * MAX77705_CURRENT_CHG_STEP;

	if (chg->bat_info && chg->bat_info->constant_charge_current_max_ua > 0)
		max = min(max, chg->bat_info->constant_charge_current_max_ua);

	return max;
}

static int max77705_set_input_current(struct max77705_charger_data *chg, int val)
{
	unsigned int regval;

	/* CHGIN_LIM code n limits the input to (n + 1) x 25 mA */
	regval = clamp_val(val, MAX77705_CURRENT_CHGIN_MIN, MAX77705_CURRENT_CHGIN_MAX) /
		 MAX77705_CURRENT_CHGIN_STEP - 1;

	return regmap_field_write(chg->rfield[MAX77705_CHG_CHGIN_LIM], regval);
}

static int max77705_get_charge_current(struct max77705_charger_data *chg,
					int *val)
{
	unsigned int reg_data;

	regmap_field_read(chg->rfield[MAX77705_CHG_CC_LIM], &reg_data);

	*val = reg_data <= 0x2 ? MAX77705_CURRENT_CHGIN_MIN : reg_data * MAX77705_CURRENT_CHG_STEP;

	return 0;
}

static int max77705_set_float_voltage(struct max77705_charger_data *chg,
					int float_voltage)
{
	int float_voltage_mv;
	unsigned int reg_data = 0;

	float_voltage_mv = float_voltage / 1000;
	reg_data = float_voltage_mv <= 4000 ? 0x0 :
		float_voltage_mv >= 4500 ? 0x23 :
		(float_voltage_mv <= 4200) ? (float_voltage_mv - 4000) / 50 :
		(((float_voltage_mv - 4200) / 10) + 0x04);

	return regmap_field_write(chg->rfield[MAX77705_CHG_CV_PRM], reg_data);
}

static int max77705_get_float_voltage(struct max77705_charger_data *chg,
					int *val)
{
	unsigned int reg_data = 0;
	int voltage_mv;

	regmap_field_read(chg->rfield[MAX77705_CHG_CV_PRM], &reg_data);
	voltage_mv = reg_data <= 0x04 ? reg_data * 50 + 4000 :
					(reg_data - 4) * 10 + 4200;
	*val = voltage_mv * 1000;

	return 0;
}

static int max77705_chg_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	struct max77705_charger_data *chg = power_supply_get_drvdata(psy);
	struct regmap *regmap = chg->regmap;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		return max77705_get_online(regmap, &val->intval);
	case POWER_SUPPLY_PROP_PRESENT:
		return max77705_check_battery(chg, &val->intval);
	case POWER_SUPPLY_PROP_STATUS:
		return max77705_get_status(chg, &val->intval);
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		return max77705_get_charge_type(chg, &val->intval);
	case POWER_SUPPLY_PROP_HEALTH:
		return max77705_get_health(chg, &val->intval);
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		return max77705_get_input_current(chg, &val->intval);
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		return max77705_get_charge_current(chg, &val->intval);
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		return max77705_get_float_voltage(chg, &val->intval);
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = chg->bat_info->voltage_max_design_uv;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = max77705_charger_model;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = max77705_charger_manufacturer;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int max77705_set_property(struct power_supply *psy,
				 enum power_supply_property psp,
				 const union power_supply_propval *val)
{
	struct max77705_charger_data *chg = power_supply_get_drvdata(psy);
	int err = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		err = max77705_set_integer(chg, MAX77705_CHG_CC_LIM,
					   MAX77705_CURRENT_CHGIN_MIN,
					   max77705_charge_current_max(chg),
					   MAX77705_CURRENT_CHG_STEP,
					   val->intval);
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		err = max77705_set_input_current(chg, val->intval);
		break;
	default:
		err = -EINVAL;
	}

	return err;
};

static int max77705_property_is_writeable(struct power_supply *psy,
					  enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		return true;
	default:
		return false;
	}
}

static const struct power_supply_desc max77705_charger_psy_desc = {
	.name = "max77705-charger",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = max77705_charger_props,
	.property_is_writeable = max77705_property_is_writeable,
	.num_properties = ARRAY_SIZE(max77705_charger_props),
	.get_property = max77705_chg_get_property,
	.set_property = max77705_set_property,
};

static void max77705_chgin_isr_work(struct work_struct *work)
{
	struct max77705_charger_data *chg =
		container_of(work, struct max77705_charger_data, chgin_work);

	power_supply_changed(chg->psy_chg);
}

/*
 * Wireless (WCIN, fed by the IDT receiver): the receiver's own firmware runs
 * the Qi link at 5 V, the charger only limits what it takes. Stock starts at
 * 480 mA and steps up, so a weak pad is not overloaded the moment it starts.
 */
static void max77705_wcin_work(struct work_struct *work)
{
	struct max77705_charger_data *chg =
		container_of(to_delayed_work(work), struct max77705_charger_data, wcin_work);
	unsigned int ok;
	int ma;

	if (regmap_read(chg->regmap, MAX77705_CHG_REG_INT_OK, &ok))
		return;

	if (!(ok & MAX77705_WCIN_OK))
		ma = MAX77705_WCIN_START_MA;
	else if (!chg->wcin_ma)
		ma = MAX77705_WCIN_START_MA;
	else
		ma = min(chg->wcin_ma + MAX77705_WCIN_STEP_MA, MAX77705_WCIN_MAX_MA);

	if (ma != chg->wcin_ma || !(ok & MAX77705_WCIN_OK))
		regmap_update_bits(chg->regmap, MAX77705_CHG_REG_CNFG_10,
				   MAX77705_WCIN_LIM_MASK, MAX77705_WCIN_LIM(ma));

	if (!(ok & MAX77705_WCIN_OK)) {
		if (chg->wcin_ma)
			dev_info(chg->dev, "wireless input gone\n");
		chg->wcin_ma = 0;
	} else {
		if (!chg->wcin_ma)
			dev_info(chg->dev, "wireless input present\n");
		chg->wcin_ma = ma;
		if (ma < MAX77705_WCIN_MAX_MA)
			queue_delayed_work(chg->wqueue, &chg->wcin_work,
					   msecs_to_jiffies(MAX77705_WCIN_STEP_MS));
	}

	power_supply_changed(chg->psy_chg);
}

static int max77705_charger_initialize(struct max77705_charger_data *chg)
{
	struct power_supply_battery_info *info;
	struct regmap *regmap = chg->regmap;
	int err;

	err = power_supply_get_battery_info(chg->psy_chg, &info);
	if (err)
		return dev_err_probe(chg->dev, err, "error on getting battery info");

	chg->bat_info = info;

	/* unlock charger setting protect */
	/* slowest LX slope */
	err = regmap_field_write(chg->rfield[MAX77705_CHGPROT], MAX77705_CHGPROT_UNLOCKED);
	if (err)
		goto err;

	err = regmap_field_write(chg->rfield[MAX77705_LX_SLOPE], MAX77705_SLOWEST_LX_SLOPE);
	if (err)
		goto err;

	/*
	 * Input voltage regulation (AICL) is the hardware's; a factory flow
	 * can leave it disabled (CNFG_06 DIS_AICL), stock clears it too.
	 */
	err = regmap_update_bits(regmap, MAX77705_CHG_REG_CNFG_06,
				 MAX77705_DIS_AICL_MASK, 0);
	if (err)
		goto err;

	/* fast charge timer disable */
	/* restart threshold disable */
	/* pre-qual charge disable */
	err = regmap_field_write(chg->rfield[MAX77705_FCHGTIME], MAX77705_FCHGTIME_DISABLE);
	if (err)
		goto err;

	err = regmap_field_write(chg->rfield[MAX77705_CHG_RSTRT], MAX77705_CHG_RSTRT_DISABLE);
	if (err)
		goto err;

	err = regmap_field_write(chg->rfield[MAX77705_CHG_PQEN], MAX77705_CHG_PQEN_DISABLE);
	if (err)
		goto err;

	err = regmap_field_write(chg->rfield[MAX77705_MODE],
			MAX77705_CHG_MASK | MAX77705_BUCK_MASK);
	if (err)
		goto err;

	/* charge current 450mA(default) */
	/* otg current limit 900mA */
	err = regmap_field_write(chg->rfield[MAX77705_OTG_ILIM], MAX77705_OTG_ILIM_900);
	if (err)
		goto err;

	/* BAT to SYS OCP 4.80A */
	err = regmap_field_write(chg->rfield[MAX77705_REG_B2SOVRC], MAX77705_B2SOVRC_4_8A);
	if (err)
		goto err;

	/* top off current 150mA */
	/* top off timer 30min */
	err = regmap_field_write(chg->rfield[MAX77705_TO], MAX77705_TO_ITH_150MA);
	if (err)
		goto err;

	err = regmap_field_write(chg->rfield[MAX77705_TO_TIME], MAX77705_TO_TIME_30M);
	if (err)
		goto err;

	err = regmap_field_write(chg->rfield[MAX77705_SYS_TRACK], MAX77705_SYS_TRACK_DISABLE);
	if (err)
		goto err;

	/*
	 * cv voltage: the battery's charge voltage if given, else its
	 * design maximum, else 4.2V. MINVSYS 3.6V(default)
	 */
	if (info->constant_charge_voltage_max_uv > 0) {
		err = max77705_set_float_voltage(chg, info->constant_charge_voltage_max_uv);
	} else if (info->voltage_max_design_uv > 0) {
		err = max77705_set_float_voltage(chg, info->voltage_max_design_uv);
	} else {
		dev_warn(chg->dev, "missing battery:voltage-max-design-microvolt\n");
		err = max77705_set_float_voltage(chg, 4200000);
	}
	if (err)
		goto err;

	/*
	 * fast-charge current from the battery, else left as found; the
	 * 6-bit CHG_CC field tops out at 63 x 50 mA
	 */
	if (info->constant_charge_current_max_ua > 0) {
		err = max77705_set_integer(chg, MAX77705_CHG_CC_LIM,
					   MAX77705_CURRENT_CHGIN_MIN,
					   max77705_charge_current_max(chg),
					   MAX77705_CURRENT_CHG_STEP,
					   info->constant_charge_current_max_ua);
		if (err)
			goto err;
	}

	err = regmap_field_write(chg->rfield[MAX77705_VCHGIN], MAX77705_VCHGIN_4_5);
	if (err)
		goto err;

	err = regmap_field_write(chg->rfield[MAX77705_WCIN], MAX77705_WCIN_4_5);
	if (err)
		goto err;

	/* take both inputs (stock keeps WCINSEL and CHGINSEL set) */
	err = regmap_update_bits(regmap, MAX77705_CHG_REG_CNFG_12,
				 MAX77705_WCINSEL | MAX77705_CHGINSEL,
				 MAX77705_WCINSEL | MAX77705_CHGINSEL);
	if (err)
		goto err;

	err = regmap_update_bits(regmap, MAX77705_CHG_REG_CNFG_10, MAX77705_WCIN_LIM_MASK,
				 MAX77705_WCIN_LIM(MAX77705_WCIN_START_MA));
	if (err)
		goto err;

	/* Watchdog timer */
	regmap_update_bits(regmap, MAX77705_CHG_REG_CNFG_00,
				MAX77705_WDTEN_MASK, 0);

	/* VBYPSET=5.0V */
	err = regmap_field_write(chg->rfield[MAX77705_VBYPSET], 0);
	if (err)
		goto err;

	/* Switching Frequency : 1.5MHz */
	err = regmap_field_write(chg->rfield[MAX77705_REG_FSW], MAX77705_CHG_FSW_1_5MHz);
	if (err)
		goto err;

	/* Auto skip mode */
	err = regmap_field_write(chg->rfield[MAX77705_REG_DISKIP], MAX77705_AUTO_SKIP);
	if (err)
		goto err;

	return 0;

err:
	return dev_err_probe(chg->dev, err, "error while configuring");

}

static int max77705_charger_probe(struct i2c_client *i2c)
{
	struct power_supply_config pscfg = {};
	struct regulator_config rcfg = {};
	struct regulator_dev *rdev;
	struct max77705_charger_data *chg;
	struct regmap_irq_chip *chip_desc;
	struct device *dev;
	struct regmap_irq_chip_data *irq_data;
	int ret;

	dev = &i2c->dev;

	chg = devm_kzalloc(dev, sizeof(*chg), GFP_KERNEL);
	if (!chg)
		return -ENOMEM;

	chg->dev = dev;
	i2c_set_clientdata(i2c, chg);

	chip_desc = devm_kmemdup(dev, &max77705_charger_irq_chip,
				 sizeof(max77705_charger_irq_chip),
				 GFP_KERNEL);
	if (!chip_desc)
		return -ENOMEM;
	chip_desc->irq_drv_data = chg;

	chg->regmap = devm_regmap_init_i2c(i2c, &max77705_chg_regmap_config);
	if (IS_ERR(chg->regmap))
		return PTR_ERR(chg->regmap);

	for (int i = 0; i < MAX77705_N_REGMAP_FIELDS; i++) {
		chg->rfield[i] = devm_regmap_field_alloc(dev, chg->regmap,
							 max77705_reg_field[i]);
		if (IS_ERR(chg->rfield[i]))
			return dev_err_probe(dev, PTR_ERR(chg->rfield[i]),
					     "cannot allocate regmap field\n");
	}

	pscfg.fwnode = dev_fwnode(dev);
	pscfg.drv_data = chg;

	chg->psy_chg = devm_power_supply_register(dev, &max77705_charger_psy_desc, &pscfg);
	if (IS_ERR(chg->psy_chg))
		return PTR_ERR(chg->psy_chg);

	ret = devm_regmap_add_irq_chip(chg->dev, chg->regmap, i2c->irq,
					IRQF_ONESHOT, 0,
					chip_desc, &irq_data);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add irq chip\n");

	chg->wqueue = devm_alloc_ordered_workqueue(dev, "%s", 0, dev_name(dev));
	if (!chg->wqueue)
		return -ENOMEM;

	ret = devm_work_autocancel(dev, &chg->chgin_work, max77705_chgin_isr_work);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize interrupt work\n");

	ret = max77705_charger_initialize(chg);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize charger IC\n");

	ret = devm_request_threaded_irq(dev, regmap_irq_get_virq(irq_data, MAX77705_CHGIN_I),
					NULL, max77705_chgin_irq,
					IRQF_TRIGGER_NONE,
					"chgin-irq", chg);
	if (ret)
		return ret;

	ret = devm_delayed_work_autocancel(dev, &chg->wcin_work, max77705_wcin_work);
	if (ret)
		return ret;

	ret = devm_request_threaded_irq(dev, regmap_irq_get_virq(irq_data, MAX77705_WCIN_I),
					NULL, max77705_wcin_irq,
					IRQF_TRIGGER_NONE,
					"wcin-irq", chg);
	if (ret)
		return ret;
	/* a pad the phone was already lying on */
	queue_delayed_work(chg->wqueue, &chg->wcin_work, 0);

	ret = devm_request_threaded_irq(dev, regmap_irq_get_virq(irq_data, MAX77705_AICL_I),
					NULL, max77705_aicl_irq,
					IRQF_TRIGGER_NONE,
					"aicl-irq", chg);
	if (ret)
		return ret;

	ret = max77705_charger_enable(chg);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable charge\n");

	rcfg.dev = dev;
	rcfg.driver_data = chg;
	rdev = devm_regulator_register(dev, &max77705_otg_desc, &rcfg);
	if (IS_ERR(rdev))
		return dev_err_probe(dev, PTR_ERR(rdev), "failed to register OTG regulator\n");

	return devm_add_action_or_reset(dev, max77705_charger_disable, chg);
}

static const struct of_device_id max77705_charger_of_match[] = {
	{ .compatible = "maxim,max77705-charger" },
	{ }
};
MODULE_DEVICE_TABLE(of, max77705_charger_of_match);

/*
 * A reset with the OTG boost running leaves the system unable to come
 * back up: the boost keeps VBUS alive through the reset. Stop it first.
 */
static void max77705_charger_shutdown(struct i2c_client *i2c)
{
	struct max77705_charger_data *chg = i2c_get_clientdata(i2c);

	regmap_field_update_bits(chg->rfield[MAX77705_MODE], MAX77705_OTG_CTRL, 0);
}

static struct i2c_driver max77705_charger_driver = {
	.driver = {
		.name = "max77705-charger",
		.of_match_table = max77705_charger_of_match,
	},
	.probe = max77705_charger_probe,
	.shutdown = max77705_charger_shutdown,
};
module_i2c_driver(max77705_charger_driver);

MODULE_AUTHOR("Dzmitry Sankouski <dsankouski@gmail.com>");
MODULE_DESCRIPTION("Maxim MAX77705 charger driver");
MODULE_LICENSE("GPL");
