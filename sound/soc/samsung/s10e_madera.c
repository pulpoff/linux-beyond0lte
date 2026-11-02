// SPDX-License-Identifier: GPL-2.0
//
// Galaxy S10e (beyond0lte) sound card: Cirrus CS47L93 codec.
//
// Two links on ABOX (exynos9820-abox, firmware-less): UAIF0 is I2S to the
// codec's AIF1 and carries capture (the mics, routed to AIF1TX1/2 in the
// codec), UAIF1 goes to the two CS35L40 amps on one DSP_A TDM bus: earpiece
// amp on slot 0, loudspeaker amp on slot 1. ABOX provides the clocks on both.
//
// MCLK1 is the PMU CLKOUT0 pad set to the 26 MHz TCXO. FLL1 locks to it and
// runs SYSCLK at 98.304 MHz, as the stock kernel does.

#include <linux/mfd/madera/core.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/cs35l41.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "../codecs/madera.h"

#define S10E_MCLK1_RATE		26000000U
#define S10E_SYSCLK_RATE	98304000U

static int s10e_late_probe(struct snd_soc_card *card)
{
	struct snd_soc_pcm_runtime *rtd;
	struct snd_soc_component *codec;
	int ret;

	rtd = snd_soc_get_pcm_runtime(card, &card->dai_link[0]);
	codec = snd_soc_rtd_to_codec(rtd, 0)->component;

	ret = snd_soc_component_set_pll(codec, MADERA_FLL1_REFCLK,
					MADERA_FLL_SRC_MCLK1, S10E_MCLK1_RATE,
					S10E_SYSCLK_RATE);
	if (ret) {
		dev_err(card->dev, "Failed to start FLL1: %d\n", ret);
		return ret;
	}

	ret = snd_soc_component_set_sysclk(codec, MADERA_CLK_SYSCLK_1,
					   MADERA_CLK_SRC_FLL1,
					   S10E_SYSCLK_RATE, SND_SOC_CLOCK_IN);
	if (ret)
		dev_err(card->dev, "Failed to set SYSCLK: %d\n", ret);

	return ret;
}

