// SPDX-License-Identifier: GPL-2.0
//
// Exynos 9820 DECON0 "handover" KMS: page flips on the pipeline S-Boot left
// running (Galaxy S10e: S6E3FA7 in MIPI command mode, HW TE trigger left
// unmasked, so DECON fetches a frame on every TE).
//
// Nothing here initialises DECON, DSIM, the D-PHY, clocks or the power
// domain. A flip writes the GF0 IDMA base (a 32-bit IOVA of SysMMU_DPUD0)
// and the window's shadow-update request; FRAME_START is vblank.
// Disabling the CRTC hands the scanout back to S-Boot's framebuffer.
//
// With psr=1 (the default) the trigger is masked again once nothing wants a
// frame: the panel then refreshes itself from its own RAM, and DECON, the
// IDMA and the DSI link stop moving 9.8 MB a frame at 60 Hz. Every update,
// and every vblank reference, unmasks it (DPU-SPEC.md 3.1, 3.4).
//
// DSIM0 is used only as a MIPI DSI host for panel commands, through its
// command FIFO on the link S-Boot brought up. The panel (a child node) is
// driven through a panel bridge; the DRM device registers once it attaches.
//
// Registers: kit/display/DPU-SPEC.md 3.3; boot state: DPU-SPEC.md 7.

#include <linux/aperture.h>
#include <linux/bitfield.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/of_irq.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_bridge_connector.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

#include "exynos9820-dp.h"

/* DECON0 */
#define DECON_GLOBAL_CONTROL		0x0000
#define DECON_GLOBAL_RUN		(BIT(8) | BIT(4) | BIT(1) | BIT(0))
#define DECON_INTERRUPT_PENDING		0x004c
#define DECON_INT_FRAME_DONE		BIT(13)
#define DECON_INT_FRAME_START		BIT(12)
#define DECON_INT_EXTRA			BIT(4)
#define DECON_SHADOW_REG_UPDATE_REQ	0x0060
#define DECON_SHADOW_GLOBAL		BIT(31)
#define DECON_HW_SW_TRIG_CONTROL	0x0070
#define DECON_HW_TRIG_EN		BIT(0)
#define DECON_HW_TRIG_MASK_DECON	BIT(4)
#define DECON_BLENDER_BG_IMAGE_SIZE_0	0x0200
#define DECON_DATA_PATH_CONTROL_0	0x0214
#define DECON_DATA_PATH_CONTROL_1	0x0218
#define DECON_NR_WINDOWS		6

/* GF0 IDMA */
#define IDMA_IN_CON			0x0008
#define IDMA_IMG_FORMAT			GENMASK(16, 11)
#define IDMA_FMT_ARGB8888		0
#define IDMA_FMT_XRGB8888		4
#define IDMA_SRC_SIZE			0x0010
#define IDMA_IMG_SIZE			0x0018
#define IDMA_IN_BASE_ADDR_Y		0x0040

/* DSIM0 command FIFO */
#define DSIM_PKTHDR			0x0058
#define DSIM_PAYLOAD			0x005c
#define DSIM_FIFOCTRL			0x0068
#define DSIM_FULL_PH_SFR		BIT(11)
#define DSIM_EMPTY_PH_SFR		BIT(10)
#define DSIM_FULL_PL_SFR		BIT(9)

#define S10E_WIDTH			1080
#define S10E_HEIGHT			2280
#define S10E_SIZE_REG			((S10E_HEIGHT << 16) | S10E_WIDTH)

struct decon_kms {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;

	/* DECON2 + DPP VGRFS (DPUD0, our IOMMU domain) into the DP link */
	struct drm_simple_display_pipe dp_pipe;
	struct drm_bridge *dp_bridge;
	void __iomem *decon2;
	void __iomem *vgrfs_dma;
	void __iomem *vgrfs_dpp;
	int dp_irq;
	bool dp_running;
	/* flip done once DECON2 has latched the buffer (event_lock) */
	struct drm_pending_vblank_event *dp_event;
	u32 dp_saved_270;

	void __iomem *decon;
	void __iomem *idma;
	void __iomem *dsim;
	unsigned int win;
	int irq;

	/* PSR: which frames the TE trigger still has to start (trig_lock) */
	spinlock_t trig_lock;
	unsigned int trig_frames;
	bool trig_vblank;
	bool trig_masked;

	struct mipi_dsi_host dsi_host;
	struct mutex dsi_lock;		/* one packet in the FIFO at a time */
	bool registered;

	/* what S-Boot was scanning out, restored (blacked) on disable */
	void *boot_fb;
	u32 boot_base;
	u32 boot_in_con;
	u32 boot_src_size;
};

static bool psr = true;
module_param(psr, bool, 0644);
MODULE_PARM_DESC(psr, "send frames only when the picture changes (default: 1)");

static inline struct decon_kms *to_decon(struct drm_device *drm)
{
	return container_of(drm, struct decon_kms, drm);
}

static void rmw(void __iomem *base, u32 reg, u32 mask, u32 val)
{
	writel((readl(base + reg) & ~mask) | (val & mask), base + reg);
}

/*
 * Let the next TE start a frame, and keep going for @frames of them. Two
 * after an update: the first latches it, and the second is the vblank that
 * drm_atomic_helper_wait_for_vblanks() may only start counting after the
 * first one has already gone by.
 */
static void decon_trig_kick(struct decon_kms *dk, unsigned int frames)
{
	unsigned long flags;

	spin_lock_irqsave(&dk->trig_lock, flags);
	dk->trig_frames = max(dk->trig_frames, frames);
	if (dk->trig_masked) {
		rmw(dk->decon, DECON_HW_SW_TRIG_CONTROL,
		    DECON_HW_TRIG_MASK_DECON, 0);
		dk->trig_masked = false;
	}
	spin_unlock_irqrestore(&dk->trig_lock, flags);
}

