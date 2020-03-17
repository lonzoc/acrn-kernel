// SPDX-License-Identifier: GPL-2.0
/*
 * FPGA based video splitter drvier
 * 
 * Copyright (C) 2020 HWTC Co.,Ltd.
 */

#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/regmap.h>
#include <linux/acpi.h>
#include <drm/drm_crtc_helper.h>

/*
 * FPGA video splitter receive video stream from other source(SoC or PC), and
 * split it into several sub video stream witch then been used as source to
 * drive several exteranl display monitor. below is a example of three sub
 * video stream.
 *
 * video input (x, y, w, h)
 * +------------------------------------+
 * |                  |                 |
 * |                  |                 |
 * |     output0      |      output1    |
 * |                  |                 |
 * |                  |                 |
 * +------------------------------------+                
 * |                                    |
 * |               output2              |
 * |                                    |
 * +------------------------------------+
 * */

/* Max video output stream */
#define MAX_VIDEO_STREAM 3

/* FPGA video splitter register defination */
#define VS_SYS_STATUS          0x0100 /* system status */
#define VS_INT_STATUS          0x0101 /* interrupt status */
#define VS_STREAM_STATUS_BASE  0x0200
#define VS_DRAM_STATUS         0x0210
/* control register  */
#define VS_DRAM_CTRL           0x0300
#define VS_STREAM_CTRL_BASE    0x0310
#define VS_STREAMIN_MODE_BASE  0x0320
#define VS_STREAMOUT_MODE_BASE 0x0330
/* Test pattern generator */
#define VS_TPG_CTRL            0x0400
#define VS_TPG_R               0x0401
#define VS_TPG_G               0x0402
#define VS_TPG_B               0x0403

struct video_param {
	u16 x;
	u16 y;
	u16 w;
	u16 h;
	u16 fps;
};

struct fpga_vs {
	struct device *dev;
	struct regmap *regmap;
	struct drm_bridge bridge;

	struct video_param video_in;
	struct video_param video_out[MAX_VIDEO_STREAM];
};

#define to_fpga_vs(x) container_of(x, struct fpga_vs, bridge)

static const struct regmap_config regmap_conf = {
	.reg_bits = 16,
	.val_bits = 16,
	.max_register = 0xffff,
	.cache_type = REGCACHE_RBTREE,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,
};

static int fpga_vs_stream_ctrl(struct fpga_vs *vs, unsigned int stream_index,
		bool enable)
{
	struct regmap *rm = vs->regmap;
	u16 val = enable ? 0x0001 : 0x0000;

	return regmap_write(rm, VS_STREAM_CTRL_BASE + stream_index, val);
}

static int fpga_vs_attach(struct drm_bridge *bridge)
{
	struct fpga_vs *vs = to_fpga_vs(bridge);
	int i, ret;

	dev_dbg(vs->dev, "FPGA video splitter attached to encoder(%s)\n",
			vs->bridge.encoder->name);

	for (i = 0; i < MAX_VIDEO_STREAM; i++) {
		ret = fpga_vs_stream_ctrl(vs, i, true);
		if (ret < 0) {
			dev_err(vs->dev, "Unable to enable video stream(%d)\n", i);
		} else {
			dev_info(vs->dev, "Video stream(%d) is enabled\n", i);
		}
	}

	return 0;
}

static void fpga_vs_detach(struct drm_bridge *bridge)
{
	struct fpga_vs *vs = to_fpga_vs(bridge);
	int i, ret;

	dev_dbg(vs->dev, "FPGA video splitter detached from encoder(%s)\n",
			vs->bridge.encoder->name);

	for (i = 0; i < MAX_VIDEO_STREAM; i++) {
		ret = fpga_vs_stream_ctrl(vs, i, false);
		if (ret < 0) {
			dev_err(vs->dev, "Unable to disable video stream(%d)\n", i);
		} else {
			dev_info(vs->dev, "Video stream(%d) is disabled\n", i);
		}
	}
}

static void fpga_vs_enable(struct drm_bridge *bridge)
{
	struct fpga_vs *vs = to_fpga_vs(bridge);

	dev_info(vs->dev, "FPGA video splitter enable\n");
}

static void fpga_vs_disable(struct drm_bridge *bridge)
{
	struct fpga_vs *vs = to_fpga_vs(bridge);

	dev_info(vs->dev, "FPGA video splitter disable\n");
}

