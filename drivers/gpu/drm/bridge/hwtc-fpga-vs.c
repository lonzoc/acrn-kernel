// SPDX-License-Identifier: GPL-2.0
/*
 * FPGA based video splitter drvier
 *
 * Copyright (C) 2020 HWTC Co.,Ltd.
 */

#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/regmap.h>
#include <linux/property.h>
#include <linux/acpi.h>
#include <linux/sysfs.h>
#include <linux/gpio/consumer.h>
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
 * |     output0      |      output1    |
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
#define VS_TPG_COLOR           0x0401  /* RGB, R first, B last*/

/* interrupt status bits */
#define SYSTEM_ERR             BIT(0)
#define DRAM_ERR               BIT(1)
#define VIDEO_STREAM_ERR       BIT(2)

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
	struct gpio_desc *irq_gpio;

	struct video_param video_in;
	struct video_param video_out[MAX_VIDEO_STREAM];

	/* status */
	u16 sys_status;
	u16 dram_status;
	u16 video_stream_status[MAX_VIDEO_STREAM];
};

static const struct regmap_config regmap_conf = {
	.reg_bits = 16,
	.val_bits = 16,
	.max_register = 0xffff,
	.cache_type = REGCACHE_RBTREE,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,
};

typedef int (*fpga_vs_handler_t)(struct fpga_vs *);
#define to_fpga_vs(x) container_of(x, struct fpga_vs, bridge)

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

/* sysfs attributies */
static ssize_t sys_status_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fpga_vs *vs = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%x\n", vs->sys_status);
}
DEVICE_ATTR_RO(sys_status);

static ssize_t dram_status_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fpga_vs *vs = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%x\n", vs->dram_status);
}
DEVICE_ATTR_RO(dram_status);

static ssize_t video_stream_status_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fpga_vs *vs = dev_get_drvdata(dev);
	int i, n;

	for (i = 0, n = 0; i < MAX_VIDEO_STREAM; i++) {
		n += scnprintf(buf + n, PAGE_SIZE - n, "%x,",
				vs->video_stream_status[i]);
	}

	return n;
}
DEVICE_ATTR_RO(video_stream_status);

static ssize_t test_pattern_ctrl_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fpga_vs *vs = dev_get_drvdata(dev);
	struct regmap *rm = vs->regmap;
	unsigned int val;
	int ret, n;

	ret = regmap_read(rm, VS_TPG_CTRL, &val);
	if (ret < 0) {
		return ret;
	}

	n = scnprintf(buf, PAGE_SIZE, "%x\n", (u16)val);
	n += scnprintf(buf + n, PAGE_SIZE - n,
			"bit0   | 0x1:enable test pattern, 0x0:disable test pattern\n");
	n += scnprintf(buf + n, PAGE_SIZE - n,
			"bit1~2 | 0x00:color bar, "
			"0x01:color defined by test_pattern_color\n");

	return n;
}

static ssize_t test_pattern_ctrl_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct fpga_vs *vs = dev_get_drvdata(dev);
	struct regmap *rm = vs->regmap;
	unsigned int ctrl;
	int ret;

	ret = kstrtou32(buf, 0, &ctrl);
	if (ret < 0) {
		return ret;
	}

	ret = regmap_write(rm, VS_TPG_CTRL, ctrl);
	if (ret < 0) {
		return ret;
	}

	return size;
}
DEVICE_ATTR_RW(test_pattern_ctrl);

static ssize_t test_pattern_color_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fpga_vs *vs = dev_get_drvdata(dev);
	struct regmap *rm = vs->regmap;
	u16 val[3];
	u32 rgb888;
	int ret, n;

	ret = regmap_bulk_read(rm, VS_TPG_COLOR, val, ARRAY_SIZE(val));
	if (ret < 0) {
		return ret;
	}

	rgb888 = (val[0] & 0xff) << 16;
	rgb888 |= (val[1] & 0xff) << 8;
	rgb888 |= val[2] & 0xff;
	n = scnprintf(buf, PAGE_SIZE, "0x%x\n", rgb888);
	n += scnprintf(buf + n, PAGE_SIZE - n,
			"RGB888 color setting, e.g. 0xAABBCC -> R=0xAA G=0xBB B=0xCC\n");

	return n;
}

static ssize_t test_pattern_color_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct fpga_vs *vs = dev_get_drvdata(dev);
	struct regmap *rm = vs->regmap;
	u32 rgb888;
	u16 rgb[3];
	int ret;

	ret = kstrtou32(buf, 0, &rgb888);
	if (ret < 0) {
		return ret;
	}

	rgb[0] = (rgb888 >> 16 ) & 0xff;
	rgb[1] = (rgb888 >> 8 ) & 0xff;
	rgb[2] = rgb888 & 0xff;
	ret = regmap_bulk_write(rm, VS_TPG_COLOR, rgb, ARRAY_SIZE(rgb));
	if (ret < 0) {
		return ret;
	}

	return size;
}
DEVICE_ATTR_RW(test_pattern_color);

static struct attribute *fpga_vs_attrs[] = {
	&dev_attr_sys_status.attr,
	&dev_attr_dram_status.attr,
	&dev_attr_video_stream_status.attr,
	&dev_attr_test_pattern_ctrl.attr,
	&dev_attr_test_pattern_color.attr,
	NULL
};

static const struct attribute_group fpga_vs_attr_group = {
	.attrs = fpga_vs_attrs,
};

