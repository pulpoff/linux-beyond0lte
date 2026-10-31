// SPDX-License-Identifier: GPL-2.0
/*
 * Exynos 9820 camera receiver: one MIPI CSI-2 link (CSIS v5.2 behind a
 * D-PHY in the DPU block) and the write DMA of the same number
 * (CSISX4_PDP_DMA), which writes virtual channel 0 straight to memory. No
 * ISP, no firmware: the frames are raw Bayer, 16 bits per pixel (10 bits
 * used, little endian), for libcamera's software ISP.
 *
 * Each instance is a media device with the sensor linked straight to one
 * video node. Buffers rotate through the DMA's first address slot, which is
 * latched at frame start: the link's frame start interrupt programs the
 * buffer for the next frame (or turns the DMA off for it when none is
 * queued: the DMA's own interrupts then stop, the link's do not), and the
 * DMA's frame end interrupt completes the buffer the frame went to.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/spinlock.h>

#include <media/media-device.h>
#include <media/v4l2-async.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mc.h>
#include <media/videobuf2-dma-contig.h>

#define CSIS_DRV_NAME		"exynos9820-csis"

/* CSIS v5.2 link */
#define CSIS_CMN_CTRL		0x004
#define  CMN_CTRL_CSI_EN		BIT(0)
#define  CMN_CTRL_SW_RESET		BIT(1)
#define  CMN_CTRL_LANES_SHIFT		8
#define  CMN_CTRL_LANES_MASK		GENMASK(9, 8)
#define  CMN_CTRL_INTERLEAVE_MASK	GENMASK(11, 10)
#define  CMN_CTRL_INTERLEAVE_VC		(2 << 10)
#define  CMN_CTRL_UPDATE_SHADOW		GENMASK(19, 16)
#define  CMN_CTRL_PHY_SEL		BIT(21)
#define CSIS_INT_MSK0		0x010
#define CSIS_INT_SRC0		0x014
#define  INT0_FRAMESTART_VC0		BIT(24)
#define  INT0_ERR_SOT_HS		GENMASK(19, 16)
#define  INT0_ERR_LOST_FS		GENMASK(15, 12)
#define  INT0_ERR_LOST_FE		GENMASK(11, 8)
#define  INT0_ERR_OVER			BIT(4)
#define  INT0_ERR_WRONG_CFG		BIT(3)
#define  INT0_ERR_ECC			BIT(2)
#define  INT0_ERR_CRC			BIT(1)
#define  INT0_ERR_ID			BIT(0)
/* VC0 only: the phase-detect data some sensors send on VC1 is not received */
#define  INT0_ERRORS			(BIT(16) | BIT(12) | BIT(8) | GENMASK(4, 0))
#define CSIS_INT_MSK1		0x018
#define CSIS_INT_SRC1		0x01c
#define CSIS_ISP_CONFIG(ch)	(0x040 + 0x10 * (ch))
#define  ISP_CONFIG_DT_SHIFT		2
#define  ISP_CONFIG_DUAL_PIXEL		(1 << 12)
#define CSIS_ISP_RESOL(ch)	(0x044 + 0x10 * (ch))
#define CSIS_FRM_CNT0		0x100
#define CSIS_DEBUG_OPT		0x500
#define CSIS_PHY_STATUS		0x700
#define CSIS_PHY_CMN_CTRL	0x704
#define  PHY_CMN_ENABLE_CLK		BIT(0)
#define  PHY_CMN_ENABLE_DAT_SHIFT	1
#define  PHY_CMN_ENABLE_DAT_MASK	GENMASK(4, 1)

/* frame start, which latches the DMA's address, and the errors; frame end comes from the DMA */
#define CSIS_INT_MSK0_VAL	(INT0_FRAMESTART_VC0 | INT0_ERRORS)
#define CSIS_INT_MSK1_VAL	(BIT(12) | BIT(8))	/* VC0 resolution mismatches */

/* WDMA, VC0 and the common block */
#define DMA_VC_CTRL		0x000
#define  DMA_VC_ENABLE			BIT(0)
#define  DMA_VC_UPDT_PTR_EN		BIT(1)
#define DMA_VC_FMT		0x004
#define DMA_VC_ADDR1		0x010
#define DMA_VC_ACT_CTRL		0x090
#define DMA_VC_RESOL		0x0a4
#define DMA_VC_FCNTSEQ		0x0b0
#define DMA_CMN_DATA_CTRL	0x400
#define DMA_CMN_INT_MASK	0x404
#define DMA_CMN_INT_SRC		0x408
#define  DMA_INT_FRM_START		BIT(4)
#define  DMA_INT_FRM_END		BIT(8)
#define  DMA_INT_ERROR			BIT(12)
#define  DMA_INT_OVERLAP		BIT(20)
#define  DMA_INT_FRAME_DROP		BIT(24)
#define DMA_CMN_OTF_FORMAT	0x41c
#define DMA_INT_MASK_VAL	(DMA_INT_FRM_START | DMA_INT_FRM_END | DMA_INT_ERROR | \
				 DMA_INT_OVERLAP | DMA_INT_FRAME_DROP)