/* At FRAME_START: mask the trigger once no one wants the next frame. */
static void decon_trig_idle(struct decon_kms *dk)
{
	spin_lock(&dk->trig_lock);
	if (dk->trig_frames)
		dk->trig_frames--;
	if (READ_ONCE(psr) && !dk->trig_frames && !dk->trig_vblank &&
	    !dk->trig_masked &&
	    !readl(dk->decon + DECON_SHADOW_REG_UPDATE_REQ)) {
		rmw(dk->decon, DECON_HW_SW_TRIG_CONTROL,
		    DECON_HW_TRIG_MASK_DECON, DECON_HW_TRIG_MASK_DECON);
		dk->trig_masked = true;
	}
	spin_unlock(&dk->trig_lock);
}

/* Point GF0 at a buffer and have DECON latch it at the next frame start. */
static void decon_scanout(struct decon_kms *dk, u32 base, u32 in_con,
			  u32 src_size)
{
	u32 val;

	/* the previous request must have been latched first (<= 1 frame) */
	if (readl_poll_timeout(dk->decon + DECON_SHADOW_REG_UPDATE_REQ, val,
			       !val, 100, 50000))
		drm_warn_once(&dk->drm, "shadow update stuck: 0x%08x\n", val);

	writel(in_con, dk->idma + IDMA_IN_CON);
	writel(src_size, dk->idma + IDMA_SRC_SIZE);
	writel(base, dk->idma + IDMA_IN_BASE_ADDR_Y);
	writel(DECON_SHADOW_GLOBAL | BIT(dk->win),
	       dk->decon + DECON_SHADOW_REG_UPDATE_REQ);
}

static int decon_pipe_check(struct drm_simple_display_pipe *pipe,
			    struct drm_plane_state *plane_state,
			    struct drm_crtc_state *crtc_state)
{
	struct drm_framebuffer *fb = plane_state->fb;
	int ret;

	ret = drm_atomic_helper_check_plane_state(plane_state, crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  false, false);
	if (ret || !fb || !plane_state->visible)
		return ret;

	/* full-screen, unpanned, stride a whole number of pixels */
	if (plane_state->src.x1 || plane_state->src.y1 ||
	    fb->width < S10E_WIDTH || fb->height < S10E_HEIGHT ||
	    fb->pitches[0] % 4 || fb->pitches[0] / 4 > 0xffff)
		return -EINVAL;

	return 0;
}

static void decon_pipe_update(struct drm_simple_display_pipe *pipe,
			      struct drm_plane_state *old_state)
{
	struct decon_kms *dk = to_decon(pipe->crtc.dev);
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_pending_vblank_event *event;
	struct drm_framebuffer *fb = state->fb;
	unsigned long flags;

	if (fb && state->visible) {
		dma_addr_t addr = drm_fb_dma_get_gem_addr(fb, state, 0);
		u32 fmt = fb->format->format == DRM_FORMAT_ARGB8888 ?
			  IDMA_FMT_ARGB8888 : IDMA_FMT_XRGB8888;

		decon_scanout(dk, lower_32_bits(addr),
			      (dk->boot_in_con & ~IDMA_IMG_FORMAT) |
			      FIELD_PREP(IDMA_IMG_FORMAT, fmt),
			      (S10E_HEIGHT << 16) | (fb->pitches[0] / 4));
	}
	/* a damage-only commit (DirtyFB, fbdev) changes pixels in place */
	decon_trig_kick(dk, 2);

	event = crtc->state->event;
	if (!event)
		return;
	crtc->state->event = NULL;

	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	if (drm_crtc_vblank_get(crtc) == 0)
		drm_crtc_arm_vblank_event(crtc, event);
	else
		drm_crtc_send_vblank_event(crtc, event);
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

static void decon_pipe_enable(struct drm_simple_display_pipe *pipe,
			      struct drm_crtc_state *crtc_state,
			      struct drm_plane_state *plane_state)
{
	/*
	 * The pipeline is already running; only the buffer changes. Vblank
	 * goes off as soon as no one holds it, so an idle screen sends nothing.
	 * Without a hardware counter the count only moves on real frames,
	 * which with PSR is what a vblank is.
	 */
	const struct drm_vblank_crtc_config config = {
		.offdelay_ms = 5000,
		.disable_immediate = true,
	};

	drm_crtc_vblank_on_config(&pipe->crtc, &config);
}

static void decon_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct decon_kms *dk = to_decon(pipe->crtc.dev);

	drm_crtc_vblank_off(&pipe->crtc);
	/*
	 * Our buffers may be freed now: scan out S-Boot's reserved one again.
	 * The panel cannot be switched off yet (no DSI driver), so "off" is
	 * black, which on OLED is pixels off.
	 */
	memset(dk->boot_fb, 0, S10E_WIDTH * S10E_HEIGHT * 4);
	wmb();
	decon_scanout(dk, dk->boot_base, dk->boot_in_con, dk->boot_src_size);
	decon_trig_kick(dk, 2);
}

/* FRAME_START comes only with a frame, so wanting vblanks means frames */
static int decon_pipe_enable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct decon_kms *dk = to_decon(pipe->crtc.dev);
	unsigned long flags;

	spin_lock_irqsave(&dk->trig_lock, flags);
	dk->trig_vblank = true;
	spin_unlock_irqrestore(&dk->trig_lock, flags);
	decon_trig_kick(dk, 1);
	return 0;
}

static void decon_pipe_disable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct decon_kms *dk = to_decon(pipe->crtc.dev);
	unsigned long flags;

	spin_lock_irqsave(&dk->trig_lock, flags);
	dk->trig_vblank = false;
	spin_unlock_irqrestore(&dk->trig_lock, flags);
}

static const struct drm_simple_display_pipe_funcs decon_pipe_funcs = {
	.check = decon_pipe_check,
	.update = decon_pipe_update,
	.enable = decon_pipe_enable,
	.disable = decon_pipe_disable,
	.enable_vblank = decon_pipe_enable_vblank,
	.disable_vblank = decon_pipe_disable_vblank,
};

static const u32 decon_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

