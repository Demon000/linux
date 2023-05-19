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

	struct device *dev;
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

static int max96724_mux_select(struct max_des_priv *des_priv, unsigned int link)
{
	struct max96724_priv *priv = des_to_priv(des_priv);
	int ret;

	ret = max96724_write(priv, 0x3, (~BIT(link * 2)) & 0xff);
	if (ret)
		return ret;

	usleep_range(3000, 5000);

	return 0;
}

static int max96724_mipi_enable(struct max_des_priv *des_priv, bool enable)
{
	struct max96724_priv *priv = des_to_priv(des_priv);
	int ret;

	if (enable) {
		ret = max96724_update_bits(priv, 0x40b, 0x02, 0x02);
		if (ret)
			return ret;

		ret = max96724_update_bits(priv, 0x8a0, 0x80, 0x80);
		if (ret)
			return ret;
	} else {
		ret = max96724_update_bits(priv, 0x8a0, 0x80, 0x00);
		if (ret)
			return ret;

		ret = max96724_update_bits(priv, 0x40b, 0x02, 0x00);
		if (ret)
			return ret;
	}

	return 0;
}

static int max96724_init(struct max_des_priv *des_priv)
{
	struct max96724_priv *priv = des_to_priv(des_priv);
	int ret;

	/* Select 2x4 or 4x2 mode. */
	ret = max96724_update_bits(priv, 0x8a0, 0x1f, BIT(des_priv->lane_config));
	if (ret)
		return ret;

	/* Set alternate memory map mode for 12bpp. */
	/* TODO: make dynamic. */
	ret = max96724_des_write(priv, 0x9b3, 0x01);
	if (ret)
		return ret;

	/* Disable all PHYs. */
	ret = max96724_des_update_bits(priv, 0x8a2, GENMASK(7, 4), 0x00);
	if (ret)
		return ret;

	/* Disable automatic stream select. */
	ret = max96724_des_update_bits(priv, 0xf4, BIT(4), 0x00);
	if (ret)
		return ret;

	/* Disable all pipes. */
	ret = max96724_update_bits(priv, 0xf4, GENMASK(3, 0), 0x00);
	if (ret)
		return ret;

	/* Disable all links. */
	ret = max96724_update_bits(priv, 0x6, GENMASK(3, 0), 0x00);
	if (ret)
		return ret;

	return 0;
}

static int max96724_init_phy(struct max_des_priv *des_priv,
			     struct max_des_phy *phy)
{
	struct max96724_priv *priv = des_to_priv(des_priv);
	unsigned int num_data_lanes = phy->mipi.num_data_lanes;
	unsigned int reg, val, shift, mask, clk_bit;
	unsigned int index = phy->index;
	unsigned int i;
	int ret;

	/* Configure a lane count. */
	/* TODO: Add support CPHY mode. */
	ret = max96724_update_bits(priv, 0x90a + 0x40 * index, GENMASK(7, 6),
				   FIELD_PREP(GENMASK(7, 6), num_data_lanes - 1));
	if (ret)
		return ret;

	if (num_data_lanes == 4) {
		mask = 0xff;
		val = 0xe4;
		shift = 0;
	} else {
		mask = 0xf;
		val = 0x4;
		shift = 4 * (index % 2);
	}

	reg = 0x8a3 + index / 2;

	/* Configure lane mapping. */
	/* TODO: Add support for lane swapping. */
	ret = max96724_update_bits(priv, reg, mask << shift, val << shift);
	if (ret)
		return ret;

	if (num_data_lanes == 4) {
		mask = 0x3f;
		clk_bit = 5;
		shift = 0;
	} else {
		mask = 0x7;
		clk_bit = 2;
		shift = 4 * (index % 2);
	}

	reg = 0x8a5 + index / 2;

	/* Configure lane polarity. */
	val = 0;
	for (i = 0; i < num_data_lanes + 1; i++)
		if (phy->mipi.lane_polarities[i])
			val |= BIT(i == 0 ? clk_bit : i < 3 ? i - 1 : i);
	ret = max96724_update_bits(priv, reg, mask << shift, val << shift);
	if (ret)
		return ret;

	/* Put DPLL block into reset. */
	ret = max96724_update_bits(priv, 0x1c00 + 0x100 * index, BIT(0), 0x00);
	if (ret)
		return ret;

	/* Set DPLL frequency. */
	reg = 0x415 + 0x3 * index;
	ret = max96724_update_bits(priv, reg, GENMASK(4, 0),
				   MAX_DES_DPLL_FREQ / 100);
	if (ret)
		return ret;

	/* Enable DPLL frequency. */
	ret = max96724_update_bits(priv, reg, BIT(5), BIT(5));
	if (ret)
		return ret;

	/* Pull DPLL block out of reset. */
	ret = max96724_update_bits(priv, 0x1c00 + 0x100 * index, BIT(0), 0x01);
	if (ret)
		return ret;

	/* Disable initial deskew. */
	ret = max96724_write(priv, 0x903 + 0x40 * index, 0x07);
	if (ret)
		return ret;

