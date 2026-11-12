// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung S2MPB02 camera PMIC: the rear flash LED, as one flash class LED
 * (torch through brightness, flash through flash_strobe). A cell of the
 * s2mpb02 regulator driver, which owns the I2C client and its regmap.
 * Register map from Samsung's downstream leds-s2mpb02.c.
 *
 * The LED is switched over I2C (FLED_CTRL1 enable and mode) unless the
 * device tree gives the TORCH_EN / FLASH_EN pins; then CTRL1 leaves it to
 * the pins, as downstream does for the camera.
 */

#include <linux/bitfield.h>
#include <linux/gpio/consumer.h>
#include <linux/jiffies.h>
#include <linux/led-class-flash.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

#define S2MPB02_REG_FLED_CTRL1	0x12
#define S2MPB02_REG_FLED_CTRL2	0x13
#define S2MPB02_REG_FLED_CUR1	0x14
#define S2MPB02_REG_FLED_TIME1	0x15
#define S2MPB02_REG_FLED_IRD2	0x1b

#define S2MPB02_FLED_ON		BIT(7)		/* CTRL1: on, by I2C */
#define S2MPB02_FLED_TORCH	BIT(6)		/* CTRL1: torch mode, else flash */
#define S2MPB02_FLED_MODE	GENMASK(7, 6)	/* 00: off, or the pins' */
#define S2MPB02_FLED_LV_EN	BIT(3)		/* low voltage mode, kept off */
#define S2MPB02_FLED_CTRL2_LOW	GENMASK(3, 0)	/* downstream writes 0x4 */
#define S2MPB02_FLASH_CUR	GENMASK(7, 4)	/* N x 100 + 50 mA */
#define S2MPB02_TORCH_CUR	GENMASK(3, 0)	/* N x 20 mA */
#define S2MPB02_FLASH_TIME	GENMASK(7, 4)	/* (N + 1) x 62.5 ms */
#define S2MPB02_TORCH_TIME	GENMASK(3, 0)
#define S2MPB02_IRD2_DISCHARGE	BIT(1)		/* no faint glow when off */

/* downstream's ceilings for this board: torch 240 mA, flash 1250 mA */
#define TORCH_MAX		12
#define FLASH_UA_MIN		150000
#define FLASH_UA_STEP		100000
#define FLASH_UA_MAX		1250000
#define TIMEOUT_US_MIN		62500
#define TIMEOUT_US_STEP		62500
#define TIMEOUT_US_MAX		1000000
#define TIMEOUT_US_DEFAULT	500000

struct s2mpb02_led {
	struct regmap *regmap;
	struct led_classdev_flash fled;
	struct gpio_desc *torch_gpio;
	struct gpio_desc *flash_gpio;
	struct mutex lock;
	unsigned long strobe_until;	/* jiffies: the hardware ends the flash */
};

static struct s2mpb02_led *to_led(struct led_classdev_flash *fled)
{
	return container_of(fled, struct s2mpb02_led, fled);
}

static int s2mpb02_led_off(struct s2mpb02_led *led)
{
	gpiod_set_value_cansleep(led->torch_gpio, 0);
	gpiod_set_value_cansleep(led->flash_gpio, 0);
	led->strobe_until = jiffies;
	return regmap_update_bits(led->regmap, S2MPB02_REG_FLED_CTRL1,
				  S2MPB02_FLED_MODE, 0);
}

static int s2mpb02_led_on(struct s2mpb02_led *led, bool torch)
{
	if (led->torch_gpio && led->flash_gpio) {
		gpiod_set_value_cansleep(torch ? led->flash_gpio : led->torch_gpio, 0);
		gpiod_set_value_cansleep(torch ? led->torch_gpio : led->flash_gpio, 1);
		return 0;
	}
	return regmap_update_bits(led->regmap, S2MPB02_REG_FLED_CTRL1,
				  S2MPB02_FLED_MODE,
				  S2MPB02_FLED_ON | (torch ? S2MPB02_FLED_TORCH : 0));
}

