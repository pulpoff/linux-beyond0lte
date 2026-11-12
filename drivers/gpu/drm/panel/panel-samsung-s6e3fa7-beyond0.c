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
 * gamma the bootloader set, as the vendor kernel does below ~60 nits. Colour is
 * the panel's mDNIe engine, set to Android's "Natural" screen mode.
 */

#include <linux/backlight.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/suspend.h>
#include <linux/sysfs.h>
#include <linux/time.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

#include "../tiny/exynos9820-decon.h"

#define AOR_DIMMEST	0x8d4
#define AOR_BRIGHTEST	0x014
#define MAX_BRIGHTNESS	255

struct beyond0_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct backlight_device *bl;
	bool asleep;		/* SLEEP_IN sent; the bootloader leaves it awake */
	bool display_on;
	struct mutex lpm_lock;	/* lpm and the self clock against the backlight */
	unsigned int lpm;	/* nits in low power mode, 0 = normal */
	/* the DDI's own clock in low power mode (Samsung's "self display") */
	const struct firmware *hands;
	bool hands_loaded;	/* in the DDI's side RAM since the last SLEEP_IN */
	bool clock_on;
	bool clock_24h;
	u16 clock_x, clock_y;
	int clock_utcoff;	/* local time minus UTC, s */
	time64_t clock_synced;	/* boottime of the last SET_TIME */
	struct work_struct resync;
	struct notifier_block pm_nb;
	u8 *side_buf;
};

static unsigned int clock_resync = 300;
module_param(clock_resync, uint, 0644);
MODULE_PARM_DESC(clock_resync, "resync the self clock on a resume at most every this many s (0: every resume)");

/*
 * Off: with it on, the hands did not always point at the centre of the dial drawn under them, as if the DDI
 * shifted one but not the other. Stock leaves it off too (7Dh enable byte 03) unless its AOD app asks for it;
 * Phosh's AOD steps the whole picture, hands included, every two hours instead.
 */
static bool self_move;
module_param(self_move, bool, 0644);
MODULE_PARM_DESC(self_move, "let the DDI shift the picture against burn-in while its clock runs (default off)");

static inline struct beyond0_panel *to_beyond0(struct drm_panel *panel)
{
	return container_of(panel, struct beyond0_panel, panel);
}

/* level 1 key around the standard DCS commands, level 2 around vendor ones */
#define key1_on(ctx)	mipi_dsi_dcs_write_seq_multi(ctx, 0x9f, 0xa5, 0xa5)
#define key1_off(ctx)	mipi_dsi_dcs_write_seq_multi(ctx, 0x9f, 0x5a, 0x5a)
#define key2_on(ctx)	mipi_dsi_dcs_write_seq_multi(ctx, 0xf0, 0x5a, 0x5a)
#define key2_off(ctx)	mipi_dsi_dcs_write_seq_multi(ctx, 0xf0, 0xa5, 0xa5)
#define key3_on(ctx)	mipi_dsi_dcs_write_seq_multi(ctx, 0xfc, 0x5a, 0x5a)
#define key3_off(ctx)	mipi_dsi_dcs_write_seq_multi(ctx, 0xfc, 0xa5, 0xa5)

/*
 * mDNIe "Natural" (Android's Settings > Display > Screen mode > Natural): the UI scenario table of
 * s6e3fa7_beyond0_a3_s0_panel_mdnie.h with the D65 white (ff fc f6) the panel's colour coordinate gives, since
 * it lies inside the calibration centre. DFh, DEh, DDh are written in that order behind the level 2 key.
 */
