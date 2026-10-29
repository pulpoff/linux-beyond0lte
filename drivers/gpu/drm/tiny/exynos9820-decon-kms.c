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
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
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
	void __iomem *decon;
	void __iomem *idma;
	void __iomem *dsim;
	unsigned int win;
	int irq;

	struct mipi_dsi_host dsi_host;
	struct mutex dsi_lock;		/* one packet in the FIFO at a time */
	bool registered;

	/* what S-Boot was scanning out, restored (blacked) on disable */
	void *boot_fb;
	u32 boot_base;
	u32 boot_in_con;
	u32 boot_src_size;
};

static inline struct decon_kms *to_decon(struct drm_device *drm)
{
	return container_of(drm, struct decon_kms, drm);
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
	/* the pipeline is already running; only the buffer changes */
	drm_crtc_vblank_on(&pipe->crtc);
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
}

static int decon_pipe_enable_vblank(struct drm_simple_display_pipe *pipe)
{
	return 0;	/* FRAME_START runs at the TE rate regardless */
}

static void decon_pipe_disable_vblank(struct drm_simple_display_pipe *pipe)
{
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

static irqreturn_t decon_irq(int irq, void *data)
{
	struct decon_kms *dk = data;
	u32 pend = readl(dk->decon + DECON_INTERRUPT_PENDING) &
		   (DECON_INT_FRAME_START | DECON_INT_FRAME_DONE |
		    DECON_INT_EXTRA);

	if (!pend)
		return IRQ_NONE;
	writel(pend, dk->decon + DECON_INTERRUPT_PENDING);

	if (pend & DECON_INT_FRAME_START)
		drm_crtc_handle_vblank(&dk->pipe.crtc);
	return IRQ_HANDLED;
}

static const struct drm_mode_config_funcs decon_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
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
	drm->mode_config.min_width = S10E_WIDTH;
	drm->mode_config.max_width = S10E_WIDTH;
	drm->mode_config.min_height = S10E_HEIGHT;
	drm->mode_config.max_height = S10E_HEIGHT;
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

	ret = drm_vblank_init(drm, 1);
	if (ret)
		return ret;

	ret = devm_request_irq(dev, dk->irq, decon_irq, 0, dev_name(dev), dk);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);

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

MODULE_DESCRIPTION("Exynos 9820 DECON0 handover KMS and DSI command host");
MODULE_LICENSE("GPL");