/*
 * DECON2 for DisplayPort, after dpu20/cal_9820 decon_reg_init()/start()/
 * stop() for DECON_OUT_DP and dpp_reg_init()/configure_params(). DECON2 is
 * cold (S-Boot never starts it) and takes window 4 on channel VGRFS: VGRFS
 * sits behind SysMMU_DPUD0 like GF0, so our GEM buffers are already
 * mapped for it. The DP controller generates the timing.
 */
#define DECON2_BASE			0x19050000
#define VGRFS_DMA_BASE			0x19076000
#define VGRFS_DPP_BASE			0x19026000
#define DECON2_IRQ_FRAME_START		195	/* GIC SPI */

#define DP_WIN				4
#define DP_CH_VGRFS			1

#define DECON_GC_TEN_BPC		BIT(20)
#define DECON_GC_CMD_MODE		BIT(8)
#define DECON_GC_RUN_STATUS		BIT(4)
#define DECON_GC_EN			BIT(1)
#define DECON_GC_EN_F			BIT(0)
#define DECON_SRAM_SHARE_ENABLE		0x0030
#define DECON_SRAM1_3_SHARE		(BIT(16) | BIT(20) | BIT(24))
#define DECON_INTERRUPT_ENABLE		0x0040
#define DECON_INT_EN			BIT(0)
#define DECON_EXTRA_INT_PENDING		0x0050
#define DECON_SCALED_SIZE_CONTROL_0	0x00a0
#define DECON_CLOCK_CONTROL_0		0x00f0
#define DECON_CC_CG_QACTIVE_MASK	(0x11111 | BIT(24) | BIT(28))
#define DECON_OUTFIFO_SIZE_CONTROL_0	0x0120
#define DECON_OUTFIFO_TH_CONTROL_0	0x012c
#define DECON_OUTFIFO_DATA_ORDER	0x0130
#define DECON_OUTFIFO_SWAP_MASK		GENMASK(6, 4)
#define DECON_WIN_EN(w)			BIT(4 * (w))
#define DECON_WIN_MAPCOLOR_EN(w)	BIT(4 * (w) + 1)
#define DECON_WIN_CHMAP(w, ch)		(((ch) & 7) << (4 * (w)))
#define DECON_WIN_CHMAP_MASK(w)		(7 << (4 * (w)))
#define DECON_DATA_PATH_CONTROL_2	0x0230
#define DECON2_NOCOMP_DPIF		0x008
#define DECON_DP_CONNECTION_CONTROL	0x0270	/* DECON0 space */
#define DECON_SEL_DP0_MASK		GENMASK(2, 0)
#define DECON_WIN_CONTROL_0(w)		(0x1000 + (w) * 0x30)	/* DECON0 space */
#define DECON_WIN_CONTROL_1(w)		(0x1004 + (w) * 0x30)
#define DECON_WIN_START_POS(w)		(0x1008 + (w) * 0x30)
#define DECON_WIN_END_POS(w)		(0x100c + (w) * 0x30)
#define DECON_WIN_START_TIME(w)		(0x1018 + (w) * 0x30)

#define IDMA_ENABLE			0x0000
#define IDMA_CLOCK_GATE_EN		BIT(9)
#define IDMA_IRQ			0x0004
#define IDMA_ALL_IRQ_MASK		(0x3fb << 1)
#define IDMA_IRQ_ENABLE			BIT(0)
#define IDMA_OUT_CON			0x000c
#define IDMA_OUT_FRAME_ALPHA		GENMASK(31, 24)
#define IDMA_SRC_OFFSET			0x0014
#define IDMA_DYNAMIC_GATING_EN		0x0058
#define IDMA_IN_CON_DEFAULT		0x02000000	/* what S-Boot leaves in GF0 */

#define DPP_ENABLE			0x0000
#define DPP_CLOCK_GATE_EN		(7 << 8)
#define DPP_IRQ				0x0004
#define DPP_ALL_IRQ_MASK		(0x21 << 1)
#define DPP_IRQ_ENABLE			BIT(0)
#define DPP_IN_CON			0x0008
#define DPP_IMG_SIZE			0x0018
#define DPP_SCALED_IMG_SIZE		0x002c
#define DPP_MAIN_H_RATIO		0x0044
#define DPP_MAIN_V_RATIO		0x0048
#define DPP_V_COEF(n, s, x)		(0x200 + (n) * 0x4 + (s) * 0x24 + (x) * 0x200)
#define DPP_H_COEF(n, s, x)		(0x290 + (n) * 0x4 + (s) * 0x24 + (x) * 0x200)
#define DPP_VGRF_HDR_CON		0x0600
#define DPP_DYNAMIC_GATING_EN		0x0a54
#define DPP_LINECNT_CON			0x0d00

/* dpu20/dpp_coef.h, selection 0 (1:1): phases 0..8 */
static const s16 vgrfs_h_coef[9][8] = {
	{   0,   0,   0, 512,   0,   0,   0,   0 },
	{  -2,   8, -25, 509,  30,  -9,   2,  -1 },
	{  -4,  14, -46, 499,  64, -19,   5,  -1 },
	{  -5,  20, -62, 482, 101, -30,   8,  -2 },
	{  -6,  23, -73, 458, 142, -41,  12,  -3 },
	{  -6,  25, -80, 429, 185, -53,  15,  -3 },
	{  -6,  26, -83, 395, 228, -63,  19,  -4 },
	{  -6,  25, -82, 357, 273, -71,  21,  -5 },
	{  -5,  23, -78, 316, 316, -78,  23,  -5 },
};

static const s16 vgrfs_v_coef[9][4] = {
	{   0, 512,   0,   0 },
	{ -15, 508,  20,  -1 },
	{ -25, 495,  45,  -3 },
	{ -31, 473,  75,  -5 },
	{ -33, 443, 110,  -8 },
	{ -33, 408, 148, -11 },
	{ -31, 367, 190, -14 },
	{ -27, 324, 234, -19 },
	{ -23, 279, 279, -23 },
};

static inline struct decon_kms *dp_pipe_to_decon(struct drm_simple_display_pipe *pipe)
{
	return to_decon(pipe->crtc.dev);
}