static int s2mpb02_torch_set(struct led_classdev *cdev,
			     enum led_brightness value)
{
	struct s2mpb02_led *led = to_led(lcdev_to_flcdev(cdev));
	int ret;

	mutex_lock(&led->lock);
	/* from off, or from a flash: the mode bits must change for it to start */
	ret = s2mpb02_led_off(led);
	if (!ret && value)
		ret = regmap_update_bits(led->regmap, S2MPB02_REG_FLED_CUR1,
					 S2MPB02_TORCH_CUR,
					 FIELD_PREP(S2MPB02_TORCH_CUR, value));
	if (!ret && value)
		ret = s2mpb02_led_on(led, true);
	mutex_unlock(&led->lock);
	return ret;
}

static int s2mpb02_flash_brightness_set(struct led_classdev_flash *fled,
					u32 brightness)
{
	/* applied at the strobe */
	return 0;
}

static int s2mpb02_flash_timeout_set(struct led_classdev_flash *fled,
				     u32 timeout)
{
	return 0;
}

static int s2mpb02_strobe_set(struct led_classdev_flash *fled, bool state)
{
	struct s2mpb02_led *led = to_led(fled);
	u32 cur = (fled->brightness.val - FLASH_UA_MIN) / FLASH_UA_STEP + 1;
	u32 time = (fled->timeout.val - TIMEOUT_US_MIN) / TIMEOUT_US_STEP;
	int ret;

	mutex_lock(&led->lock);
	ret = s2mpb02_led_off(led);
	if (ret || !state)
		goto out;
	ret = regmap_update_bits(led->regmap, S2MPB02_REG_FLED_CUR1,
				 S2MPB02_FLASH_CUR,
				 FIELD_PREP(S2MPB02_FLASH_CUR, cur));
	if (!ret)
		ret = regmap_update_bits(led->regmap, S2MPB02_REG_FLED_TIME1,
					 S2MPB02_FLASH_TIME,
					 FIELD_PREP(S2MPB02_FLASH_TIME, time));
	if (!ret)
		ret = s2mpb02_led_on(led, false);
	if (!ret)
		led->strobe_until = jiffies + usecs_to_jiffies(fled->timeout.val);
out:
	mutex_unlock(&led->lock);
	return ret;
}

/* there is no status bit: the flash is on until its timeout */
static int s2mpb02_strobe_get(struct led_classdev_flash *fled, bool *state)
{
	struct s2mpb02_led *led = to_led(fled);

	mutex_lock(&led->lock);
	*state = time_before(jiffies, led->strobe_until);
	mutex_unlock(&led->lock);
	return 0;
}

static const struct led_flash_ops s2mpb02_flash_ops = {
	.flash_brightness_set	= s2mpb02_flash_brightness_set,
	.timeout_set		= s2mpb02_flash_timeout_set,
	.strobe_set		= s2mpb02_strobe_set,
	.strobe_get		= s2mpb02_strobe_get,
};

static int s2mpb02_led_init(struct s2mpb02_led *led)
{
	int ret;

	/* off and left to the pins, low voltage mode off */
	ret = regmap_update_bits(led->regmap, S2MPB02_REG_FLED_CTRL1,
				 S2MPB02_FLED_MODE | S2MPB02_FLED_LV_EN, 0);
	if (!ret)
		ret = regmap_update_bits(led->regmap, S2MPB02_REG_FLED_CTRL2,
					 S2MPB02_FLED_CTRL2_LOW, 0x4);
	/* the torch's own timer at its longest, as downstream sets it */
	if (!ret)
		ret = regmap_update_bits(led->regmap, S2MPB02_REG_FLED_TIME1,
					 S2MPB02_TORCH_TIME, S2MPB02_TORCH_TIME);
	if (!ret)
		ret = regmap_update_bits(led->regmap, S2MPB02_REG_FLED_IRD2,
					 S2MPB02_IRD2_DISCHARGE,
					 S2MPB02_IRD2_DISCHARGE);
	return ret;
}