static const u8 beyond0_mdnie_df[] = {
	0xdf, 0x11, 0x6a, 0x9a, 0x25, 0x1a, 0x16, 0x2a, 0x00, 0x37, 0x5a, 0x00,
	0x4e, 0xc5, 0x00, 0x5d, 0x17, 0x00, 0x30, 0xc3, 0xe0, 0x3a, 0x27, 0xfa,
	0xfb, 0x55, 0xe6, 0x43, 0xef, 0xff, 0xfc, 0xf6, 0x8d, 0xe0, 0xf9, 0x3a,
	0xf5, 0x27, 0xe6, 0x84, 0x43, 0xf7, 0xef, 0x4c, 0xfa, 0x37, 0xfb, 0x1f,
	0x55, 0xed, 0xff, 0x00, 0xfc, 0x00, 0xf6, 0x00,
};
static const u8 beyond0_mdnie_de[] = {
	0xde, 0x00, 0x40, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x06, 0x18, 0x00,
	0x96, 0xf0, 0x01, 0x0e, 0x01, 0x00, 0x66, 0x00, 0x17, 0x00, 0xff, 0x00,
	0x00, 0x07, 0xff, 0x07, 0xff, 0x14, 0x00, 0x02, 0x00, 0x02, 0x01, 0xf4,
	0x08, 0x8a, 0x20, 0x2d, 0x04, 0x0f, 0x01, 0x20, 0x00, 0x08, 0x10, 0x18,
	0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70, 0x78,
	0x80, 0x88, 0x90, 0x98, 0xa0, 0xa8, 0xb0, 0xb8, 0xc0, 0xc8, 0xd0, 0xd8,
	0xe0, 0xe8, 0xf0, 0xf8, 0x01, 0x00, 0x00, 0x08, 0x10, 0x18, 0x20, 0x28,
	0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70, 0x78, 0x80, 0x88,
	0x90, 0x98, 0xa0, 0xa8, 0xb0, 0xb8, 0xc0, 0xc8, 0xd0, 0xd8, 0xe0, 0xe8,
	0xf0, 0xf8, 0x01, 0x00,
};
static const u8 beyond0_mdnie_dd[] = {
	0xdd, 0x01, 0x00, 0xf0, 0x07, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff, 0x04, 0x01,
};

static void beyond0_set_mdnie(struct mipi_dsi_multi_context *ctx)
{
	key2_on(ctx);
	mipi_dsi_dcs_write_buffer_multi(ctx, beyond0_mdnie_df, sizeof(beyond0_mdnie_df));
	mipi_dsi_dcs_write_buffer_multi(ctx, beyond0_mdnie_de, sizeof(beyond0_mdnie_de));
	mipi_dsi_dcs_write_buffer_multi(ctx, beyond0_mdnie_dd, sizeof(beyond0_mdnie_dd));
	key2_off(ctx);
}

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

/*
 * Low power mode for an always-on display (Samsung's HLPM): the panel refreshes from its own memory at a
 * low rate and drive voltage, at a fixed luminance of 2, 10, 30 or 60 nit whatever the backlight says.
 * Sequences from Samsung's s6e3fa7_beyond0_a3_s0_panel.h (alpm-enter-seq and alpm-exit-seq, HLPM layer).
 */
static const struct {
	unsigned int nits;
	u8 bb[4];	/* LPM_NIT */
	u8 mode;	/* LPM_MODE, DCS 0x53 */
} beyond0_lpm[] = {
	{  2, { 0x05, 0x8c, 0x88, 0xb4 }, 0x03 },
	{ 10, { 0x05, 0x8c, 0x78, 0x86 }, 0x02 },
	{ 30, { 0x05, 0x8c, 0x48, 0x90 }, 0x02 },
	{ 60, { 0x05, 0x0c, 0x08, 0x14 }, 0x02 },
};

#define avc2(ctx, on)	mipi_dsi_dcs_write_seq_multi(ctx, 0xf4, 0xeb, 0x1e, 0x19, 0x3a, 0x9f, 0x0f, 0x09, \
						     (on) ? 0xc0 : 0x80)

