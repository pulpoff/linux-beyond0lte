// SPDX-License-Identifier: GPL-2.0
//
// Exynos 9820 ABOX audio block, driven without its CA32 firmware.
//
// ABOX normally runs Samsung's "calliope" firmware on two internal CA32 cores
// and the kernel only talks to it over IPC. The data path itself (RDMA ->
// SPUS -> SIFS -> UAIF) is plain memory-mapped hardware, so this driver keeps
// the CA32 cores off and programs that path from the AP: RDMA0 reads a DMA32
// buffer and feeds UAIF1, the TDM bus to the CS35L40 speaker/earpiece amps.
// Capture is the mirror image: UAIF0 (I2S from the CS47L93 codec) -> NSRC0 ->
// SIFM0 -> WDMA1, the same route the stock firmware sets up to record.
//
// The AUD power domain loses all CMU_AUD and ABOX state when it powers down,
// so everything is (re)programmed on runtime resume. SYSREG_AUD is never
// touched: any access to it kills the SoC on this phone.

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/sched/clock.h>
#include <sound/dmaengine_pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

/* CMU_AUD */
#define CMU_PLL_LOCKTIME_AUD0		0x0000
#define CMU_PLL_CON0_AUD0		0x0120
#define CMU_PLL_CON3_AUD0		0x012c
#define CMU_PLL_ENABLE			BIT(31)
#define CMU_PLL_STABLE			BIT(29)
#define CMU_PLL_MUX_SEL			BIT(4)
#define CMU_DIV_AUDIF			0x1800
#define CMU_DIV_BUS			0x1804
#define CMU_DIV_BUSP			0x1808
#define CMU_DIV_CNT			0x180c
#define CMU_DIV_UAIF(x)			(0x182c + 4 * (x))
#define CMU_DIV_BUSY			BIT(16)
#define CMU_MUX_UAIF(x)			(0x100c + 4 * (x))

/*
 * PLL_AUD0 at 1179.648 MHz (the 48 kHz family), the values S-Boot uses:
 * 26 MHz * (45 + 24319 / 65536) / 1.
 */
#define AUD_PLL_RATE			1179648000UL
#define AUD_PLL_CON0			(CMU_PLL_ENABLE | (45 << 16) | (1 << 8))
#define AUD_PLL_K			0x5eff
#define AUD_AUDIF_RATE			(AUD_PLL_RATE / 16)

/* ABOX SFR */
#define ABOX_IP_INDEX			0x0000
#define ABOX_VERSION			0x0004
#define ABOX_SYSPOWER_CTRL		0x0010
#define ABOX_SYSPOWER_STATUS		0x0014
#define ABOX_QCHANNEL_DISABLE		0x0038
#define ABOX_QCH_BCLK_UAIF(x)		BIT(2 + (x))
#define ABOX_ROUTE_CTRL0		0x0040
#define ABOX_ROUTE_UAIF_SPK(x)		(0xf << (4 * (x)))
#define ABOX_ROUTE_SIFS1		2
#define ABOX_ROUTE_CTRL1		0x0044
#define ABOX_ROUTE_NSRC(x)		(0xf << (4 * (x)))
#define ABOX_ROUTE_NSRC_UAIF(x)		(8 + (x))
#define ABOX_SPUS_CTRL0			0x0200
#define ABOX_SPUS_CTRL1			0x0204
#define ABOX_SBANK_RDMA(x)		(0x0220 + 4 * (x))
#define ABOX_SBANK_MIXP			0x0280
#define ABOX_SIFS_CNT(x)		((x) == 4 ? 0x028c : 0x0290 + 4 * (x))
#define ABOX_SPUM_CTRL0			0x0300
#define ABOX_SPUM_NSRC_CHAIN(x)		(0xf << (4 + 4 * (x)))	/* OUT: 0 = WDMA, ASRC off */
#define ABOX_SBANK_NSRC(x)		(0x0328 + 4 * (x))

#define ABOX_UAIF_CTRL0(x)		(0x0500 + 0x10 * (x))
#define ABOX_UAIF_SPK_ENABLE		BIT(0)
#define ABOX_UAIF_MIC_ENABLE		BIT(1)
#define ABOX_UAIF_MODE_PROVIDER		BIT(2)
#define ABOX_UAIF_CTRL0_RESET		0x01000010
#define ABOX_UAIF_CTRL1(x)		(0x0504 + 0x10 * (x))
#define ABOX_UAIF_FORMAT		GENMASK(28, 24)
#define ABOX_UAIF_BCLK_POLARITY		BIT(23)
#define ABOX_UAIF_WS_MODE		BIT(22)
#define ABOX_UAIF_WS_POLAR		BIT(21)
#define ABOX_UAIF_SLOT_MAX		GENMASK(20, 18)
#define ABOX_UAIF_SBIT_MAX		GENMASK(17, 12)
#define ABOX_UAIF_VALID_STR		GENMASK(11, 6)
#define ABOX_UAIF_VALID_END		GENMASK(5, 0)
#define ABOX_UAIF_STATUS(x)		(0x050c + 0x10 * (x))

/* RDMA (playback) and WDMA (capture) have the same register layout */
#define ABOX_RDMA(x)			(0x1000 + 0x100 * (x))
#define ABOX_WDMA(x)			(0x3000 + 0x100 * (x))
#define ABOX_DMA_CTRL0			0x00
#define ABOX_DMA_ENABLE		BIT(0)
#define ABOX_DMA_CTRL0_B5		BIT(5)	/* set by the stock firmware, meaning unknown */
#define ABOX_DMA_FORMAT		GENMASK(12, 8)	/* same encoding as UAIF FORMAT */
#define ABOX_DMA_BURST_LEN		GENMASK(22, 19)
#define ABOX_DMA_CTRL1			0x04
#define ABOX_DMA_BUF_STR		0x08
#define ABOX_DMA_BUF_END		0x0c
#define ABOX_DMA_BUF_OFFSET		0x10
#define ABOX_DMA_STR_POINT		0x14
#define ABOX_DMA_VOL_FACTOR		0x18
#define ABOX_DMA_VOL_CHANGE		0x1c
#define ABOX_DMA_STATUS		0x30
#define ABOX_DMA_PROGRESS		BIT(31)
#define ABOX_DMA_RBUF_OFFSET		GENMASK(27, 12)
#define ABOX_DMA_RBUF_CNT		GENMASK(11, 0)

#define ABOX_IP_INDEX_VAL		0x41424f58	/* "ABOX" */

/* PMU: AUD pads are held in retention after the domain powers on */
#define PMU_PAD_RETENTION_AUD		0x1920
#define PMU_PAD_RETENTION_RELEASE	BIT(11)

#define ABOX_UAIF_MIC			0	/* UAIF0: I2S to the codec AIF1 */
#define ABOX_UAIF_SPK			1	/* UAIF1: the amp TDM bus */
#define ABOX_NUM_UAIF			2
#define ABOX_RDMA_SPK			0
#define ABOX_WDMA_MIC			1	/* hard-wired to SIFM0 */

#define ABOX_BUFFER_BYTES		(256 * 1024)

/*
 * ABOX addresses behind SysMMU_AUD, as the calliope firmware is linked
 * (stock abox.h). Below 0x80000000 is SRAM, not the IOMMU.
 */
#define IOVA_FW_DRAM			0x80000000
#define IOVA_RDMA_BUF(x)		(0x91000000 + SZ_1M * (x))
#define IOVA_WDMA_BUF(x)		(0x92000000 + SZ_1M * (x))
#define IOVA_VSS			0xa0000000
#define IOVA_VSS_PARAM			0xa1000000
#define IOVA_PRIVATE			0xe0000000
#define IOVA_CHIPID			0x10000000
#define FW_DRAM_SIZE			0x1c00000	/* CONFIG_..._ABOX_DRAM_SIZE */
#define FW_PRIVATE_SIZE			SZ_8M
/* inside the modem_if carve-out (stock shm_ipc vss/vparam) */
#define VSS_PHYS			0xf7800000
#define VSS_SIZE			SZ_8M
#define VSS_PARAM_PHYS			0xfb700000
#define VSS_PARAM_SIZE			SZ_2M

struct abox {
	struct device *dev;
	void __iomem *sfr;
	void __iomem *cmu;
	struct regmap *pmu;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins_active;
	struct pinctrl_state *pins_idle;

	/* indexed by SNDRV_PCM_STREAM_PLAYBACK (RDMA0) / _CAPTURE (WDMA1) */
	struct abox_dma {
		struct abox *abox;
		u32 base;
		struct snd_pcm_substream *substream;
		struct hrtimer timer;
		ktime_t period;
		unsigned int sw_pos;	/* bytes, used when the DMA reports nothing */
		u32 format;
		u32 iova;
		size_t mapped;		/* bytes mapped at iova, 0 = none */
	} dma[2];

	unsigned int fmt[ABOX_NUM_UAIF];

	/* SysMMU_AUD domain; NULL keeps the IOMMU in bypass (old DT) */
	struct iommu_domain *domain;

	/* calliope firmware mode */
	void __iomem *sram;
	resource_size_t sram_size;
	void __iomem *timer;
	void __iomem *sysreg;
	void __iomem *gicd;
	void __iomem *gicc;
	void *fw_mem;			/* firmware DRAM, then the private area */
	phys_addr_t fw_phys;
	int irq;
	struct device *card_dev;	/* allocates DMA buffers (no IOMMU) */
	struct abox_fw {
		bool running;
		bool in_call;
		bool own;	/* debug: our PCMs program their DMAs anyway */
		u32 uaif0_div;	/* our CMU UAIF0 divider, put back at hangup */
		u8 ack_rdma;	/* RDMAs the firmware wants PLTDAI_ACKs for */
		u32 acks;
		void *call_buf;
		dma_addr_t call_buf_phys;
		struct completion boot_done;
		u32 version;
		spinlock_t tx_lock;
		unsigned int rx_count;
	} fw;
};

static inline u32 abox_readl(struct abox *abox, u32 reg)
{
	return readl(abox->sfr + reg);
}

static inline void abox_writel(struct abox *abox, u32 reg, u32 val)
{
	writel(val, abox->sfr + reg);
}

static void abox_update(struct abox *abox, u32 reg, u32 mask, u32 val)
{
	abox_writel(abox, reg, (abox_readl(abox, reg) & ~mask) | (val & mask));
}

static int abox_cmu_div(struct abox *abox, u32 reg, u32 ratio)
{
	u32 val;

	writel(ratio, abox->cmu + reg);
	return readl_poll_timeout_atomic(abox->cmu + reg, val,
					 !(val & CMU_DIV_BUSY), 1, 1000);
}

