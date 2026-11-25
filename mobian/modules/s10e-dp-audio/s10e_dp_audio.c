// SPDX-License-Identifier: GPL-2.0-only
/*
 * S10e DisplayPort audio, a test module: the Exynos 9820 DP controller's SST1 audio on the link that exynos9820-dp
 * brought up, programmed as stock's dpu20/cal_9820/displayport_reg.c does it (the EVT1 branches). Samples reach
 * the controller's audio FIFO through PDMA0, a PL330, on request 28, as stock's sound/soc/samsung/dp_dma.c feeds
 * them. U-Boot's DT has no PDMA0, so the module adds it by overlay (dp-audio.dtso) and populates it itself.
 *
 *   bist=1   no card: the controller's own sine until the module is removed (proves the link side alone)
 *   else     ALSA card "S10eHDMI": 2 channels, S16_LE or S24_LE, 32 to 192 kHz, and an "HDMI Jack" that is in
 *            while the link's video stream runs, so that UCM's priority can move the sound to the TV and back
 *
 * The registers are only touched while the Type-C port reports HPD, and with the DP's gtc clock held: the port's
 * notifier runs before exynos9820-dp's own work can power the link down and gate gtc. A stream cut by an unplug
 * stalls until the jack has moved it away; stopping it then touches nothing (the next power-up resets the block).
 */
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/usb/max77705-dp.h>
#include <linux/workqueue.h>
#include <sound/dmaengine_pcm.h>
#include <sound/jack.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "dp-audio-dtbo.h"

#define DP_BASE				0x130b0000
#define DP_SIZE				0x10000

#define SST1_FUNCTION_EN		0x001c
#define  SST1_LH_PWR_ON_STATUS		BIT(5)
#define  SST1_AUDIO_FIFO_FUNC_EN	BIT(2)
#define  SST1_AUDIO_FUNC_EN		BIT(1)
#define  SST1_VIDEO_FUNC_EN		BIT(0)
#define SST1_MAIN_CONTROL		0x5000
#define  MAUD_MODE			BIT(10)
#define SST1_MAIN_FIFO_CONTROL		0x5004
#define  CLEAR_AUDIO_FIFO		BIT(3)
#define SST1_INT_STATUS1		0x5028
#define SST1_MAUD_MASTER_MODE		0x505c
#define SST1_NAUD_MASTER_MODE		0x5060
#define SST1_MAUD_SFR			0x5064
#define SST1_NAUD_SFR			0x5068
#define SST1_AUDIO_CONTROL		0x5800
#define  DMA_REQ_GEN_EN			BIT(30)
#define  DMA_BURST_SEL			GENMASK(20, 18)
#define  AUDIO_BIT_MAPPING		GENMASK(17, 16)
#define  PCM_SIZE			GENMASK(14, 13)
#define  AUDIO_CH_STATUS_SAME		BIT(5)
#define SST1_AUDIO_ENABLE		0x5804
#define  AUDIO_EN			BIT(0)
#define SST1_AUDIO_MASTER_TIMING_GEN	0x5808
#define  AUDIO_MASTER_TIME_GEN		BIT(0)
#define SST1_AUDIO_DMA_REQUEST		0x580c
#define SST1_AUDIO_FIFO			0x5818
#define SST1_AUDIO_BIST_CONTROL		0x5830
#define  SIN_AMPL			GENMASK(7, 4)
#define  AUD_BIST_EN			BIT(0)
#define SST1_AUDIO_BIST_CH_STATUS0	0x5834
#define SST1_AUDIO_BIST_CH_STATUS1	0x5838
#define SST1_AUDIO_BUFFER_CONTROL	0x583c
#define  MASTER_AUDIO_CH_COUNT		GENMASK(16, 12)
#define  MASTER_AUDIO_BUFFER_LEVEL	GENMASK(11, 5)
#define SST1_AUDIO_CH_1_4_REMAP		0x5840
#define SST1_AUDIO_CH_5_8_REMAP		0x5844
#define SST1_AUDIO_CH_1_2_STATUS0	0x5860
#define SST1_AUDIO_CH_1_2_STATUS1	0x5864
/* the IEC 60958 channel status, in both the BIST and the stream copies */
#define  CS_CLK_ACCUR			GENMASK(29, 28)
#define  CS_FS_FREQ			GENMASK(27, 24)
#define  CS_CH_NUM			GENMASK(23, 20)
#define  CS_SOURCE_NUM			GENMASK(19, 16)
#define  CS_WORD_LENGTH			GENMASK(3, 1)
#define  CS_WORD_MAX			BIT(0)
#define SST1_INFOFRAME_UPDATE		0x5c04
#define SST1_INFOFRAME_SEND		0x5c08
#define  AUDIO_INFO			BIT(3)
#define SST1_INFOFRAME_AUDIO0		0x5c50
#define SST1_INFOFRAME_AUDIO1		0x5c54
#define SST1_INFOFRAME_AUDIO2		0x5c58