/* dpp_reg_init() and the size-independent part of configure_params() */
static void vgrfs_init(struct decon_kms *dk)
{
	int i, j, k;

	rmw(dk->vgrfs_dma, IDMA_IRQ, IDMA_ALL_IRQ_MASK | IDMA_IRQ_ENABLE, IDMA_IRQ_ENABLE);
	rmw(dk->vgrfs_dma, IDMA_ENABLE, IDMA_CLOCK_GATE_EN, 0);
	writel(0, dk->vgrfs_dma + IDMA_DYNAMIC_GATING_EN);
	rmw(dk->vgrfs_dma, IDMA_OUT_CON, IDMA_OUT_FRAME_ALPHA, IDMA_OUT_FRAME_ALPHA);
	rmw(dk->vgrfs_dpp, DPP_IRQ, DPP_ALL_IRQ_MASK | DPP_IRQ_ENABLE, DPP_IRQ_ENABLE);
	rmw(dk->vgrfs_dpp, DPP_ENABLE, DPP_CLOCK_GATE_EN, 0);
	writel(0, dk->vgrfs_dpp + DPP_DYNAMIC_GATING_EN);
	rmw(dk->vgrfs_dpp, DPP_LINECNT_CON, 0x3, 0x1);

	writel(1 << 20, dk->vgrfs_dpp + DPP_MAIN_H_RATIO);
	writel(1 << 20, dk->vgrfs_dpp + DPP_MAIN_V_RATIO);
	for (i = 0; i < 9; i++)
		for (j = 0; j < 8; j++)
			for (k = 0; k < 2; k++)
				writel((u32)(s32)vgrfs_h_coef[i][j], dk->vgrfs_dpp + DPP_H_COEF(i, j, k));
	for (i = 0; i < 9; i++)
		for (j = 0; j < 4; j++)
			for (k = 0; k < 2; k++)
				writel((u32)(s32)vgrfs_v_coef[i][j], dk->vgrfs_dpp + DPP_V_COEF(i, j, k));
	writel(0, dk->vgrfs_dpp + DPP_VGRF_HDR_CON);
	writel(0, dk->vgrfs_dpp + DPP_IN_CON);
}

/* the buffer; latched with DECON2's next shadow update */
static void vgrfs_set_fb(struct decon_kms *dk, struct drm_plane_state *state)
{
	struct drm_framebuffer *fb = state->fb;
	u32 w = drm_rect_width(&state->dst), h = drm_rect_height(&state->dst);
	u32 x = state->src.x1 >> 16, y = state->src.y1 >> 16;
	u32 fmt = fb->format->format == DRM_FORMAT_ARGB8888 ?
		  IDMA_FMT_ARGB8888 : IDMA_FMT_XRGB8888;

	writel((y << 16) | x, dk->vgrfs_dma + IDMA_SRC_OFFSET);
	writel((fb->height << 16) | (fb->pitches[0] / 4), dk->vgrfs_dma + IDMA_SRC_SIZE);
	writel((h << 16) | w, dk->vgrfs_dma + IDMA_IMG_SIZE);
	writel((h << 16) | w, dk->vgrfs_dpp + DPP_IMG_SIZE);
	writel((h << 16) | w, dk->vgrfs_dpp + DPP_SCALED_IMG_SIZE);
	writel(lower_32_bits(drm_fb_dma_get_gem_addr(fb, state, 0)),
	       dk->vgrfs_dma + IDMA_IN_BASE_ADDR_Y);
	writel(IDMA_IN_CON_DEFAULT | FIELD_PREP(IDMA_IMG_FORMAT, fmt),
	       dk->vgrfs_dma + IDMA_IN_CON);
}

static int decon2_start(struct decon_kms *dk, const struct drm_display_mode *mode,
			struct drm_plane_state *state)
{
	void __iomem *d2 = dk->decon2, *d0 = dk->decon;
	u32 size = (mode->vdisplay << 16) | mode->hdisplay, v;
	int ret;

	vgrfs_init(dk);
	vgrfs_set_fb(dk, state);

	/* decon_reg_init(2); DECON0's CLOCK_CONTROL_0 bit 28 is already set */
	rmw(d2, DECON_CLOCK_CONTROL_0, DECON_CC_CG_QACTIVE_MASK, 0);
	writel(DECON_SRAM1_3_SHARE, d2 + DECON_SRAM_SHARE_ENABLE);
	rmw(d2, DECON_GLOBAL_CONTROL, DECON_GC_CMD_MODE, 0);
	writel(size, d2 + DECON_BLENDER_BG_IMAGE_SIZE_0);
	writel(size, d2 + DECON_SCALED_SIZE_CONTROL_0);
	rmw(d2, DECON_HW_SW_TRIG_CONTROL, DECON_HW_TRIG_EN, DECON_HW_TRIG_EN);
	rmw(d2, DECON_OUTFIFO_DATA_ORDER, DECON_OUTFIFO_SWAP_MASK, 0);	/* RGB for DP */
	rmw(d2, DECON_DATA_PATH_CONTROL_2, GENMASK(25, 24) | GENMASK(7, 0), DECON2_NOCOMP_DPIF);
	writel(size, d2 + DECON_OUTFIFO_SIZE_CONTROL_0);
	rmw(d2, DECON_OUTFIFO_TH_CONTROL_0, 0x7, 0x5);
	rmw(d2, DECON_GLOBAL_CONTROL, DECON_GC_TEN_BPC | DECON_GC_EN_F, 0);
	writel(DECON_INT_FRAME_DONE | DECON_INT_FRAME_START, d2 + DECON_INTERRUPT_PENDING);
	writel(BIT(8) | BIT(4), d2 + DECON_EXTRA_INT_PENDING);
	writel(DECON_INT_FRAME_START | DECON_INT_EN, d2 + DECON_INTERRUPT_ENABLE);
	dk->dp_saved_270 = readl(d0 + DECON_DP_CONNECTION_CONTROL);
	rmw(d0, DECON_DP_CONNECTION_CONTROL, DECON_SEL_DP0_MASK, 2);