static int abox_cmu_init(struct abox *abox)
{
	u32 val;
	int ret;

	/* dividers first, so nothing sees 1.18 GHz when the PLL comes up */
	ret = abox_cmu_div(abox, CMU_DIV_BUS, 6);		/* 168.5 MHz */
	ret = ret ?: abox_cmu_div(abox, CMU_DIV_BUSP, 2);	/* 56.2 MHz */
	ret = ret ?: abox_cmu_div(abox, CMU_DIV_AUDIF, 15);	/* 73.728 MHz */
	ret = ret ?: abox_cmu_div(abox, CMU_DIV_CNT, 3);
	if (ret)
		return ret;

	writel(0x96, abox->cmu + CMU_PLL_LOCKTIME_AUD0);
	writel(AUD_PLL_K, abox->cmu + CMU_PLL_CON3_AUD0);
	writel(AUD_PLL_CON0, abox->cmu + CMU_PLL_CON0_AUD0);
	ret = readl_poll_timeout(abox->cmu + CMU_PLL_CON0_AUD0, val,
				 val & CMU_PLL_STABLE, 10, 10000);
	if (ret) {
		dev_err(abox->dev, "PLL_AUD0 did not lock: 0x%08x\n", val);
		return ret;
	}
	writel(AUD_PLL_CON0 | CMU_PLL_MUX_SEL, abox->cmu + CMU_PLL_CON0_AUD0);

	/* UAIF BCLK from DIV_CLK_AUD_UAIFn, not from the pad */
	writel(0, abox->cmu + CMU_MUX_UAIF(ABOX_UAIF_MIC));
	writel(0, abox->cmu + CMU_MUX_UAIF(ABOX_UAIF_SPK));
	return 0;
}

static int abox_hw_init(struct abox *abox)
{
	u32 val;
	int i, ret;

	ret = abox_cmu_init(abox);
	if (ret)
		return ret;

	val = abox_readl(abox, ABOX_IP_INDEX);
	if (val != ABOX_IP_INDEX_VAL) {
		dev_err(abox->dev, "unexpected IP index 0x%08x\n", val);
		return -ENODEV;
	}

	/*
	 * GPIO_AUD loses its configuration with the domain, so the pins are
	 * set up again here (they are not a "default" state, which pinctrl
	 * would apply once at bind), then released from retention.
	 */
	ret = pinctrl_select_state(abox->pinctrl, abox->pins_active);
	if (ret)
		return ret;
	regmap_update_bits(abox->pmu, PMU_PAD_RETENTION_AUD,
			   PMU_PAD_RETENTION_RELEASE, PMU_PAD_RETENTION_RELEASE);

	abox_writel(abox, ABOX_SYSPOWER_CTRL, 1);
	ret = readl_poll_timeout(abox->sfr + ABOX_SYSPOWER_STATUS, val,
				 val & 1, 10, 10000);
	if (ret) {
		dev_err(abox->dev, "SYSPOWER did not come up\n");
		return ret;
	}

	/* SRAM bank layout downstream writes at init; reset is "no bank" */
	for (i = 0; i < 12; i++)
		abox_writel(abox, ABOX_SBANK_RDMA(i), (512 << 16) | (i * 512));
	abox_writel(abox, ABOX_SBANK_MIXP, (512 << 16) | 0x3000);

	/*
	 * RDMA0 -> SPUS chain 0 -> SIFS1 is the reset routing, set anyway:
	 * chain 0 IN = RDMA, OUT = SIFS1, no ASRC; SIFS1 takes SPUS OUT0.
	 * Then SIFS1 -> UAIF1.
	 */
	abox_update(abox, ABOX_SPUS_CTRL0, GENMASK(4, 0), 0);
	abox_update(abox, ABOX_SPUS_CTRL1, GENMASK(15, 12), 0);
	abox_update(abox, ABOX_ROUTE_CTRL0, ABOX_ROUTE_UAIF_SPK(ABOX_UAIF_SPK),
		    ABOX_ROUTE_SIFS1 << (4 * ABOX_UAIF_SPK));

	/*
	 * Capture: UAIF0 -> NSRC0 -> SIFM0 -> WDMA1. The firmware puts an
	 * ASRC in NSRC0; both sides run at 48 kHz here, so it is bypassed.
	 */
	abox_writel(abox, ABOX_SBANK_NSRC(0), (512 << 16) | 0x400);
	abox_update(abox, ABOX_SPUM_CTRL0, ABOX_SPUM_NSRC_CHAIN(0), 0);
	abox_update(abox, ABOX_ROUTE_CTRL1, ABOX_ROUTE_NSRC(0),
		    ABOX_ROUTE_NSRC_UAIF(ABOX_UAIF_MIC));

	return 0;
}

static int abox_runtime_resume(struct device *dev)
{
	struct abox *abox = dev_get_drvdata(dev);

	return abox_hw_init(abox);
}

static int abox_runtime_suspend(struct device *dev)
{
	struct abox *abox = dev_get_drvdata(dev);

	pinctrl_select_state(abox->pinctrl, abox->pins_idle);
	abox_writel(abox, ABOX_SYSPOWER_CTRL, 0);
	return 0;
}

/* UAIF DAIs: UAIF0 (codec, capture) and UAIF1 (amps, playback) */

static int abox_uaif_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct abox *abox = snd_soc_dai_get_drvdata(dai);

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_DSP_A:
	case SND_SOC_DAIFMT_I2S:
		break;
	default:
		return -EINVAL;
	}

	/* ABOX provides BCLK and the frame sync; codec and amps are consumers */
	switch (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) {
	case SND_SOC_DAIFMT_BP_FP:
		break;
	default:
		return -EINVAL;
	}

	abox->fmt[dai->id] = fmt;
	return 0;
}

static int abox_uaif_hw_params(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params,
			       struct snd_soc_dai *dai)
{
	struct abox *abox = snd_soc_dai_get_drvdata(dai);
	unsigned int id = dai->id;
	unsigned int ch = params_channels(params);
	unsigned int width = params_physical_width(params);
	unsigned long bclk = params_rate(params) * ch * width;
	unsigned int code, ratio;
	u32 ctrl1;
	int ret;

	switch (width) {
	case 16:
		code = 1;
		break;
	case 24:
		code = 2;
		break;
	case 32:
		code = 3;
		break;
	default:
		return -EINVAL;
	}

	if (AUD_AUDIF_RATE % bclk) {
		dev_err(abox->dev, "BCLK %lu Hz not reachable\n", bclk);
		return -EINVAL;
	}
	ratio = AUD_AUDIF_RATE / bclk - 1;
	ret = abox_cmu_div(abox, CMU_DIV_UAIF(id), ratio);
	if (ret)
		return ret;

	ctrl1 = FIELD_PREP(ABOX_UAIF_FORMAT, (ch - 1) | (code << 3)) |
		FIELD_PREP(ABOX_UAIF_SLOT_MAX, ch - 1) |
		FIELD_PREP(ABOX_UAIF_SBIT_MAX, width - 1);
	if ((abox->fmt[id] & SND_SOC_DAIFMT_FORMAT_MASK) == SND_SOC_DAIFMT_DSP_A)
		ctrl1 |= ABOX_UAIF_WS_MODE |
			 FIELD_PREP(ABOX_UAIF_VALID_STR, width - 1) |
			 FIELD_PREP(ABOX_UAIF_VALID_END, width - 1);
	switch (abox->fmt[id] & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		/* downstream maps its board's "bitclock-inversion" to 0 */
		ctrl1 |= ABOX_UAIF_BCLK_POLARITY;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		break;
	default:
		return -EINVAL;
	}

	abox_writel(abox, ABOX_UAIF_CTRL1(id), ctrl1);
	abox_writel(abox, ABOX_UAIF_CTRL0(id),
		    ABOX_UAIF_CTRL0_RESET | ABOX_UAIF_MODE_PROVIDER);
	if (id == ABOX_UAIF_SPK)
		abox_writel(abox, ABOX_SIFS_CNT(1), 0);
	abox_update(abox, ABOX_QCHANNEL_DISABLE, ABOX_QCH_BCLK_UAIF(id),
		    ABOX_QCH_BCLK_UAIF(id));

	dev_dbg(abox->dev, "UAIF%u %u ch x %u bit, BCLK %lu Hz, CTRL1 0x%08x\n",
		id, ch, width, bclk, ctrl1);
	return 0;
}

static int abox_uaif_trigger(struct snd_pcm_substream *substream, int cmd,
			     struct snd_soc_dai *dai)
{
	struct abox *abox = snd_soc_dai_get_drvdata(dai);
	u32 enable = substream->stream == SNDRV_PCM_STREAM_PLAYBACK ?
		     ABOX_UAIF_SPK_ENABLE : ABOX_UAIF_MIC_ENABLE;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		abox_update(abox, ABOX_UAIF_CTRL0(dai->id), enable, enable);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		abox_update(abox, ABOX_UAIF_CTRL0(dai->id), enable, 0);
		return 0;
	}
	return -EINVAL;
}

static int abox_uaif_hw_free(struct snd_pcm_substream *substream,
			     struct snd_soc_dai *dai)
{
	struct abox *abox = snd_soc_dai_get_drvdata(dai);

	abox_update(abox, ABOX_QCHANNEL_DISABLE, ABOX_QCH_BCLK_UAIF(dai->id), 0);
	return 0;
}

static const struct snd_soc_dai_ops abox_uaif_ops = {
	.set_fmt	= abox_uaif_set_fmt,
	.hw_params	= abox_uaif_hw_params,
	.trigger	= abox_uaif_trigger,
	.hw_free	= abox_uaif_hw_free,
};

static struct snd_soc_dai_driver abox_dais[] = {
	{
		.name = "abox-uaif0",
		.id = ABOX_UAIF_MIC,
		.capture = {
			.stream_name = "UAIF0 Capture",
			.channels_min = 2,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				   SNDRV_PCM_FMTBIT_S32_LE,
		},
		.ops = &abox_uaif_ops,
	},
	{
		.name = "abox-uaif1",
		.id = ABOX_UAIF_SPK,
		.playback = {
			.stream_name = "UAIF1 Playback",
			.channels_min = 2,
			.channels_max = 4,
			.rates = SNDRV_PCM_RATE_48000,
			.formats = SNDRV_PCM_FMTBIT_S32_LE,
		},
		.ops = &abox_uaif_ops,
	},
};

/* PCMs: RDMA0 playback, WDMA1 capture */

static const struct snd_pcm_hardware abox_pcm_hw[] = {
	[SNDRV_PCM_STREAM_PLAYBACK] = {
		.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
			SNDRV_PCM_INFO_INTERLEAVED |
			SNDRV_PCM_INFO_BLOCK_TRANSFER,
		.formats = SNDRV_PCM_FMTBIT_S32_LE,
		.rates = SNDRV_PCM_RATE_48000,
		.rate_min = 48000,
		.rate_max = 48000,
		.channels_min = 2,
		.channels_max = 4,
		.buffer_bytes_max = ABOX_BUFFER_BYTES,
		.period_bytes_min = 1024,
		.period_bytes_max = ABOX_BUFFER_BYTES / 2,
		.periods_min = 2,
		.periods_max = 32,
	},
	[SNDRV_PCM_STREAM_CAPTURE] = {
		.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
			SNDRV_PCM_INFO_INTERLEAVED |
			SNDRV_PCM_INFO_BLOCK_TRANSFER,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S32_LE,
		.rates = SNDRV_PCM_RATE_48000,
		.rate_min = 48000,
		.rate_max = 48000,
		.channels_min = 2,
		.channels_max = 2,
		.buffer_bytes_max = ABOX_BUFFER_BYTES,
		.period_bytes_min = 1024,
		.period_bytes_max = ABOX_BUFFER_BYTES / 2,
		.periods_min = 2,
		.periods_max = 32,
	},
};