/* PCM_SIZE */
#define PCM_16				0
#define PCM_20				1
#define PCM_24				2
/* AUDIO_BIT_MAPPING: a 32-bit container as it is, or two 16-bit samples to a word */
#define MAPPING_NORMAL			0
#define MAPPING_PACKED2			2
/* CS_CLK_ACCUR: stock says NOT_MATCH, the audio clock being made from the link's */
#define CLK_ACCUR_NOT_MATCH		3

#define NUM_CLKS			4
/* the jack follows the link's video stream, which mutter starts some time after HPD */
#define POLL_MS				500

static bool bist;
module_param(bist, bool, 0444);
MODULE_PARM_DESC(bist, "play the controller's own sine instead of registering a card");

static unsigned int link_khz = 540000;
module_param(link_khz, uint, 0644);
MODULE_PARM_DESC(link_khz, "the link's symbol clock in kHz (162000, 270000, 540000), as exynos9820-dp trained it");

static void __iomem *dpa_regs;
static struct clk *dpa_gtc;
static struct clk *dpa_clks[NUM_CLKS];
static int dpa_ovcs = -1;
static struct device_node *dpa_bus;
static struct platform_device *dpa_root;
static bool dpa_driver_registered;

/*
 * HPD and the clock that make the registers safe. A register access checks dpa_live and finishes under
 * dpa_port_lock (or, in the atomic trigger, dpa_trigger_lock), and an unplug clears dpa_live under both
 * before it lets gtc go.
 */
static DEFINE_MUTEX(dpa_port_lock);
static DEFINE_SPINLOCK(dpa_trigger_lock);
static bool dpa_hpd;			/* the port's, as last notified */
static bool dpa_gtc_held;
static bool dpa_live;			/* registers usable: HPD and gtc held; cleared at once on unplug */
static struct notifier_block dpa_nb;
static struct work_struct dpa_port_work;
static struct delayed_work dpa_poll;
static struct snd_soc_jack dpa_jack;
static bool dpa_jack_ready;
static bool dpa_jack_in;

/* the one stream's setup, from hw_params; the card holds the device's drvdata */
static struct {
	unsigned int rate, channels, pcm_size, mapping, burst;
} dpa_cfg;
static struct snd_dmaengine_dai_dma_data dpa_dma;

static void dpa_update(u32 reg, u32 mask, u32 val)
{
	u32 v = readl(dpa_regs + reg);

	writel((v & ~mask) | (val & mask), dpa_regs + reg);
}

/* the link's video stream is running: exynos9820-dp's stream on, with its LH channel */
static bool dpa_link_up(void)
{
	u32 v = readl(dpa_regs + SST1_FUNCTION_EN);

	return (v & (SST1_VIDEO_FUNC_EN | SST1_LH_PWR_ON_STATUS)) == (SST1_VIDEO_FUNC_EN | SST1_LH_PWR_ON_STATUS);
}