	/* window 4 on VGRFS, full screen, blend "copy" like DECON0's window 5 */
	writel(0x102, d0 + DECON_WIN_CONTROL_0(DP_WIN));
	writel(0, d0 + DECON_WIN_CONTROL_1(DP_WIN));
	writel(0, d0 + DECON_WIN_START_POS(DP_WIN));
	writel(((mode->vdisplay - 1) << 16) | (mode->hdisplay - 1),
	       d0 + DECON_WIN_END_POS(DP_WIN));
	writel(0, d0 + DECON_WIN_START_TIME(DP_WIN));
	rmw(d2, DECON_DATA_PATH_CONTROL_1, DECON_WIN_CHMAP_MASK(DP_WIN),
	    DECON_WIN_CHMAP(DP_WIN, DP_CH_VGRFS));
	rmw(d2, DECON_DATA_PATH_CONTROL_0, DECON_WIN_EN(DP_WIN) | DECON_WIN_MAPCOLOR_EN(DP_WIN),
	    DECON_WIN_EN(DP_WIN));

	/* decon_reg_start(): the DP side already powered the LH channel */
	rmw(d2, DECON_GLOBAL_CONTROL, DECON_GC_EN | DECON_GC_EN_F, DECON_GC_EN | DECON_GC_EN_F);
	writel(DECON_SHADOW_GLOBAL | BIT(DP_WIN), d2 + DECON_SHADOW_REG_UPDATE_REQ);
	ret = readl_poll_timeout(d2 + DECON_GLOBAL_CONTROL, v, v & DECON_GC_RUN_STATUS, 10, 20000);
	if (ret)
		drm_err(&dk->drm, "DECON2 did not start (%08x)\n", v);
	return ret;
}

/* decon_reg_stop(): window off, per-frame off, wait for idle */
static void decon2_stop(struct decon_kms *dk)
{
	void __iomem *d2 = dk->decon2;
	u32 v;

	writel(0, d2 + DECON_INTERRUPT_ENABLE);
	rmw(d2, DECON_DATA_PATH_CONTROL_0, DECON_WIN_EN(DP_WIN), 0);
	writel(DECON_SHADOW_GLOBAL | BIT(DP_WIN), d2 + DECON_SHADOW_REG_UPDATE_REQ);
	rmw(d2, DECON_GLOBAL_CONTROL, DECON_GC_EN_F, 0);
	if (readl_poll_timeout(d2 + DECON_GLOBAL_CONTROL, v, !(v & DECON_GC_RUN_STATUS),
			       100, 100000))
		drm_err(&dk->drm, "DECON2 did not stop (%08x)\n", v);
	rmw(dk->decon, DECON_DP_CONNECTION_CONTROL, DECON_SEL_DP0_MASK, dk->dp_saved_270);
}

static int decon_dp_check(struct drm_simple_display_pipe *pipe,
			  struct drm_plane_state *plane_state,
			  struct drm_crtc_state *crtc_state)
{
	struct drm_framebuffer *fb = plane_state->fb;
	int ret;

	ret = drm_atomic_helper_check_plane_state(plane_state, crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  false, false);
	if (ret || !fb || !plane_state->visible)
		return ret;

	/* full-screen window, stride a whole number of pixels */
	if (drm_rect_width(&plane_state->dst) != crtc_state->mode.hdisplay ||
	    drm_rect_height(&plane_state->dst) != crtc_state->mode.vdisplay ||
	    fb->pitches[0] % 4 || fb->pitches[0] / 4 > 0xffff)
		return -EINVAL;
	return 0;
}

/*
 * The flip is done when DECON2 has taken the shadow registers, not at the
 * next FRAME_START: an update written just after a latch sees that frame's
 * FRAME_START first, and a client told then would draw into the buffer that
 * is still being scanned out (flicker in fullscreen games).
 */