static inline struct abox_dma *abox_dma(struct snd_soc_component *component,
					struct snd_pcm_substream *substream)
{
	struct abox *abox = snd_soc_component_get_drvdata(component);

	return &abox->dma[substream->stream];
}

static u32 abox_dma_read(struct abox_dma *dma, u32 reg)
{
	return abox_readl(dma->abox, dma->base + reg);
}

static void abox_dma_write(struct abox_dma *dma, u32 reg, u32 val)
{
	abox_writel(dma->abox, dma->base + reg, val);
}

static enum hrtimer_restart abox_period_timer(struct hrtimer *timer)
{
	struct abox_dma *dma = container_of(timer, struct abox_dma, timer);
	struct snd_pcm_substream *substream = READ_ONCE(dma->substream);

	if (!substream)
		return HRTIMER_NORESTART;

	dma->sw_pos += snd_pcm_lib_period_bytes(substream);
	dma->sw_pos %= snd_pcm_lib_buffer_bytes(substream);
	snd_pcm_period_elapsed(substream);
	hrtimer_forward_now(timer, dma->period);
	return HRTIMER_RESTART;
}

static int abox_pcm_open(struct snd_soc_component *component,
			 struct snd_pcm_substream *substream)
{
	struct abox *abox = snd_soc_component_get_drvdata(component);
	int ret;

	ret = pm_runtime_resume_and_get(abox->dev);
	if (ret)
		return ret;

	snd_soc_set_runtime_hwparams(substream, &abox_pcm_hw[substream->stream]);
	/* DMA addresses are 16-byte units in places; keep periods aligned */
	snd_pcm_hw_constraint_step(substream->runtime, 0,
				   SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 64);
	snd_pcm_hw_constraint_step(substream->runtime, 0,
				   SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 64);
	return 0;
}

static int abox_pcm_close(struct snd_soc_component *component,
			  struct snd_pcm_substream *substream)
{
	struct abox *abox = snd_soc_component_get_drvdata(component);

	pm_runtime_mark_last_busy(abox->dev);
	pm_runtime_put_autosuspend(abox->dev);
	return 0;
}

/* the firmware drives the DMAs, unless debugfs "own" handed them back */
static bool abox_fw_owns_dma(struct abox *abox)
{
	return abox->fw.running && !abox->fw.own;
}

static int abox_pcm_hw_params(struct snd_soc_component *component,
			      struct snd_pcm_substream *substream,
			      struct snd_pcm_hw_params *params)
{
	struct abox_dma *dma = abox_dma(component, substream);

	/*
	 * (ch - 1) | (bytes - 1) << 3: stock RDMA7 runs 0x1b for 4 ch x 32 bit,
	 * WDMA1 0x09 for the 2 ch x 16 bit Android records.
	 */
	dma->format = (params_channels(params) - 1) |
		      ((params_physical_width(params) / 8 - 1) << 3);
	dma->period = ns_to_ktime(div_u64((u64)NSEC_PER_SEC *
					  params_period_size(params),
					  params_rate(params)));
	return 0;
}

static int abox_pcm_prepare(struct snd_soc_component *component,
			    struct snd_pcm_substream *substream)
{
	struct abox_dma *dma = abox_dma(component, substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	dma_addr_t addr = runtime->dma_addr;
	size_t bytes = snd_pcm_lib_buffer_bytes(substream);
	int ret;

	dma->sw_pos = 0;
	if (dma->abox->domain) {
		struct iommu_domain *domain = dma->abox->domain;
		/*
		 * Under calliope the firmware programs this DMA itself, with
		 * its own sizes (RDMA0/WDMA1 are the call's VPCMOUT/VPCMIN
		 * links): map the whole preallocated buffer.
		 */
		size_t map = abox_fw_owns_dma(dma->abox) ? substream->dma_buffer.bytes
						   : PAGE_ALIGN(bytes);

		if (dma->mapped)
			iommu_unmap(domain, dma->iova, dma->mapped);
		dma->mapped = 0;
		/* runtime->dma_addr is physical: the card device has no IOMMU */
		ret = iommu_map(domain, dma->iova, addr, map,
				IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
		if (ret)
			return ret;
		dma->mapped = map;
		addr = dma->iova;
	}
	/*
	 * With calliope running the DMAs are the firmware's: the stream only
	 * keeps the codec, amps and UAIFs powered (the hrtimer moves the
	 * pointer). Used for calls.
	 */
	if (abox_fw_owns_dma(dma->abox)) {
		memset(runtime->dma_area, 0, substream->dma_buffer.bytes);
		return 0;
	}

	/*
	 * The firmware normally programs these. Values match what the stock
	 * firmware leaves in RDMA7 while Android plays to the speaker, and in
	 * WDMA1 while it records: END is exclusive, OFFSET is the period
	 * size, STR_POINT the start.
	 */
	abox_dma_write(dma, ABOX_DMA_CTRL0,
		       FIELD_PREP(ABOX_DMA_BURST_LEN, 8) |
		       FIELD_PREP(ABOX_DMA_FORMAT, dma->format) |
		       ABOX_DMA_CTRL0_B5);
	abox_dma_write(dma, ABOX_DMA_BUF_STR, lower_32_bits(addr));
	abox_dma_write(dma, ABOX_DMA_BUF_END, lower_32_bits(addr + bytes));
	abox_dma_write(dma, ABOX_DMA_BUF_OFFSET,
		       snd_pcm_lib_period_bytes(substream));
	abox_dma_write(dma, ABOX_DMA_STR_POINT, lower_32_bits(addr));
	abox_dma_write(dma, ABOX_DMA_VOL_FACTOR, 0x800000);
	dma->sw_pos = 0;
	return 0;
}

static int abox_pcm_trigger(struct snd_soc_component *component,
			    struct snd_pcm_substream *substream, int cmd)
{
	struct abox_dma *dma = abox_dma(component, substream);
	u32 val;

	if (abox_fw_owns_dma(dma->abox)) {
		switch (cmd) {
		case SNDRV_PCM_TRIGGER_START:
		case SNDRV_PCM_TRIGGER_RESUME:
		case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
			WRITE_ONCE(dma->substream, substream);
			hrtimer_start(&dma->timer, dma->period, HRTIMER_MODE_REL);
			return 0;
		default:
			hrtimer_try_to_cancel(&dma->timer);
			WRITE_ONCE(dma->substream, NULL);
			return 0;
		}
	}

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		WRITE_ONCE(dma->substream, substream);
		abox_dma_write(dma, ABOX_DMA_CTRL0,
			       abox_dma_read(dma, ABOX_DMA_CTRL0) |
			       ABOX_DMA_ENABLE);
		hrtimer_start(&dma->timer, dma->period, HRTIMER_MODE_REL);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		hrtimer_try_to_cancel(&dma->timer);
		WRITE_ONCE(dma->substream, NULL);
		abox_dma_write(dma, ABOX_DMA_CTRL0,
			       abox_dma_read(dma, ABOX_DMA_CTRL0) &
			       ~ABOX_DMA_ENABLE);
		/* the DMA drains its burst before PROGRESS drops (<= 40 ms) */
		readl_poll_timeout_atomic(dma->abox->sfr + dma->base +
					  ABOX_DMA_STATUS, val,
					  !(val & ABOX_DMA_PROGRESS), 10, 40000);
		return 0;
	}
	return -EINVAL;
}

static snd_pcm_uframes_t abox_pcm_pointer(struct snd_soc_component *component,
					  struct snd_pcm_substream *substream)
{
	struct abox_dma *dma = abox_dma(component, substream);
	u32 status = abox_dma_read(dma, ABOX_DMA_STATUS);
	unsigned int pos;

	/*
	 * RBUF_OFFSET is the start of the period being transferred and
	 * RBUF_CNT the 16-byte units moved within it so far, so the position
	 * moves smoothly instead of in whole periods (PipeWire xruns on the
	 * jumps). WDMA1 reports the same way: 0x0f/0x1e/0x2d + CNT while
	 * Android records in 0xf00-byte periods.
	 */
	if (!abox_fw_owns_dma(dma->abox) && (status & ABOX_DMA_PROGRESS))
		pos = (FIELD_GET(ABOX_DMA_RBUF_OFFSET, status) +
		       FIELD_GET(ABOX_DMA_RBUF_CNT, status)) << 4;
	else
		pos = dma->sw_pos;

	return bytes_to_frames(substream->runtime,
			       pos % snd_pcm_lib_buffer_bytes(substream));
}

/*
 * Under calliope, RDMA0's buffer carries the call's processed uplink
 * (VPCMOUT): a keep-alive stream's writes are dropped instead of
 * overwriting it.
 */
static int abox_pcm_copy(struct snd_soc_component *component,
			 struct snd_pcm_substream *substream, int channel,
			 unsigned long pos, struct iov_iter *iter,
			 unsigned long bytes)
{
	struct abox_dma *dma = abox_dma(component, substream);
	void *buf = substream->runtime->dma_area + pos;

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		return copy_to_iter(buf, bytes, iter) == bytes ? 0 : -EFAULT;
	if (abox_fw_owns_dma(dma->abox)) {
		iov_iter_advance(iter, bytes);
		return 0;
	}
	return copy_from_iter(buf, bytes, iter) == bytes ? 0 : -EFAULT;
}

static int abox_pcm_new(struct snd_soc_component *component,
			struct snd_soc_pcm_runtime *rtd)
{
	struct abox *abox = snd_soc_component_get_drvdata(component);

	/*
	 * With SysMMU_AUD in use, abox->dev belongs to an unmanaged domain the
	 * DMA API knows nothing about: allocate against the card instead and
	 * map the buffer at the stock IOVA in prepare.
	 */
	abox->card_dev = rtd->card->dev;
	snd_pcm_set_managed_buffer_all(rtd->pcm, SNDRV_DMA_TYPE_DEV,
				       abox->domain ? rtd->card->dev : abox->dev,
				       ABOX_BUFFER_BYTES, ABOX_BUFFER_BYTES);
	return 0;
}

static int abox_pcm_hw_free(struct snd_soc_component *component,
			    struct snd_pcm_substream *substream)
{
	struct abox_dma *dma = abox_dma(component, substream);