/* IEC 60958 sampling frequency */
static int dpa_cs_fs(unsigned int rate)
{
	switch (rate) {
	case 32000:
		return 0x3;
	case 44100:
		return 0x0;
	case 48000:
		return 0x2;
	case 88200:
		return 0x8;
	case 96000:
		return 0xa;
	case 176400:
		return 0xc;
	case 192000:
		return 0xe;
	}
	return -EINVAL;
}

/* displayport_audio_enable(), or with @sine displayport_audio_bist_enable() */
static void dpa_start(unsigned int rate, unsigned int ch, unsigned int pcm_size, unsigned int mapping,
		      unsigned int burst, bool sine)
{
	u32 cs0 = FIELD_PREP(CS_CLK_ACCUR, CLK_ACCUR_NOT_MATCH) | FIELD_PREP(CS_FS_FREQ, dpa_cs_fs(rate)) |
		  FIELD_PREP(CS_CH_NUM, ch) | FIELD_PREP(CS_SOURCE_NUM, ch);
	u32 cs1 = pcm_size == PCM_24 ? FIELD_PREP(CS_WORD_LENGTH, 5) | CS_WORD_MAX :
		  FIELD_PREP(CS_WORD_LENGTH, pcm_size == PCM_20 ? 5 : 1);

	/*
	 * Asynchronous: Maud/Naud = 512 fs / the link's symbol clock (stock's audio_async_m_n, computed), and the
	 * sample clock made from the link: fs = LS/2 * MAUD_MASTER / NAUD_MASTER.
	 */
	dpa_update(SST1_MAIN_CONTROL, MAUD_MODE, 0);
	writel(DIV_ROUND_CLOSEST_ULL(512ULL * rate * 32768, link_khz * 1000ULL), dpa_regs + SST1_MAUD_SFR);
	writel(32768, dpa_regs + SST1_NAUD_SFR);
	dpa_update(SST1_FUNCTION_EN, SST1_AUDIO_FUNC_EN, SST1_AUDIO_FUNC_EN);
	dpa_update(SST1_AUDIO_CONTROL, DMA_BURST_SEL | PCM_SIZE | AUDIO_BIT_MAPPING,
		   FIELD_PREP(DMA_BURST_SEL, burst - 1) | FIELD_PREP(PCM_SIZE, pcm_size) |
		   FIELD_PREP(AUDIO_BIT_MAPPING, mapping));
	dpa_update(SST1_AUDIO_BUFFER_CONTROL, MASTER_AUDIO_CH_COUNT, FIELD_PREP(MASTER_AUDIO_CH_COUNT, ch - 1));
	dpa_update(SST1_MAIN_FIFO_CONTROL, CLEAR_AUDIO_FIFO, 0);
	dpa_update(SST1_FUNCTION_EN, SST1_AUDIO_FIFO_FUNC_EN, SST1_AUDIO_FIFO_FUNC_EN);
	writel(rate, dpa_regs + SST1_MAUD_MASTER_MODE);
	writel(link_khz * 500, dpa_regs + SST1_NAUD_MASTER_MODE);
	/* stock's order: FL, FR, then C and LFE swapped */
	writel(3 << 24 | 4 << 16 | 2 << 8 | 1, dpa_regs + SST1_AUDIO_CH_1_4_REMAP);
	writel(8 << 24 | 7 << 16 | 6 << 8 | 5, dpa_regs + SST1_AUDIO_CH_5_8_REMAP);
	dpa_update(SST1_AUDIO_ENABLE, AUDIO_EN, AUDIO_EN);
	if (sine)
		writel(FIELD_PREP(SIN_AMPL, 0xf) | AUD_BIST_EN, dpa_regs + SST1_AUDIO_BIST_CONTROL);
	dpa_update(SST1_AUDIO_BIST_CH_STATUS0, CS_CLK_ACCUR | CS_FS_FREQ | CS_CH_NUM | CS_SOURCE_NUM, cs0);
	dpa_update(SST1_AUDIO_CH_1_2_STATUS0, CS_CLK_ACCUR | CS_FS_FREQ | CS_CH_NUM | CS_SOURCE_NUM, cs0);
	dpa_update(SST1_AUDIO_BIST_CH_STATUS1, CS_WORD_LENGTH | CS_WORD_MAX, cs1);
	dpa_update(SST1_AUDIO_CH_1_2_STATUS1, CS_WORD_LENGTH | CS_WORD_MAX, cs1);
	dpa_update(SST1_AUDIO_CONTROL, AUDIO_CH_STATUS_SAME, AUDIO_CH_STATUS_SAME);
	dpa_update(SST1_AUDIO_MASTER_TIMING_GEN, AUDIO_MASTER_TIME_GEN, AUDIO_MASTER_TIME_GEN);

	/* the audio InfoFrame: PB1 the channel count - 1 (coding and size from the stream), PB4 0 = FL/FR */
	writel(ch - 1, dpa_regs + SST1_INFOFRAME_AUDIO0);
	writel(0, dpa_regs + SST1_INFOFRAME_AUDIO1);
	writel(0, dpa_regs + SST1_INFOFRAME_AUDIO2);
	dpa_update(SST1_INFOFRAME_UPDATE, AUDIO_INFO, AUDIO_INFO);
	dpa_update(SST1_INFOFRAME_SEND, AUDIO_INFO, AUDIO_INFO);
}