static void decon_dp_send_event(struct decon_kms *dk, struct drm_crtc *crtc, bool armed)
{
	struct drm_pending_vblank_event *event = crtc->state->event;
	unsigned long flags;

	if (!event)
		return;
	crtc->state->event = NULL;
	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	if (armed && !dk->dp_event && drm_crtc_vblank_get(crtc) == 0)
		dk->dp_event = event;
	else
		drm_crtc_send_vblank_event(crtc, event);
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

/* under event_lock: hand back the held flip event */
static void decon_dp_complete_event(struct decon_kms *dk)
{
	struct drm_crtc *crtc = &dk->dp_pipe.crtc;

	drm_crtc_send_vblank_event(crtc, dk->dp_event);
	drm_crtc_vblank_put(crtc);
	dk->dp_event = NULL;
}

static void decon_dp_enable(struct drm_simple_display_pipe *pipe,
			    struct drm_crtc_state *crtc_state,
			    struct drm_plane_state *plane_state)
{
	struct decon_kms *dk = dp_pipe_to_decon(pipe);
	const struct drm_display_mode *mode = &crtc_state->adjusted_mode;

	/* downstream order: the DP stream and its LH channel first, then DECON2 */
	if (exynos9820_dp_stream_on(dk->dp_bridge, mode))
		return;
	if (decon2_start(dk, mode, plane_state)) {
		decon2_stop(dk);
		exynos9820_dp_stream_off(dk->dp_bridge);
		return;
	}
	dk->dp_running = true;
	drm_info(&dk->drm, "DisplayPort: %s@%d\n", mode->name, drm_mode_vrefresh(mode));
	drm_crtc_vblank_on(&pipe->crtc);
}

static void decon_dp_disable(struct drm_simple_display_pipe *pipe)
{
	struct decon_kms *dk = dp_pipe_to_decon(pipe);

	unsigned long flags;

	drm_crtc_vblank_off(&pipe->crtc);
	spin_lock_irqsave(&dk->drm.event_lock, flags);
	if (dk->dp_event)
		decon_dp_complete_event(dk);
	spin_unlock_irqrestore(&dk->drm.event_lock, flags);
	if (!dk->dp_running)
		return;
	decon2_stop(dk);
	exynos9820_dp_stream_off(dk->dp_bridge);
	dk->dp_running = false;
}

static void decon_dp_update(struct drm_simple_display_pipe *pipe,
			    struct drm_plane_state *old_state)
{
	struct decon_kms *dk = dp_pipe_to_decon(pipe);
	struct drm_plane_state *state = pipe->plane.state;
	u32 val;

	if (dk->dp_running && state->fb && state->visible) {
		if (readl_poll_timeout(dk->decon2 + DECON_SHADOW_REG_UPDATE_REQ, val,
				       !val, 100, 50000))
			drm_warn_once(&dk->drm, "DECON2 shadow update stuck: 0x%08x\n", val);
		vgrfs_set_fb(dk, state);
		writel(DECON_SHADOW_GLOBAL | BIT(DP_WIN),
		       dk->decon2 + DECON_SHADOW_REG_UPDATE_REQ);
	}
	decon_dp_send_event(dk, &pipe->crtc, dk->dp_running);
}

static int decon_dp_enable_vblank(struct drm_simple_display_pipe *pipe)
{
	return 0;	/* FRAME_START is on whenever DECON2 runs */
}

static void decon_dp_disable_vblank(struct drm_simple_display_pipe *pipe)
{
}

static const struct drm_simple_display_pipe_funcs decon_dp_pipe_funcs = {
	.check = decon_dp_check,
	.update = decon_dp_update,
	.enable = decon_dp_enable,
	.disable = decon_dp_disable,
	.enable_vblank = decon_dp_enable_vblank,
	.disable_vblank = decon_dp_disable_vblank,
};

static irqreturn_t decon2_irq(int irq, void *data)
{
	struct decon_kms *dk = data;
	u32 pend = readl(dk->decon2 + DECON_INTERRUPT_PENDING) &
		   (DECON_INT_FRAME_START | DECON_INT_FRAME_DONE | DECON_INT_EXTRA);

	if (!pend)
		return IRQ_NONE;
	writel(pend, dk->decon2 + DECON_INTERRUPT_PENDING);
	if (pend & DECON_INT_FRAME_START) {
		drm_crtc_handle_vblank(&dk->dp_pipe.crtc);
		spin_lock(&dk->drm.event_lock);
		if (dk->dp_event && !(readl(dk->decon2 + DECON_SHADOW_REG_UPDATE_REQ) &
				      (DECON_SHADOW_GLOBAL | BIT(DP_WIN))))
			decon_dp_complete_event(dk);
		spin_unlock(&dk->drm.event_lock);
	}
	return IRQ_HANDLED;
}

/* DECON2's FRAME_START has no node in our DT yet: map the GIC SPI here */
static int decon2_map_irq(struct decon_kms *dk)
{
	struct device_node *gic = of_irq_find_parent(dk->drm.dev->of_node);
	struct irq_fwspec spec = {
		.param_count = 3,
		.param = { 0 /* GIC_SPI */, DECON2_IRQ_FRAME_START, IRQ_TYPE_LEVEL_HIGH },
	};
	int irq;

	if (!gic)
		return -ENODEV;
	spec.fwnode = of_fwnode_handle(gic);
	irq = irq_create_fwspec_mapping(&spec);
	of_node_put(gic);
	return irq ?: -EINVAL;
}

static void decon_put_bridge(void *bridge)
{
	drm_bridge_put(bridge);
}

/* the DP output, if the DP controller is there; the panel works without it */
static int decon_dp_init(struct decon_kms *dk)
{
	struct drm_device *drm = &dk->drm;
	struct device *dev = drm->dev;
	struct drm_connector *connector;
	struct device_node *np;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "samsung,exynos9820-dp");
	if (!np || !of_device_is_available(np)) {
		of_node_put(np);
		return 0;
	}
	dk->dp_bridge = of_drm_find_and_get_bridge(np);
	of_node_put(np);
	if (!dk->dp_bridge) {
		drm_warn(drm, "DisplayPort controller not bound, no external output\n");
		return 0;
	}
	ret = devm_add_action_or_reset(dev, decon_put_bridge, dk->dp_bridge);
	if (ret)
		return ret;

	dk->decon2 = devm_ioremap(dev, DECON2_BASE, 0x1000);
	dk->vgrfs_dma = devm_ioremap(dev, VGRFS_DMA_BASE, 0x1000);
	dk->vgrfs_dpp = devm_ioremap(dev, VGRFS_DPP_BASE, 0x1000);
	if (!dk->decon2 || !dk->vgrfs_dma || !dk->vgrfs_dpp)
		return -ENOMEM;
	if (readl(dk->decon2 + DECON_GLOBAL_CONTROL) & DECON_GC_RUN_STATUS) {
		drm_warn(drm, "DECON2 already running, no external output\n");
		return 0;
	}

	ret = drm_simple_display_pipe_init(drm, &dk->dp_pipe, &decon_dp_pipe_funcs,
					   decon_formats, ARRAY_SIZE(decon_formats),
					   NULL, NULL);
	if (ret)
		return ret;
	ret = drm_bridge_attach(&dk->dp_pipe.encoder, dk->dp_bridge, NULL,
				DRM_BRIDGE_ATTACH_NO_CONNECTOR);
	if (ret)
		return ret;
	connector = drm_bridge_connector_init(drm, &dk->dp_pipe.encoder);
	if (IS_ERR(connector))
		return PTR_ERR(connector);
	ret = drm_connector_attach_encoder(connector, &dk->dp_pipe.encoder);
	if (ret)
		return ret;

	dk->dp_irq = decon2_map_irq(dk);
	if (dk->dp_irq < 0)
		return dk->dp_irq;
	return devm_request_irq(dev, dk->dp_irq, decon2_irq, 0, "decon2", dk);
}