/* DMA top, shared by the four WDMAs */
#define DMA_TOP_ARB_PRI		0x00
#define DMA_TOP_CTRL		0x08
#define  DMA_TOP_IP_PROCESSING		BIT(1)
#define DMA_TOP_SRAM_SPLIT	0x0c
#define DMA_TOP_MATRIX		0x10
#define DMA_TOP_TEST_PATTERN	0x20

/* SYSREG_ISPPRE */
#define SYSREG_SC_CON2		0x40c		/* WDMA n input at + 4n: 0 = its own CSIS */
#define SYSREG_PHY_RESET	0x500		/* 1 = released */

#define MIPI_DT_RAW10		0x2b
#define CSIS_MIN_SIZE		64
#define CSIS_MAX_WIDTH		8192
#define CSIS_MAX_HEIGHT		8192

struct csis_format {
	u32 fourcc;
	u32 code;
};

static const struct csis_format csis_formats[] = {
	{ V4L2_PIX_FMT_SGRBG10, MEDIA_BUS_FMT_SGRBG10_1X10 },
	{ V4L2_PIX_FMT_SRGGB10, MEDIA_BUS_FMT_SRGGB10_1X10 },
	{ V4L2_PIX_FMT_SBGGR10, MEDIA_BUS_FMT_SBGGR10_1X10 },
	{ V4L2_PIX_FMT_SGBRG10, MEDIA_BUS_FMT_SGBRG10_1X10 },
};

struct csis_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct csis {
	struct device *dev;
	void __iomem *csis, *wdma, *phy, *dma_top, *sysreg, *pmu_iso;
	struct clk_bulk_data *clks;
	int n_clks;
	u32 id, reset_bit;
	bool dc_combo;
	unsigned int lanes;

	struct media_device mdev;
	struct v4l2_device v4l2_dev;
	struct v4l2_async_notifier notifier;
	struct v4l2_subdev *sensor;
	u32 sensor_pad;
	struct video_device vdev;
	struct media_pad vdev_pad;
	struct media_pipeline pipe;
	struct vb2_queue queue;
	struct mutex lock;		/* video node and queue */
	struct v4l2_pix_format fmt;
	const struct csis_format *cfmt;

	spinlock_t slock;		/* the fields below, against the IRQ */
	struct list_head queued;
	struct csis_buffer *next;	/* programmed, latched at the next frame start */
	struct csis_buffer *active;	/* being written */
	u64 active_ts;
	u32 active_seq;
	u32 sequence;
	u32 errors, err_csis0, err_csis1, err_dma;
	bool streaming;
};

static inline struct csis_buffer *to_csis_buffer(struct vb2_v4l2_buffer *vb)
{
	return container_of(vb, struct csis_buffer, vb);
}

static const struct csis_format *csis_find_format(u32 fourcc)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(csis_formats); i++)
		if (csis_formats[i].fourcc == fourcc)
			return &csis_formats[i];
	return NULL;
}

static void rmw(void __iomem *r, u32 mask, u32 val)
{
	writel((readl(r) & ~mask) | (val & mask), r);
}

/* ------------------------------------------------------------------------
 * Hardware
 */

/* the D-PHY settle count, from stock's per-sensor values */
static u32 csis_settle(u32 mbps)
{
	if (mbps <= 1810)
		return 9;
	if (mbps <= 2060)
		return 10;
	if (mbps <= 2180)
		return 11;
	return 12;
}

/* stock __set_phy_cfg_0502_0002_dphy for 1500 <= speed < 3000; the D/C combo PHY adds SD_ANA_CON7 on lanes 0-2 */
static void csis_phy_config(struct csis *c, u32 mbps)
{
	void __iomem *p = c->phy;
	u32 settle = csis_settle(mbps);
	unsigned int i;

	writel(0x00000001, p + 0x00);	/* SC_GNR_CON0 */
	writel(0x00001450, p + 0x04);	/* SC_GNR_CON1 */
	writel(0x00000004, p + 0x08);	/* SC_ANA_CON0 */
	writel(0x00009000, p + 0x0c);	/* SC_ANA_CON1 */
	writel(0x00000005, p + 0x10);	/* SC_ANA_CON2 */
	writel(0x00000600, p + 0x14);	/* SC_ANA_CON3 */
	writel(0x00000301, p + 0x30);	/* SC_TIME_CON0 */
	for (i = 0; i < c->lanes; i++) {
		void __iomem *l = p + 0x100 * (i + 1);

		writel(0x00000001, l + 0x00);		/* SD_GNR_CON0 */
		writel(0x00001450, l + 0x04);		/* SD_GNR_CON1 */
		writel(0x00000004, l + 0x08);		/* SD_ANA_CON0 */
		writel(0x00009000, l + 0x0c);		/* SD_ANA_CON1 */
		writel(0x00000005 | ((mbps >= 2000 ? 2 : 3) << 8), l + 0x10);	/* SD_ANA_CON2 skew_delay_sel */
		writel(0x00000600, l + 0x14);		/* SD_ANA_CON3 */
		if (c->dc_combo && i < 3)
			writel(0x00000040, l + 0x24);	/* SD_ANA_CON7 */
		rmw(l + 0x30, GENMASK(8, 0), settle);	/* SD_TIME_CON0: settle, settle_clk_sel 0 */
		writel(0x00000003, l + 0x34);		/* SD_TIME_CON1 */
		rmw(l + 0x40, BIT(0), 1);		/* SD_DESKEW_CON0 skew_cal_en */
		writel(0x0000081a, l + 0x50);		/* SD_DESKEW_CON4 */
	}
}