	if (dma->mapped)
		iommu_unmap(dma->abox->domain, dma->iova, dma->mapped);
	dma->mapped = 0;
	return 0;
}

/*
 * calliope firmware (stage 1: boot it and see BOOT_DONE). Layout and
 * sequence from the stock driver, kit/callaudio/ABOX-FW-SPEC.md.
 */

#define PMU_AUD_CPU0_POWER		0x2d80
#define PMU_AUD_CPU0_STANDBY		0x2da4	/* 0x11 = in WFI */
#define PMU_AUD_CPU1_POWER		0x2e00
#define PMU_AUD_CPU1_STANDBY		0x2e24
#define SYSREG_AUD_MISC_CON		0x0504
#define ABOX_CORE0_PC			0x507c	/* core0 GPR[31] */
#define TIMER_CTRL1(x)			(0x0604 + 0x20 * (x))
#define TIMER_CURVALUD_LSB(x)		(0x0610 + 0x20 * (x))

#define GICD_CTLR			0x000
#define GICD_IGROUPR(x)			(0x080 + 4 * (x))
#define GICD_ISENABLER(x)		(0x100 + 4 * (x))
#define GICD_IPRIORITYR(x)		(0x400 + 4 * (x))
#define GICD_ITARGETSR(x)		(0x800 + 4 * (x))
#define GICD_SGIR			0xf00
#define GICC_CTLR			0x00
#define GICC_PMR			0x04
#define GICC_IAR			0x0c
#define GICC_EOIR			0x10
#define GICC_DIR			0x1000
#define ABOX_SGI_MSG			15	/* IPC v2 doorbell, both ways */
#define ABOX_IRQ_WDT			94
#define ABOX_GIC_CPU_CORE0		BIT(0)
#define ABOX_GIC_CPU_AP			BIT(2)

/* IPC v2 queues in firmware DRAM (abox_msg.h, packed) */
#define FW_IPC_TX			0x850000	/* AP -> ABOX */
#define FW_IPC_RX			0x854000	/* ABOX -> AP */
#define FW_IPC_SIZE			0x4000
#define Q_CMD_IDX_S			0
#define Q_CMD_IDX_E			4
#define Q_CMD_ELEM			8
#define Q_CMD_ELEM_SIZE			56
#define Q_CMD_LEN			16
#define Q_DATA				(Q_CMD_ELEM + Q_CMD_LEN * Q_CMD_ELEM_SIZE)	/* 904 */
#define Q_DATA_IDX_S			(Q_DATA + 0)
#define Q_DATA_IDX_E			(Q_DATA + 4)
#define Q_DATA_LEN			(Q_DATA + 8)
#define Q_DATA_ELEM			(Q_DATA + 12)
#define Q_DATA_SIZE			(FW_IPC_SIZE - Q_DATA_ELEM)		/* 15468 */

/* ABOX_IPC_MSG: ipcid, task_id, then IPC_SYSTEM_MSG msgtype, param1-3, bundle */
#define IPC_MSG_SIZE			764
#define IPC_SYSTEM			1
#define ABOX_SUSPEND			1
#define ABOX_RESUME			2
#define ABOX_BOOT_DONE			3
#define ABOX_CHANGE_GEAR		4
#define ABOX_CHANGED_GEAR		9
#define ABOX_REPORT_LOG			0x10

static const struct {
	const char *name;
	u32 offset;
	bool sram;
	bool required;
} abox_fw_files[] = {
	{ "calliope_sram.bin", 0x0, true, true },
	{ "calliope_sram_2.bin", 0x32000, true, true },
	{ "calliope_dram.bin", 0x0, false, true },
	{ "calliope_dram_2.bin", 0x280000, false, true },
	{ "calliope2.dt", 0x11fe000 },
	{ "txse1.bin", 0x1200000 },
	{ "rxse.bin", 0x1300000 },
	{ "txse2.bin", 0x13b0000 },
	{ "txse3.bin", 0x1800000 },
	{ "SoundBoosterParam.bin", 0x4fc000 },
	{ "APDV_AUDIO_SLSI.bin", 0x4d0000 },
	{ "APTV_AUDIO_SLSI.bin", 0x7b6000 },
	{ "APSV_AUDIO_SLSI.bin", 0x7bc000 },
	{ "APBargeIn_AUDIO_SLSI.bin", 0x7dc000 },
	{ "APBiBF_AUDIO_SLSI.bin", 0x7df000 },
	{ "AP_AUDIO_SLSI.bin", 0x7e0000 },
};

static inline void *abox_fw_q(struct abox *abox, u32 q)
{
	return abox->fw_mem + q;
}

static inline s32 abox_q_rd(struct abox *abox, u32 q, u32 off)
{
	return READ_ONCE(*(s32 *)(abox_fw_q(abox, q) + off));
}

static inline void abox_q_wr(struct abox *abox, u32 q, u32 off, s32 val)
{
	WRITE_ONCE(*(s32 *)(abox_fw_q(abox, q) + off), val);
}

static void abox_gic_sgi(struct abox *abox, unsigned int sgi)
{
	/* target list: CPU interface 0 = core0 */
	writel((ABOX_GIC_CPU_CORE0 << 16) | (sgi & 0xf), abox->gicd + GICD_SGIR);
}

/* stock abox_msg_send(): one data record = s32 size + payload */
static int abox_fw_send(struct abox *abox, const void *msg, u32 size,
			s32 a0, s32 a1, s32 a2)
{
	const u32 *w = msg;
	u32 need = 4 + size, s, e, idx_e, ce;
	unsigned long flags;
	void *cmd;
	int ret = 0;

	spin_lock_irqsave(&abox->fw.tx_lock, flags);
	s = abox_q_rd(abox, FW_IPC_TX, Q_DATA_IDX_S);
	e = abox_q_rd(abox, FW_IPC_TX, Q_DATA_IDX_E);
	if (s > e) {
		if (s - e < need)
			ret = -EBUSY;
	} else if (Q_DATA_SIZE - e < need) {
		if (s < need)
			ret = -EBUSY;
		else
			e = 0;
	}
	ce = abox_q_rd(abox, FW_IPC_TX, Q_CMD_IDX_E);
	if (!ret && abox_q_rd(abox, FW_IPC_TX, Q_CMD_IDX_S) == (ce + 1) % Q_CMD_LEN)
		ret = -EBUSY;
	if (ret)
		goto out;

	idx_e = e;
	*(s32 *)(abox_fw_q(abox, FW_IPC_TX) + Q_DATA_ELEM + idx_e) = size;
	memcpy(abox_fw_q(abox, FW_IPC_TX) + Q_DATA_ELEM + idx_e + 4, msg, size);
	abox_q_wr(abox, FW_IPC_TX, Q_DATA_IDX_E, (idx_e + need) % Q_DATA_SIZE);

	cmd = abox_fw_q(abox, FW_IPC_TX) + Q_CMD_ELEM + ce * Q_CMD_ELEM_SIZE;
	memset(cmd, 0, Q_CMD_ELEM_SIZE);
	((s32 *)cmd)[0] = w[0];			/* id = ipcid */
	((s32 *)cmd)[1] = w[2];			/* cmd = msgtype */
	((s32 *)cmd)[2] = a0;			/* arg[0..2] */
	((s32 *)cmd)[3] = a1;
	((s32 *)cmd)[4] = a2;
	((u32 *)cmd)[5] = idx_e;		/* data_idx */
	*(u64 *)(cmd + 40) = sched_clock();	/* time_put */
	mb();
	abox_q_wr(abox, FW_IPC_TX, Q_CMD_IDX_E, (ce + 1) % Q_CMD_LEN);
	mb();
	abox_gic_sgi(abox, ABOX_SGI_MSG);
out:
	spin_unlock_irqrestore(&abox->fw.tx_lock, flags);
	if (ret)
		dev_err_ratelimited(abox->dev, "IPC tx queue full\n");
	return ret;
}

static int abox_fw_send_system(struct abox *abox, u32 type, u32 p1, u32 p2,
			       u32 p3, const u64 *bundle, int nbundle)
{
	u32 msg[IPC_MSG_SIZE / 4] = { IPC_SYSTEM, 0, type, p1, p2, p3 };
	int i;

	for (i = 0; i < nbundle; i++)
		memcpy(&msg[6 + 2 * i], &bundle[i], 8);
	return abox_fw_send(abox, msg, sizeof(msg), p1, p2, p3);
}

#define ABOX_SET_MODE			0x50
#define ABOX_SET_TYPE			0x60
#define ABOX_START_VSS			0xa0
#define ABOX_STOP_VSS			0xa1
#define ABOX_UPDATE_COMPONENT_CONTROL	0xc1
#define ABOX_VOID_COMPONENT		0xab0c	/* calliope2.dt controls */
#define AUDIO_MODE_NORMAL		0
#define AUDIO_MODE_IN_CALL		2
#define SOUND_TYPE_VOICE		0
#define IPC_PCMPLAYBACK			2
#define IPC_PCMCAPTURE			3
#define PCM_PLTDAI_OPEN			11
#define PCM_PLTDAI_CLOSE		12
#define PCM_PLTDAI_HW_PARAMS		14
#define PCM_PLTDAI_HW_FREE		15
#define PCM_PLTDAI_PREPARE		16
#define PCM_PLTDAI_TRIGGER		17
#define PCM_PLTDAI_POINTER		18
#define PCM_PLTDAI_ACK			22
#define PCM_SET_BUFFER			20

/* process context only: waits for the firmware to drain a full tx queue */
static int abox_fw_send_wait(struct abox *abox, const u32 *msg, s32 a0, s32 a1,
			     s32 a2)
{
	int i, ret;

	for (i = 0; i < 50; i++) {
		ret = abox_fw_send(abox, msg, IPC_MSG_SIZE, a0, a1, a2);
		if (ret != -EBUSY)
			break;
		usleep_range(1000, 2000);
	}
	return ret;
}

#define IPC_ABOX_CONFIG			0xa
#define SET_ASRC_FACTOR_CP		0x30

/*
 * What stock's abox_cmpnt_restore() sends after every boot: SIF/PIF rates
 * (config 1..14), formats (0x10..0x1d: width, channels), the CP ASRC factor.
 * Values from the stock mixer during a call; UAIF1 (amps) runs 4 slots.
 */
static const u8 abox_sif_width[14] = {
	24, 32, 16, 16,			/* SIFS0, SIFS1, SIFS2, PIFS1 */
	16, 24, 24, 32, 16, 16, 24,	/* SIFM0..6 */
	16, 16, 16,			/* SIFS3, SIFS4, PIFS0 */
};

static int abox_fw_config(struct abox *abox, u32 type, s32 p1, s32 p2)
{
	u32 msg[IPC_MSG_SIZE / 4] = { IPC_ABOX_CONFIG, 0, type, p1, p2 };

	return abox_fw_send_wait(abox, msg, p1, p2, 0);
}

static int abox_fw_restore(struct abox *abox)
{
	int i, ret = 0;

	for (i = 0; !ret && i < 14; i++)
		ret = abox_fw_config(abox, 1 + i, 48000, 0);
	for (i = 0; !ret && i < 14; i++)
		ret = abox_fw_config(abox, 0x10 + i, abox_sif_width[i],
				     i == 1 ? 4 : 2);
	return ret ?: abox_fw_config(abox, SET_ASRC_FACTOR_CP, 36864, 0);
}

static int abox_fw_control(struct abox *abox, u32 id, s32 val)
{
	u32 msg[IPC_MSG_SIZE / 4] = { IPC_SYSTEM, 0, ABOX_UPDATE_COMPONENT_CONTROL,
				      ABOX_VOID_COMPONENT, id, 0, val };

	return abox_fw_send_wait(abox, msg, ABOX_VOID_COMPONENT, id, 0);
}

/* stock abox_rdma/wdma: one message per PCM op, task_id = channel */
static int abox_fw_pcm(struct abox *abox, u32 ipc, u32 ch, u32 type,
		       u32 p0, u32 p1, u32 p2)
{
	u32 msg[IPC_MSG_SIZE / 4] = { ipc, ch, type, ch, p0, p1, p2 };

	return abox_fw_send_wait(abox, msg, ch, 0, 0);
}

static void abox_fw_call_ack(struct abox *abox, u32 ch, u32 pointer);

static void abox_fw_handle(struct abox *abox, const u32 *m, u32 size)
{
	u32 id = m[0], type = m[2];

	abox->fw.rx_count++;
	if (id == IPC_SYSTEM && type == ABOX_BOOT_DONE) {
		abox->fw.version = m[5];
		complete(&abox->fw.boot_done);
		return;
	}
	if (id == IPC_PCMPLAYBACK && type == PCM_PLTDAI_ACK && m[3] < 8) {
		/* the firmware asks to be told the app pointer (stock .ack) */
		if (m[4])
			abox->fw.ack_rdma |= BIT(m[3]);
		else
			abox->fw.ack_rdma &= ~BIT(m[3]);
		dev_info(abox->dev, "RDMA%u ack %s\n", m[3], m[4] ? "on" : "off");
		return;
	}
	if (id == IPC_PCMPLAYBACK && type == PCM_PLTDAI_POINTER && m[3] < 8 &&
	    (abox->fw.ack_rdma & BIT(m[3])))
		abox_fw_call_ack(abox, m[3], m[4]);
	if ((id == IPC_PCMPLAYBACK || id == IPC_PCMCAPTURE) &&
	    type == PCM_PLTDAI_POINTER)
		return;		/* period ticks of the call DMAs */
	if (id == IPC_SYSTEM && type == ABOX_CHANGE_GEAR) {
		/* nothing scales the A32 here: it runs from the AUD PLL */
		abox_fw_send_system(abox, ABOX_CHANGED_GEAR, AUD_PLL_RATE, 0, 0,
				    NULL, 0);
		return;
	}
	dev_info_ratelimited(abox->dev, "fw msg %u/0x%x %08x %08x %08x (%u bytes)\n",
			     id, type, m[3], m[4], m[5], size);
}

static void abox_fw_rx(struct abox *abox)
{
	u32 buf[IPC_MSG_SIZE / 4];

	while (abox_q_rd(abox, FW_IPC_RX, Q_CMD_IDX_S) !=
	       abox_q_rd(abox, FW_IPC_RX, Q_CMD_IDX_E)) {
		u32 cs = abox_q_rd(abox, FW_IPC_RX, Q_CMD_IDX_S) % Q_CMD_LEN;
		void *cmd = abox_fw_q(abox, FW_IPC_RX) + Q_CMD_ELEM +
			    cs * Q_CMD_ELEM_SIZE;
		u32 di = ((u32 *)cmd)[5] % Q_DATA_SIZE;
		void *rec = abox_fw_q(abox, FW_IPC_RX) + Q_DATA_ELEM + di;
		u32 size = *(u32 *)rec;

		rmb();
		memset(buf, 0, sizeof(buf));
		memcpy(buf, rec + 4, min_t(u32, size, sizeof(buf)));
		abox_q_wr(abox, FW_IPC_RX, Q_DATA_IDX_S, (di + 4 + size) % Q_DATA_SIZE);
		abox_q_wr(abox, FW_IPC_RX, Q_CMD_IDX_S, (cs + 1) % Q_CMD_LEN);
		abox_fw_handle(abox, buf, size);
	}
}

static irqreturn_t abox_gic_irq(int irq, void *data)
{
	struct abox *abox = data;
	irqreturn_t ret = IRQ_NONE;
	u32 iar, id;

	for (;;) {
		iar = readl(abox->gicc + GICC_IAR);
		id = iar & 0x3ff;
		if (id >= 1020)
			break;
		writel(iar, abox->gicc + GICC_EOIR);
		if (id < 16)
			writel(iar, abox->gicc + GICC_DIR);
		ret = IRQ_HANDLED;
		if (id == ABOX_SGI_MSG)
			abox_fw_rx(abox);
		else if (id == ABOX_IRQ_WDT)
			dev_err_ratelimited(abox->dev, "calliope watchdog (PC 0x%08x)\n",
					    abox_readl(abox, ABOX_CORE0_PC));
		else
			dev_dbg(abox->dev, "ABOX GIC irq %u\n", id);
	}
	return ret;
}

static void abox_gic_init(struct abox *abox)
{
	int i;

	/* abox_gic_init_gic() + abox_gic_enable_irq(), all group 0 */
	writel(0xff, abox->gicc + GICC_PMR);
	writel(3, abox->gicd + GICD_CTLR);
	for (i = 0; i < 40; i++)
		writel(0x10101010, abox->gicd + GICD_IPRIORITYR(i));
	writel(3, abox->gicc + GICC_CTLR);
	writel(1, abox->gicd + GICD_CTLR);
	for (i = 0; i < 4; i++)
		writel(0, abox->gicd + GICD_IGROUPR(i));
	/* the firmware watchdog goes to the AP */
	writeb(ABOX_GIC_CPU_AP, abox->gicd + GICD_ITARGETSR(0) + ABOX_IRQ_WDT);
	writel(BIT(ABOX_IRQ_WDT % 32), abox->gicd + GICD_ISENABLER(ABOX_IRQ_WDT / 32));
}

static int abox_fw_download(struct abox *abox)
{
	int i, ret;

	memset_io(abox->sram, 0, abox->sram_size);
	memset(abox->fw_mem, 0, FW_DRAM_SIZE);

	for (i = 0; i < ARRAY_SIZE(abox_fw_files); i++) {
		const struct firmware *f;
		size_t max = abox_fw_files[i].sram ? abox->sram_size : FW_DRAM_SIZE;

		ret = request_firmware_direct(&f, abox_fw_files[i].name, abox->dev);
		if (ret) {
			if (abox_fw_files[i].required) {
				dev_err(abox->dev, "%s missing: %d\n",
					abox_fw_files[i].name, ret);
				return ret;
			}
			dev_info(abox->dev, "%s not found, skipped\n",
				 abox_fw_files[i].name);
			continue;
		}
		if (abox_fw_files[i].offset + f->size > max) {
			release_firmware(f);
			return -EFBIG;
		}
		if (abox_fw_files[i].sram) {
			/* 32-bit writes only into ABOX SRAM */
			u32 tail = 0;
			size_t n = f->size & ~3;

			__iowrite32_copy(abox->sram + abox_fw_files[i].offset,
					 f->data, n / 4);
			if (f->size & 3) {
				memcpy(&tail, f->data + n, f->size & 3);
				writel(tail, abox->sram + abox_fw_files[i].offset + n);
			}
		} else {
			memcpy(abox->fw_mem + abox_fw_files[i].offset, f->data,
			       f->size);
		}
		dev_dbg(abox->dev, "%s: %zu bytes at %s +0x%x\n",
			abox_fw_files[i].name, f->size,
			abox_fw_files[i].sram ? "SRAM" : "DRAM",
			abox_fw_files[i].offset);
		release_firmware(f);
	}

	/* the DRAM memset wiped the queue headers: abox_msg_init() again */
	memset(abox_fw_q(abox, FW_IPC_TX), 0, FW_IPC_SIZE);
	memset(abox_fw_q(abox, FW_IPC_RX), 0, FW_IPC_SIZE);
	abox_q_wr(abox, FW_IPC_TX, Q_DATA_LEN, Q_DATA_SIZE);
	abox_q_wr(abox, FW_IPC_RX, Q_DATA_LEN, Q_DATA_SIZE);
	wmb();
	return 0;
}

static void abox_fw_cores(struct abox *abox, bool on)
{
	u32 v = on ? 1 : 0;

	regmap_update_bits(abox->pmu, PMU_AUD_CPU0_POWER, 1, v);
	regmap_update_bits(abox->pmu, PMU_AUD_CPU1_POWER, 1, v);
}

static int abox_fw_boot(struct abox *abox)
{
	u64 atime, bundle[2];
	u32 pc[4], misc;
	int i, ret;

	if (!abox->domain || !abox->fw_mem || !abox->gicd || abox->irq < 0)
		return -ENODEV;
	if (abox->fw.running)
		return -EALREADY;
	/* our own DMA use must end first; keep-alive streams start after */
	if (READ_ONCE(abox->dma[0].substream) || READ_ONCE(abox->dma[1].substream))
		return -EBUSY;

	/* keeps pd_aud, the AUD PLL and SYSPOWER up (abox_hw_init) */
	ret = pm_runtime_resume_and_get(abox->dev);
	if (ret)
		return ret;

	dev_info(abox->dev, "CPU mux 0x%08x user 0x%08x pll-div 0x%08x, timer 0x%08x, IP 0x%08x\n",
		 readl(abox->cmu + 0x1000), readl(abox->cmu + 0x100),
		 readl(abox->cmu + 0x1828), readl(abox->timer + TIMER_CTRL1(0)),
		 abox_readl(abox, ABOX_IP_INDEX));

	/* safe only with the AUD PLL running (it is: abox_hw_init) */
	misc = readl(abox->sysreg + SYSREG_AUD_MISC_CON);
	writel(1, abox->sysreg + SYSREG_AUD_MISC_CON);

	abox_gic_init(abox);
	ret = abox_fw_download(abox);
	if (ret)
		goto err;
	dev_info(abox->dev, "firmware loaded, MISC_CON was 0x%x, compat word 0x%08x\n",
		 misc, readl(abox->sram + 0x30040));

	reinit_completion(&abox->fw.boot_done);
	abox->fw.rx_count = 0;
	abox->fw.running = true;
	abox->fw.ack_rdma = 0;
	enable_irq(abox->irq);
	abox_fw_cores(abox, true);

	for (i = 0; i < ARRAY_SIZE(pc); i++) {
		udelay(20);
		pc[i] = abox_readl(abox, ABOX_CORE0_PC);
	}
	dev_info(abox->dev, "core0 PC %08x %08x %08x %08x\n", pc[0], pc[1], pc[2], pc[3]);

	if (!wait_for_completion_timeout(&abox->fw.boot_done, msecs_to_jiffies(500))) {
		dev_err(abox->dev, "no BOOT_DONE (PC 0x%08x, %u msgs, panic word 0x%08x)\n",
			abox_readl(abox, ABOX_CORE0_PC), abox->fw.rx_count,
			readl(abox->sram + abox->sram_size - 4));
		ret = -ETIMEDOUT;
		goto err_cores;
	}
	dev_info(abox->dev, "Calliope is ready to sing (version:%c%c%c%c)\n",
		 abox->fw.version >> 24, (abox->fw.version >> 16) & 0xff,
		 (abox->fw.version >> 8) & 0xff, abox->fw.version & 0xff);

	/* abox_cpu_pm_ipc(resume): sched_clock + ABOX timer in ns (26 MHz) */
	atime = readq(abox->timer + TIMER_CURVALUD_LSB(1));
	bundle[0] = sched_clock();
	bundle[1] = div_u64(atime * 500, 13);
	abox_fw_send_system(abox, ABOX_RESUME, 0, 0, 0, bundle, 2);
	return 0;

err_cores:
	abox_fw_cores(abox, false);
	disable_irq(abox->irq);
	abox->fw.running = false;
err:
	pm_runtime_put(abox->dev);
	return ret;
}

static void abox_fw_stop(struct abox *abox)
{
	u32 v;
	int ret;

	if (!abox->fw.running)
		return;

	abox_fw_send_system(abox, ABOX_SUSPEND, 0, 0, 0, NULL, 0);
	ret = regmap_read_poll_timeout(abox->pmu, PMU_AUD_CPU0_STANDBY, v,
				       (v & 0x11) == 0x11, 100, 100000);
	if (ret)
		dev_warn(abox->dev, "core0 did not reach standby (0x%x)\n", v);
	abox_fw_cores(abox, false);
	disable_irq(abox->irq);
	abox->fw.running = false;
	/* the domain going down resets the cores; hw_init redoes the rest */
	pm_runtime_put(abox->dev);
}

/*
 * Handset call through calliope, as stock Android sets it up
 * (kit/callaudio/STOCK-CALL-CAPTURE.md): IN_CALL/VOICE, the VSS ports
 * linked into the graph, START_VSS, the "call" DMAs RDMA4/WDMA2 opened
 * the way the HAL opens pcm4p/pcm14c, and the stock in-call routing.
 * The codec, amps and UAIFs are powered by keep-alive streams on our
 * two PCMs, started before this.
 */
#define CALL_PERIOD_FRAMES		480
#define CALL_PERIODS			4
#define CALL_BUF_SIZE			SZ_64K	/* per DMA, mapped at its IOVA */

/*
 * The DMAs stock has open during a handset call (abox_dump: CTRL0 format
 * and period offset): VSS downlink/uplink on RDMA4/WDMA2 in ABOX SRAM, the
 * mixed downlink back into WDMA3, CHCONV4CH out through RDMA7 to the amps,
 * amp feedback on WDMA4/WDMA7.
 */
static const struct abox_call_dma {
	bool capture;
	u8 id, ch, width;
	u32 rate;
} abox_call_dmas[] = {
	{ false, 4, 2, 16, 32000 },
	{ true,  2, 4, 16, 32000 },
	{ true,  3, 2, 32, 48000 },
	{ true,  4, 4, 32, 48000 },
	{ true,  7, 4, 16, 48000 },
	{ false, 7, 4, 32, 48000 },
};

static const struct { u32 id; s32 val; } abox_call_controls[] = {
	{ 0xda, 1 },	/* VPCMOUT_DAI0_EN */
	{ 0x64, 1 },	/* RDMA0_EN */
	{ 0x65, 41 },	/* RDMA0_A = VPCMOUT_DAI0 */
	{ 0x8b, 1 },	/* WDMA1_EN */
	{ 0xc5, 1 },	/* VPCMIN_DAI0_EN */
	{ 0xc6, 14 },	/* VPCMIN_DAI0_A = WDMA1 */
	{ 0x91, 1 },	/* WDMA3_EN */
	{ 0xce, 1 },	/* CHCONV4CH_EN */
	{ 0xcf, 16 },	/* CHCONV4CH_A = WDMA3 */
	{ 0x79, 1 },	/* RDMA7_EN */
	{ 0x7a, 37 },	/* RDMA7_A = CHCONV4CH */
	{ 0x94, 1 },	/* WDMA4_EN */
	{ 0xd4, 1 },	/* REFEXT4CH_EN */
	{ 0xd5, 17 },	/* REFEXT4CH_A = WDMA4 */
};

/* SFR values stock leaves during a handset call (abox_dump) */
static const struct { u32 reg, val; } abox_call_sfr[] = {
	{ 0x0020, 0x00004444 }, { 0x0030, 0x7fffffff }, { 0x0034, 0x0000001f },
	{ 0x0038, 0x0000000c },
	{ 0x0040, 0x00000020 }, { 0x0044, 0x08009188 }, { 0x0048, 0x00300000 },
	{ 0x0200, 0x04310842 }, { 0x0204, 0x00067011 }, { 0x0210, 0x70130000 },
	{ 0x0214, 0xb498265a }, { 0x0218, 0x04810800 },
	{ 0x0220, 0x02000000 }, { 0x0224, 0x02000200 }, { 0x0228, 0x02000400 },
	{ 0x022c, 0x02000600 }, { 0x0230, 0x02000800 }, { 0x0234, 0x02000a00 },
	{ 0x0238, 0x02000c00 }, { 0x023c, 0x02000e00 },
	{ 0x0280, 0x02003000 }, { 0x0290, 0x000000ff },
	{ 0x0300, 0x10001102 }, { 0x0304, 0x00000948 }, { 0x0310, 0x12745630 },
	{ 0x0320, 0x02000000 }, { 0x0324, 0x02000200 }, { 0x0328, 0x02000400 },
	{ 0x032c, 0x02000600 }, { 0x0330, 0x02000800 }, { 0x0334, 0x02000a00 },
	{ 0x0348, 0x02001200 }, { 0x034c, 0x02001400 }, { 0x0350, 0x02001600 },
	{ 0x0354, 0x02001800 }, { 0x0358, 0x02001a00 },
	/* UAIF0 codec 2 ch, UAIF1 amps 4 slots with feedback (MIC) on */
	{ 0x0504, 0x11857000 }, { 0x0500, 0x01000016 },
	{ 0x0514, 0x1b4df7df }, { 0x0510, 0x01000017 },
};

static u32 abox_call_iova(const struct abox_call_dma *d)
{
	return d->capture ? IOVA_WDMA_BUF(d->id) : IOVA_RDMA_BUF(d->id);
}

/*
 * The sample-rate converters stock's AP side (asrc_config_sync) sets for a
 * call, from its log: { SPUS?, id, in Hz, out Hz, out bits, APF coef }.
 * SPUS 10 is RDMA4's downlink 32 -> 48 kHz, SPUM 6 the uplink 48 -> 32 kHz
 * into WDMA2; the rest run 48 -> 48 kHz. Unprogrammed, they pass silence.
 */
#define CMU_AUD_DIV_UAIF0		0x182c
#define ABOX_SPUS_ASRC(x)		(0x2000 + 0x100 * (x))
#define ABOX_SPUM_ASRC(x)		(0x4000 + 0x100 * (x))
#define ASRC_CTRL_SYNC			(1 << 23 | 1 << 20)	/* OS/IS source: sync */
#define ASRC_CTRL_RATIO_8		(3 << 4 | 3 << 2)	/* DCMF, OVSF */

static const struct { bool spus; u8 id; u32 isr, osr; u8 width, apf; }
abox_call_asrc[] = {
	{ true, 10, 32000, 48000, 24, 1 },
	{ true,  3, 48000, 48000, 24, 0 },
	{ true,  2, 48000, 48000, 32, 0 },
	{ false, 6, 48000, 32000, 24, 0 },
	{ false, 5, 48000, 48000, 24, 0 },
	{ false, 4, 48000, 48000, 32, 0 },
	{ false, 3, 48000, 48000, 24, 0 },
	{ false, 1, 48000, 48000, 24, 0 },
};

static void abox_call_asrc_config(struct abox *abox)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(abox_call_asrc); i++) {
		typeof(abox_call_asrc[0]) *a = &abox_call_asrc[i];
		u32 base = a->spus ? ABOX_SPUS_ASRC(a->id) : ABOX_SPUM_ASRC(a->id);
		/* asrc_table: 98304 unless converting down (48 -> 32 kHz) */
		u32 ifactor = a->isr > a->osr ? 65536 : 98304;
		u32 ofactor = ifactor * (a->isr / 100) / (a->osr / 100);

		abox_writel(abox, base + 0x00, ASRC_CTRL_SYNC | ASRC_CTRL_RATIO_8 |
			    (a->width / 8 - 1) << 6);
		abox_writel(abox, base + 0x10, ifactor);
		abox_writel(abox, base + 0x14, ifactor / 20);
		abox_writel(abox, base + 0x18, ofactor);
		abox_writel(abox, base + 0x1c, ofactor / 20);
		abox_writel(abox, base + 0x2c, a->apf << 6);
	}
}