static void beyond0_lpm_enter(struct mipi_dsi_multi_context *ctx, unsigned int i)
{
	u8 bb[] = { 0xbb, beyond0_lpm[i].bb[0], beyond0_lpm[i].bb[1], beyond0_lpm[i].bb[2],
		    beyond0_lpm[i].bb[3] };
	u8 mode[] = { MIPI_DCS_WRITE_CONTROL_DISPLAY, beyond0_lpm[i].mode };

	key2_on(ctx);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xb1, 0x08, 0x98);	/* LPM AOR */
	mipi_dsi_dcs_write_seq_multi(ctx, 0xf7, 0x03);
	key2_off(ctx);
	mipi_dsi_usleep_range(ctx, 16700, 17700);		/* a frame at 60 Hz */
	key2_on(ctx);
	mipi_dsi_dcs_write_buffer_multi(ctx, bb, sizeof(bb));
	mipi_dsi_dcs_write_buffer_multi(ctx, mode, sizeof(mode));
	avc2(ctx, false);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xf7, 0x03);
	key2_off(ctx);
	mipi_dsi_msleep(ctx, 124);
	/*
	 * ENTER_AOD (stock aod_enter_to_lpm): the AOD porch setting, bit 7 of 76h's 3rd byte; level 3 key only. No
	 * frame between B0h and FEh, or the frame takes the offset and FEh 24 lands on p0.
	 */
	exynos9820_decon_hold_frames(ctx->dsi->host, true);
	key3_on(ctx);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xb0, 0x33);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xfe, 0x24);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x76, 0x08, 0x10, 0x84, 0x38, 0x08, 0xe8);
	key3_off(ctx);
	exynos9820_decon_hold_frames(ctx->dsi->host, false);
}

static void beyond0_lpm_exit(struct mipi_dsi_multi_context *ctx)
{
	/* EXIT_AOD (stock aod_exit_from_lpm): self move off, normal porch, the clock and its timer off */
	key2_on(ctx);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x7d, 0x01);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x76, 0x08, 0x10, 0x04, 0x38, 0x08, 0xe8);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x77, 0x00, 0x00);
	/* LPM_OFF_NIT: stock's alpm-exit-seq sends BB 00 (its LPM_OFF_MAPTBL patches the 05 away) */
	mipi_dsi_dcs_write_seq_multi(ctx, 0xbb, 0x00);
	avc2(ctx, true);
	mipi_dsi_dcs_write_seq_multi(ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x00);
	key2_off(ctx);
}

/*
 * The self clock: in low power mode the DDI draws analog hands over its frame memory and moves them by its own
 * timer, so the AP can sleep for hours with the time right. From Samsung's s6e3fa7_aod.c/s6e3fa7_aod_panel.h:
 * 77h holds one block of parameters (p1 enables, p3-p6 the time, p7 the update rate in 30 Hz frames, p8 a step
 * control, p9-p13 the dial's centre and rotation, p15-p20 each hand's pivot in its band), 78h 01 latches it.
 * The hands are a 360x120 RGBA image (three 40-row bands: hour, minute, second, each pointing right from (90, 20))
 * in the DDI's side RAM: region 01 of 75h, then 4Ch/5Ch packets.
 */
#define HANDS_FW		"samsung/s6e3fa7-beyond0-aod-hands.bin"
#define HANDS_SIZE		(360 * 120 * 4)
#define SIDE_RAM_CHUNK		496	/* stock: at most 511 bytes and a multiple of 16, inside the 512-byte FIFO */
#define SIDE_RAM_SETTLE_MS	1200
#define CLOCK_EN		0x07	/* A_CLK_EN | DISP_ON | TIME_EN */
#define CLOCK_24H		0x20
#define CLOCK_RATE_1S		0x1e	/* 30 frames */