/* displayport_audio_wait_buf_full(), then displayport_audio_disable(); sleeps */
static void dpa_stop(void)
{
	dpa_update(SST1_AUDIO_MASTER_TIMING_GEN, AUDIO_MASTER_TIME_GEN, 0);
	dpa_update(SST1_AUDIO_CONTROL, DMA_REQ_GEN_EN, 0);
	dpa_update(SST1_AUDIO_BIST_CONTROL, AUD_BIST_EN, 0);
	if (readl(dpa_regs + SST1_AUDIO_ENABLE) & AUDIO_EN) {
		usleep_range(1000, 2000);
		dpa_update(SST1_AUDIO_ENABLE, AUDIO_EN, 0);
		dpa_update(SST1_FUNCTION_EN, SST1_AUDIO_FIFO_FUNC_EN, 0);
		dpa_update(SST1_MAIN_FIFO_CONTROL, CLEAR_AUDIO_FIFO, CLEAR_AUDIO_FIFO);
	}
	dpa_update(SST1_INFOFRAME_UPDATE, AUDIO_INFO, 0);
	dpa_update(SST1_INFOFRAME_SEND, AUDIO_INFO, 0);
}

/* /sys/module/s10e_dp_audio/parameters/regs: the registers stock reads back, and the FIFO's level */
static int dpa_regs_get(char *buf, const struct kernel_param *kp)
{
	static const u32 regs[] = {
		SST1_FUNCTION_EN, SST1_MAIN_CONTROL, SST1_MAIN_FIFO_CONTROL, SST1_INT_STATUS1, SST1_AUDIO_CONTROL,
		SST1_AUDIO_ENABLE, SST1_AUDIO_MASTER_TIMING_GEN, SST1_AUDIO_DMA_REQUEST, SST1_AUDIO_BIST_CONTROL,
		SST1_AUDIO_BUFFER_CONTROL, SST1_INFOFRAME_SEND,
	};
	int i, n = 0;

	if (!dpa_regs)
		return sysfs_emit(buf, "not mapped\n");
	guard(mutex)(&dpa_port_lock);
	if (!bist && !READ_ONCE(dpa_live))
		return sysfs_emit(buf, "no link (jack %s)\n", dpa_jack_in ? "in" : "out");
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		n += sysfs_emit_at(buf, n, "%04x: %08x\n", regs[i], readl(dpa_regs + regs[i]));
	n += sysfs_emit_at(buf, n, "fifo level %lu\n",
			   FIELD_GET(MASTER_AUDIO_BUFFER_LEVEL, readl(dpa_regs + SST1_AUDIO_BUFFER_CONTROL)));
	n += sysfs_emit_at(buf, n, "jack %s\n", dpa_jack_in ? "in" : "out");
	return n;
}