static void csis_dma_set(struct csis *c, struct csis_buffer *b)
{
	if (b) {
		writel(vb2_dma_contig_plane_dma_addr(&b->vb.vb2_buf, 0), c->wdma + DMA_VC_ADDR1);
		writel(DMA_VC_ENABLE | DMA_VC_UPDT_PTR_EN, c->wdma + DMA_VC_CTRL);
	} else {
		writel(0, c->wdma + DMA_VC_CTRL);
	}
}

static void csis_hw_start(struct csis *c, u32 mbps)
{
	u32 w = c->fmt.width, h = c->fmt.height;
	int n;

	/* PHY: isolation bypass (shared with the display on CSIS0: only ever set it), reset released */
	writel(readl(c->pmu_iso) | BIT(0), c->pmu_iso);
	rmw(c->sysreg + SYSREG_PHY_RESET, BIT(c->reset_bit), 0);
	rmw(c->sysreg + SYSREG_PHY_RESET, BIT(c->reset_bit), BIT(c->reset_bit));
	writel(0, c->sysreg + SYSREG_SC_CON2 + 4 * c->id);
	csis_phy_config(c, mbps);

	writel(readl(c->csis + CSIS_CMN_CTRL) | CMN_CTRL_SW_RESET, c->csis + CSIS_CMN_CTRL);
	for (n = 0; n < 10 && (readl(c->csis + CSIS_CMN_CTRL) & CMN_CTRL_SW_RESET); n++)
		udelay(10);
	writel(CSIS_INT_MSK0_VAL, c->csis + CSIS_INT_MSK0);
	writel(CSIS_INT_MSK1_VAL, c->csis + CSIS_INT_MSK1);
	writel(~0, c->csis + CSIS_INT_SRC0);
	writel(~0, c->csis + CSIS_INT_SRC1);
	rmw(c->csis + CSIS_CMN_CTRL, CMN_CTRL_LANES_MASK, (c->lanes - 1) << CMN_CTRL_LANES_SHIFT);
	rmw(c->csis + CSIS_PHY_CMN_CTRL, PHY_CMN_ENABLE_DAT_MASK,
	    GENMASK(c->lanes - 1, 0) << PHY_CMN_ENABLE_DAT_SHIFT);
	/*
	 * Channels by virtual channel: CH0 takes VC0, the picture. The 2L4
	 * sends phase-detect data on VC1; in CH0-only mode those packets
	 * became a storm of lost FS/FE errors, and a channel that takes them
	 * with nothing behind it overflows. So the other channels wait for VC3,
	 * which no sensor here sends, and VC1 goes nowhere.
	 */
	rmw(c->csis + CSIS_CMN_CTRL, CMN_CTRL_INTERLEAVE_MASK, CMN_CTRL_INTERLEAVE_VC);
	writel(1, c->csis + CSIS_DEBUG_OPT);				/* pixel align */
	writel((MIPI_DT_RAW10 << ISP_CONFIG_DT_SHIFT) | ISP_CONFIG_DUAL_PIXEL, c->csis + CSIS_ISP_CONFIG(0));
	writel((h << 16) | w, c->csis + CSIS_ISP_RESOL(0));
	for (n = 1; n < 4; n++)
		writel(3 | (MIPI_DT_RAW10 << ISP_CONFIG_DT_SHIFT) | ISP_CONFIG_DUAL_PIXEL,
		       c->csis + CSIS_ISP_CONFIG(n));

	/* WDMA VC0: legacy storage, 16 bits per pixel */
	writel(0, c->wdma + DMA_VC_CTRL);
	writel(0, c->wdma + DMA_CMN_OTF_FORMAT);
	writel(0, c->wdma + DMA_CMN_DATA_CTRL);
	writel(DMA_INT_MASK_VAL, c->wdma + DMA_CMN_INT_MASK);
	writel(readl(c->wdma + DMA_CMN_INT_SRC), c->wdma + DMA_CMN_INT_SRC);
	writel(0, c->wdma + DMA_VC_FMT);
	writel(w, c->wdma + DMA_VC_RESOL);
	writel(1, c->wdma + DMA_VC_FCNTSEQ);
	csis_dma_set(c, c->next);

	writel(6, c->dma_top + DMA_TOP_ARB_PRI);
	writel(readl(c->dma_top + DMA_TOP_CTRL) | DMA_TOP_IP_PROCESSING, c->dma_top + DMA_TOP_CTRL);
	writel((0x480 << 16) | 0x480, c->dma_top + DMA_TOP_SRAM_SPLIT);
	writel(0, c->dma_top + DMA_TOP_MATRIX);
	writel(readl(c->dma_top + DMA_TOP_TEST_PATTERN) & ~BIT(31), c->dma_top + DMA_TOP_TEST_PATTERN);

	rmw(c->csis + CSIS_CMN_CTRL, CMN_CTRL_UPDATE_SHADOW, CMN_CTRL_UPDATE_SHADOW);
	rmw(c->csis + CSIS_CMN_CTRL, CMN_CTRL_PHY_SEL, 0);		/* D-PHY */
	rmw(c->csis + CSIS_PHY_CMN_CTRL, PHY_CMN_ENABLE_CLK, PHY_CMN_ENABLE_CLK);
	rmw(c->csis + CSIS_CMN_CTRL, CMN_CTRL_CSI_EN, CMN_CTRL_CSI_EN);
}

