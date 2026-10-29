// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung S6E3FA7 "beyond0 A3 S0" panel of the Galaxy S10e (1080x2280 AMOLED,
 * MIPI DSI command mode, 4 lanes).
 *
 * The bootloader lights the panel and the DSI link, and nothing on mainline
 * can power-cycle it yet (no DSIM/D-PHY init, no display PMIC sequencing), so
 * this driver takes it over lit: off is DISPLAY_OFF + SLEEP_IN, on is
 * SLEEP_OUT, the init registers again, then DISPLAY_ON. Reset and supplies
 * are never touched.
 *
 * Sequences and bytes are from the Samsung vendor kernel,
 * panel/s6e3fa7/s6e3fa7_beyond0_a3_s0_panel.h (init/display-on/display-off/
 * exit command tables). Brightness is AOR (AMOLED off ratio) only, at the
 * gamma the bootloader set, as the vendor kernel does below ~60 nits.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

#define AOR_DIMMEST	0x8d4
#define AOR_BRIGHTEST	0x014
#define MAX_BRIGHTNESS	255

struct beyond0_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct backlight_device *bl;
	bool asleep;		/* SLEEP_IN sent; the bootloader leaves it awake */
	bool display_on;
};

static inline struct beyond0_panel *to_beyond0(struct drm_panel *panel)
{
	return container_of(panel, struct beyond0_panel, panel);
}

/* level 1 key around the standard DCS commands, level 2 around vendor ones */
#define key1_on(ctx)	mipi_dsi_dcs_write_seq_multi(ctx, 0x9f, 0xa5, 0xa5)
#define key1_off(ctx)	mipi_dsi_dcs_write_seq_multi(ctx, 0x9f, 0x5a, 0x5a)
#define key2_on(ctx)	mipi_dsi_dcs_write_seq_multi(ctx, 0xf0, 0x5a, 0x5a)
#define key2_off(ctx)	mipi_dsi_dcs_write_seq_multi(ctx, 0xf0, 0xa5, 0xa5)

static void beyond0_set_aor(struct mipi_dsi_multi_context *ctx, int level)
{
	u32 aor = AOR_DIMMEST - (AOR_DIMMEST - AOR_BRIGHTEST) * level /
		  MAX_BRIGHTNESS;
	u8 b1[] = { 0xb1, aor >> 8, aor & 0xff };

	key2_on(ctx);
	mipi_dsi_dcs_write_buffer_multi(ctx, b1, sizeof(b1));
	mipi_dsi_dcs_write_seq_multi(ctx, 0xf7, 0x03);	/* gamma/AOR update */
	key2_off(ctx);
}

static int beyond0_prepare(struct drm_panel *panel)
{
	struct beyond0_panel *bp = to_beyond0(panel);
	struct mipi_dsi_multi_context ctx = { .dsi = bp->dsi };

	if (!bp->asleep)
		return 0;

	/* init-seq without the reset and supply steps */
	mipi_dsi_dcs_exit_sleep_mode_multi(&ctx);
	mipi_dsi_msleep(&ctx, 20);
	key1_on(&ctx);
	mipi_dsi_dcs_set_tear_on_multi(&ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	key1_off(&ctx);
	key2_on(&ctx);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xed, 0x4c);			/* ERR_FG */
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xb9, 0x00, 0x00, 0x14, 0x00, 0x18,
				     0x00, 0x00, 0x00, 0x00, 0x11, 0x03, 0x02,
				     0x40, 0x02, 0x40);			/* TSP_HSYNC */
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xf4, 0xeb, 0x1e, 0x19, 0x3a, 0x9f,
				     0x0f, 0x09, 0xc0);			/* AVC2_ON */
	/* FFC for the 1143 Mbit/s lane rate the bootloader programs */
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xc5, 0x09, 0x10, 0xc8, 0x1d, 0x39,
				     0x0f, 0x26, 0xe6, 0x24, 0xd2, 0x7a, 0x77,
				     0x73, 0x00, 0xff, 0x78, 0x8b, 0x08, 0x00);
	mipi_dsi_dcs_write_seq_multi(&ctx, 0xc4, 0x5b, 0x41, 0x74, 0x0e, 0x00,
				     0x72, 0x5a, 0x72, 0x72);		/* PARTIAL */
	key2_off(&ctx);
	beyond0_set_aor(&ctx, backlight_get_brightness(bp->bl));

	if (!ctx.accum_err)
		bp->asleep = false;
	return ctx.accum_err;
}

