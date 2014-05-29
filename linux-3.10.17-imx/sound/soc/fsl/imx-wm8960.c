/*
 * Copyright (C) 2014 Freescale Semiconductor, Inc.
 *
 * Based on imx-wm8962.c
 * Copyright (C) 2012 Freescale Semiconductor, Inc.
 * Copyright (C) 2012 Linaro Ltd.
 *
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of_i2c.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/clk.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include <sound/pcm_params.h>
#include <sound/soc-dapm.h>
#include <linux/pinctrl/consumer.h>

#include "../codecs/wm8960.h"
#include "imx-audmux.h"

#define DAI_NAME_SIZE	32

struct imx_wm8960_data {
	struct snd_soc_dai_link dai;
	struct snd_soc_card card;
	char codec_dai_name[DAI_NAME_SIZE];
	char platform_name[DAI_NAME_SIZE];
	struct clk *codec_mclk;
};

struct imx_priv {
	int hp_gpio;
	int hp_active_low;
	int mic_gpio;
	int mic_active_low;
	struct snd_soc_codec *codec;
	struct platform_device *pdev;
	struct snd_pcm_substream *first_stream;
	struct snd_pcm_substream *second_stream;
};
static struct imx_priv card_priv;

static struct snd_soc_jack imx_hp_jack;
static struct snd_soc_jack_pin imx_hp_jack_pins[] = {
	{
		.pin = "Headphone Jack",
		.mask = SND_JACK_HEADPHONE,
	},
};
static struct snd_soc_jack_gpio imx_hp_jack_gpio = {
	.name = "headphone detect",
	.report = SND_JACK_HEADPHONE,
	.debounce_time = 250,
	.invert = 1,
};

static struct snd_soc_jack imx_mic_jack;
static struct snd_soc_jack_pin imx_mic_jack_pins[] = {
	{
		.pin = "AMIC",
		.mask = SND_JACK_MICROPHONE,
	},
};
static struct snd_soc_jack_gpio imx_mic_jack_gpio = {
	.name = "microphone detect",
	.report = SND_JACK_MICROPHONE,
	.debounce_time = 250,
	.invert = 0,
};

static int hpjack_status_check(void)
{
	struct imx_priv *priv = &card_priv;
	struct platform_device *pdev = priv->pdev;
	char *envp[3], *buf;
	int hp_status, ret;

	if (!gpio_is_valid(priv->hp_gpio))
		return 0;

	hp_status = gpio_get_value(priv->hp_gpio) ? 1 : 0;

	buf = kmalloc(32, GFP_ATOMIC);
	if (!buf) {
		dev_err(&pdev->dev, "%s kmalloc failed\n", __func__);
		return -ENOMEM;
	}

	if (hp_status != priv->hp_active_low) {
		snprintf(buf, 32, "STATE=%d", 2);
		snd_soc_dapm_disable_pin(&priv->codec->dapm, "Ext Spk");
		ret = imx_hp_jack_gpio.report;
	} else {
		snprintf(buf, 32, "STATE=%d", 0);
		snd_soc_dapm_enable_pin(&priv->codec->dapm, "Ext Spk");
		ret = 0;
	}

	envp[0] = "NAME=headphone";
	envp[1] = buf;
	envp[2] = NULL;
	kobject_uevent_env(&pdev->dev.kobj, KOBJ_CHANGE, envp);
	kfree(buf);

	return ret;
}

static int micjack_status_check(void)
{
	struct imx_priv *priv = &card_priv;
	struct platform_device *pdev = priv->pdev;
	char *envp[3], *buf;
	int mic_status, ret;

	if (!gpio_is_valid(priv->mic_gpio))
		return 0;

	mic_status = gpio_get_value(priv->mic_gpio) ? 1 : 0;

	buf = kmalloc(32, GFP_ATOMIC);
	if (!buf) {
		dev_err(&pdev->dev, "%s kmalloc failed\n", __func__);
		return -ENOMEM;
	}

	if (mic_status != priv->mic_active_low) {
		snprintf(buf, 32, "STATE=%d", 2);
		snd_soc_dapm_disable_pin(&priv->codec->dapm, "DMIC");
		ret = imx_mic_jack_gpio.report;
	} else {
		snprintf(buf, 32, "STATE=%d", 0);
		snd_soc_dapm_enable_pin(&priv->codec->dapm, "DMIC");
		ret = 0;
	}

	envp[0] = "NAME=microphone";
	envp[1] = buf;
	envp[2] = NULL;
	kobject_uevent_env(&pdev->dev.kobj, KOBJ_CHANGE, envp);
	kfree(buf);

	return ret;
}


static const struct snd_soc_dapm_widget imx_wm8960_dapm_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_SPK("Ext Spk", NULL),
	SND_SOC_DAPM_MIC("AMIC", NULL),
	SND_SOC_DAPM_MIC("DMIC", NULL),
};

static int imx_hifi_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_card *card = codec_dai->codec->card;
	struct imx_wm8960_data *data = snd_soc_card_get_drvdata(card);

	if (!codec_dai->active)
		clk_enable(data->codec_mclk);

	return 0;
}

static void imx_hifi_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_card *card = codec_dai->codec->card;
	struct imx_wm8960_data *data = snd_soc_card_get_drvdata(card);

	if (!codec_dai->active)
		clk_disable(data->codec_mclk);

	return;
}

static int imx_hifi_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct imx_priv *priv = &card_priv;
	struct device *dev = &priv->pdev->dev;
	struct snd_soc_card *card = codec_dai->codec->card;
	struct imx_wm8960_data *data = snd_soc_card_get_drvdata(card);
	unsigned int sample_rate = params_rate(params);
	snd_pcm_format_t sample_format = params_format(params);
	u32 dai_format, pll_out;
	int ret = 0;

	if (!priv->first_stream) {
		priv->first_stream = substream;
	} else {
		priv->second_stream = substream;

		/* We suppose the two substream are using same params */
		return 0;
	}

	dai_format = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
		SND_SOC_DAIFMT_CBM_CFM;

	/* set codec DAI configuration */
	ret = snd_soc_dai_set_fmt(codec_dai, dai_format);
	if (ret) {
		dev_err(dev, "failed to set codec dai fmt: %d\n", ret);
		return ret;
	}

	pll_out = sample_rate * 256 * 2;

	ret = snd_soc_dai_set_pll(codec_dai, 0, 0,
			clk_get_rate(data->codec_mclk), pll_out);
	if (ret) {
		dev_err(dev, "failed to start PLL: %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_clkdiv(codec_dai, WM8960_SYSCLKDIV,
			WM8960_SYSCLK_DIV_2);
	if (ret) {
		dev_err(dev, "failed to set SYSCLKDIV: %d\n", ret);
		return ret;
	}

	return 0;
}

