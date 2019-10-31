// SPDX-License-Identifier: GPL-2.0
/*
 * I2C translayer driver
 *
 * Copyright (C) 2018 HWTC Ltd.
 *    Yulong Cai <yulongc@hwt.com.cn>
 *
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/regmap.h>
#include <linux/i2c.h>
#include <linux/acpi.h>
//#include <linux/interrupt.h>

struct ti_devdata {
	struct device *dev;
	struct regmap *regmap;
	struct regmap *regmap_remote;
};

#if 0
static irqreturn_t  ti_i2c_irq(int irq, void *data)
{
	static int cnt = 0;
	pr_err("==hwtc: %s:irq triggered %d\n", __func__, cnt++);
	return IRQ_HANDLED;
}
#endif

static const struct regmap_config ti_regmap_config = {
	.reg_bits = 8,
	.val_bits =8,
	.max_register = 0xff,
	.cache_type = REGCACHE_RBTREE,
};

/* TI SerDes registers */
#define TISER_ID 0x00
#define TISER_GENCFG 0x03
#define TISER_SLAVE0_ID 0x07
#define TISER_SLAVE0_ALIAS 0x08
#define TISER_REG_GPIO0 0x0D
#define TISER_REG_GPIO1_2 0x0E
#define TISER_REG_I2CCTL 0x17
#define TISER_REG_SCL_HIGHTIME 0x26
#define TISER_REG_SCL_LOWTIME 0x27

static int ti_des_set_i2cclk(struct ti_devdata *devdata)
{
	struct regmap  *regmap = devdata->regmap_remote;

	/* I2C clock frequency setting */
	regmap_write(regmap, 0x26, 0x14);
	regmap_write(regmap, 0x27, 0x14);

	return 0;
}

static int ti_des_init(struct ti_devdata *devdata)
{
	struct regmap *regmap = devdata->regmap_remote;
	int ret;
	u32 val;

	ret = regmap_read(regmap, TISER_ID, &val);
	if (ret < 0) {
		dev_err(devdata->dev, "communication error: %d", ret);
		return ret;
	}

	dev_info(devdata->dev, "TIDES ID 0x%02x", val);

	/* enable I2C pass-through mode */
	regmap_update_bits(regmap, TISER_GENCFG, BIT(3), BIT(3));

	/* configure GPIO0:TP_RESET, GPIO remote-control + output mode */
	regmap_update_bits(regmap, 0x1D, GENMASK(2, 0), 0x05);

	return 0;
}

static int ti_new_remote(struct ti_devdata *devdata, unsigned short addr)
{
	struct i2c_client *c = to_i2c_client(devdata->dev);
	struct i2c_client *remote;
	struct i2c_board_info binfo = {I2C_BOARD_INFO("rmt", addr)};

	remote = i2c_new_device(c->adapter, &binfo);
	devdata->regmap_remote = devm_regmap_init_i2c(remote, &ti_regmap_config);

	return devdata->regmap_remote ? 0:-ENODEV;
}

static int ti_ser_init(struct ti_devdata *devdata)
{
	struct regmap  *regmap = devdata->regmap;
	int ret;
	u32 val;

	ret = regmap_read(regmap, TISER_ID, &val);
	if (ret < 0) {
		dev_err(devdata->dev, "communication error: %d", ret);
		return ret;
	}

	dev_info(devdata->dev, "TISER ID 0x%02x", val);

	/* enable I2C pass-through mode */
	regmap_update_bits(regmap, TISER_GENCFG, BIT(3), BIT(3));
	/* pass all i2c slave device */
	regmap_update_bits(regmap, TISER_REG_I2CCTL, BIT(7), BIT(7));

	/* configure GPIO0:TP_RESET, GPIO input mode */
	regmap_update_bits(regmap, TISER_REG_GPIO0, GENMASK(2, 0), 0x03);

	ret = ti_new_remote(devdata, 0x30);
	if (ret < 0) {
		dev_err(devdata->dev, "failed to create remote (%d)", ret);
	}

	return ret;
}

static int ti_i2c_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	struct ti_devdata *devdata;
	int ret;

	devdata = devm_kzalloc(&client->dev, sizeof(*devdata), GFP_KERNEL);
	if (!devdata) {
		dev_err(&client->dev, "No memory for devdata\n");
		ret = -ENOMEM;
		goto err_mem;
	}

	devdata->dev = &client->dev;

	devdata->regmap = devm_regmap_init_i2c(client, &ti_regmap_config);
	if (IS_ERR(devdata->regmap)) {
		ret = PTR_ERR(devdata->regmap);
		dev_err(&client->dev, "regmap init failed, err %d\n", ret);
		goto err_regmap;
	}

	ret = ti_ser_init(devdata);
	if (ret < 0) {
		dev_err(&client->dev, "failed to int serializer (%d)", ret);
		goto err_regmap;
	}

	/*
	 * TODO: write a standalone driver to driver ti-948 deserializer IC
	 */
	ret = ti_des_init(devdata);
	ret |= ti_des_set_i2cclk(devdata);

	return ret;
err_regmap:
err_mem:
	return ret;
}

#ifdef CONFIG_ACPI
const struct acpi_device_id ti_acpi_devid[] = {
	{"TXNW0949", 0},
	{},
};
MODULE_DEVICE_TABLE(acpi, ti_acpi_devid);
#endif

static struct i2c_driver ti_i2c_driver = {
	.driver = {
		.name = "hwtc-i2c",
		.acpi_match_table = ACPI_PTR(ti_acpi_devid),
	},
	.probe = ti_i2c_probe,
};

module_i2c_driver(ti_i2c_driver);

MODULE_DESCRIPTION("TI SerDes driver for TI949");
MODULE_AUTHOR("Yulong Cai <yulongc@hwtc.com.cn>");
MODULE_LICENSE("GPL v2");