static int abox_fw_call_pcm(struct abox *abox, const struct abox_call_dma *d,
			    bool start)
{
	u32 ipc = d->capture ? IPC_PCMCAPTURE : IPC_PCMPLAYBACK, ch = d->id;
	u32 period = CALL_PERIOD_FRAMES * d->ch * d->width / 8;
	int ret = 0;

	if (!start) {
		abox_fw_pcm(abox, ipc, ch, PCM_PLTDAI_TRIGGER, 0, 0, 0);
		abox_fw_pcm(abox, ipc, ch, PCM_PLTDAI_HW_FREE, 0, 0, 0);
		return abox_fw_pcm(abox, ipc, ch, PCM_PLTDAI_CLOSE, 0, 0, 0);
	}
	ret = ret ?: abox_fw_pcm(abox, ipc, ch, PCM_PLTDAI_OPEN, 0, 0, 0);
	ret = ret ?: abox_fw_pcm(abox, ipc, ch, PCM_SET_BUFFER, abox_call_iova(d),
				 period, CALL_PERIODS);
	ret = ret ?: abox_fw_pcm(abox, ipc, ch, PCM_PLTDAI_HW_PARAMS, d->rate,
				 d->width, d->ch);
	ret = ret ?: abox_fw_pcm(abox, ipc, ch, PCM_PLTDAI_PREPARE, 0, 0, 0);
	ret = ret ?: abox_fw_pcm(abox, ipc, ch, PCM_PLTDAI_TRIGGER, 1, 0, 0);
	return ret;
}