/* self move at the 1 s rate (stock's s6e3fa7_aod_self_move_pos_tbl[3]): the DDI shifts the picture against burn-in */
static const u8 beyond0_self_move_1s[] = {
	0x7d,
	0xdd, 0x00, 0x23, 0x00, 0x27, 0x00, 0x23, 0x00, 0x77, 0x00, 0x73, 0x00, 0x27, 0x00, 0x33, 0x00,
	0x17, 0x00, 0x73, 0x00, 0x77, 0x00, 0x23, 0x00, 0x27, 0x00, 0x23, 0x00, 0x77, 0x00, 0x73, 0x00,
	0x27, 0x00, 0x33, 0x00, 0x17, 0x00, 0x73, 0x00, 0x77, 0x00, 0x23, 0x00, 0x27, 0x00, 0x23, 0x00,
	0x77, 0x00, 0x73, 0x00, 0x27, 0x00, 0x33, 0x00, 0x17, 0x00, 0x73, 0x00, 0x04, 0x04, 0x04, 0x04,
	0x01, 0x0c, 0x0c, 0x0c, 0x0c, 0x03, 0x04, 0x04, 0x04, 0x04, 0x01, 0x0c, 0x0c, 0x0c, 0x0c, 0x03,
	0x04, 0x04, 0x04, 0x04, 0x01, 0x0c, 0x0c, 0x0c, 0x0c, 0x03,
};

static void beyond0_side_ram(struct beyond0_panel *bp, struct mipi_dsi_multi_context *ctx, u8 region,
			     const u8 *data, size_t len)
{
	size_t off, n;

	/*
	 * The clock off (as "off" stops it), then a wait before the first write. After a crash reset (2026-11-08) the
	 * bands were noise, and uploads sent right behind the stop kept only their last third; the same upload a
	 * second after the stop was whole. Most likely the clock goes on reading side RAM for a while and side RAM
	 * ignores writes meanwhile. Stock only uploads after a power-on, with the clock long stopped.
	 */
	key2_on(ctx);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x7d, 0x01);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x77, 0x00, 0x00);
	key2_off(ctx);
	mipi_dsi_msleep(ctx, SIDE_RAM_SETTLE_MS);

	/* and no frame inside it: on the first AOD after a boot, with the shell drawing, an upload kept only its
	 * first third (2026-11-08) */
	exynos9820_decon_hold_frames(bp->dsi->host, true);
	key2_on(ctx);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x77, 0x00, 0x00);	/* stock's INIT_SIDE_RAM */
	mipi_dsi_dcs_write_buffer_multi(ctx, (const u8 []){ 0x75, region }, 2);
	for (off = 0; off < len && !ctx->accum_err; off += n) {
		n = min_t(size_t, len - off, SIDE_RAM_CHUNK);
		bp->side_buf[0] = off ? 0x5c : 0x4c;
		memcpy(bp->side_buf + 1, data + off, n);
		mipi_dsi_dcs_write_buffer_multi(ctx, bp->side_buf, n + 1);
	}
	key2_off(ctx);
	exynos9820_decon_hold_frames(bp->dsi->host, false);
}

/*
 * The whole 77h block in one packet, p0 on: the enables (p1), the time (p3-p6), the update rate in 30 Hz frames
 * (p7), the step control (p8), the dial's centre (p9-p12) and each hand's pivot (p15-p20). It was a SET_TIME of
 * p0-p8 and the centre after B0h 09 (start at p9), but a frame's packets between B0h and the command it is for take
 * the offset: the centre and the pivots then landed on the time, the hands went to the top left corner, out of
 * shape, with a "second hand" ticking (owner, 11-07 and 11-08, mostly on the first AOD after a boot, when the shell
 * still draws). Stock sends a 15-parameter SET_TIME whose p9-p14 zeros land on the centre, so the block is one.
 * p8 is always 03: stock sends 13 after the first update after entering low power mode, and the 13s were among the
 * updates that came out wrong; the clock started with 03 is the one the owner verified. 78h 01 latches it. The
 * sub-second byte's unit is unknown, so it stays 0 and the time is rounded to the nearest second.
 */