static void s2mpb02_led_release(void *data)
{
	struct s2mpb02_led *led = data;

	mutex_lock(&led->lock);
	s2mpb02_led_off(led);
	mutex_unlock(&led->lock);
}

static int s2mpb02_led_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct led_init_data init_data = {};
	struct fwnode_handle *child;
	struct led_classdev *cdev;
	struct s2mpb02_led *led;
	int ret;

	led = devm_kzalloc(dev, sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;
	led->regmap = dev_get_regmap(dev->parent, NULL);
	if (!led->regmap)
		return dev_err_probe(dev, -ENODEV, "no regmap from the PMIC\n");
	ret = devm_mutex_init(dev, &led->lock);
	if (ret)
		return ret;

	led->torch_gpio = devm_gpiod_get_optional(dev, "torch", GPIOD_OUT_LOW);
	if (IS_ERR(led->torch_gpio))
		return dev_err_probe(dev, PTR_ERR(led->torch_gpio), "torch pin\n");
	led->flash_gpio = devm_gpiod_get_optional(dev, "flash", GPIOD_OUT_LOW);
	if (IS_ERR(led->flash_gpio))
		return dev_err_probe(dev, PTR_ERR(led->flash_gpio), "flash pin\n");

	ret = s2mpb02_led_init(led);
	if (ret)
		return dev_err_probe(dev, ret, "no answer\n");
	platform_set_drvdata(pdev, led);
	ret = devm_add_action_or_reset(dev, s2mpb02_led_release, led);
	if (ret)
		return ret;

	cdev = &led->fled.led_cdev;
	cdev->max_brightness = TORCH_MAX;
	cdev->brightness_set_blocking = s2mpb02_torch_set;
	cdev->flags |= LED_DEV_CAP_FLASH | LED_CORE_SUSPENDRESUME;

	led->fled.ops = &s2mpb02_flash_ops;
	led->fled.brightness = (struct led_flash_setting) {
		.min = FLASH_UA_MIN, .step = FLASH_UA_STEP,
		.max = FLASH_UA_MAX, .val = FLASH_UA_MAX,
	};
	led->fled.timeout = (struct led_flash_setting) {
		.min = TIMEOUT_US_MIN, .step = TIMEOUT_US_STEP,
		.max = TIMEOUT_US_MAX, .val = TIMEOUT_US_DEFAULT,
	};

	/* named by the DT's led node; without one, white:flash all the same */
	child = device_get_next_child_node(dev, NULL);
	if (child) {
		init_data.fwnode = child;
		init_data.devicename = "s2mpb02";
		ret = devm_led_classdev_flash_register_ext(dev, &led->fled, &init_data);
		fwnode_handle_put(child);
	} else {
		cdev->name = "white:flash";
		ret = devm_led_classdev_flash_register_ext(dev, &led->fled, NULL);
	}
	if (ret)
		return dev_err_probe(dev, ret, "cannot register the LED\n");
	return 0;
}

static void s2mpb02_led_shutdown(struct platform_device *pdev)
{
	struct s2mpb02_led *led = platform_get_drvdata(pdev);

	if (led)
		s2mpb02_led_release(led);
}

static const struct of_device_id s2mpb02_led_of_match[] = {
	{ .compatible = "samsung,s2mpb02-flash" },
	{ }
};
MODULE_DEVICE_TABLE(of, s2mpb02_led_of_match);

static const struct platform_device_id s2mpb02_led_id[] = {
	{ "s2mpb02-flash" },
	{ }
};
MODULE_DEVICE_TABLE(platform, s2mpb02_led_id);

static struct platform_driver s2mpb02_led_driver = {
	.driver = {
		.name = "s2mpb02-flash",
		.of_match_table = s2mpb02_led_of_match,
	},
	.probe = s2mpb02_led_probe,
	.shutdown = s2mpb02_led_shutdown,
	.id_table = s2mpb02_led_id,
};
module_platform_driver(s2mpb02_led_driver);

MODULE_DESCRIPTION("Samsung S2MPB02 flash and torch LED");
MODULE_LICENSE("GPL");
