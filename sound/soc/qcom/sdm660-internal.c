// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023, Richard Acayan. All rights reserved.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>
#include <sound/soc-dapm.h>

#include "common.h"
#include "qdsp6/q6afe.h"

#define DEFAULT_SAMPLE_RATE_48K		48000
#define TDM_BCLK_RATE			6144000

static int snd_sdm660_int_startup(struct snd_pcm_substream *stream)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(stream);
	struct snd_soc_dai *codec;
	struct snd_soc_dai *cpu;
	int i, ret;

	for_each_rtd_cpu_dais(rtd, i, cpu)
		snd_soc_dai_set_sysclk(cpu,
			Q6AFE_LPASS_CLK_ID_SEC_TDM_IBIT,
			TDM_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);

	// Only use this for TDM.
	for_each_rtd_codec_dais(rtd, i, codec) {
		ret = snd_soc_dai_set_fmt(codec, SND_SOC_DAIFMT_DSP_A
					       | SND_SOC_DAIFMT_CBC_CFC
					       | SND_SOC_DAIFMT_IB_NF);
		if (ret != 0) {
			dev_err(codec->dev, "set dai format failed\n");
			return ret;
		}

		ret = snd_soc_dai_set_sysclk(codec, 0,
					     TDM_BCLK_RATE,
					     SNDRV_PCM_STREAM_PLAYBACK);
		if (ret != 0) {
			dev_err(codec->dev, "set dai sysclk failed\n");
			return ret;
		}

		ret = snd_soc_component_set_sysclk(codec->component,
						   0, 0,
						   TDM_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		if (ret != 0) {
			dev_err(codec->dev, "set dai sysclk failed\n");
			return ret;
		}
	}

	return 0;
}

static void snd_sdm660_int_shutdown(struct snd_pcm_substream *stream)
{
}

static unsigned int tdm_slot_off[] = {
	0, 4, 8, 12, 16, 20, 24, 28
};

static int snd_sdm660_int_hw_params(struct snd_pcm_substream *stream,
				    struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(stream);
	struct snd_soc_dai *cpu;
	unsigned int channels;
	int i, ret;

	for_each_rtd_cpu_dais(rtd, i, cpu) {
		channels = params_channels(params);

		ret = snd_soc_dai_set_tdm_slot(cpu, 0, (1 << channels) - 1, 8, 16);
		if (ret) {
			dev_err(cpu->dev, "set tdm slot failed\n");
			return ret;
		}

		ret = snd_soc_dai_set_channel_map(cpu, 0, NULL,
						  channels, tdm_slot_off);
		if (ret) {
			dev_err(cpu->dev, "set channel map failed\n");
			return ret;
		}
	}

	return 0;
}

static int snd_sdm660_int_hw_free(struct snd_pcm_substream *stream)
{
	return 0;
}

static int snd_sdm660_int_prepare(struct snd_pcm_substream *stream)
{
	return 0;
}

static const struct snd_soc_ops sdm660_int_ops = {
	.startup = snd_sdm660_int_startup,
	.shutdown = snd_sdm660_int_shutdown,
	.hw_params = snd_sdm660_int_hw_params,
	.hw_free = snd_sdm660_int_hw_free,
	.prepare = snd_sdm660_int_prepare,
};

static int sdm660_int_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
					 struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
			SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	rate->min = rate->max = DEFAULT_SAMPLE_RATE_48K;
	channels->min = channels->max = 2;
	snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S16_LE);

	return 0;
}

static const struct snd_soc_dapm_widget sdm845_snd_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_SPK("Left Spk", NULL),
	SND_SOC_DAPM_SPK("Right Spk", NULL),
	SND_SOC_DAPM_MIC("Int Mic", NULL),
};

static int sdm660_int_dai_init(struct snd_soc_pcm_runtime *rtd)
{
	return 0;
}

static void snd_sdm660_int_add_ops(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link;
	int i;

	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm == 1) {
			link->ops = &sdm660_int_ops;
			link->be_hw_params_fixup = sdm660_int_be_hw_params_fixup;
		}

		link->init = sdm660_int_dai_init;
	}
}

static const struct snd_soc_dapm_widget snd_sdm660_int_dapm_widgets[] = {
};

static int snd_sdm660_int_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct device *dev = &pdev->dev;
	int ret;

	card = devm_kzalloc(dev, sizeof(struct snd_soc_card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	card->driver_name = "sdm660-internal";
	card->dapm_widgets = snd_sdm660_int_dapm_widgets;
	card->num_dapm_widgets = ARRAY_SIZE(snd_sdm660_int_dapm_widgets);
	card->dev = dev;
	card->owner = THIS_MODULE;

	ret = qcom_snd_parse_of(card);
	if (ret)
		return ret;

	snd_sdm660_int_add_ops(card);

	return devm_snd_soc_register_card(dev, card);
}

static int snd_sdm660_int_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id snd_sdm660_int_device_id[] = {
	{ .compatible = "qcom,sdm660-internal-sndcard", },
	{ }
};
MODULE_DEVICE_TABLE(of, snd_sdm660_int_device_id);

static struct platform_driver snd_sdm660_int_driver = {
	.probe = snd_sdm660_int_probe,
	.remove = snd_sdm660_int_remove,
	.driver = {
		.name = "sdm660-int-sndcard",
		.of_match_table = snd_sdm660_int_device_id,
	},
};
module_platform_driver(snd_sdm660_int_driver);

MODULE_DESCRIPTION("sdm660 Internal ASoC Machine Driver");
MODULE_LICENSE("GPL");