static void beyond0_clock_set(struct beyond0_panel *bp, struct mipi_dsi_multi_context *ctx)
{
	struct timespec64 ts;
	struct tm tm;
	u8 t[] = { 0x77, 0x00, CLOCK_EN | (bp->clock_24h ? CLOCK_24H : 0), 0x00,
		   0x00, 0x00, 0x00, 0x00, CLOCK_RATE_1S, 0x03,
		   bp->clock_x >> 8, bp->clock_x & 0xff, bp->clock_y >> 8, bp->clock_y & 0xff, 0x00, 0x00,
		   0x5a, 0x14, 0x5a, 0x14, 0x5a, 0x14 };

	ktime_get_real_ts64(&ts);
	time64_to_tm(ts.tv_sec + (ts.tv_nsec >= NSEC_PER_SEC / 2), bp->clock_utcoff, &tm);
	t[4] = tm.tm_hour;
	t[5] = tm.tm_min;
	t[6] = tm.tm_sec;

	key2_on(ctx);
	mipi_dsi_dcs_write_buffer_multi(ctx, t, sizeof(t));
	mipi_dsi_dcs_write_seq_multi(ctx, 0x78, 0x01);
	mipi_dsi_msleep(ctx, 34);
	key2_off(ctx);
	bp->clock_synced = ktime_get_boottime_seconds();
}

/*
 * Stock's SELF_MOVE_ON, which it sends after every analog clock setup: the table at p6 on (it sent B0h 06 first),
 * then the enable byte, 03 as is (no icon, no move) or 01 (02 cleared, as after its AOD app's SELF_MOVE_EN), in one
 * packet here for the same reason as the 77h block. Not 7D 01 instead: that is the reset of LPM exit, and with the
 * clock running it threw the hands to the top left corner.
 */
static void beyond0_self_move(struct beyond0_panel *bp, struct mipi_dsi_multi_context *ctx, bool on)
{
	u8 *buf = bp->side_buf;
	static const u8 head[] = { 0x7d, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00 };

	BUILD_BUG_ON(sizeof(head) + sizeof(beyond0_self_move_1s) - 1 > SIDE_RAM_CHUNK + 1);
	memcpy(buf, head, sizeof(head));
	buf[2] = on ? 0x01 : 0x03;
	/* the table without its 7Dh */
	memcpy(buf + sizeof(head), beyond0_self_move_1s + 1, sizeof(beyond0_self_move_1s) - 1);

	key2_on(ctx);
	mipi_dsi_dcs_write_buffer_multi(ctx, buf, sizeof(head) + sizeof(beyond0_self_move_1s) - 1);
	key2_off(ctx);
}

static int beyond0_prepare(struct drm_panel *panel)
{
	struct beyond0_panel *bp = to_beyond0(panel);
	struct mipi_dsi_multi_context ctx = { .dsi = bp->dsi };

	guard(mutex)(&bp->lpm_lock);
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
	/* SLEEP_IN loses the colour engine's tables */
	beyond0_set_mdnie(&ctx);
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

	guard(mutex)(&bp->lpm_lock);
	/* the panel keeps low power mode across SLEEP_IN; prepare brings it back at the backlight's level */
	if (bp->lpm) {
		beyond0_lpm_exit(&ctx);
		WRITE_ONCE(bp->lpm, 0);
		WRITE_ONCE(bp->clock_on, false);
	}
	/* stock reloads side RAM after every power-off; this panel keeps power, but SLEEP_IN may not keep it */
	bp->hands_loaded = false;
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

	guard(mutex)(&bp->lpm_lock);
	/* applied by prepare when the panel wakes up, or when it leaves low power mode */
	if (bp->asleep || bp->lpm)
		return 0;

	beyond0_set_aor(&ctx, backlight_get_brightness(bl));
	return ctx.accum_err;
}

/* lpm: 0 for the normal mode, or 2, 10, 30 or 60 for low power mode at that many nits */
static ssize_t lpm_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct beyond0_panel *bp = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(bp->lpm));
}