static const struct snd_soc_dapm_widget s10e_widgets[] = {
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_MIC("Main Mic", NULL),
	SND_SOC_DAPM_MIC("Sub Mic", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
};

static const struct snd_soc_dapm_route s10e_routes[] = {
	{ "Headphone", NULL, "HPOUT2L" },
	{ "Headphone", NULL, "HPOUT2R" },
	{ "IN3L", NULL, "Main Mic" },
	{ "Main Mic", NULL, "MICBIAS1A" },
	{ "IN4L", NULL, "Sub Mic" },
	{ "Sub Mic", NULL, "MICBIAS1B" },
	{ "IN1BLN", NULL, "Headset Mic" },
	{ "IN1BLP", NULL, "Headset Mic" },
	{ "Headset Mic", NULL, "MICBIAS2A" },
};

static const struct snd_kcontrol_new s10e_controls[] = {
	SOC_DAPM_PIN_SWITCH("Headphone"),
	SOC_DAPM_PIN_SWITCH("Main Mic"),
	SOC_DAPM_PIN_SWITCH("Sub Mic"),
	SOC_DAPM_PIN_SWITCH("Headset Mic"),
};

static int s10e_amp_hw_params(struct snd_pcm_substream *substream,
			      struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	unsigned int bclk = params_rate(params) * params_channels(params) *
			    params_physical_width(params);
	struct snd_soc_dai *dai;
	unsigned int slot;
	int i, ret;

	for_each_rtd_codec_dais(rtd, i, dai) {
		/* codec 0 = earpiece (slot 0), codec 1 = loudspeaker (slot 1) */
		slot = i;
		ret = snd_soc_component_set_sysclk(dai->component,
						   CS35L41_CLKID_SCLK, 0, bclk,
						   SND_SOC_CLOCK_IN);
		ret = ret ?: snd_soc_dai_set_sysclk(dai, CS35L41_CLKID_SCLK,
						    bclk, SND_SOC_CLOCK_IN);
		ret = ret ?: snd_soc_dai_set_channel_map(dai, 0, NULL, 1, &slot);
		if (ret) {
			dev_err(rtd->dev, "amp %d clock/slot setup: %d\n", i,
				ret);
			return ret;
		}
	}

	return 0;
}

static const struct snd_soc_ops s10e_amp_ops = {
	.hw_params = s10e_amp_hw_params,
};

SND_SOC_DAILINK_DEFS(aif1,
	DAILINK_COMP_ARRAY(COMP_CPU("abox-uaif0")),
	DAILINK_COMP_ARRAY(COMP_CODEC(NULL, "cs47l92-aif1")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(speaker,
	DAILINK_COMP_ARRAY(COMP_CPU("abox-uaif1")),
	DAILINK_COMP_ARRAY(COMP_CODEC(NULL, "cs35l41-pcm"),
			   COMP_CODEC(NULL, "cs35l41-pcm")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

/*
 * The call itself, digitally (exynos9820-abox.c): the downlink as the CP
 * sends it, and an uplink that can replace the mics. hw:0,2 and hw:0,3.
 */
SND_SOC_DAILINK_DEFS(call_dl,
	DAILINK_COMP_ARRAY(COMP_CPU("abox-call-dl")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(call_ul,
	DAILINK_COMP_ARRAY(COMP_CPU("abox-call-ul")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

static struct snd_soc_dai_link s10e_dai_links[] = {
	{
		.name = "Codec AIF1",
		.stream_name = "Codec AIF1",
		/* board: plain i2s, ABOX provides the clocks */
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			   SND_SOC_DAIFMT_CBC_CFC,
		.capture_only = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(aif1),
	},
	{
		.name = "Speaker",
		.stream_name = "Speaker",
		/* board: dsp_a + bitclock-inversion, ABOX provides the clocks */
		.dai_fmt = SND_SOC_DAIFMT_DSP_A | SND_SOC_DAIFMT_IB_NF |
			   SND_SOC_DAIFMT_CBC_CFC,
		.playback_only = 1,
		.ops = &s10e_amp_ops,
		SND_SOC_DAILINK_REG(speaker),
	},
	{
		.name = "Call Downlink",
		.stream_name = "Call Downlink",
		.capture_only = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(call_dl),
	},
	{
		.name = "Call Uplink",
		.stream_name = "Call Uplink",
		.playback_only = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(call_ul),
	},
};

static struct snd_soc_card s10e_card = {
	.name = "S10e",
	.owner = THIS_MODULE,
	.dai_link = s10e_dai_links,
	.num_links = ARRAY_SIZE(s10e_dai_links),
	.dapm_widgets = s10e_widgets,
	.num_dapm_widgets = ARRAY_SIZE(s10e_widgets),
	.dapm_routes = s10e_routes,
	.num_dapm_routes = ARRAY_SIZE(s10e_routes),
	.controls = s10e_controls,
	.num_controls = ARRAY_SIZE(s10e_controls),
	.late_probe = s10e_late_probe,
};

static int s10e_madera_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct snd_soc_card *card = &s10e_card;
	struct device_node *codec, *cpu;
	struct snd_soc_dai_link *aif1 = &s10e_dai_links[0];
	struct snd_soc_dai_link *spk = &s10e_dai_links[1];
	struct snd_soc_dai_link *call_dl = &s10e_dai_links[2];
	struct snd_soc_dai_link *call_ul = &s10e_dai_links[3];

	codec = of_parse_phandle(dev->of_node, "samsung,audio-codec", 0);
	if (!codec)
		return dev_err_probe(dev, -EINVAL,
				     "missing samsung,audio-codec\n");

	aif1->codecs->of_node = codec;

	cpu = of_parse_phandle(dev->of_node, "samsung,audio-cpu", 0);
	if (!cpu)
		return dev_err_probe(dev, -EINVAL, "missing samsung,audio-cpu\n");
	aif1->cpus->of_node = cpu;
	aif1->platforms->of_node = cpu;
	spk->cpus->of_node = cpu;
	spk->platforms->of_node = cpu;
	call_dl->cpus->of_node = cpu;
	call_dl->platforms->of_node = cpu;
	call_ul->cpus->of_node = cpu;
	call_ul->platforms->of_node = cpu;
	for (int i = 0; i < spk->num_codecs; i++) {
		spk->codecs[i].of_node = of_parse_phandle(dev->of_node,
							  "samsung,speaker-amps",
							  i);
		if (!spk->codecs[i].of_node)
			return dev_err_probe(dev, -EINVAL,
					     "missing samsung,speaker-amps %d\n",
					     i);
	}

	card->dev = dev;

	return devm_snd_soc_register_card(dev, card);
}

static const struct of_device_id s10e_madera_of_match[] = {
	{ .compatible = "samsung,beyond0lte-madera" },
	{ }
};
MODULE_DEVICE_TABLE(of, s10e_madera_of_match);

static struct platform_driver s10e_madera_driver = {
	.driver = {
		.name = "s10e-madera",
		.of_match_table = s10e_madera_of_match,
	},
	.probe = s10e_madera_probe,
};
module_platform_driver(s10e_madera_driver);

MODULE_DESCRIPTION("Galaxy S10e CS47L93 sound card");
MODULE_LICENSE("GPL");