static int fpga_vs_system_err_handler(struct fpga_vs *vs)
{
	vs->sys_status = 0xDEAD;

	// TODO: do a hard reset to recovery the FPGA
	return 0;
}

static int fpga_vs_dram_err_handler(struct fpga_vs *vs)
{
	struct regmap *rm = vs->regmap;
	unsigned int val;
	int ret;

	ret = regmap_read(rm, VS_DRAM_STATUS, &val);
	if (!ret) {
		vs->dram_status = val;
		kobject_uevent(&vs->dev->kobj, KOBJ_CHANGE);
	}

	return ret;
}

static int fpga_vs_stream_err_handler(struct fpga_vs *vs)
{
	struct regmap *rm = vs->regmap;
	u16 val[MAX_VIDEO_STREAM];
	int ret;

	// TODO: read all stream status
	ret = regmap_bulk_read(rm, VS_STREAM_STATUS_BASE, val, MAX_VIDEO_STREAM);
	if (!ret) {
		memcpy(vs->video_stream_status, val, sizeof(val));
		kobject_uevent(&vs->dev->kobj, KOBJ_CHANGE);
	}

	return ret;
}

/* keep the bit order same with VS_INT_STATUS register defination */
static const fpga_vs_handler_t fpga_vs_handlers[] = {
	[0] = fpga_vs_system_err_handler,
	[1] = fpga_vs_dram_err_handler,
	[2] = fpga_vs_stream_err_handler,
};

static irqreturn_t fpga_vs_irq_thread(int irq, void *data)
{
	struct fpga_vs *vs =  data;
	struct regmap *rm = vs->regmap;
	fpga_vs_handler_t handler;
	unsigned int val;
	int ret, i;

	ret = regmap_read(rm, VS_INT_STATUS, &val);
	if (ret < 0) {
		goto out;
	}

	for (i = 0;  i < ARRAY_SIZE(fpga_vs_handlers); i++) {
		if (val & BIT(i)) {
			handler = fpga_vs_handlers[i];
			if (handler) {
				ret = handler(vs);
				// TODO: we should check ret
			}
		}
	}

out:
	return IRQ_HANDLED;
}

static int fpga_vs_get_device_property(struct fpga_vs *vs)
{
	struct device *dev = vs->dev;
	struct video_param *vp;
	u16 val[5 * MAX_VIDEO_STREAM];
	int ret, i;

	ret = device_property_read_u16_array(dev, "video-in-param", val, 5);
	if (ret < 0) {
		dev_err(dev, "Failed to get 'video-in-param': %d\n", ret);
		return ret;
	}

	vp = &vs->video_in;
	vp->x = val[0];
	vp->y = val[1];
	vp->w = val[2];
	vp->h = val[3];
	vp->fps = val[4];

	ret = device_property_read_u16_array(dev, "video-out-param", val,
			5 * MAX_VIDEO_STREAM);
	if (ret < 0) {
		dev_err(dev, "Failed to get 'video-out-param': %d\n", ret);
		return ret;
	}

	for (i = 0; i < MAX_VIDEO_STREAM; i++) {
		vp = &vs->video_out[i];
		vp->x = val[0 + i * 5];
		vp->y = val[1 + i * 5];
		vp->w = val[2 + i * 5];
		vp->h = val[3 + i * 5];
		vp->fps = val[4 + i * 5];
	}

	return 0;
}

static const struct drm_bridge_funcs fpga_vs_bridge_funcs = {
	.attach = fpga_vs_attach,
	.detach = fpga_vs_detach,
	.mode_set = fpga_vs_mode_set,
	.enable = fpga_vs_enable,
	.disable = fpga_vs_disable,
};

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

	ret = fpga_vs_get_device_property(fpga_vs);
	if (ret < 0) {
		dev_err(fpga_vs->dev, "Failed to get device properties: %d\n", ret);
		goto err;
	}

	fpga_vs->regmap = devm_regmap_init_spi(spi, &regmap_conf);
	if (IS_ERR(fpga_vs->regmap)) {
		ret = PTR_ERR(fpga_vs->regmap);
		dev_err(fpga_vs->dev, "Failed to initialize regmap: %d", ret);
		goto err;
	}

	fpga_vs->irq_gpio = devm_gpiod_get_index(fpga_vs->dev, "irq", 0, GPIOD_IN);
	if (IS_ERR(fpga_vs->irq_gpio)) {
		ret = PTR_ERR(fpga_vs->irq_gpio);
		goto err;
	}

	ret = gpiod_to_irq(fpga_vs->irq_gpio);
	if (ret < 0) {
		goto err;
	}

	ret = devm_request_threaded_irq(fpga_vs->dev, ret, NULL,
			fpga_vs_irq_thread, IRQF_TRIGGER_LOW, NULL, fpga_vs);
	if (ret < 0) {
		goto err;
	}

	ret = fpga_vs_configure(fpga_vs);
	if (ret < 0) {
		goto err;
	}

	ret = sysfs_create_group(&fpga_vs->dev->kobj, &fpga_vs_attr_group);
	if (ret < 0) {
		dev_err(fpga_vs->dev, "Failed to create sysfs files\n");
		goto err;
	}

	fpga_vs->bridge.funcs = &fpga_vs_bridge_funcs;
	drm_bridge_add(&fpga_vs->bridge);

	dev_set_drvdata(fpga_vs->dev, fpga_vs);

	return 0;
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