static ssize_t lpm_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	struct beyond0_panel *bp = dev_get_drvdata(dev);
	struct mipi_dsi_multi_context ctx = { .dsi = bp->dsi };
	unsigned int nits, i;
	int ret;

	ret = kstrtouint(buf, 0, &nits);
	if (ret)
		return ret;
	for (i = 0; nits && i < ARRAY_SIZE(beyond0_lpm); i++)
		if (beyond0_lpm[i].nits == nits)
			break;
	if (i == ARRAY_SIZE(beyond0_lpm))
		return -EINVAL;

	guard(mutex)(&bp->lpm_lock);
	if (bp->asleep)
		return -EBUSY;
	if (nits == bp->lpm)
		return len;
	if (nits) {
		beyond0_lpm_enter(&ctx, i);
	} else {
		beyond0_lpm_exit(&ctx);
		beyond0_set_aor(&ctx, backlight_get_brightness(bp->bl));
		WRITE_ONCE(bp->clock_on, false);
	}
	if (ctx.accum_err)
		return ctx.accum_err;
	WRITE_ONCE(bp->lpm, nits);
	return len;
}
static DEVICE_ATTR_RW(lpm);

/*
 * self_clock: in low power mode, "analog X Y UTCOFF [12|24]" starts the DDI's analog clock centred at (X, Y) at the
 * local time UTCOFF seconds ahead of UTC (written again with the same centre, it only sets the time and offset),
 * "off" stops it, "reload" reads the hand image again (it reaches the DDI when the clock next starts from off).
 * Leaving low power mode stops it too, and it is -EBUSY outside it, as stock's ioctls return -EAGAIN there. The
 * DDI's oscillator is its own, so the time is set again on a resume (see beyond0_pm_notify).
 */
static ssize_t self_clock_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct beyond0_panel *bp = dev_get_drvdata(dev);

	guard(mutex)(&bp->lpm_lock);
	if (!bp->clock_on)
		return sysfs_emit(buf, "off\n");
	return sysfs_emit(buf, "analog %u %u %d %u\n", bp->clock_x, bp->clock_y, bp->clock_utcoff,
			  bp->clock_24h ? 24 : 12);
}

static ssize_t self_clock_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	struct beyond0_panel *bp = dev_get_drvdata(dev);
	struct mipi_dsi_multi_context ctx = { .dsi = bp->dsi };
	unsigned int x, y, fmt = 24;
	int off, ret;

	if (sysfs_streq(buf, "off")) {
		guard(mutex)(&bp->lpm_lock);
		if (!bp->clock_on)
			return len;
		key2_on(&ctx);
		mipi_dsi_dcs_write_seq_multi(&ctx, 0x7d, 0x01);
		mipi_dsi_dcs_write_seq_multi(&ctx, 0x77, 0x00, 0x00);
		key2_off(&ctx);
		WRITE_ONCE(bp->clock_on, false);
		return ctx.accum_err ?: len;
	}

	if (sysfs_streq(buf, "reload")) {
		guard(mutex)(&bp->lpm_lock);
		release_firmware(bp->hands);
		bp->hands = NULL;
		bp->hands_loaded = false;
		return len;
	}

	ret = sscanf(buf, "analog %u %u %d %u", &x, &y, &off, &fmt);
	if (ret < 3 || x >= 1080 || y >= 2280 || off < -14 * 3600 || off > 14 * 3600 || (fmt != 12 && fmt != 24))
		return -EINVAL;

	/* the image first, outside the lock: request_firmware can take a while */
	if (!bp->hands) {
		const struct firmware *fw;

		ret = request_firmware(&fw, HANDS_FW, dev);
		if (ret)
			return ret;
		if (fw->size != HANDS_SIZE) {
			dev_err(dev, "%s: %zu bytes, not %u\n", HANDS_FW, fw->size, HANDS_SIZE);
			release_firmware(fw);
			return -EINVAL;
		}
		guard(mutex)(&bp->lpm_lock);
		if (bp->hands)
			release_firmware(fw);
		else
			bp->hands = fw;
	}

	guard(mutex)(&bp->lpm_lock);
	if (bp->asleep || !bp->lpm)
		return -EBUSY;
	bp->clock_utcoff = off;
	bp->clock_24h = fmt == 24;
	if (bp->clock_on && x == bp->clock_x && y == bp->clock_y) {
		beyond0_clock_set(bp, &ctx);
		return ctx.accum_err ?: len;
	}
	if (!bp->hands_loaded) {
		beyond0_side_ram(bp, &ctx, 0x01, bp->hands->data, HANDS_SIZE);
		if (ctx.accum_err)
			return ctx.accum_err;
		bp->hands_loaded = true;
	}
	bp->clock_x = x;
	bp->clock_y = y;
	beyond0_clock_set(bp, &ctx);
	beyond0_self_move(bp, &ctx, self_move);
	if (ctx.accum_err)
		return ctx.accum_err;
	WRITE_ONCE(bp->clock_on, true);
	return len;
}
static DEVICE_ATTR_RW(self_clock);