/*
 * A playing stock HAL keeps the app pointer ahead of the DMA; with nobody
 * writing, the firmware treats the stream as underrun and holds its
 * VOL_FACTOR at 0. Claim two periods queued ahead on every tick.
 */
static void abox_fw_call_ack(struct abox *abox, u32 ch, u32 pointer)
{
	u32 msg[IPC_MSG_SIZE / 4] = { IPC_PCMPLAYBACK, ch, PCM_PLTDAI_ACK, ch };
	int i;

	for (i = 0; i < ARRAY_SIZE(abox_call_dmas); i++) {
		const struct abox_call_dma *d = &abox_call_dmas[i];
		u32 period = CALL_PERIOD_FRAMES * d->ch * d->width / 8;

		if (d->capture || d->id != ch)
			continue;
		msg[4] = (pointer + 2 * period) % (period * CALL_PERIODS);
		abox_fw_send(abox, msg, IPC_MSG_SIZE, ch, 0, 0);
		abox->fw.acks++;
		return;
	}
}

static int abox_fw_call_start(struct abox *abox)
{
	int i, ret;

	if (abox->fw.in_call)
		return -EALREADY;
	if (!abox->fw.running) {
		ret = abox_fw_boot(abox);
		if (ret)
			return ret;
		msleep(20);	/* let it settle after RESUME */
	}

	if (!abox->fw.call_buf) {
		if (!abox->card_dev)
			return -ENODEV;
		abox->fw.call_buf = dma_alloc_coherent(abox->card_dev,
						       ARRAY_SIZE(abox_call_dmas) *
						       CALL_BUF_SIZE,
						       &abox->fw.call_buf_phys,
						       GFP_KERNEL);
		if (!abox->fw.call_buf)
			return -ENOMEM;
		for (i = 0, ret = 0; !ret && i < ARRAY_SIZE(abox_call_dmas); i++)
			ret = iommu_map(abox->domain,
					abox_call_iova(&abox_call_dmas[i]),
					abox->fw.call_buf_phys + i * CALL_BUF_SIZE,
					CALL_BUF_SIZE, IOMMU_READ | IOMMU_WRITE,
					GFP_KERNEL);
		if (ret)
			return ret;
	}
	memset(abox->fw.call_buf, 0, ARRAY_SIZE(abox_call_dmas) * CALL_BUF_SIZE);

	ret = abox_fw_restore(abox);
	msleep(20);	/* drain: the system sends below do not wait */
	ret = ret ?: abox_fw_send_system(abox, ABOX_SET_MODE, AUDIO_MODE_IN_CALL,
					 0, 0, NULL, 0);
	ret = ret ?: abox_fw_send_system(abox, ABOX_SET_TYPE, SOUND_TYPE_VOICE,
					 0, 0, NULL, 0);
	for (i = 0; !ret && i < ARRAY_SIZE(abox_call_controls); i++)
		ret = abox_fw_control(abox, abox_call_controls[i].id,
				      abox_call_controls[i].val);
	ret = ret ?: abox_fw_send_system(abox, ABOX_START_VSS, 0, 0, 0, NULL, 0);
	if (ret)
		return ret;
	msleep(5);	/* the tx queue holds 16 commands */

	/*
	 * Stock's UAIF0 runs 24-bit slots (CTRL1 below) off a /32 BCLK,
	 * 2.304 MHz; ours is /24 for 32-bit slots. Mixing the two samples
	 * the mics at 64 kHz and the far end hears them a fourth low.
	 */
	abox->fw.uaif0_div = readl(abox->cmu + CMU_AUD_DIV_UAIF0);
	writel(0x1f, abox->cmu + CMU_AUD_DIV_UAIF0);
	abox_call_asrc_config(abox);
	for (i = 0; i < ARRAY_SIZE(abox_call_sfr); i++)
		abox_writel(abox, abox_call_sfr[i].reg, abox_call_sfr[i].val);

	for (i = 0, ret = 0; !ret && i < ARRAY_SIZE(abox_call_dmas); i++)
		ret = abox_fw_call_pcm(abox, &abox_call_dmas[i], true);
	if (ret)
		return ret;
	abox->fw.in_call = true;
	dev_info(abox->dev, "call audio up\n");
	return 0;
}

