// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim MAX96717 GMSL2 Serializer Driver
 *
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include "max_ser.h"

struct max96717_priv {
	struct max_ser_priv ser_priv;

	struct device *dev;
	struct i2c_client *client;
	struct regmap *regmap;
};

#define ser_to_priv(ser) \
	container_of(ser, struct max96717_priv, ser_priv)

static int max96717_read(struct max96717_priv *priv, int reg)
{
	int ret, val;

	ret = regmap_read(priv->regmap, reg, &val);
	if (ret) {
		dev_err(priv->dev, "read 0x%04x failed\n", reg);
		return ret;
	}

	return val;
}

static int max96717_write(struct max96717_priv *priv, unsigned int reg, u8 val)
{
	int ret;

	ret = regmap_write(priv->regmap, reg, val);
	if (ret)
		dev_err(priv->dev, "write 0x%04x failed\n", reg);

	return ret;
}

static int max96717_update_bits(struct max96717_priv *priv, unsigned int reg,
				u8 mask, u8 val)
{
	int ret;

	ret = regmap_update_bits(priv->regmap, reg, mask, val);
	if (ret)
		dev_err(priv->dev, "update 0x%04x failed\n", reg);

	return ret;
}

static int max96717_wait_for_device(struct max96717_priv *priv)
{
	unsigned int i;
	int ret;

	for (i = 0; i < 100; i++) {
		ret = max96717_read(priv, 0x0);
		if (ret >= 0)
			return 0;

		msleep(10);

		dev_err(priv->dev, "Retry %u waiting for serializer: %d\n", i, ret);
	}

	return ret;
}

static int max96717_mipi_enable(struct max_ser_priv *ser_priv, bool enable)
{
	struct max96717_priv *priv = ser_to_priv(ser_priv);

	return max96717_update_bits(priv, 0x2, BIT(6), enable ? BIT(6) : 0);
}

static int max96717_override_bpp(struct max96717_priv *priv, u8 bpp)
{
	int ret;

	/* Disable Auto BPP mode. */
	ret = max96717_update_bits(priv, 0x110, BIT(3), 0x00);
	if (ret)
		return ret;

	/* Software override BPP. */
	ret = max96717_update_bits(priv, 0x31e, GENMASK(4, 0),
				   FIELD_PREP(GENMASK(4, 0), bpp));
	if (ret)
		return ret;

	/* Enable software override BPP. */
	ret = max96717_update_bits(priv, 0x31e, BIT(5), BIT(5));
	if (ret)
		return ret;

	return 0;
}

static int max96717_set_dt(struct max_ser_priv *ser_priv, u32 code)
{
	const struct max_ser_format *fmt = max_ser_format_by_code(code);
	struct max96717_priv *priv = ser_to_priv(ser_priv);
	int ret;

	if (!fmt)
		return -EINVAL;

	/* TODO: implement all other supported formats. */

	ret = max96717_update_bits(priv, 0x312, BIT(2), 0);
	if (ret)
		return ret;

	ret = max96717_update_bits(priv, 0x313, BIT(6) | BIT(2), 0);
	if (ret)
		return ret;

	switch (fmt->dt) {
	case MAX_SER_DT_YUV422_8B:
	case MAX_SER_DT_YUV422_10B:
		break;
	case MAX_SER_DT_RAW8:
		ret = max96717_update_bits(priv, 0x312, BIT(2), BIT(2));
		if (ret)
			return ret;

		ret = max96717_override_bpp(priv, 16);
		if (ret)
			return ret;

		break;
	case MAX_SER_DT_RAW10:
		ret = max96717_update_bits(priv, 0x313, BIT(2), BIT(2));
		if (ret)
			return ret;

		ret = max96717_override_bpp(priv, 20);
		if (ret)
			return ret;

		break;
	case MAX_SER_DT_RAW12:
		ret = max96717_update_bits(priv, 0x313, BIT(6), BIT(6));
		if (ret)
			return ret;

		ret = max96717_override_bpp(priv, 24);
		if (ret)
			return ret;

		break;
	default:
		dev_err(priv->dev, "Data type %02x not implemented\n", fmt->dt);
	}

	return 0;
}

static int max96717_init_ch(struct max_ser_priv *ser_priv,
			    struct max_ser_subdev_priv *sd_priv)
{
	struct max96717_priv *priv = ser_to_priv(ser_priv);
	unsigned int num_data_lanes = sd_priv->mipi.num_data_lanes;
	unsigned int index = sd_priv->index;
	unsigned int val, shift, mask;
	unsigned int i;
	int ret;

	if (num_data_lanes == 4)
		val = 0x3;
	else
		val = 0x1;

	shift = index * 4;
	mask = 0x3;

	/* Configure a lane count. */
	/* TODO: Add support for 1-lane configurations. */
	ret = max96717_update_bits(priv, 0x331, mask << shift, val << shift);
	if (ret)
		return ret;

	/* Configure lane mapping. */
	/* TODO: Add support for lane swapping. */
	ret = max96717_update_bits(priv, 0x332, 0xf0, 0xe0);
	if (ret)
		return ret;

	ret = max96717_update_bits(priv, 0x333, 0x0f, 0x04);
	if (ret)
		return ret;