static void csis_hw_stop(struct csis *c)
{
	rmw(c->csis + CSIS_CMN_CTRL, CMN_CTRL_CSI_EN, 0);
	writel(0, c->wdma + DMA_VC_CTRL);
	writel(0, c->wdma + DMA_CMN_INT_MASK);
	writel(0, c->csis + CSIS_INT_MSK0);
	writel(0, c->csis + CSIS_INT_MSK1);
	usleep_range(20000, 25000);	/* let a frame in flight end */
	writel(0, c->csis + CSIS_PHY_CMN_CTRL);
	rmw(c->sysreg + SYSREG_PHY_RESET, BIT(c->reset_bit), 0);
}

static irqreturn_t csis_dma_irq(int irq, void *data)
{
	struct csis *c = data;
	u32 src = readl(c->wdma + DMA_CMN_INT_SRC);
	struct csis_buffer *b;

	writel(src, c->wdma + DMA_CMN_INT_SRC);

	spin_lock(&c->slock);
	if (!c->streaming)
		goto out;

	if (src & (DMA_INT_ERROR | DMA_INT_OVERLAP | DMA_INT_FRAME_DROP)) {
		c->err_dma |= src;
		c->errors++;
	}
	if (src & DMA_INT_FRM_END) {
		b = c->active;
		c->active = NULL;
		if (b) {
			b->vb.vb2_buf.timestamp = c->active_ts;
			b->vb.sequence = c->active_seq;
			b->vb.field = V4L2_FIELD_NONE;
			vb2_buffer_done(&b->vb.vb2_buf, VB2_BUF_STATE_DONE);
		}
	}
out:
	spin_unlock(&c->slock);
	return IRQ_HANDLED;
}