static irqreturn_t decon_irq(int irq, void *data)
{
	struct decon_kms *dk = data;
	u32 pend = readl(dk->decon + DECON_INTERRUPT_PENDING) &
		   (DECON_INT_FRAME_START | DECON_INT_FRAME_DONE |
		    DECON_INT_EXTRA);

	if (!pend)
		return IRQ_NONE;
	writel(pend, dk->decon + DECON_INTERRUPT_PENDING);

	if (pend & DECON_INT_FRAME_START) {
		drm_crtc_handle_vblank(&dk->pipe.crtc);
		decon_trig_idle(dk);
	}
	return IRQ_HANDLED;
}

static const struct drm_mode_config_funcs decon_mode_config_funcs = {
	/* front-buffer writers (fbdev, DirtyFB) must commit to get a frame */
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

DEFINE_DRM_GEM_DMA_FOPS(decon_fops);

static const struct drm_driver decon_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	/* "exynos": Mesa's kmsro then renders with panfrost into our buffers */
	.name = "exynos",
	.desc = "Exynos 9820 DECON handover",
	.major = 1,
	.minor = 0,
	.fops = &decon_fops,
	DRM_GEM_DMA_DRIVER_OPS,
	DRM_FBDEV_DMA_DRIVER_OPS,
};

/* Refuse anything but the state S-Boot is known to leave (DPU-SPEC.md 7). */
static int decon_check_handover(struct decon_kms *dk)
{
	struct device *dev = dk->drm.dev;
	u32 ctrl = readl(dk->decon + DECON_GLOBAL_CONTROL);
	u32 trig = readl(dk->decon + DECON_HW_SW_TRIG_CONTROL);
	u32 path0 = readl(dk->decon + DECON_DATA_PATH_CONTROL_0);
	u32 path1 = readl(dk->decon + DECON_DATA_PATH_CONTROL_1);
	unsigned int w, nwin = 0;

	if ((ctrl & DECON_GLOBAL_RUN) != DECON_GLOBAL_RUN ||
	    (trig & (DECON_HW_TRIG_EN | DECON_HW_TRIG_MASK_DECON)) !=
	    DECON_HW_TRIG_EN) {
		dev_err(dev, "DECON not running continuously (0x%08x 0x%08x)\n",
			ctrl, trig);
		return -ENODEV;
	}

	for (w = 0; w < DECON_NR_WINDOWS; w++) {
		if (path0 & BIT(4 * w)) {
			dk->win = w;
			nwin++;
		}
	}
	if (nwin != 1 || ((path1 >> (4 * dk->win)) & 7) != 0) {
		dev_err(dev, "expected one window on GF0 (0x%08x 0x%08x)\n",
			path0, path1);
		return -ENODEV;
	}

	dk->boot_in_con = readl(dk->idma + IDMA_IN_CON);
	dk->boot_src_size = readl(dk->idma + IDMA_SRC_SIZE);
	dk->boot_base = readl(dk->idma + IDMA_IN_BASE_ADDR_Y);
	if (readl(dk->decon + DECON_BLENDER_BG_IMAGE_SIZE_0) != S10E_SIZE_REG ||
	    readl(dk->idma + IDMA_IMG_SIZE) != S10E_SIZE_REG ||
	    FIELD_GET(IDMA_IMG_FORMAT, dk->boot_in_con) != IDMA_FMT_ARGB8888) {
		dev_err(dev, "unexpected boot scanout (IN_CON 0x%08x)\n",
			dk->boot_in_con);
		return -ENODEV;
	}

	dev_info(dev, "taking over window %u, GF0 at 0x%08x\n", dk->win,
		 dk->boot_base);
	return 0;
}

/* DSI host on S-Boot's link: short and long writes through the command FIFO */

static int dsim_wait_clear(struct decon_kms *dk, u32 bits)
{
	u32 val;

	return readl_poll_timeout(dk->dsim + DSIM_FIFOCTRL, val, !(val & bits),
				  10, 100000);
}

static ssize_t decon_dsi_transfer(struct mipi_dsi_host *host,
				  const struct mipi_dsi_msg *msg)
{
	struct decon_kms *dk = container_of(host, struct decon_kms, dsi_host);
	struct mipi_dsi_packet pkt;
	u32 val;
	size_t i;
	int ret;

	if (msg->rx_len)
		return -EOPNOTSUPP;	/* no BTA/RX path yet */

	ret = mipi_dsi_create_packet(&pkt, msg);
	if (ret)
		return ret;

	guard(mutex)(&dk->dsi_lock);

	ret = dsim_wait_clear(dk, DSIM_FULL_PH_SFR);
	for (i = 0; !ret && i < pkt.payload_length; i += 4) {
		size_t n = min_t(size_t, 4, pkt.payload_length - i);

		val = 0;
		memcpy(&val, pkt.payload + i, n);
		ret = dsim_wait_clear(dk, DSIM_FULL_PL_SFR);
		if (!ret)
			writel(le32_to_cpu((__force __le32)val),
			       dk->dsim + DSIM_PAYLOAD);
	}
	if (!ret) {
		writel(pkt.header[0] | pkt.header[1] << 8 | pkt.header[2] << 16,
		       dk->dsim + DSIM_PKTHDR);
		ret = readl_poll_timeout(dk->dsim + DSIM_FIFOCTRL, val,
					 val & DSIM_EMPTY_PH_SFR, 10, 100000);
	}
	if (ret) {
		dev_warn_ratelimited(dk->drm.dev,
				     "DSIM FIFO stuck (FIFOCTRL 0x%08x)\n",
				     readl(dk->dsim + DSIM_FIFOCTRL));
		return ret;
	}

	return msg->tx_len;
}