static int imx_hifi_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct imx_priv *priv = &card_priv;
	struct device *dev = &priv->pdev->dev;
	int ret;

	/* We don't need to handle anything if there's no substream running */
	if (!priv->first_stream)
		return 0;

	if (priv->first_stream == substream)
		priv->first_stream = priv->second_stream;
	priv->second_stream = NULL;

	if (!priv->first_stream) {
		/*
		 * Continuously setting FLL would cause playback distortion.
		 * We can fix it just by mute codec after playback.
		 */
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			snd_soc_dai_digital_mute(codec_dai, 1, substream->stream);

		/* Disable FLL and let codec do pm_runtime_put() */
		ret = snd_soc_dai_set_pll(codec_dai, 0, 0, 0, 0);
		if (ret < 0) {
			dev_err(dev, "failed to stop PLL: %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static struct snd_soc_ops imx_hifi_ops = {
	.startup = imx_hifi_startup,
	.shutdown = imx_hifi_shutdown,
	.hw_params = imx_hifi_hw_params,
	.hw_free = imx_hifi_hw_free,
};

static int imx_wm8960_gpio_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_codec *codec = rtd->codec;
	struct imx_priv *priv = &card_priv;

	priv->codec = codec;

	if (gpio_is_valid(priv->hp_gpio)) {
		imx_hp_jack_gpio.gpio = priv->hp_gpio;
		imx_hp_jack_gpio.jack_status_check = hpjack_status_check;

		snd_soc_jack_new(codec, "Headphone Jack", SND_JACK_HEADPHONE, &imx_hp_jack);
		snd_soc_jack_add_pins(&imx_hp_jack,
				ARRAY_SIZE(imx_hp_jack_pins), imx_hp_jack_pins);
		snd_soc_jack_add_gpios(&imx_hp_jack, 1, &imx_hp_jack_gpio);
	}

	if (gpio_is_valid(priv->mic_gpio)) {
		imx_mic_jack_gpio.gpio = priv->mic_gpio;
		imx_mic_jack_gpio.jack_status_check = micjack_status_check;

		snd_soc_jack_new(codec, "AMIC", SND_JACK_MICROPHONE, &imx_mic_jack);
		snd_soc_jack_add_pins(&imx_mic_jack,
				ARRAY_SIZE(imx_mic_jack_pins), imx_mic_jack_pins);
		snd_soc_jack_add_gpios(&imx_mic_jack, 1, &imx_mic_jack_gpio);
	}

	return 0;
}

static ssize_t show_headphone(struct device_driver *dev, char *buf)
{
	struct imx_priv *priv = &card_priv;
	int hp_status;

	if (!gpio_is_valid(priv->hp_gpio)) {
		strcpy(buf, "no detect gpio connected\n");
		return strlen(buf);
	}

	/* Check if headphone is plugged in */
	hp_status = gpio_get_value(priv->hp_gpio) ? 1 : 0;

	if (hp_status != priv->hp_active_low)
		strcpy(buf, "headphone\n");
	else
		strcpy(buf, "speaker\n");

	return strlen(buf);
}

static DRIVER_ATTR(headphone, S_IRUGO | S_IWUSR, show_headphone, NULL);

static ssize_t show_mic(struct device_driver *dev, char *buf)
{
	struct imx_priv *priv = &card_priv;
	int mic_status;

	if (!gpio_is_valid(priv->mic_gpio)) {
		strcpy(buf, "no detect gpio connected\n");
		return strlen(buf);
	}

	/* Check if analog microphone is plugged in */
	mic_status = gpio_get_value(priv->mic_gpio) ? 1 : 0;

	if (mic_status != priv->mic_active_low)
		strcpy(buf, "amic\n");
	else
		strcpy(buf, "dmic\n");

	return strlen(buf);
}

static DRIVER_ATTR(microphone, S_IRUGO | S_IWUSR, show_mic, NULL);

static int imx_wm8960_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *ssi_np, *codec_np;
	struct platform_device *ssi_pdev;
	struct imx_priv *priv = &card_priv;
	struct i2c_client *codec_dev;
	struct imx_wm8960_data *data;
	int int_port, ext_port;
	int ret;

	priv->pdev = pdev;

	ret = of_property_read_u32(np, "mux-int-port", &int_port);
	if (ret) {
		dev_err(&pdev->dev, "mux-int-port missing or invalid\n");
		return ret;
	}
	ret = of_property_read_u32(np, "mux-ext-port", &ext_port);
	if (ret) {
		dev_err(&pdev->dev, "mux-ext-port missing or invalid\n");
		return ret;
	}

	/*
	 * The port numbering in the hardware manual starts at 1, while
	 * the audmux API expects it starts at 0.
	 */
	int_port--;
	ext_port--;
	ret = imx_audmux_v2_configure_port(int_port,
			IMX_AUDMUX_V2_PTCR_SYN |
			IMX_AUDMUX_V2_PTCR_TFSEL(ext_port) |
			IMX_AUDMUX_V2_PTCR_TCSEL(ext_port) |
			IMX_AUDMUX_V2_PTCR_TFSDIR |
			IMX_AUDMUX_V2_PTCR_TCLKDIR,
			IMX_AUDMUX_V2_PDCR_RXDSEL(ext_port));
	if (ret) {
		dev_err(&pdev->dev, "audmux internal port setup failed\n");
		return ret;
	}
	imx_audmux_v2_configure_port(ext_port,
			IMX_AUDMUX_V2_PTCR_SYN,
			IMX_AUDMUX_V2_PDCR_RXDSEL(int_port));
	if (ret) {
		dev_err(&pdev->dev, "audmux external port setup failed\n");
		return ret;
	}

	ssi_np = of_parse_phandle(pdev->dev.of_node, "ssi-controller", 0);
	codec_np = of_parse_phandle(pdev->dev.of_node, "audio-codec", 0);
	if (!ssi_np || !codec_np) {
		dev_err(&pdev->dev, "phandle missing or invalid\n");
		ret = -EINVAL;
		goto fail;
	}

	ssi_pdev = of_find_device_by_node(ssi_np);
	if (!ssi_pdev) {
		dev_err(&pdev->dev, "failed to find SSI platform device\n");
		ret = -EINVAL;
		goto fail;
	}
	codec_dev = of_find_i2c_device_by_node(codec_np);
	if (!codec_dev || !codec_dev->driver) {
		dev_err(&pdev->dev, "failed to find codec platform device\n");
		ret = -EINVAL;
		goto fail;
	}

	priv->first_stream = NULL;
	priv->second_stream = NULL;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
		goto fail;
	}

	data->codec_mclk = devm_clk_get(&codec_dev->dev, NULL);
	if (IS_ERR(data->codec_mclk)) {
		ret = PTR_ERR(data->codec_mclk);
		dev_err(&codec_dev->dev, "failed to get codec clk: %d\n", ret);
		goto fail;
	}
	clk_prepare(data->codec_mclk);

	priv->hp_gpio = of_get_named_gpio_flags(np, "hp-det-gpios", 0,
				(enum of_gpio_flags *)&priv->hp_active_low);
	priv->mic_gpio = of_get_named_gpio_flags(np, "mic-det-gpios", 0,
				(enum of_gpio_flags *)&priv->mic_active_low);

	data->dai.name = "HiFi";
	data->dai.stream_name = "HiFi";
	data->dai.codec_dai_name = "wm8960-hifi";
	data->dai.codec_of_node = codec_np;
	data->dai.cpu_dai_name = dev_name(&ssi_pdev->dev);
	data->dai.platform_of_node = ssi_np;
	data->dai.ops = &imx_hifi_ops;
	data->dai.init = &imx_wm8960_gpio_init;
	data->dai.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			    SND_SOC_DAIFMT_CBM_CFM;

	data->card.dev = &pdev->dev;
	ret = snd_soc_of_parse_card_name(&data->card, "model");
	if (ret)
		goto fail_clk;
	ret = snd_soc_of_parse_audio_routing(&data->card, "audio-routing");
	if (ret)
		goto fail_clk;
	data->card.num_links = 1;
	data->card.dai_link = &data->dai;
	data->card.dapm_widgets = imx_wm8960_dapm_widgets;
	data->card.num_dapm_widgets = ARRAY_SIZE(imx_wm8960_dapm_widgets);

	platform_set_drvdata(pdev, &data->card);
	snd_soc_card_set_drvdata(&data->card, data);

	ret = snd_soc_register_card(&data->card);
	if (ret) {
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n", ret);
		goto fail_clk;
	}

	if (gpio_is_valid(priv->hp_gpio)) {
		ret = driver_create_file(pdev->dev.driver, &driver_attr_headphone);
		if (ret) {
			dev_err(&pdev->dev, "create hp attr failed (%d)\n", ret);
			goto fail_hp;
		}
	}

	if (gpio_is_valid(priv->mic_gpio)) {
		ret = driver_create_file(pdev->dev.driver, &driver_attr_microphone);
		if (ret) {
			dev_err(&pdev->dev, "create mic attr failed (%d)\n", ret);
			goto fail_mic;
		}
	}

	goto fail;