static const struct kernel_param_ops dpa_regs_ops = {
	.get = dpa_regs_get,
};
module_param_cb(regs, &dpa_regs_ops, NULL, 0444);

static int dpa_dai_probe(struct snd_soc_dai *dai)
{
	snd_soc_dai_init_dma_data(dai, &dpa_dma, NULL);
	return 0;
}

static int dpa_startup(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	guard(mutex)(&dpa_port_lock);
	return READ_ONCE(dpa_live) && dpa_link_up() ? 0 : -ENODEV;
}

static int dpa_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params,
			 struct snd_soc_dai *dai)
{
	unsigned int width = params_physical_width(params);

	switch (params_width(params)) {
	case 16:
		dpa_cfg.pcm_size = PCM_16;
		break;
	case 24:
		dpa_cfg.pcm_size = PCM_24;
		break;
	default:
		return -EINVAL;
	}
	if (dpa_cs_fs(params_rate(params)) < 0)
		return -EINVAL;
	dpa_cfg.rate = params_rate(params);
	dpa_cfg.channels = params_channels(params);
	dpa_cfg.mapping = width == 32 ? MAPPING_NORMAL : MAPPING_PACKED2;
	/* a burst is one frame of words, as stock asks the PL330 for */
	dpa_cfg.burst = width * dpa_cfg.channels / 32;
	dpa_dma.maxburst = dpa_cfg.burst;
	return 0;
}

static int dpa_prepare(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	guard(mutex)(&dpa_port_lock);
	if (!READ_ONCE(dpa_live) || !dpa_link_up())
		return -ENODEV;
	dpa_stop();
	dpa_start(dpa_cfg.rate, dpa_cfg.channels, dpa_cfg.pcm_size, dpa_cfg.mapping, dpa_cfg.burst, false);
	return 0;
}

/* after the DMA starts and before it stops (ASoC's default order), as stock's dma_trigger() */
static int dpa_trigger(struct snd_pcm_substream *substream, int cmd, struct snd_soc_dai *dai)
{
	bool live;

	guard(spinlock_irqsave)(&dpa_trigger_lock);
	live = READ_ONCE(dpa_live);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (!live)
			return -EIO;
		dpa_update(SST1_AUDIO_MASTER_TIMING_GEN, AUDIO_MASTER_TIME_GEN, AUDIO_MASTER_TIME_GEN);
		dpa_update(SST1_AUDIO_CONTROL, DMA_REQ_GEN_EN, DMA_REQ_GEN_EN);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		if (live) {
			dpa_update(SST1_AUDIO_MASTER_TIMING_GEN, AUDIO_MASTER_TIME_GEN, 0);
			dpa_update(SST1_AUDIO_CONTROL, DMA_REQ_GEN_EN, 0);
		}
		return 0;
	}
	return -EINVAL;
}

static int dpa_hw_free(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	guard(mutex)(&dpa_port_lock);
	if (READ_ONCE(dpa_live))
		dpa_stop();
	return 0;
}

static void dpa_report(bool in)
{
	if (!dpa_jack_ready || in == dpa_jack_in)
		return;
	dpa_jack_in = in;
	snd_soc_jack_report(&dpa_jack, in ? SND_JACK_AVOUT : 0, SND_JACK_AVOUT);
	pr_info("s10e-dp-audio: jack %s\n", in ? "in" : "out");
}

