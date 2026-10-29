// SPDX-License-Identifier: GPL-2.0
/*
 * Galaxy S10e (beyond0lte) panel brightness while mainline has no Exynos
 * 9820 DSI driver: the S6E3FA7 keeps running on what S-Boot set up, and this
 * sends the AOR (AMOLED off ratio) part of the downstream set-bl sequence
 * straight into the DSIM0 command FIFO (dpu20/cal_9820/dsim_reg.c,
 * panel/s6e3fa7/s6e3fa7_beyond0_a3_s0_panel.h). Below ~60 nits downstream
 * dims with AOR alone at a fixed gamma, so that is all this changes.
 */
#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>

#define DSIM0_BASE	0x19080000
#define DSIM_PKTHDR	0x58
#define DSIM_PAYLOAD	0x5c
#define DSIM_FIFOCTRL	0x68
#define FULL_PH_SFR	BIT(11)
#define EMPTY_PH_SFR	BIT(10)
#define FULL_PL_SFR	BIT(9)

#define DCS_SHORT_WRITE_PARAM	0x15
#define DCS_LONG_WRITE		0x39

#define AOR_DIMMEST	0x8d4
#define AOR_BRIGHTEST	0x014
#define MAX_LEVEL	255

static void __iomem *dsim;
static struct backlight_device *bl;
static DEFINE_MUTEX(dsim_lock);

static int wait_clear(u32 bits)
{
	int i;

	for (i = 0; i < 10000; i++) {
		if (!(readl(dsim + DSIM_FIFOCTRL) & bits))
			return 0;
		udelay(10);
	}
	return -ETIMEDOUT;
}

static int dsi_write(const u8 *b, int n)
{
	int i, j;

	if (wait_clear(FULL_PH_SFR))
		return -ETIMEDOUT;
	if (n == 2) {
		writel(DCS_SHORT_WRITE_PARAM | b[0] << 8 | b[1] << 16, dsim + DSIM_PKTHDR);
	} else {
		for (i = 0; i < n; i += 4) {
			u32 w = 0;

			for (j = 0; j < 4 && i + j < n; j++)
				w |= (u32)b[i + j] << (8 * j);
			if (wait_clear(FULL_PL_SFR))
				return -ETIMEDOUT;
			writel(w, dsim + DSIM_PAYLOAD);
		}
		writel(DCS_LONG_WRITE | n << 8, dsim + DSIM_PKTHDR);
	}
	for (i = 0; i < 10000 && !(readl(dsim + DSIM_FIFOCTRL) & EMPTY_PH_SFR); i++)
		udelay(10);
	return 0;
}

static int s10e_bl_update(struct backlight_device *bd)
{
	static const u8 key_on[] = { 0xf0, 0x5a, 0x5a }, key_off[] = { 0xf0, 0xa5, 0xa5 };
	static const u8 update[] = { 0xf7, 0x03 };
	int level = backlight_get_brightness(bd);
	u32 aor = AOR_DIMMEST - (AOR_DIMMEST - AOR_BRIGHTEST) * level / MAX_LEVEL;
	u8 cmd[3] = { 0xb1, aor >> 8, aor & 0xff };
	int ret;

	mutex_lock(&dsim_lock);
	ret = dsi_write(key_on, 3) ?: dsi_write(cmd, 3) ?: dsi_write(update, 2) ?:
	      dsi_write(key_off, 3);
	mutex_unlock(&dsim_lock);
	if (ret)
		pr_warn("s10e-panel-bl: DSIM FIFO stuck (FIFOCTRL %08x)\n",
			readl(dsim + DSIM_FIFOCTRL));
	return ret;
}

static const struct backlight_ops s10e_bl_ops = {
	.update_status = s10e_bl_update,
};

static int __init s10e_bl_init(void)
{
	struct backlight_properties props = {
		.type = BACKLIGHT_PLATFORM,
		.max_brightness = MAX_LEVEL,
		.brightness = 0,	/* AOR_DIMMEST: easy on the OLED until told otherwise */
		.scale = BACKLIGHT_SCALE_LINEAR,
	};

	if (!of_machine_is_compatible("samsung,beyond0lte"))
		return -ENODEV;
	dsim = ioremap(DSIM0_BASE, 0x1000);
	if (!dsim)
		return -ENOMEM;
	bl = backlight_device_register("s10e-panel", NULL, NULL, &s10e_bl_ops, &props);
	if (IS_ERR(bl)) {
		iounmap(dsim);
		return PTR_ERR(bl);
	}
	backlight_update_status(bl);
	return 0;
}

static void __exit s10e_bl_exit(void)
{
	backlight_device_unregister(bl);
	iounmap(dsim);
}

module_init(s10e_bl_init);
module_exit(s10e_bl_exit);
MODULE_DESCRIPTION("Galaxy S10e panel brightness through the DSIM0 command FIFO");
MODULE_LICENSE("GPL");