fail_mic:
	driver_remove_file(pdev->dev.driver, &driver_attr_headphone);
fail_hp:
	snd_soc_unregister_card(&data->card);
fail_clk:
	clk_unprepare(data->codec_mclk);
fail:
	if (ssi_np)
		of_node_put(ssi_np);
	if (codec_np)
		of_node_put(codec_np);

	return ret;
}

static int imx_wm8960_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct imx_wm8960_data *data = snd_soc_card_get_drvdata(card);

	clk_unprepare(data->codec_mclk);

	driver_remove_file(pdev->dev.driver, &driver_attr_microphone);
	driver_remove_file(pdev->dev.driver, &driver_attr_headphone);

	snd_soc_unregister_card(card);

	return 0;
}

static const struct of_device_id imx_wm8960_dt_ids[] = {
	{ .compatible = "fsl,imx-audio-wm8960", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx_wm8960_dt_ids);

static struct platform_driver imx_wm8960_driver = {
	.driver = {
		.name = "imx-wm8960",
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
		.of_match_table = imx_wm8960_dt_ids,
	},
	.probe = imx_wm8960_probe,
	.remove = imx_wm8960_remove,
};
module_platform_driver(imx_wm8960_driver);

MODULE_AUTHOR("Freescale Semiconductor, Inc.");
MODULE_DESCRIPTION("Freescale i.MX WM8960 ASoC machine driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:imx-wm8960");