static int beyond0_enable(struct drm_panel *panel)
{
	struct beyond0_panel *bp = to_beyond0(panel);
	struct mipi_dsi_multi_context ctx = { .dsi = bp->dsi };

	if (bp->display_on)
		return 0;

	/* the vendor driver turns the display on after the first frame */
	mipi_dsi_msleep(&ctx, 34);
	key1_on(&ctx);
	mipi_dsi_dcs_set_display_on_multi(&ctx);
	key1_off(&ctx);

	if (!ctx.accum_err)
		bp->display_on = true;
	return ctx.accum_err;
}

static int beyond0_disable(struct drm_panel *panel)
{
	struct beyond0_panel *bp = to_beyond0(panel);
	struct mipi_dsi_multi_context ctx = { .dsi = bp->dsi };

	key1_on(&ctx);
	mipi_dsi_dcs_set_display_off_multi(&ctx);
	key1_off(&ctx);
	bp->display_on = false;
	return ctx.accum_err;
}

static int beyond0_unprepare(struct drm_panel *panel)
{
	struct beyond0_panel *bp = to_beyond0(panel);
	struct mipi_dsi_multi_context ctx = { .dsi = bp->dsi };

	/* exit-seq */
	key1_on(&ctx);
	mipi_dsi_dcs_enter_sleep_mode_multi(&ctx);
	key1_off(&ctx);
	mipi_dsi_msleep(&ctx, 120);
	bp->asleep = true;
	return ctx.accum_err;
}

static const struct drm_display_mode beyond0_mode = {
	/* command mode: these only pace vblank; porches from the vendor DT */
	.clock = 1086 * 2299 * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 2,
	.hsync_end = 1080 + 2 + 2,
	.htotal = 1080 + 2 + 2 + 2,
	.vdisplay = 2280,
	.vsync_start = 2280 + 3,
	.vsync_end = 2280 + 3 + 1,
	.vtotal = 2280 + 3 + 1 + 15,
	.width_mm = 63,
	.height_mm = 133,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int beyond0_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &beyond0_mode);
}

static const struct drm_panel_funcs beyond0_panel_funcs = {
	.prepare = beyond0_prepare,
	.enable = beyond0_enable,
	.disable = beyond0_disable,
	.unprepare = beyond0_unprepare,
	.get_modes = beyond0_get_modes,
};

static int beyond0_bl_update_status(struct backlight_device *bl)
{
	struct beyond0_panel *bp = bl_get_data(bl);
	struct mipi_dsi_multi_context ctx = { .dsi = bp->dsi };

	/* applied by prepare when the panel wakes up */
	if (bp->asleep)
		return 0;

	beyond0_set_aor(&ctx, backlight_get_brightness(bl));
	return ctx.accum_err;
}

static const struct backlight_ops beyond0_bl_ops = {
	.update_status = beyond0_bl_update_status,
};

static int beyond0_probe(struct mipi_dsi_device *dsi)
{
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = MAX_BRIGHTNESS / 2,
		.max_brightness = MAX_BRIGHTNESS,
		.scale = BACKLIGHT_SCALE_LINEAR,
	};
	struct device *dev = &dsi->dev;
	struct beyond0_panel *bp;
	int ret;

	bp = devm_drm_panel_alloc(dev, struct beyond0_panel, panel,
				  &beyond0_panel_funcs, DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(bp))
		return PTR_ERR(bp);

	bp->dsi = dsi;
	bp->display_on = true;
	mipi_dsi_set_drvdata(dsi, bp);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS;

	bp->bl = devm_backlight_device_register(dev, dev_name(dev), dev, bp,
						&beyond0_bl_ops, &props);
	if (IS_ERR(bp->bl))
		return dev_err_probe(dev, PTR_ERR(bp->bl),
				     "failed to register backlight\n");
	bp->panel.backlight = bp->bl;

	drm_panel_add(&bp->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret) {
		drm_panel_remove(&bp->panel);
		return dev_err_probe(dev, ret, "failed to attach to DSI host\n");
	}

	/* the lit panel shows the bootloader's AOR until told otherwise */
	backlight_update_status(bp->bl);
	return 0;
}

static void beyond0_remove(struct mipi_dsi_device *dsi)
{
	struct beyond0_panel *bp = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&bp->panel);
}

static const struct of_device_id beyond0_of_match[] = {
	{ .compatible = "samsung,s6e3fa7-beyond0" },
	{ }
};
MODULE_DEVICE_TABLE(of, beyond0_of_match);

static struct mipi_dsi_driver beyond0_driver = {
	.probe = beyond0_probe,
	.remove = beyond0_remove,
	.driver = {
		.name = "panel-samsung-s6e3fa7-beyond0",
		.of_match_table = beyond0_of_match,
	},
};
module_mipi_dsi_driver(beyond0_driver);

MODULE_DESCRIPTION("Samsung S6E3FA7 panel of the Galaxy S10e (bootloader handover)");
MODULE_LICENSE("GPL");