static int decon_drm_init(struct decon_kms *dk, struct drm_bridge *bridge)
{
	struct drm_device *drm = &dk->drm;
	struct device *dev = drm->dev;
	int ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	/* the panel's pipe checks its own size; the DP output goes to 4096x2160 */
	drm->mode_config.min_width = 640;
	drm->mode_config.max_width = 4096;
	drm->mode_config.min_height = 480;
	drm->mode_config.max_height = 4096;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.funcs = &decon_mode_config_funcs;

	ret = drm_simple_display_pipe_init(drm, &dk->pipe, &decon_pipe_funcs,
					   decon_formats,
					   ARRAY_SIZE(decon_formats), NULL,
					   NULL);
	if (ret)
		return ret;
	drm_plane_enable_fb_damage_clips(&dk->pipe.plane);

	/* the panel bridge creates the DSI connector */
	ret = drm_simple_display_pipe_attach_bridge(&dk->pipe, bridge);
	if (ret)
		return ret;

	ret = decon_dp_init(dk);
	if (ret)
		drm_warn(drm, "no DisplayPort output: %d\n", ret);

	ret = drm_vblank_init(drm, drm->mode_config.num_crtc);
	if (ret)
		return ret;

	ret = devm_request_irq(dev, dk->irq, decon_irq, 0, dev_name(dev), dk);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);

	/* enables the DP bridge's HPD, so plugging in a monitor sends a uevent */
	drmm_kms_helper_poll_init(drm);

	/* evict simpledrm from S-Boot's framebuffer */
	ret = aperture_remove_conflicting_devices(dk->boot_base,
						  S10E_WIDTH * S10E_HEIGHT * 4,
						  decon_driver.name);
	if (ret)
		return ret;

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;
	dk->registered = true;

	drm_client_setup(drm, NULL);
	return 0;
}

static int decon_dsi_attach(struct mipi_dsi_host *host,
			    struct mipi_dsi_device *dsi)
{
	struct decon_kms *dk = container_of(host, struct decon_kms, dsi_host);
	struct device *dev = dk->drm.dev;
	struct drm_bridge *bridge;
	struct drm_panel *panel;
	int ret;

	if (dk->registered)
		return -EBUSY;	/* one panel, and no re-init after a detach */

	panel = of_drm_find_panel(dsi->dev.of_node);
	if (IS_ERR(panel))
		return PTR_ERR(panel);

	bridge = devm_drm_panel_bridge_add_typed(dev, panel,
						 DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(bridge))
		return PTR_ERR(bridge);

	ret = decon_drm_init(dk, bridge);
	if (ret)
		dev_err(dev, "DRM init failed: %d\n", ret);
	return ret;
}

static int decon_dsi_detach(struct mipi_dsi_host *host,
			    struct mipi_dsi_device *dsi)
{
	struct decon_kms *dk = container_of(host, struct decon_kms, dsi_host);

	if (dk->registered) {
		drm_dev_unplug(&dk->drm);
		drm_atomic_helper_shutdown(&dk->drm);
	}
	return 0;
}

static const struct mipi_dsi_host_ops decon_dsi_host_ops = {
	.attach = decon_dsi_attach,
	.detach = decon_dsi_detach,
	.transfer = decon_dsi_transfer,
};

static int decon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct decon_kms *dk;
	int ret;

	dk = devm_drm_dev_alloc(dev, &decon_driver, struct decon_kms, drm);
	if (IS_ERR(dk))
		return PTR_ERR(dk);
	platform_set_drvdata(pdev, dk);

	dk->decon = devm_platform_ioremap_resource_byname(pdev, "decon");
	if (IS_ERR(dk->decon))
		return PTR_ERR(dk->decon);
	dk->idma = devm_platform_ioremap_resource_byname(pdev, "idma-gf0");
	if (IS_ERR(dk->idma))
		return PTR_ERR(dk->idma);
	dk->dsim = devm_platform_ioremap_resource_byname(pdev, "dsim");
	if (IS_ERR(dk->dsim))
		return PTR_ERR(dk->dsim);

	ret = decon_check_handover(dk);
	if (ret)
		return ret;

	dk->boot_fb = devm_memremap(dev, dk->boot_base,
				    S10E_WIDTH * S10E_HEIGHT * 4, MEMREMAP_WC);
	if (IS_ERR(dk->boot_fb))
		return PTR_ERR(dk->boot_fb);

	/* IDMA base registers are 32-bit, so are the IOVAs */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	/*
	 * S-Boot left SysMMU_DPUD0 in bypass. The first runtime resume turns
	 * translation on (through the IOMMU's device link) with S-Boot's
	 * buffer already mapped 1:1 from the reserved-memory iommu-addresses,
	 * so the running scanout never faults. The reference is never
	 * dropped: a suspended SysMMU goes back to bypass under our IOVAs.
	 */
	if (device_iommu_mapped(dev)) {
		ret = devm_pm_runtime_enable(dev);
		if (ret)
			return ret;
		ret = pm_runtime_resume_and_get(dev);
		if (ret)
			return ret;
	}

	dk->irq = platform_get_irq(pdev, 0);
	if (dk->irq < 0)
		return dk->irq;

	ret = devm_mutex_init(dev, &dk->dsi_lock);
	if (ret)
		return ret;

	/* the panel's probe attaches to this host, which registers the DRM device */
	dk->dsi_host.dev = dev;
	dk->dsi_host.ops = &decon_dsi_host_ops;
	return mipi_dsi_host_register(&dk->dsi_host);
}

static void decon_remove(struct platform_device *pdev)
{
	struct decon_kms *dk = platform_get_drvdata(pdev);

	mipi_dsi_host_unregister(&dk->dsi_host);
}

static void decon_shutdown(struct platform_device *pdev)
{
	struct decon_kms *dk = platform_get_drvdata(pdev);

	if (dk->registered)
		drm_atomic_helper_shutdown(&dk->drm);
}

static const struct of_device_id decon_of_match[] = {
	{ .compatible = "samsung,exynos9820-decon-handover" },
	{ }
};
MODULE_DEVICE_TABLE(of, decon_of_match);

static struct platform_driver decon_platform_driver = {
	.driver = {
		.name = "exynos9820-decon-kms",
		.of_match_table = decon_of_match,
	},
	.probe = decon_probe,
	.remove = decon_remove,
	.shutdown = decon_shutdown,
};
module_platform_driver(decon_platform_driver);

MODULE_SOFTDEP("pre: exynos9820-dp");
MODULE_DESCRIPTION("Exynos 9820 DECON0 handover KMS and DSI command host");
MODULE_LICENSE("GPL");