	/* Disable periodic deskeq. */
	ret = max96724_write(priv, 0x904 + 0x40 * index, 0x01);
	if (ret)
		return ret;

	/* Enable PHY. */
	val = BIT(index) << 4;
	ret = max96724_update_bits(priv, 0x8a2, val, val);
	if (ret)
		return ret;

	return 0;
}

static int max96724_init_pipe_remap(struct max96724_priv *priv,
				    struct max_des_pipe *pipe,
				    struct max_des_dt_vc_remap *remap,
				    unsigned int i)
{
	unsigned int index = pipe->index;
	unsigned int reg, val, shift, mask;
	int ret;

	/* Set source Data Type and Virtual Channel. */
	/* TODO: implement extended Virtual Channel. */
	reg = 0x90d + 0x40 * index + i * 2;
	ret = max96724_write(priv, reg,
			     MAX_DES_DT_VC(remap->from_dt, remap->from_vc));
	if (ret)
		return ret;

	/* Set destination Data Type and Virtual Channel. */
	/* TODO: implement extended Virtual Channel. */
	reg = 0x90e + 0x40 * index + i * 2;
	ret = max96724_write(priv, reg,
			     MAX_DES_DT_VC(remap->to_dt, remap->to_vc));
	if (ret)
		return ret;

	/* Set destination PHY. */
	reg = 0x92d + 0x40 * index + i / 4;
	shift = (i % 4) * 2;
	mask = 0x3 << shift;
	val = (remap->phy & 0x3) << shift;
	ret = max96724_update_bits(priv, reg, mask, val);
	if (ret)
		return ret;

	/* Enable remap. */
	reg = 0x90b + 0x40 * index + i / 8;
	val = BIT(i % 8);
	ret = max96724_update_bits(priv, reg, val, val);
	if (ret)
		return ret;

	return 0;
}

static int max96724_init_pipe_remaps(struct max96724_priv *priv,
				     struct max_des_pipe *pipe)
{
	unsigned int i;
	int ret;

	for (i = 0; i < pipe->num_remaps; i++) {
		struct max_des_dt_vc_remap *remap = &pipe->remaps[i];

		ret = max96724_init_pipe_remap(priv, pipe, remap, i);
		if (ret)
			return ret;
	}

	return 0;
}

static int max96724_init_pipe(struct max_des_priv *des_priv,
			      struct max_des_pipe *pipe)
{
	struct max96724_priv *priv = des_to_priv(des_priv);
	unsigned int index = pipe->index;
	unsigned int reg, val, shift;
	int ret;

	/* Set destination PHY. */
	shift = index * 2;
	ret = max96724_update_bits(priv, 0x8ca, 0x3 << shift,
				   pipe->dest_phy << shift);
	if (ret)
		return ret;

	shift = 4;
	ret = max96724_update_bits(priv, 0x939 + 0x40 * index, 0x3 << shift,
				   pipe->dest_phy << shift);
	if (ret)
		return ret;

	/* Enable pipe. */
	ret = max96724_update_bits(priv, 0xf4, BIT(index), BIT(index));
	if (ret)
		return ret;

	/* Set source stream. */
	reg = 0xf0 + index / 2;
	shift = 4 * (index % 2);
	ret = max96724_update_bits(priv, reg, 0x3 << shift, pipe->src_stream_id << shift);
	if (ret)
		return ret;

	/* Set source link. */
	shift += 2;
	ret = max96724_update_bits(priv, reg, 0x3 << shift, pipe->src_link << shift);
	if (ret)
		return ret;

	return max_des_init_pipe_remaps(priv, pipe);
}

static int max96724_init_link(struct max_des_priv *des_priv,
			      struct max_des_link *link)
{
	struct max96724_priv *priv = des_to_priv(des_priv);
	unsigned int index = link->index;
	unsigned int val;
	int ret;

	/* Enable link. */
	val = BIT(index);
	ret = max96724_update_bits(priv, 0x6, val, val);
	if (ret)
		return ret;

	return 0;
}

static int max96724_post_init(struct max_des_priv *des_priv)
{
	struct max96724_priv *priv = des_to_priv(des_priv);
	int ret;

	/* One-shot reset all PHYs. */
	ret = max_des_write(priv, 0x18, 0x0f);
	if (ret)
		return ret;

	/*
	 * Wait for 2ms to allow the link to resynchronize after the
	 * configuration change.
	 */
	usleep_range(2000, 5000);

	return 0;
}

static const struct max_des_ops max96724_ops = {
	.mux_select = max96724_mux_select,
	.mipi_enable = max96724_mipi_enable,
	.init = max96724_init,
	.init_phy = max96724_init_phy,
	.init_pipe = max96724_init_pipe,
	.post_init = max96724_post_init,
};

static int max96724_probe(struct i2c_client *client)
{
	struct max96724_priv *priv;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &client->dev;
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
	priv->des_priv.ops = &max96724_ops;

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
