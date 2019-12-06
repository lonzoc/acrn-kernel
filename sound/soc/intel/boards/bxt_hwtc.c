/*
 * Intel Broxton-P I2S Machine Driver for IVI reference platform
 * Copyright (c) 2017, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>

static const struct snd_kcontrol_new broxton_hwtc_controls[] = {
	SOC_DAPM_PIN_SWITCH("Speaker"),
	SOC_DAPM_PIN_SWITCH("SpeakerSos"),
};

static const struct snd_soc_dapm_widget broxton_hwtc_widgets[] = {
	SND_SOC_DAPM_SPK("Speaker", NULL),
	SND_SOC_DAPM_SPK("SpeakerSos", NULL),
	SND_SOC_DAPM_MIC("MainMic", NULL),
	SND_SOC_DAPM_HP("BtHfpUl", NULL),
	SND_SOC_DAPM_MIC("BtHfpDl", NULL),
};

static const struct snd_soc_dapm_route broxton_hwtc_map[] = {
	/* Speaker BE connections */
	{ "Speaker", NULL, "ssp1 Tx"},
	{ "ssp1 Tx", NULL, "codec0_out"},

	{ "SpeakerSos", NULL, "ssp1 Tx"},
	{ "ssp1 Tx", NULL, "codec0_out"},

	{ "codec0_in", NULL, "ssp2 Rx"},
	{ "ssp2 Rx", NULL, "MainMic"},

	{ "BtHfp_ssp5_in", NULL, "ssp5 Rx"},
	{ "ssp5 Rx", NULL, "BtHfpDl"},

	{ "BtHfpUl", NULL, "ssp5 Tx"},
	{ "ssp5 Tx", NULL, "BtHfp_ssp5_out"},
};

static struct snd_soc_dai_link broxton_hwtc_dais[] = {
	/* Front End DAI links */
	{
		.name = "Speaker Port",
		.stream_name = "Speaker",
		.cpu_dai_name = "Speaker Pin",
		.platform_name = "0000:00:0e.0",
		.nonatomic = 1,
		.dynamic = 1,
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.trigger = {
			SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST
		},
		.dpcm_playback = 1,
	},
	{
		.name = "SpeakerSos Port",
		.stream_name = "SpeakerSos",
		.cpu_dai_name = "SpeakerSos Pin",
		.platform_name = "0000:00:0e.0",
		.nonatomic = 1,
		.dynamic = 1,
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.trigger = {
			SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST
		},
		.dpcm_playback = 1,
	},
	{
		.name = "MainMic Port",
		.stream_name = "MainMic Cp",
		.cpu_dai_name = "Dirana Cp Pin",
		.platform_name = "0000:00:0e.0",
		.nonatomic = 1,
		.dynamic = 1,
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.dpcm_capture = 1,
	},
	{
		.name = "BtHfp Capture Port",
		.stream_name = "BtHfp Cp",
		.cpu_dai_name = "BtHfp Cp Pin",
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.platform_name = "0000:00:0e.0",
		.init = NULL,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		.nonatomic = 1,
		.dynamic = 1,
	},
	{
		.name = "BtHfp Playback Port",
		.stream_name = "BtHfp Pb",
		.cpu_dai_name = "BtHfp Pb Pin",
		.platform_name = "0000:00:0e.0",
		.nonatomic = 1,
		.dynamic = 1,
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.trigger = {
			SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST
		},
		.dpcm_playback = 1,
	},

	/* BE  DAI links */
	{
		/* SSP1 - ADAU1467 Playback */
		.name = "SSP1-Codec",
		.id = 0,
		.cpu_dai_name = "SSP1 Pin",
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.platform_name = "0000:00:0e.0",
		.ignore_suspend = 1,
		.dpcm_playback = 1,
		.no_pcm = 1,
	},
	{
		/* SSP2 - ADAU1467 Capture */
		.name = "SSP2-Codec",
		.id = 1,
		.cpu_dai_name = "SSP2 Pin",
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.platform_name = "0000:00:0e.0",
		.ignore_suspend = 1,
		.dpcm_capture = 1,
		.no_pcm = 1,
	},
	{
		/* SSP5 - BT */
		.name = "SSP5-Codec",
		.id = 2,
		.cpu_dai_name = "SSP5 Pin",
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.platform_name = "0000:00:0e.0",
		.ignore_suspend = 1,
		.dpcm_capture = 1,
		.dpcm_playback = 1,
		.no_pcm = 1,
	}
};

static int bxt_add_dai_link(struct snd_soc_card *card,
			struct snd_soc_dai_link *link)
{
	link->platform_name = "0000:00:0e.0";
	link->nonatomic = 1;
	return 0;
}

/* broxton audio machine driver for TDF8532 */
static struct snd_soc_card broxton_hwtc = {
	.name = "broxton_hwtc",
	.dai_link = broxton_hwtc_dais,
	.num_links = ARRAY_SIZE(broxton_hwtc_dais),
	.controls = broxton_hwtc_controls,
	.num_controls = ARRAY_SIZE(broxton_hwtc_controls),
	.dapm_widgets = broxton_hwtc_widgets,
	.num_dapm_widgets = ARRAY_SIZE(broxton_hwtc_widgets),
	.dapm_routes = broxton_hwtc_map,
	.num_dapm_routes = ARRAY_SIZE(broxton_hwtc_map),
	.fully_routed = true,
	.add_dai_link = bxt_add_dai_link,
};

static int broxton_hwtc_audio_probe(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "%s registering %s\n", __func__, pdev->name);
	broxton_hwtc.dev = &pdev->dev;
	return snd_soc_register_card(&broxton_hwtc);
}

static int broxton_hwtc_audio_remove(struct platform_device *pdev)
{
	snd_soc_unregister_card(&broxton_hwtc);
	return 0;
}

static struct platform_driver broxton_hwtc_audio = {
	.probe = broxton_hwtc_audio_probe,
	.remove = broxton_hwtc_audio_remove,
	.driver = {
		.name = "bxt_hwtc",
		.pm = &snd_soc_pm_ops,
	},
};

module_platform_driver(broxton_hwtc_audio)

/* Module information */
MODULE_DESCRIPTION("Intel SST Audio for HWTC CDC");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:bxt_hwtc");