static void fpga_vs_mode_set(struct drm_bridge *bridge,
		struct drm_display_mode *mode, struct drm_display_mode *adjusted_mode)
{
	struct fpga_vs *vs = to_fpga_vs(bridge);

	dev_info(vs->dev, "FPGA video splitter mode set\n");
}

static const struct drm_bridge_funcs fpga_vs_bridge_funcs = {
	.attach = fpga_vs_attach,
	.detach = fpga_vs_detach,
	.mode_set = fpga_vs_mode_set,
	.enable = fpga_vs_enable,
	.disable = fpga_vs_disable,
};

static int fpga_vs_configure_video_stream(struct fpga_vs *vs)
{
	struct regmap *rm = vs->regmap;
	int ret, i;
	u16 val[5];

	/* configure input video stream */
	val[0] = vs->video_in.w;
	val[1] = vs->video_in.h;
	val[2] = vs->video_in.fps;
	ret = regmap_bulk_write(rm, VS_STREAMIN_MODE_BASE, val, 3);
	if (ret < 0) {
		dev_err(vs->dev, "Failed to setup input video parameters: %d\n", ret);
		return ret;
	}

	/* configure output video stream */
	for (i = 0; i < MAX_VIDEO_STREAM; i++) {
		val[0] = vs->video_out[i].x;
		val[1] = vs->video_out[i].y;
		val[2] = vs->video_out[i].w;
		val[3] = vs->video_out[i].h;
		val[4] = vs->video_out[i].fps;

		ret = regmap_bulk_write(rm, VS_STREAMOUT_MODE_BASE + 5 * i, val, 5);
		if (ret < 0) {
			dev_err(vs->dev, "Failed to setup output video parameters: %d\n",
					ret);
			return ret;
		}
	}

	return 0;
}

static int fpga_vs_configure(struct fpga_vs *vs)
{
	int ret;

	ret = fpga_vs_configure_video_stream(vs);
	return ret;
}

static int fpga_vs_probe(struct spi_device *spi)
{
	struct fpga_vs *fpga_vs;
	int ret;

	fpga_vs = devm_kzalloc(&spi->dev, sizeof(*fpga_vs), GFP_KERNEL);
	if (!fpga_vs) {
		ret = -ENOMEM;
		goto err;
	}

	fpga_vs->dev = &spi->dev;

	fpga_vs->regmap = devm_regmap_init_spi(spi, &regmap_conf);
	if (IS_ERR(fpga_vs->regmap)) {
		ret = PTR_ERR(fpga_vs->regmap);
		dev_err(fpga_vs->dev, "Failed to initialize regmap: %d", ret);
		goto err_regmap;
	}

	ret = fpga_vs_configure(fpga_vs);
	if (ret < 0) {
		goto err_conf;
	}

	fpga_vs->bridge.funcs = &fpga_vs_bridge_funcs;
	drm_bridge_add(&fpga_vs->bridge);

	dev_set_drvdata(fpga_vs->dev, fpga_vs);

	return 0;
err_regmap:
err_conf:
err:
	return ret;	
}

static const struct spi_device_id spi_id_table[] = {
	{"FPGA_VS", 0},
	{},
};
MODULE_DEVICE_TABLE(spi, spi_id_table);

#ifdef CONFIG_OF
static const struct of_device_id  of_match[] = {
	{.compatible = "hwtc,fpga_vs"},
	{},
};
MODULE_DEVICE_TABLE(of, of_match
#endif

#ifdef CONFIG_ACPI
static const struct acpi_device_id acpi_match[] = {
	{"HWTC0801", 0},
	{},
};
MODULE_DEVICE_TABLE(acpi, acpi_match);
#endif

static struct spi_driver fpga_vs_drv = {
	.driver = {
		.name = "FPGA_VS",
		.of_match_table = of_match_ptr(of_match),
		.acpi_match_table = ACPI_PTR(acpi_match),
	},
	.probe = fpga_vs_probe,
	.id_table = spi_id_table,
};
module_spi_driver(fpga_vs_drv);

MODULE_AUTHOR("Yulong Cai <yulongc@hwtc.com.cn>");
MODULE_DESCRIPTION("HWTC FPGA based video splitter driver");
MODULE_LICENSE("GPL v2");