/* while HPD is up: the jack is in while the video stream runs */
static void dpa_poll_fn(struct work_struct *work)
{
	if (!READ_ONCE(dpa_live))
		return;
	dpa_report(dpa_link_up());
	queue_delayed_work(system_freezable_wq, &dpa_poll, msecs_to_jiffies(POLL_MS));
}

static void dpa_port_work_fn(struct work_struct *work)
{
	mutex_lock(&dpa_port_lock);
	if (READ_ONCE(dpa_hpd)) {
		if (!dpa_gtc_held && !clk_prepare_enable(dpa_gtc))
			dpa_gtc_held = true;
		if (dpa_gtc_held && !READ_ONCE(dpa_live)) {
			WRITE_ONCE(dpa_live, true);
			queue_delayed_work(system_freezable_wq, &dpa_poll, 0);
		}
	} else {
		spin_lock_irq(&dpa_trigger_lock);
		WRITE_ONCE(dpa_live, false);
		spin_unlock_irq(&dpa_trigger_lock);
		cancel_delayed_work_sync(&dpa_poll);
		dpa_report(false);
		if (dpa_gtc_held)
			clk_disable_unprepare(dpa_gtc);
		dpa_gtc_held = false;
	}
	mutex_unlock(&dpa_port_lock);
}

/* the Type-C port's IRQ thread, ahead of exynos9820-dp's work: on an unplug, stop touching the registers now */
static int dpa_port_event(struct notifier_block *nb, unsigned long state, void *data)
{
	bool hpd = (state & (MAX77705_DP_CONFIGURED | MAX77705_DP_HPD)) ==
		   (MAX77705_DP_CONFIGURED | MAX77705_DP_HPD);

	WRITE_ONCE(dpa_hpd, hpd);
	if (!hpd)
		WRITE_ONCE(dpa_live, false);
	schedule_work(&dpa_port_work);
	return NOTIFY_OK;
}

static void dpa_port_stop(void *data)
{
	max77705_dp_unregister_notifier(&dpa_nb);
	WRITE_ONCE(dpa_hpd, false);
	WRITE_ONCE(dpa_live, false);
	cancel_work_sync(&dpa_port_work);
	dpa_port_work_fn(NULL);
	dpa_jack_ready = false;
}

static const struct snd_soc_dai_ops dpa_dai_ops = {
	.probe = dpa_dai_probe,
	.startup = dpa_startup,
	.hw_params = dpa_hw_params,
	.prepare = dpa_prepare,
	.trigger = dpa_trigger,
	.hw_free = dpa_hw_free,
};

#define DPA_RATES	(SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 | \
			 SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_176400 | \
			 SNDRV_PCM_RATE_192000)
#define DPA_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE)

static struct snd_soc_dai_driver dpa_dai = {
	.name = "s10e-dp-audio",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = DPA_RATES,
		.formats = DPA_FORMATS,
	},
	.ops = &dpa_dai_ops,
};

static const struct snd_soc_component_driver dpa_component = {
	.name = "s10e-dp-audio",
	.legacy_dai_naming = 1,
};

static const struct snd_pcm_hardware dpa_pcm_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID,
	.formats = DPA_FORMATS,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = 256 * 1024,
	.period_bytes_min = 256,
	.period_bytes_max = 64 * 1024,
	.periods_min = 2,
	.periods_max = 128,
	.fifo_size = 32,
};

static const struct snd_dmaengine_pcm_config dpa_pcm_config = {
	.pcm_hardware = &dpa_pcm_hw,
	.prepare_slave_config = snd_dmaengine_pcm_prepare_slave_config,
	.prealloc_buffer_size = 256 * 1024,
};