/* the frame has started into c->next (latched): program the one after it */
static void csis_frame_start(struct csis *c)
{
	if (c->active) {
		/* no end seen for it: give it back as broken */
		c->active->vb.sequence = c->active_seq;
		vb2_buffer_done(&c->active->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	c->active = c->next;
	c->active_ts = ktime_get_ns();
	c->active_seq = c->sequence++;
	c->next = list_first_entry_or_null(&c->queued, struct csis_buffer, list);
	if (c->next)
		list_del(&c->next->list);
	csis_dma_set(c, c->next);
}

static irqreturn_t csis_irq(int irq, void *data)
{
	struct csis *c = data;
	u32 src0 = readl(c->csis + CSIS_INT_SRC0);
	u32 src1 = readl(c->csis + CSIS_INT_SRC1);
	u32 err0 = src0 & INT0_ERRORS, err1 = src1 & CSIS_INT_MSK1_VAL;

	writel(src0, c->csis + CSIS_INT_SRC0);
	writel(src1, c->csis + CSIS_INT_SRC1);
	spin_lock(&c->slock);
	if (c->streaming && (src0 & INT0_FRAMESTART_VC0))
		csis_frame_start(c);
	c->err_csis0 |= err0;
	c->err_csis1 |= err1;
	if (err0 || err1)
		c->errors++;
	spin_unlock(&c->slock);
	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------------
 * vb2
 */

static int csis_queue_setup(struct vb2_queue *q, unsigned int *nbuffers, unsigned int *nplanes,
			    unsigned int sizes[], struct device *alloc_devs[])
{
	struct csis *c = vb2_get_drv_priv(q);

	if (*nplanes)
		return sizes[0] < c->fmt.sizeimage ? -EINVAL : 0;
	*nplanes = 1;
	sizes[0] = c->fmt.sizeimage;
	return 0;
}

static int csis_buf_prepare(struct vb2_buffer *vb)
{
	struct csis *c = vb2_get_drv_priv(vb->vb2_queue);

	if (vb2_plane_size(vb, 0) < c->fmt.sizeimage)
		return -EINVAL;
	/* the DMA takes 32-bit addresses */
	if (upper_32_bits(vb2_dma_contig_plane_dma_addr(vb, 0)))
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, c->fmt.sizeimage);
	return 0;
}

static void csis_buf_queue(struct vb2_buffer *vb)
{
	struct csis *c = vb2_get_drv_priv(vb->vb2_queue);
	struct csis_buffer *b = to_csis_buffer(to_vb2_v4l2_buffer(vb));
	unsigned long flags;

	spin_lock_irqsave(&c->slock, flags);
	list_add_tail(&b->list, &c->queued);
	spin_unlock_irqrestore(&c->slock, flags);
}

static void csis_return_buffers(struct csis *c, enum vb2_buffer_state state)
{
	struct csis_buffer *b, *t;
	unsigned long flags;

	spin_lock_irqsave(&c->slock, flags);
	if (c->active)
		vb2_buffer_done(&c->active->vb.vb2_buf, state);
	if (c->next)
		vb2_buffer_done(&c->next->vb.vb2_buf, state);
	c->active = c->next = NULL;
	list_for_each_entry_safe(b, t, &c->queued, list) {
		list_del(&b->list);
		vb2_buffer_done(&b->vb.vb2_buf, state);
	}
	spin_unlock_irqrestore(&c->slock, flags);
}

static int csis_check_format(struct csis *c)
{
	struct v4l2_subdev_format sfmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.pad = c->sensor_pad,
	};
	int ret;

	ret = v4l2_subdev_call_state_active(c->sensor, pad, get_fmt, &sfmt);
	if (ret)
		return ret;
	if (sfmt.format.width != c->fmt.width || sfmt.format.height != c->fmt.height ||
	    sfmt.format.code != c->cfmt->code) {
		dev_dbg(c->dev, "sensor %ux%u %#x, video node %ux%u %#x\n",
			sfmt.format.width, sfmt.format.height, sfmt.format.code,
			c->fmt.width, c->fmt.height, c->cfmt->code);
		return -EPIPE;
	}
	return 0;
}

static int csis_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct csis *c = vb2_get_drv_priv(q);
	s64 link_freq;
	unsigned long flags;
	u32 mbps;
	int ret;

	ret = csis_check_format(c);
	if (ret)
		goto err_return;

	link_freq = v4l2_get_link_freq(&c->sensor->entity.pads[c->sensor_pad], 10, c->lanes * 2);
	if (link_freq < 0) {
		ret = link_freq;
		goto err_return;
	}
	mbps = div_u64(link_freq * 2, 1000000);
	if (mbps < 1500 || mbps >= 3000) {
		dev_err(c->dev, "%u Mbps per lane is not supported\n", mbps);
		ret = -EINVAL;
		goto err_return;
	}

	ret = video_device_pipeline_start(&c->vdev, &c->pipe);
	if (ret)
		goto err_return;

	ret = pm_runtime_resume_and_get(c->dev);
	if (ret)
		goto err_pipeline;

	spin_lock_irqsave(&c->slock, flags);
	c->sequence = 0;
	c->errors = c->err_csis0 = c->err_csis1 = c->err_dma = 0;
	c->active = NULL;
	c->next = list_first_entry_or_null(&c->queued, struct csis_buffer, list);
	if (c->next)
		list_del(&c->next->list);
	c->streaming = true;
	spin_unlock_irqrestore(&c->slock, flags);

	csis_hw_start(c, mbps);

	ret = v4l2_subdev_call(c->sensor, video, s_stream, 1);
	if (ret)
		goto err_hw;
	dev_dbg(c->dev, "streaming %ux%u at %u Mbps, settle %u\n", c->fmt.width, c->fmt.height,
		mbps, csis_settle(mbps));
	return 0;

err_hw:
	spin_lock_irqsave(&c->slock, flags);
	c->streaming = false;
	spin_unlock_irqrestore(&c->slock, flags);
	csis_hw_stop(c);
	pm_runtime_put(c->dev);
err_pipeline:
	video_device_pipeline_stop(&c->vdev);
err_return:
	csis_return_buffers(c, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void csis_stop_streaming(struct vb2_queue *q)
{
	struct csis *c = vb2_get_drv_priv(q);
	unsigned long flags;

	v4l2_subdev_call(c->sensor, video, s_stream, 0);
	spin_lock_irqsave(&c->slock, flags);
	c->streaming = false;
	spin_unlock_irqrestore(&c->slock, flags);
	csis_hw_stop(c);
	if (c->errors)
		dev_info(c->dev, "%u frames, %u error interrupts (CSIS %#x %#x, DMA %#x)\n",
			 c->sequence, c->errors, c->err_csis0, c->err_csis1, c->err_dma);
	pm_runtime_put(c->dev);
	video_device_pipeline_stop(&c->vdev);
	csis_return_buffers(c, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops csis_vb2_ops = {
	.queue_setup = csis_queue_setup,
	.buf_prepare = csis_buf_prepare,
	.buf_queue = csis_buf_queue,
	.start_streaming = csis_start_streaming,
	.stop_streaming = csis_stop_streaming,
};

/* ------------------------------------------------------------------------
 * Video node
 */

static int csis_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	struct csis *c = video_drvdata(file);

	strscpy(cap->driver, CSIS_DRV_NAME, sizeof(cap->driver));
	snprintf(cap->card, sizeof(cap->card), "Exynos9820 CSIS%u", c->id);
	return 0;
}

static int csis_enum_fmt(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	unsigned int i, n = 0;

	for (i = 0; i < ARRAY_SIZE(csis_formats); i++) {
		if (f->mbus_code && csis_formats[i].code != f->mbus_code)
			continue;
		if (n++ == f->index) {
			f->pixelformat = csis_formats[i].fourcc;
			return 0;
		}
	}
	return -EINVAL;
}

static int csis_enum_framesizes(struct file *file, void *fh, struct v4l2_frmsizeenum *fsize)
{
	if (fsize->index || !csis_find_format(fsize->pixel_format))
		return -EINVAL;
	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise.min_width = CSIS_MIN_SIZE;
	fsize->stepwise.max_width = CSIS_MAX_WIDTH;
	fsize->stepwise.step_width = 8;
	fsize->stepwise.min_height = CSIS_MIN_SIZE;
	fsize->stepwise.max_height = CSIS_MAX_HEIGHT;
	fsize->stepwise.step_height = 2;
	return 0;
}

static const struct csis_format *csis_try_fmt(struct v4l2_pix_format *pix)
{
	const struct csis_format *cf = csis_find_format(pix->pixelformat);

	if (!cf)
		cf = &csis_formats[0];
	pix->pixelformat = cf->fourcc;
	pix->width = clamp_t(u32, ALIGN(pix->width, 8), CSIS_MIN_SIZE, CSIS_MAX_WIDTH);
	pix->height = clamp_t(u32, ALIGN(pix->height, 2), CSIS_MIN_SIZE, CSIS_MAX_HEIGHT);
	/* the DMA writes lines back to back */
	pix->bytesperline = pix->width * 2;
	pix->sizeimage = pix->bytesperline * pix->height;
	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = V4L2_COLORSPACE_RAW;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_601;
	pix->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	pix->xfer_func = V4L2_XFER_FUNC_NONE;
	return cf;
}

static int csis_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct csis *c = video_drvdata(file);

	f->fmt.pix = c->fmt;
	return 0;
}

static int csis_try_fmt_vid_cap(struct file *file, void *fh, struct v4l2_format *f)
{
	csis_try_fmt(&f->fmt.pix);
	return 0;
}

static int csis_s_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct csis *c = video_drvdata(file);

	if (vb2_is_busy(&c->queue))
		return -EBUSY;
	c->cfmt = csis_try_fmt(&f->fmt.pix);
	c->fmt = f->fmt.pix;
	return 0;
}

static const struct v4l2_ioctl_ops csis_ioctl_ops = {
	.vidioc_querycap = csis_querycap,
	.vidioc_enum_fmt_vid_cap = csis_enum_fmt,
	.vidioc_enum_framesizes = csis_enum_framesizes,
	.vidioc_g_fmt_vid_cap = csis_g_fmt,
	.vidioc_s_fmt_vid_cap = csis_s_fmt,
	.vidioc_try_fmt_vid_cap = csis_try_fmt_vid_cap,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations csis_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.unlocked_ioctl = video_ioctl2,
	.mmap = vb2_fop_mmap,
	.poll = vb2_fop_poll,
};

static int csis_link_validate(struct media_link *link)
{
	return 0;	/* checked against the sensor at stream on */
}

static const struct media_entity_operations csis_vdev_entity_ops = {
	.link_validate = csis_link_validate,
};

static int csis_register_video(struct csis *c)
{
	struct video_device *vdev = &c->vdev;
	struct vb2_queue *q = &c->queue;
	struct v4l2_format f = { .fmt.pix = {
		.width = 1920, .height = 1080, .pixelformat = V4L2_PIX_FMT_SGRBG10,
	} };
	int ret;

	/* a sensor bound again: the node of the last bind was unregistered (csis_notify_unbind), start clean */
	memset(vdev, 0, sizeof(*vdev));
	memset(q, 0, sizeof(*q));

	c->cfmt = csis_try_fmt(&f.fmt.pix);
	c->fmt = f.fmt.pix;

	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_DMABUF;
	q->lock = &c->lock;
	q->drv_priv = c;
	q->buf_struct_size = sizeof(struct csis_buffer);
	q->ops = &csis_vb2_ops;
	q->mem_ops = &vb2_dma_contig_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->min_queued_buffers = 1;
	q->dev = c->dev;
	ret = vb2_queue_init(q);
	if (ret)
		return ret;

	snprintf(vdev->name, sizeof(vdev->name), "exynos9820-csis%u", c->id);
	vdev->fops = &csis_fops;
	vdev->ioctl_ops = &csis_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->v4l2_dev = &c->v4l2_dev;
	vdev->queue = q;
	vdev->lock = &c->lock;
	vdev->vfl_dir = VFL_DIR_RX;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_IO_MC;
	vdev->entity.ops = &csis_vdev_entity_ops;
	video_set_drvdata(vdev, c);

	c->vdev_pad.flags = MEDIA_PAD_FL_SINK | MEDIA_PAD_FL_MUST_CONNECT;
	ret = media_entity_pads_init(&vdev->entity, 1, &c->vdev_pad);
	if (ret)
		return ret;

	return video_register_device(vdev, VFL_TYPE_VIDEO, -1);
}

/* ------------------------------------------------------------------------
 * Async: the sensor
 */

static int csis_notify_bound(struct v4l2_async_notifier *n, struct v4l2_subdev *sd,
			     struct v4l2_async_connection *asd)
{
	struct csis *c = container_of(n, struct csis, notifier);
	int pad;

	pad = media_entity_get_fwnode_pad(&sd->entity, asd->match.fwnode, MEDIA_PAD_FL_SOURCE);
	if (pad < 0)
		return pad;
	c->sensor = sd;
	c->sensor_pad = pad;
	return 0;
}

static int csis_notify_complete(struct v4l2_async_notifier *n)
{
	struct csis *c = container_of(n, struct csis, notifier);
	int ret;

	ret = csis_register_video(c);
	if (ret)
		return ret;
	ret = media_create_pad_link(&c->sensor->entity, c->sensor_pad, &c->vdev.entity, 0,
				    MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
	if (ret)
		goto err_video;
	ret = v4l2_device_register_subdev_nodes(&c->v4l2_dev);
	if (ret)
		goto err_video;
	if (!media_devnode_is_registered(c->mdev.devnode)) {
		ret = media_device_register(&c->mdev);
		if (ret)
			goto err_video;
	}
	dev_info(c->dev, "%s on %u lanes\n", c->sensor->name, c->lanes);
	return 0;

err_video:
	video_unregister_device(&c->vdev);
	return ret;
}

/*
 * The sensor going away (its driver unbound or reloaded): the video node goes with it, so that the next bind
 * registers a new one. The media device stays; the sensor's entity and link leave it with the subdev.
 */
static void csis_notify_unbind(struct v4l2_async_notifier *n, struct v4l2_subdev *sd,
			       struct v4l2_async_connection *asd)
{
	struct csis *c = container_of(n, struct csis, notifier);

	if (video_is_registered(&c->vdev))
		vb2_video_unregister_device(&c->vdev);
	c->sensor = NULL;
}

static const struct v4l2_async_notifier_operations csis_notify_ops = {
	.bound = csis_notify_bound,
	.complete = csis_notify_complete,
	.unbind = csis_notify_unbind,
};

static int csis_parse_endpoint(struct csis *c)
{
	struct v4l2_fwnode_endpoint vep = { .bus_type = V4L2_MBUS_CSI2_DPHY };
	struct v4l2_async_connection *asd;
	struct fwnode_handle *ep;
	int ret;

	v4l2_async_nf_init(&c->notifier, &c->v4l2_dev);

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(c->dev), 0, 0, FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!ep)
		return dev_err_probe(c->dev, -ENODEV, "no endpoint\n");
	ret = v4l2_fwnode_endpoint_parse(ep, &vep);
	if (ret) {
		fwnode_handle_put(ep);
		return dev_err_probe(c->dev, ret, "bad endpoint\n");
	}
	c->lanes = vep.bus.mipi_csi2.num_data_lanes;
	if (c->lanes < 1 || c->lanes > 4) {
		fwnode_handle_put(ep);
		return dev_err_probe(c->dev, -EINVAL, "%u lanes\n", c->lanes);
	}
	asd = v4l2_async_nf_add_fwnode_remote(&c->notifier, ep, struct v4l2_async_connection);
	fwnode_handle_put(ep);
	if (IS_ERR(asd))
		return PTR_ERR(asd);
	c->notifier.ops = &csis_notify_ops;
	return 0;
}

/* ------------------------------------------------------------------------
 * Platform device
 */

static void __iomem *csis_map(struct platform_device *pdev, const char *name)
{
	struct resource *r = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);

	if (!r)
		return IOMEM_ERR_PTR(-ENODEV);
	/* not requested: the DMA top, SYSREG and PMU are shared */
	return devm_ioremap(&pdev->dev, r->start, resource_size(r)) ? : IOMEM_ERR_PTR(-ENOMEM);
}