static void abox_fw_call_stop(struct abox *abox)
{
	int i;

	if (!abox->fw.in_call)
		return;

	for (i = ARRAY_SIZE(abox_call_dmas) - 1; i >= 0; i--)
		abox_fw_call_pcm(abox, &abox_call_dmas[i], false);
	msleep(5);
	abox_fw_send_system(abox, ABOX_SET_MODE, AUDIO_MODE_NORMAL, 0, 0, NULL, 0);
	writel(abox->fw.uaif0_div, abox->cmu + CMU_AUD_DIV_UAIF0);
	abox->fw.in_call = false;
	dev_info(abox->dev, "call audio down\n");
}

static ssize_t abox_fw_write(struct file *file, const char __user *ubuf,
			     size_t len, loff_t *ppos)
{
	struct abox *abox = file_inode(file)->i_private;
	char buf[32] = {};
	u32 id, val;
	int ret = 0;

	if (copy_from_user(buf, ubuf, min(len, sizeof(buf) - 1)))
		return -EFAULT;
	if (sscanf(buf, "ctl %i %i", &id, &val) == 2)	/* calliope2.dt control */
		ret = abox->fw.running ? abox_fw_control(abox, id, val) : -EIO;
	else if (sscanf(buf, "sfr %i %i", &id, &val) == 2 && id < 0x1000 && !(id & 3))
		abox_writel(abox, id, val);
	else if (sscanf(buf, "spk %i", &id) == 1 && id < 8 &&
		 !READ_ONCE(abox->dma[0].substream)) {
		/* debug: our playback PCM on another RDMA */
		abox->dma[0].base = ABOX_RDMA(id);
		abox->dma[0].iova = IOVA_RDMA_BUF(id);
	} else if (sysfs_streq(buf, "own"))
		abox->fw.own = true;
	else if (sysfs_streq(buf, "unown"))
		abox->fw.own = false;
	else if (sysfs_streq(buf, "tone") && abox->fw.in_call) {
		/* 1 kHz square, -24 dBFS, all 4 slots, into RDMA7 (the amps) */
		s32 *t = abox->fw.call_buf + 5 * CALL_BUF_SIZE;
		int n;

		abox_fw_control(abox, 0x7a, 0);		/* RDMA7_A = None */
		for (n = 0; n < CALL_BUF_SIZE / 16; n++) {
			s32 v = (n % 48) < 24 ? 0x08000000 : -0x08000000;

			t[4 * n] = t[4 * n + 1] = t[4 * n + 2] = t[4 * n + 3] = v;
		}
	} else if (sysfs_streq(buf, "boot"))
		ret = abox_fw_boot(abox);
	else if (sysfs_streq(buf, "stop")) {
		abox_fw_call_stop(abox);
		abox_fw_stop(abox);
	} else if (sysfs_streq(buf, "call"))
		ret = abox_fw_call_start(abox);
	else if (sysfs_streq(buf, "hangup"))
		abox_fw_call_stop(abox);
	else
		ret = -EINVAL;
	return ret ? ret : len;
}

static u32 abox_peak(const void __iomem *io, const void *mem, u32 bytes,
		     u32 width)
{
	u32 i, peak = 0;
	s32 v;

	for (i = 0; i + width / 8 <= bytes; i += width / 8) {
		if (width == 16)
			v = (s16)(io ? readw(io + i) : *(const u16 *)(mem + i));
		else
			v = (s32)(io ? readl(io + i) : *(const u32 *)(mem + i)) >> 16;
		peak = max_t(u32, peak, abs(v));
	}
	return peak;		/* of 32767 */
}

/* per call DMA: STATUS, buffer start, peak sample of the whole buffer */
static void abox_fw_show_call(struct seq_file *s, struct abox *abox)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(abox_call_dmas); i++) {
		const struct abox_call_dma *d = &abox_call_dmas[i];
		u32 base = d->capture ? ABOX_WDMA(d->id) : ABOX_RDMA(d->id);
		u32 str = abox_readl(abox, base + ABOX_DMA_BUF_STR);
		u32 end = abox_readl(abox, base + ABOX_DMA_BUF_END);
		u32 peak = 0;

		if (str >= 0x18d00000 && end > str && end <= 0x18d40000)
			peak = abox_peak(abox->sram + str - 0x18d00000, NULL,
					 end - str, d->width);
		else if (str == abox_call_iova(d))
			peak = abox_peak(NULL, abox->fw.call_buf + i * CALL_BUF_SIZE,
					 min(end - str, (u32)CALL_BUF_SIZE), d->width);
		seq_printf(s, "%cDMA%u status %08x buf %08x vol %08x peak %5u\n",
			   d->capture ? 'W' : 'R', d->id,
			   abox_readl(abox, base + ABOX_DMA_STATUS), str,
			   abox_readl(abox, base + ABOX_DMA_VOL_FACTOR), peak);
	}
	/* the VPCMOUT/VPCMIN links, in our keep-alive streams' buffers */
	for (i = 0; i < 2; i++) {
		struct abox_dma *dma = &abox->dma[i];
		struct snd_pcm_substream *ss = READ_ONCE(dma->substream);
		u32 peak = 0;

		if (ss && ss->runtime)
			peak = abox_peak(NULL, ss->runtime->dma_area,
					 ss->dma_buffer.bytes, 16);
		seq_printf(s, "%s status %08x buf %08x peak %5u\n",
			   i ? "WDMA1" : "RDMA0",
			   abox_readl(abox, dma->base + ABOX_DMA_STATUS),
			   abox_readl(abox, dma->base + ABOX_DMA_BUF_STR), peak);
	}
}

static int abox_fw_show(struct seq_file *s, void *unused)
{
	struct abox *abox = s->private;

	seq_printf(s, "running %d in_call %d version 0x%08x msgs %u ack 0x%02x/%u\n",
		   abox->fw.running, abox->fw.in_call, abox->fw.version,
		   abox->fw.rx_count, abox->fw.ack_rdma, abox->fw.acks);
	if (abox->fw.running)
		seq_printf(s, "PC 0x%08x tx %d/%d rx %d/%d\n",
			   abox_readl(abox, ABOX_CORE0_PC),
			   abox_q_rd(abox, FW_IPC_TX, Q_CMD_IDX_S),
			   abox_q_rd(abox, FW_IPC_TX, Q_CMD_IDX_E),
			   abox_q_rd(abox, FW_IPC_RX, Q_CMD_IDX_S),
			   abox_q_rd(abox, FW_IPC_RX, Q_CMD_IDX_E));
	if (abox->fw.in_call)
		abox_fw_show_call(s, abox);
	return 0;
}

static int abox_fw_open(struct inode *inode, struct file *file)
{
	return single_open(file, abox_fw_show, inode->i_private);
}