SND_SOC_DAILINK_DEFS(hdmi,
	DAILINK_COMP_ARRAY(COMP_CPU("s10e-dp-audio")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

static struct snd_soc_dai_link dpa_link = {
	.name = "HDMI",
	.stream_name = "HDMI",
	.playback_only = 1,
	SND_SOC_DAILINK_REG(hdmi),
};

static int dpa_late_probe(struct snd_soc_card *card)
{
	int ret = snd_soc_card_jack_new(card, "HDMI", SND_JACK_AVOUT, &dpa_jack);

	if (ret)
		return ret;
	dpa_jack_in = false;
	dpa_jack_ready = true;
	return 0;
}

static struct snd_soc_card dpa_card = {
	.name = "S10eHDMI",
	.owner = THIS_MODULE,
	.dai_link = &dpa_link,
	.num_links = 1,
	.late_probe = dpa_late_probe,
};

static int dpa_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret;

	dpa_dma.addr = DP_BASE + SST1_AUDIO_FIFO;
	dpa_dma.addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	dpa_dma.maxburst = 1;

	ret = devm_snd_soc_register_component(dev, &dpa_component, &dpa_dai, 1);
	if (ret)
		return dev_err_probe(dev, ret, "DAI\n");
	/* the PL330 may not be there yet: a deferral brings us back */
	ret = devm_snd_dmaengine_pcm_register(dev, &dpa_pcm_config, 0);
	if (ret)
		return dev_err_probe(dev, ret, "PCM on PDMA0\n");

	hdmi_cpus[0].of_node = dev->of_node;
	hdmi_platforms[0].of_node = dev->of_node;
	dpa_card.dev = dev;
	ret = devm_snd_soc_register_card(dev, &dpa_card);
	if (ret)
		return dev_err_probe(dev, ret, "card\n");

	/* the jack exists now; the notifier reports the port's current state at once */
	dpa_nb.notifier_call = dpa_port_event;
	dpa_nb.priority = 1;		/* ahead of exynos9820-dp's, which powers the link down */
	ret = max77705_dp_register_notifier(&dpa_nb);
	if (ret)
		return dev_err_probe(dev, ret, "Type-C port notifier\n");
	ret = devm_add_action_or_reset(dev, dpa_port_stop, NULL);
	if (ret)
		return ret;
	dev_info(dev, "card S10eHDMI, link %u kHz\n", link_khz);
	return 0;
}

static const struct of_device_id dpa_of_match[] = {
	{ .compatible = "s10e,dp-audio-test" },
	{ }
};

static struct platform_driver dpa_driver = {
	.probe = dpa_probe,
	.driver = {
		.name = "s10e-dp-audio",
		.of_match_table = dpa_of_match,
	},
};

static void dpa_teardown(void)
{
	int i;

	if (dpa_root) {
		/*
		 * of_platform_depopulate() only acts on a parent with a DT node, and the root has none. Everything populated
		 * must go before the overlay does: a device outliving its node's parent oopses on its next uevent (11-06).
		 */
		device_for_each_child_reverse(&dpa_root->dev, NULL, of_platform_device_destroy);
		of_node_clear_flag(dpa_bus, OF_POPULATED_BUS);
		platform_device_unregister(dpa_root);
		dpa_root = NULL;
	}
	if (dpa_driver_registered)
		platform_driver_unregister(&dpa_driver);
	dpa_driver_registered = false;
	for (i = 0; i < NUM_CLKS; i++) {
		if (!IS_ERR_OR_NULL(dpa_clks[i])) {
			clk_disable_unprepare(dpa_clks[i]);
			clk_put(dpa_clks[i]);
		}
		dpa_clks[i] = NULL;
	}
	of_node_put(dpa_bus);
	dpa_bus = NULL;
	if (dpa_ovcs >= 0)
		of_overlay_remove(&dpa_ovcs);
	dpa_ovcs = -1;
}

/* PDMA0 by overlay, its gates on before anything reads it, then the PL330 and our device */
static int dpa_setup_card(void)
{
	struct device_node *audio;
	int i, ret;

	ret = of_overlay_fdt_apply(dp_audio_dtbo, dp_audio_dtbo_len, &dpa_ovcs, NULL);
	if (ret) {
		pr_err("s10e-dp-audio: overlay: %d\n", ret);
		dpa_ovcs = -1;
		return ret;
	}
	dpa_bus = of_find_node_by_path("/soc@0/s10e-dp-audio");
	audio = dpa_bus ? of_get_child_by_name(dpa_bus, "audio") : NULL;
	if (!audio) {
		ret = -ENODEV;
		goto fail;
	}
	for (i = 0; i < NUM_CLKS; i++) {
		dpa_clks[i] = of_clk_get(audio, i);
		if (IS_ERR(dpa_clks[i])) {
			ret = PTR_ERR(dpa_clks[i]);
			break;
		}
		ret = clk_prepare_enable(dpa_clks[i]);
		if (ret) {
			clk_put(dpa_clks[i]);
			dpa_clks[i] = NULL;
			break;
		}
	}
	of_node_put(audio);
	if (ret) {
		pr_err("s10e-dp-audio: PDMA0 clocks: %d\n", ret);
		goto fail;
	}

	ret = platform_driver_register(&dpa_driver);
	if (ret)
		goto fail;
	dpa_driver_registered = true;
	dpa_root = platform_device_register_simple("s10e-dp-audio-root", PLATFORM_DEVID_NONE, NULL, 0);
	if (IS_ERR(dpa_root)) {
		ret = PTR_ERR(dpa_root);
		dpa_root = NULL;
		goto fail;
	}
	ret = of_platform_populate(dpa_bus, NULL, NULL, &dpa_root->dev);
	if (ret)
		goto fail;
	return 0;

fail:
	dpa_teardown();
	return ret;
}

static int __init dpa_init(void)
{
	struct device_node *dp;
	int ret;

	dp = of_find_compatible_node(NULL, NULL, "samsung,exynos9820-dp");
	if (!dp)
		return -ENODEV;
	dpa_gtc = of_clk_get_by_name(dp, "gtc");
	of_node_put(dp);
	if (IS_ERR(dpa_gtc))
		return PTR_ERR(dpa_gtc);
	dpa_regs = ioremap(DP_BASE, DP_SIZE);
	if (!dpa_regs) {
		ret = -ENOMEM;
		goto put_gtc;
	}

	if (bist) {
		ret = clk_prepare_enable(dpa_gtc);
		if (ret)
			goto unmap;
		if (!dpa_link_up()) {
			pr_err("s10e-dp-audio: no DisplayPort stream running\n");
			clk_disable_unprepare(dpa_gtc);
			ret = -ENODEV;
			goto unmap;
		}
		dpa_start(48000, 2, PCM_16, MAPPING_PACKED2, 1, true);
		pr_info("s10e-dp-audio: BIST sine on, link %u kHz\n", link_khz);
		return 0;
	}

	/* the card stays, plugged or not; the Type-C port's notifier, from the probe on, holds gtc while HPD */
	INIT_WORK(&dpa_port_work, dpa_port_work_fn);
	INIT_DELAYED_WORK(&dpa_poll, dpa_poll_fn);
	ret = dpa_setup_card();
	if (ret)
		goto unmap;
	return 0;

unmap:
	iounmap(dpa_regs);
	dpa_regs = NULL;
put_gtc:
	clk_put(dpa_gtc);
	return ret;
}
module_init(dpa_init);

static void __exit dpa_exit(void)
{
	if (bist) {
		dpa_stop();
		clk_disable_unprepare(dpa_gtc);
	} else {
		/* the device's removal stops the port's notifier and lets gtc go (dpa_port_stop) */
		dpa_teardown();
	}
	iounmap(dpa_regs);
	clk_put(dpa_gtc);
}
module_exit(dpa_exit);

MODULE_DESCRIPTION("S10e DisplayPort audio over PDMA0 (test)");
MODULE_LICENSE("GPL");