static int csis_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct csis *c;
	int irq, ret;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->dev = dev;
	mutex_init(&c->lock);
	spin_lock_init(&c->slock);
	INIT_LIST_HEAD(&c->queued);

	if (of_property_read_u32(dev->of_node, "samsung,csis-id", &c->id) || c->id > 3 ||
	    of_property_read_u32(dev->of_node, "samsung,phy-reset-bit", &c->reset_bit))
		return dev_err_probe(dev, -EINVAL, "samsung,csis-id / samsung,phy-reset-bit\n");
	c->dc_combo = of_property_read_bool(dev->of_node, "samsung,dc-combo-phy");

	c->csis = devm_platform_ioremap_resource_byname(pdev, "csis");
	if (IS_ERR(c->csis))
		return PTR_ERR(c->csis);
	c->wdma = devm_platform_ioremap_resource_byname(pdev, "wdma");
	if (IS_ERR(c->wdma))
		return PTR_ERR(c->wdma);
	c->phy = csis_map(pdev, "phy");
	c->dma_top = csis_map(pdev, "dma-top");
	c->sysreg = csis_map(pdev, "sysreg");
	c->pmu_iso = csis_map(pdev, "pmu-iso");
	if (IS_ERR(c->phy) || IS_ERR(c->dma_top) || IS_ERR(c->sysreg) || IS_ERR(c->pmu_iso))
		return dev_err_probe(dev, -ENODEV, "phy / dma-top / sysreg / pmu-iso\n");

	c->n_clks = devm_clk_bulk_get_all(dev, &c->clks);
	if (c->n_clks < 0)
		return dev_err_probe(dev, c->n_clks, "clocks\n");

	irq = platform_get_irq_byname(pdev, "csis");
	if (irq < 0)
		return irq;
	ret = devm_request_irq(dev, irq, csis_irq, 0, dev_name(dev), c);
	if (ret)
		return ret;
	irq = platform_get_irq_byname(pdev, "dma");
	if (irq < 0)
		return irq;
	ret = devm_request_irq(dev, irq, csis_dma_irq, 0, dev_name(dev), c);
	if (ret)
		return ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	c->mdev.dev = dev;
	strscpy(c->mdev.model, "Exynos9820 CSIS", sizeof(c->mdev.model));
	snprintf(c->mdev.bus_info, sizeof(c->mdev.bus_info), "platform:%s", dev_name(dev));
	media_device_init(&c->mdev);
	c->v4l2_dev.mdev = &c->mdev;
	ret = v4l2_device_register(dev, &c->v4l2_dev);
	if (ret)
		goto err_mdev;

	platform_set_drvdata(pdev, c);
	pm_runtime_enable(dev);

	ret = csis_parse_endpoint(c);
	if (ret)
		goto err_v4l2;
	ret = v4l2_async_nf_register(&c->notifier);
	if (ret)
		goto err_nf;
	return 0;