static const struct file_operations abox_fw_fops = {
	.open		= abox_fw_open,
	.read		= seq_read,
	.write		= abox_fw_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int abox_iommu_fault(struct iommu_domain *domain, struct device *dev,
			    unsigned long iova, int flags, void *token)
{
	dev_err_ratelimited(dev, "SysMMU fault at IOVA 0x%08lx (%s)\n", iova,
			    flags & IOMMU_FAULT_WRITE ? "write" : "read");
	return -ENOSYS;
}

/* firmware mode resources; all optional, a DT without them keeps bypass */
static int abox_fw_probe(struct abox *abox, struct platform_device *pdev)
{
	static const struct { u32 iova; phys_addr_t pa; size_t size; } fixed[] = {
		{ IOVA_VSS, VSS_PHYS, VSS_SIZE },
		{ IOVA_VSS_PARAM, VSS_PARAM_PHYS, VSS_PARAM_SIZE },
		{ IOVA_CHIPID, IOVA_CHIPID, PAGE_SIZE },
	};
	struct device *dev = abox->dev;
	struct resource *res, rmem;
	struct device_node *np;
	int i, ret;

	if (!device_iommu_mapped(dev))
		return 0;

	abox->domain = iommu_paging_domain_alloc(dev);
	if (IS_ERR(abox->domain)) {
		ret = PTR_ERR(abox->domain);
		abox->domain = NULL;
		return dev_err_probe(dev, ret, "no IOMMU domain\n");
	}
	ret = iommu_attach_device(abox->domain, dev);
	if (ret) {
		iommu_domain_free(abox->domain);
		abox->domain = NULL;
		return dev_err_probe(dev, ret, "IOMMU attach failed\n");
	}
	iommu_set_fault_handler(abox->domain, abox_iommu_fault, abox);
	abox->dma[SNDRV_PCM_STREAM_PLAYBACK].iova = IOVA_RDMA_BUF(ABOX_RDMA_SPK);
	abox->dma[SNDRV_PCM_STREAM_CAPTURE].iova = IOVA_WDMA_BUF(ABOX_WDMA_MIC);

	np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!np)
		return 0;
	ret = of_address_to_resource(np, 0, &rmem);
	of_node_put(np);
	if (ret || resource_size(&rmem) < FW_DRAM_SIZE + FW_PRIVATE_SIZE)
		return dev_err_probe(dev, -EINVAL, "bad firmware memory-region\n");
	abox->fw_phys = rmem.start;
	abox->fw_mem = devm_memremap(dev, rmem.start, FW_DRAM_SIZE + FW_PRIVATE_SIZE,
				     MEMREMAP_WC);
	if (IS_ERR(abox->fw_mem))
		return PTR_ERR(abox->fw_mem);

	ret = iommu_map(abox->domain, IOVA_FW_DRAM, abox->fw_phys, FW_DRAM_SIZE,
			IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
	ret = ret ?: iommu_map(abox->domain, IOVA_PRIVATE,
			       abox->fw_phys + FW_DRAM_SIZE, FW_PRIVATE_SIZE,
			       IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
	for (i = 0; !ret && i < ARRAY_SIZE(fixed); i++)
		ret = iommu_map(abox->domain, fixed[i].iova, fixed[i].pa,
				fixed[i].size, IOMMU_READ | IOMMU_WRITE,
				GFP_KERNEL);
	if (ret)
		return dev_err_probe(dev, ret, "IOMMU map failed\n");

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sram");
	abox->sram = devm_platform_ioremap_resource_byname(pdev, "sram");
	abox->timer = devm_platform_ioremap_resource_byname(pdev, "timer");
	abox->sysreg = devm_platform_ioremap_resource_byname(pdev, "sysreg");
	abox->gicd = devm_platform_ioremap_resource_byname(pdev, "gicd");
	abox->gicc = devm_platform_ioremap_resource_byname(pdev, "gicc");
	if (IS_ERR(abox->sram) || IS_ERR(abox->timer) || IS_ERR(abox->sysreg) ||
	    IS_ERR(abox->gicd) || IS_ERR(abox->gicc)) {
		abox->gicd = NULL;
		return dev_err_probe(dev, -EINVAL, "firmware registers missing\n");
	}
	abox->sram_size = resource_size(res);

	init_completion(&abox->fw.boot_done);
	spin_lock_init(&abox->fw.tx_lock);
	abox->irq = platform_get_irq(pdev, 0);
	if (abox->irq < 0)
		return abox->irq;
	/* only while the firmware runs: the GIC is dead with pd_aud off */
	ret = devm_request_irq(dev, abox->irq, abox_gic_irq, IRQF_NO_AUTOEN,
			       "abox-gic", abox);
	if (ret)
		return ret;

	dev_info(dev, "SysMMU_AUD domain, firmware DRAM at %pa\n", &abox->fw_phys);
	return 0;
}

/* register dump for bring-up: /sys/kernel/debug/exynos9820-abox/regs */
static int abox_regs_show(struct seq_file *s, void *unused)
{
	struct abox *abox = s->private;
	static const u32 regs[] = {
		ABOX_VERSION, ABOX_SYSPOWER_STATUS, ABOX_QCHANNEL_DISABLE,
		ABOX_ROUTE_CTRL0, ABOX_ROUTE_CTRL1, ABOX_SPUS_CTRL0,
		ABOX_SPUS_CTRL1, ABOX_SPUM_CTRL0, ABOX_SBANK_RDMA(0),
		ABOX_SBANK_NSRC(0), ABOX_SIFS_CNT(1),
		ABOX_UAIF_CTRL0(0), ABOX_UAIF_CTRL1(0), ABOX_UAIF_STATUS(0),
		ABOX_UAIF_CTRL0(1), ABOX_UAIF_CTRL1(1), ABOX_UAIF_STATUS(1),
	};
	int i, d;

	if (pm_runtime_get_if_active(abox->dev) <= 0) {
		seq_puts(s, "suspended\n");
		return 0;
	}

	for (i = 0; i < ARRAY_SIZE(regs); i++)
		seq_printf(s, "%04x: %08x\n", regs[i], abox_readl(abox, regs[i]));
	for (d = 0; d < ARRAY_SIZE(abox->dma); d++)
		for (i = 0; i <= ABOX_DMA_STATUS; i += 4)
			seq_printf(s, "%04x: %08x\n", abox->dma[d].base + i,
				   abox_dma_read(&abox->dma[d], i));
	seq_printf(s, "cmu pll %08x audif %08x uaif0 %08x uaif1 %08x\n",
		   readl(abox->cmu + CMU_PLL_CON0_AUD0),
		   readl(abox->cmu + CMU_DIV_AUDIF),
		   readl(abox->cmu + CMU_DIV_UAIF(0)),
		   readl(abox->cmu + CMU_DIV_UAIF(1)));

	pm_runtime_put(abox->dev);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(abox_regs);

static const struct snd_soc_component_driver abox_component = {
	.name		= "exynos9820-abox",
	.open		= abox_pcm_open,
	.close		= abox_pcm_close,
	.hw_params	= abox_pcm_hw_params,
	.hw_free	= abox_pcm_hw_free,
	.prepare	= abox_pcm_prepare,
	.trigger	= abox_pcm_trigger,
	.pointer	= abox_pcm_pointer,
	.copy		= abox_pcm_copy,
	.pcm_new	= abox_pcm_new,
};

static int abox_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dentry *dir;
	struct abox *abox;
	int ret;

	abox = devm_kzalloc(dev, sizeof(*abox), GFP_KERNEL);
	if (!abox)
		return -ENOMEM;
	abox->dev = dev;
	platform_set_drvdata(pdev, abox);

	abox->sfr = devm_platform_ioremap_resource_byname(pdev, "sfr");
	if (IS_ERR(abox->sfr))
		return PTR_ERR(abox->sfr);
	abox->cmu = devm_platform_ioremap_resource_byname(pdev, "cmu");
	if (IS_ERR(abox->cmu))
		return PTR_ERR(abox->cmu);

	abox->pmu = syscon_regmap_lookup_by_phandle(dev->of_node,
						    "samsung,pmu-syscon");
	if (IS_ERR(abox->pmu))
		return dev_err_probe(dev, PTR_ERR(abox->pmu), "no PMU syscon\n");

	abox->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(abox->pinctrl))
		return dev_err_probe(dev, PTR_ERR(abox->pinctrl), "no pinctrl\n");
	abox->pins_active = pinctrl_lookup_state(abox->pinctrl, "active");
	if (IS_ERR(abox->pins_active))
		return dev_err_probe(dev, PTR_ERR(abox->pins_active),
				     "no active pin state\n");
	abox->pins_idle = pinctrl_lookup_state(abox->pinctrl, "idle");
	if (IS_ERR(abox->pins_idle))
		return dev_err_probe(dev, PTR_ERR(abox->pins_idle),
				     "no idle pin state\n");

	/* DMA buffer addresses are 32 bit (IOVAs with SysMMU_AUD, else physical) */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	abox->irq = -ENXIO;

	abox->dma[SNDRV_PCM_STREAM_PLAYBACK].base = ABOX_RDMA(ABOX_RDMA_SPK);
	abox->dma[SNDRV_PCM_STREAM_CAPTURE].base = ABOX_WDMA(ABOX_WDMA_MIC);
	for (int i = 0; i < ARRAY_SIZE(abox->dma); i++) {
		abox->dma[i].abox = abox;
		hrtimer_setup(&abox->dma[i].timer, abox_period_timer,
			      CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	}

	/*
	 * No register access here: the domain is still in the state S-Boot
	 * left it, and only a genpd power cycle (TZPC save + restore SMC)
	 * makes ABOX safe to touch. Runtime resume does all the programming.
	 */
	pm_runtime_set_autosuspend_delay(dev, 3000);
	pm_runtime_use_autosuspend(dev);
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	ret = abox_fw_probe(abox, pdev);
	if (ret)
		return ret;

	dir = debugfs_create_dir("exynos9820-abox", NULL);
	debugfs_create_file("regs", 0400, dir, abox, &abox_regs_fops);
	if (abox->gicd)
		debugfs_create_file("firmware", 0600, dir, abox, &abox_fw_fops);

	return devm_snd_soc_register_component(dev, &abox_component, abox_dais,
					       ARRAY_SIZE(abox_dais));
}

static const struct dev_pm_ops abox_pm_ops = {
	RUNTIME_PM_OPS(abox_runtime_suspend, abox_runtime_resume, NULL)
};

static const struct of_device_id abox_of_match[] = {
	{ .compatible = "samsung,exynos9820-abox" },
	{ }
};
MODULE_DEVICE_TABLE(of, abox_of_match);

static struct platform_driver abox_driver = {
	.driver = {
		.name = "exynos9820-abox",
		.of_match_table = abox_of_match,
		.pm = pm_ptr(&abox_pm_ops),
	},
	.probe = abox_probe,
};
module_platform_driver(abox_driver);

MODULE_DESCRIPTION("Exynos 9820 ABOX audio (firmware-less RDMA/WDMA/UAIF)");
MODULE_LICENSE("GPL");