	/* Configure lane polarity. */
	/* Lower two lanes. */
	val = 0;
	for (i = 0; i < 3 && i < num_data_lanes + 1; i++)
		if (sd_priv->mipi.lane_polarities[i])
			val |= BIT(i == 0 ? 2 : i - 1);
	ret = max96717_update_bits(priv, 0x335, 0x7, val);
	if (ret)
		return ret;

	/* Upper two lanes. */
	val = 0;
	shift = 4;
	for (i = 3; i < num_data_lanes + 1; i++)
		if (sd_priv->mipi.lane_polarities[i])
			val |= BIT(i - 3);
	ret = max96717_update_bits(priv, 0x334, 0x7 << shift, val << shift);
	if (ret)
		return ret;

	/* Set stream ID. */
	ret = max96717_write(priv, 0x5b, sd_priv->stream_id);
	if (ret)
		return ret;

	return 0;
}

static int _max96717_set_tunnel_mode(struct max96717_priv *priv, bool enable)
{
	unsigned int mask = BIT(7);

	return max96717_update_bits(priv, 0x383, mask, enable ? mask : 0x00);
}

static int max96717_init(struct max_ser_priv *ser_priv)
{
	struct max96717_priv *priv = ser_to_priv(ser_priv);
	int ret;

	/*
	 * Set CMU2 PFDDIV to 1.1V for correct functionality of the device,
	 * as mentioned in the datasheet, under section MANDATORY REGISTER PROGRAMMING.
	 */
	ret = max96717_update_bits(priv, 0x302, 0x70, 0x10);
	if (ret)
		return ret;

	ret = max96717_mipi_enable(ser_priv, false);
	if (ret)
		return ret;

	ret = _max96717_set_tunnel_mode(priv, false);
	if (ret)
		return ret;

	return 0;
}

static int max96717_set_tunnel_mode(struct max_ser_priv *ser_priv)
{
	struct max96717_priv *priv = ser_to_priv(ser_priv);

	return _max96717_set_tunnel_mode(priv, true);
}

static int max96717_post_init(struct max_ser_priv *ser_priv)
{
	struct max96717_priv *priv = ser_to_priv(ser_priv);
	int ret;

	/* Enable RCLK output at fastest slew rate on GPIO 4. */
	ret = max96717_update_bits(priv, 0x6, BIT(5), BIT(5));
	if (ret)
		return ret;

	ret = max96717_update_bits(priv, 0x3, GENMASK(1, 0), 0b00);
	if (ret)
		return ret;

	ret = max96717_update_bits(priv, 0x570, GENMASK(5, 4),
				   FIELD_PREP(GENMASK(5, 4), 0b00));
	if (ret)
		return ret;

	ret = max96717_update_bits(priv, 0x3f1, BIT(7), BIT(7));
	if (ret)
		return ret;

	ret = max96717_update_bits(priv, 0x3f1, BIT(0), BIT(0));
	if (ret)
		return ret;

	ret = max96717_update_bits(priv, 0x3f1, GENMASK(5, 1),
				   FIELD_PREP(GENMASK(5, 1), 0x4));
	if (ret)
		return ret;

	msleep(2000);

	/* Enable GPIO 0. */
	/* TODO: Implement pinctrl. */
	ret = max96717_write(priv, 0x2be, 0x80);
	if (ret)
		return ret;

	msleep(2000);

	ret = max96717_write(priv, 0x2be, 0x90);
	if (ret)
		return ret;

	ret = max96717_write(priv, 0x2bf, 0x60);
	if (ret)
		return ret;

	msleep(2000);

	return 0;
}

static const struct max_ser_ops max96717_ops = {
	.mipi_enable = max96717_mipi_enable,
	.set_dt = max96717_set_dt,
	.init = max96717_init,
	.set_tunnel_mode = max96717_set_tunnel_mode,
	.init_ch = max96717_init_ch,
	.post_init = max96717_post_init,
};

static int max96717_probe(struct i2c_client *client)
{
	struct max96717_priv *priv;
	int ret;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &client->dev;
	priv->client = client;
	i2c_set_clientdata(client, priv);

	priv->regmap = devm_regmap_init_i2c(client, &max_ser_i2c_regmap);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	priv->ser_priv.dev = &client->dev;
	priv->ser_priv.client = client;
	priv->ser_priv.ops = &max96717_ops;

	ret = max96717_wait_for_device(priv);
	if (ret)
		return ret;

	return max_ser_probe(&priv->ser_priv);
}

static int max96717_remove(struct i2c_client *client)
{
	struct max96717_priv *priv = i2c_get_clientdata(client);

	return max_ser_remove(&priv->ser_priv);
}

static const struct of_device_id max96717_of_ids[] = {
	{ .compatible = "maxim,max96717", },
	{ }
};
MODULE_DEVICE_TABLE(of, max96717_of_ids);

static struct i2c_driver max96717_i2c_driver = {
	.driver	= {
		.name	= "max96717",
		.of_match_table = max96717_of_ids,
	},
	.probe_new	= max96717_probe,
	.remove		= max96717_remove,
};

module_i2c_driver(max96717_i2c_driver);

MODULE_DESCRIPTION("MAX96717 GMSL2 Serializer Driver");
MODULE_AUTHOR("Cosmin Tanislav <cosmin.tanislav@analog.com>");
MODULE_LICENSE("GPL");