err_nf:
	v4l2_async_nf_cleanup(&c->notifier);
err_v4l2:
	pm_runtime_disable(dev);
	v4l2_device_unregister(&c->v4l2_dev);
err_mdev:
	media_device_cleanup(&c->mdev);
	return ret;
}

static void csis_remove(struct platform_device *pdev)
{
	struct csis *c = platform_get_drvdata(pdev);

	v4l2_async_nf_unregister(&c->notifier);
	v4l2_async_nf_cleanup(&c->notifier);
	media_device_unregister(&c->mdev);
	if (video_is_registered(&c->vdev))
		vb2_video_unregister_device(&c->vdev);
	v4l2_device_unregister(&c->v4l2_dev);
	media_device_cleanup(&c->mdev);
	pm_runtime_disable(&pdev->dev);
}

static int csis_runtime_suspend(struct device *dev)
{
	struct csis *c = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(c->n_clks, c->clks);
	return 0;
}

static int csis_runtime_resume(struct device *dev)
{
	struct csis *c = dev_get_drvdata(dev);

	return clk_bulk_prepare_enable(c->n_clks, c->clks);
}

static const struct dev_pm_ops csis_pm_ops = {
	RUNTIME_PM_OPS(csis_runtime_suspend, csis_runtime_resume, NULL)
};

static const struct of_device_id csis_of_match[] = {
	{ .compatible = "samsung,exynos9820-csis" },
	{ }
};
MODULE_DEVICE_TABLE(of, csis_of_match);

static struct platform_driver csis_driver = {
	.probe = csis_probe,
	.remove = csis_remove,
	.driver = {
		.name = CSIS_DRV_NAME,
		.of_match_table = csis_of_match,
		.pm = pm_ptr(&csis_pm_ops),
	},
};
module_platform_driver(csis_driver);

MODULE_DESCRIPTION("Exynos 9820 MIPI CSI-2 receiver and write DMA");
MODULE_LICENSE("GPL");
