// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim MAX96724 Quad GMSL2 Deserializer Driver
 *
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of_graph.h>
#include <linux/regmap.h>

#include "max_des.h"

static const struct regmap_config max96724_i2c_regmap = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0x1f00,
};

struct max96724_priv {
	struct max_des_priv des_priv;

	struct regmap *regmap;
	struct gpio_desc *gpiod_pwdn;
};

#define des_to_priv(des) \
	container_of(des, struct max96724_priv, des_priv)

static int max96724_read(struct max96724_priv *priv, int reg)
{
	int ret, val;

	ret = regmap_read(priv->regmap, reg, &val);
	if (ret) {
		dev_err(priv->dev, "read 0x%04x failed\n", reg);
		return ret;
	}

	return val;
}

static int max96724_write(struct max96724_priv *priv, unsigned int reg, u8 val)
{
	int ret;

	ret = regmap_write(priv->regmap, reg, val);
	if (ret)
		dev_err(priv->dev, "write 0x%04x failed\n", reg);

	return ret;
}

static int max96724_update_bits(struct max96724_priv *priv, unsigned int reg,
			        u8 mask, u8 val)
{
	int ret;

	ret = regmap_update_bits(priv->regmap, reg, mask, val);
	if (ret)
		dev_err(priv->dev, "update 0x%04x failed\n", reg);

	return ret;
}

static int max96724_wait_for_device(struct max_des_priv *priv)
{
	unsigned int i, val;
	int ret;

	for (i = 0; i < 100; i++) {
		ret = max96724_read(priv, 0x0);
		if (ret >= 0)
			return 0;

		msleep(10);

		dev_err(priv->dev, "Retry %u waiting for deserializer: %d\n", i, ret);
	}

	return ret;
}

static int max96724_reset(struct max_des_priv *priv)
{
	int ret;

	ret = max96724_wait_for_device(priv);
	if (ret)
		return ret;

	ret = max96724_update_bits(priv, 0x13, 0x40, 0x40);
	if (ret)
		return ret;

	ret = max96724_wait_for_device(priv);
	if (ret)
		return ret;

	return 0;
}

static int max96724_probe(struct i2c_client *client)
{
	struct max96724_priv *priv;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	i2c_set_clientdata(client, priv);

	priv->regmap = devm_regmap_init_i2c(client, &max96724_i2c_regmap);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	priv->gpiod_pwdn = devm_gpiod_get_optional(&client->dev, "enable",
						   GPIOD_OUT_HIGH);
	if (IS_ERR(priv->gpiod_pwdn))
		return PTR_ERR(priv->gpiod_pwdn);

	gpiod_set_consumer_name(priv->gpiod_pwdn, "max96724-pwdn");
	gpiod_set_value_cansleep(priv->gpiod_pwdn, 1);

	if (priv->gpiod_pwdn)
		usleep_range(4000, 5000);

	priv->des_priv.dev = &client->dev;
	priv->des_priv.client = client;
	priv->des_priv.regmap = priv->regmap;

	ret = max96724_reset(priv);
	if (ret)
		return ret;

	return max_des_probe(&priv->des_priv);
}

static int max96724_remove(struct i2c_client *client)
{
	struct max96724_priv *priv = i2c_get_clientdata(client);

	gpiod_set_value_cansleep(priv->gpiod_pwdn, 0);

	return max_des_remove(&priv->des_priv);
}

static const struct of_device_id max96724_of_table[] = {
	{ .compatible = "maxim,max96724" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, max96724_of_table);

static struct i2c_driver max96724_i2c_driver = {
	.driver	= {
		.name = "max96724",
		.of_match_table	= of_match_ptr(max96724_of_table),
	},
	.probe_new = max96724_probe,
	.remove = max96724_remove,
};

module_i2c_driver(max96724_i2c_driver);

MODULE_DESCRIPTION("Maxim MAX96724 Quad GMSL2 Deserializer Driver");
MODULE_AUTHOR("Cosmin Tanislav <cosmin.tanislav@analog.com>");
MODULE_LICENSE("GPL");