static struct attribute *beyond0_attrs[] = {
	&dev_attr_lpm.attr,
	&dev_attr_self_clock.attr,
	NULL
};
ATTRIBUTE_GROUPS(beyond0);

static const struct backlight_ops beyond0_bl_ops = {
	.update_status = beyond0_bl_update_status,
};

static void beyond0_resync(struct work_struct *work)
{
	struct beyond0_panel *bp = container_of(work, struct beyond0_panel, resync);
	struct mipi_dsi_multi_context ctx = { .dsi = bp->dsi };

	guard(mutex)(&bp->lpm_lock);
	if (!bp->clock_on || bp->asleep || !bp->lpm)
		return;
	beyond0_clock_set(bp, &ctx);
	if (ctx.accum_err)
		dev_warn_ratelimited(&bp->dsi->dev, "self clock not resynced: %d\n", ctx.accum_err);
}

/*
 * The AP sleeps through the self clock, so a resume is when it can be set right again. Freezable: a suspend that
 * starts meanwhile waits for it, rather than catching the panel mid-command.
 */
static int beyond0_pm_notify(struct notifier_block *nb, unsigned long event, void *unused)
{
	struct beyond0_panel *bp = container_of(nb, struct beyond0_panel, pm_nb);

	if (event == PM_POST_SUSPEND && READ_ONCE(bp->clock_on) &&
	    ktime_get_boottime_seconds() - READ_ONCE(bp->clock_synced) >= clock_resync)
		queue_work(system_freezable_wq, &bp->resync);
	return NOTIFY_DONE;
}

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
	bp->side_buf = devm_kmalloc(dev, SIDE_RAM_CHUNK + 1, GFP_KERNEL);
	if (!bp->side_buf)
		return -ENOMEM;
	ret = devm_mutex_init(dev, &bp->lpm_lock);
	if (ret)
		return ret;
	INIT_WORK(&bp->resync, beyond0_resync);
	bp->pm_nb.notifier_call = beyond0_pm_notify;
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
	register_pm_notifier(&bp->pm_nb);
	/* and S-Boot's own mDNIe tables, not Android's: a violet cast */
	{
		struct mipi_dsi_multi_context ctx = { .dsi = dsi };

		beyond0_set_mdnie(&ctx);
		if (ctx.accum_err)
			dev_warn(dev, "mDNIe tables not written: %d\n", ctx.accum_err);
	}
	return 0;
}

static void beyond0_remove(struct mipi_dsi_device *dsi)
{
	struct beyond0_panel *bp = mipi_dsi_get_drvdata(dsi);

	unregister_pm_notifier(&bp->pm_nb);
	cancel_work_sync(&bp->resync);
	mipi_dsi_detach(dsi);
	drm_panel_remove(&bp->panel);
	release_firmware(bp->hands);
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
		.dev_groups = beyond0_groups,
	},
};
module_mipi_dsi_driver(beyond0_driver);

MODULE_DESCRIPTION("Samsung S6E3FA7 panel of the Galaxy S10e (bootloader handover)");
MODULE_FIRMWARE(HANDS_FW);
MODULE_LICENSE("GPL");
