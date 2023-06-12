// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim MAX96717 GMSL2 Serializer Driver
 *
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include "max_ser.h"

#define MAX96717_PIPES_NUM			4
#define MAX96717_PHYS_NUM			2
#define MAX96717_LANE_CONFIGS_NUM		4

struct max96717_priv {
	struct max_ser_priv ser_priv;
	const struct max96717_chip_info *info;

	struct device *dev;
	struct i2c_client *client;
	struct regmap *regmap;
};

struct max96717_chip_info {
	bool has_tunnel_mode;
	unsigned int num_pipes;
	unsigned int num_dts_per_pipe;
	unsigned int pipe_hw_ids[MAX96717_PIPES_NUM];
	unsigned int num_phys;
	unsigned int phy_hw_ids[MAX96717_PHYS_NUM];
	unsigned int num_lane_configs;
	unsigned int lane_configs[MAX96717_LANE_CONFIGS_NUM][MAX96717_PHYS_NUM];
	unsigned int phy_configs[MAX96717_LANE_CONFIGS_NUM];
};

#define ser_to_priv(ser) \
	container_of(ser, struct max96717_priv, ser_priv)

static int max96717_read(struct max96717_priv *priv, int reg)
{
	int ret, val;

	ret = regmap_read(priv->regmap, reg, &val);
	dev_err(priv->dev, "read %d 0x%x = 0x%02x\n", ret, reg, val);
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
	dev_err(priv->dev, "write %d 0x%x = 0x%02x\n", ret, reg, val);
	if (ret)
		dev_err(priv->dev, "write 0x%04x failed\n", reg);

	return ret;
}

static int max96717_update_bits(struct max96717_priv *priv, unsigned int reg,
				u8 mask, u8 val)
{
	int ret;

	ret = regmap_update_bits(priv->regmap, reg, mask, val);
	dev_err(priv->dev, "update %d 0x%x 0x%02x = 0x%02x\n", ret, reg, mask, val);
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

static unsigned int max96717_pipe_id(struct max96717_priv *priv,
				     struct max_ser_pipe *pipe)
{
	return priv->info->pipe_hw_ids[pipe->index];
}

static unsigned int max96717_phy_id(struct max96717_priv *priv,
				    struct max_ser_phy *phy)
{
	return priv->info->phy_hw_ids[phy->index];
}

static int max96717_set_pipe_enable(struct max_ser_priv *ser_priv,
				    struct max_ser_pipe *pipe, bool enable)
{
	struct max96717_priv *priv = ser_to_priv(ser_priv);
	unsigned int index = max96717_pipe_id(priv, pipe);
	unsigned int mask = BIT(index + 4);

	return max96717_update_bits(priv, 0x2, mask, enable ? mask : 0);
}

static int max96717_set_pipe_dt_en(struct max96717_priv *priv,
				   struct max_ser_pipe *pipe,
				   unsigned int i, bool en)
{
	unsigned int index = max96717_pipe_id(priv, pipe);
	unsigned int reg, mask;

	if (i < 2) {
		reg = 0x315 + index * 2 + i;
		mask = BIT(6);
	} else if (i < 6) {
		reg = 0x3d0 + index / 2;
		mask = BIT(i);
	} else {
		reg = 0x3dc + index * 2 + i;
		mask = BIT(6);
	}

	return max96717_update_bits(priv, reg, mask, en ? mask : 0);
}

static int max96717_set_pipe_dt(struct max96717_priv *priv,
				struct max_ser_pipe *pipe,
				unsigned int i)
{
	unsigned int index = max96717_pipe_id(priv, pipe);
	u32 dt = pipe->dts[i];
	unsigned int reg;

	if (i < 2)
		reg = 0x315 + index * 2 + i;
	else if (i < 6)
		reg = 0x3c0 + index * 4 + i;
	else
		reg = 0x3dc + index * 2 + i;

	return max96717_update_bits(priv, reg, GENMASK(5, 0), dt);
}

static int max96717_set_pipe_dbl(struct max96717_priv *priv,
				 struct max_ser_pipe *pipe,
				 unsigned int i, bool en)
{
	const struct max_ser_format *fmt = max_ser_format_by_dt(pipe->dts[i]);
	unsigned int index = max96717_pipe_id(priv, pipe);
	bool dbl = en && fmt->dbl;
	unsigned int reg, mask;
	int ret;

	/* Set 8bit double mode. */
	mask = BIT(index);
	ret = max96717_update_bits(priv, 0x312, mask,
				   dbl && fmt->bpp == 8 ? mask : 0);
	if (ret)
		return ret;

	/* Set 10bit double mode. */
	mask = BIT(index);
	ret = max96717_update_bits(priv, 0x313, mask,
				   dbl && fmt->bpp == 10 ? mask : 0);
	if (ret)
		return ret;

	/* Set 12bit double mode. */
	mask = BIT(index) << 4;
	ret = max96717_update_bits(priv, 0x313, mask,
				   dbl && fmt->bpp == 12 ? mask : 0);
	if (ret)
		return ret;

	/* Set BPP. */
	reg = 0x31c + index;
	ret = max96717_update_bits(priv, reg, GENMASK(4, 0),
				   dbl ? fmt->bpp * 2 : 0);
	if (ret)
		return ret;

	/* Set Auto BPP mode. */
	reg = 0x100 + 0x8 * index;
	mask = BIT(3);
	ret = max96717_update_bits(priv, reg, mask, dbl ? 0 : mask);
	if (ret)
		return ret;

	/* Set override BPP. */
	reg = 0x31c + index;
	mask = BIT(5);
	ret = max96717_update_bits(priv, reg, mask, dbl ? mask : 0);
	if (ret)
		return ret;

	return 0;
}

static int max96717_update_pipe_dts(struct max_ser_priv *ser_priv,
				    struct max_ser_pipe *pipe)
{
	struct max96717_priv *priv = ser_to_priv(ser_priv);
	unsigned int i;
	int ret;

	for (i = 0; i < pipe->num_dts; i++) {
		ret = max96717_set_pipe_dt(priv, pipe, i);
		if (ret)
			return ret;

		/*
		 * TODO: implement algorithm to decide if double mode should be
		 * used based on all the DTs. Might need to be done in deserializer also.
		 */
		ret = max96717_set_pipe_dbl(priv, pipe, i, true);
		if (ret)
			return ret;

		ret = max96717_set_pipe_dt_en(priv, pipe, i, true);
		if (ret)
			return ret;
	}

	/* Disable unused DTs. */
	for (i = pipe->num_dts; i < ser_priv->ops->num_dts_per_pipe; i++) {
		ret = max96717_set_pipe_dt_en(priv, pipe, i, false);
		if (ret)
			return ret;
	}

	return 0;
}

static int max96717_init_phy(struct max_ser_priv *ser_priv,
			     struct max_ser_phy *phy)
{
	struct max96717_priv *priv = ser_to_priv(ser_priv);
	unsigned int num_data_lanes = phy->mipi.num_data_lanes;
	unsigned int index = max96717_phy_id(priv, phy);
	unsigned int val, shift, mask;
	unsigned int i;
	int ret;

	if (num_data_lanes == 4)
		val = 0x3;
	else
		val = 0x1;

	shift = index == 1 ? 4 : 0;
	mask = GENMASK(1, 0);

	/* Configure a lane count. */
	/* TODO: Add support for 1-lane configurations. */
	ret = max96717_update_bits(priv, 0x331, mask << shift, val << shift);
	if (ret)
		return ret;

	/* Configure lane mapping. */
	/* TODO: Add support for lane swapping. */
	/* TODO: Handle PHY A. */
	ret = max96717_update_bits(priv, 0x332, 0xf0, 0xe0);
	if (ret)
		return ret;

	ret = max96717_update_bits(priv, 0x333, 0x0f, 0x04);
	if (ret)
		return ret;

	/* Configure lane polarity. */
	/* Lower two lanes. */
	/* TODO: Handle PHY A. */
	val = 0;
	for (i = 0; i < 3 && i < num_data_lanes + 1; i++)
		if (phy->mipi.lane_polarities[i])
			val |= BIT(i == 0 ? 2 : i - 1);
	ret = max96717_update_bits(priv, 0x335, 0x7, val);
	if (ret)
		return ret;

	/* Upper two lanes. */
	val = 0;
	shift = 4;
	mask = GENMASK(2, 0);
	for (i = 3; i < num_data_lanes + 1; i++)
		if (phy->mipi.lane_polarities[i])
			val |= BIT(i - 3);
	ret = max96717_update_bits(priv, 0x334, mask << shift, val << shift);
	if (ret)
		return ret;

	/* Enable PHY. */
	shift = 4;
	mask = BIT(index) << shift;
	ret = max96717_update_bits(priv, 0x308, mask, mask);
	if (ret)
		return ret;

	return 0;
}

static int max96717_init_pipe_stream_id(struct max96717_priv *priv,
					struct max_ser_pipe *pipe)
{
	unsigned int index = max96717_pipe_id(priv, pipe);

	return max96717_write(priv, 0x53 + 0x4 * index, pipe->stream_id);
}

static int max96717_init_pipe(struct max_ser_priv *ser_priv,
			      struct max_ser_pipe *pipe)
{
	struct max_ser_phy *phy = max_ser_pipe_phy(ser_priv, pipe);
	struct max96717_priv *priv = ser_to_priv(ser_priv);
	unsigned int index = max96717_pipe_id(priv, pipe);
	unsigned int phy_id = max96717_phy_id(priv, phy);
	unsigned int val, shift, mask;
	int ret;

	/* Map pipe to PHY. */
	mask = BIT(index);
	val = phy_id == 1 ? mask : 0;
	ret = max96717_update_bits(priv, 0x308, mask, val);
	if (ret)
		return ret;

	/* Enable pipe output to PHY. */
	shift = phy_id == 1 ? 4 : 0;
	mask = BIT(index) << shift;
	ret = max96717_update_bits(priv, 0x311, mask, mask);
	if (ret)
		return ret;

	ret = max96717_init_pipe_stream_id(priv, pipe);
	if (ret)
		return ret;

	ret = max96717_set_pipe_enable(ser_priv, pipe, false);
	if (ret)
		return ret;

	return 0;
}

static int _max96717_set_tunnel_mode(struct max96717_priv *priv, bool enable)
{
	unsigned int mask = BIT(7);

	return max96717_update_bits(priv, 0x383, mask, enable ? mask : 0x00);
}

static int max96717_init_pipes_stream_ids(struct max96717_priv *priv)
{
	struct max_ser_priv *ser_priv = &priv->ser_priv;
	unsigned int used_stream_ids = 0;
	struct max_ser_pipe *pipe;
	unsigned int i;
	int ret;

	for (i = 0; i < ser_priv->ops->num_pipes; i++) {
		pipe = max_ser_pipe_by_id(ser_priv, i);

		if (!pipe->enabled)
			continue;

		if (used_stream_ids & BIT(pipe->stream_id)) {
			dev_err(priv->dev, "Duplicate stream %u\n", pipe->index);
			return -EINVAL;
		}

		used_stream_ids |= BIT(pipe->stream_id);
	}

	for (i = 0; i < ser_priv->ops->num_pipes; i++) {
		pipe = max_ser_pipe_by_id(ser_priv, i);

		if (pipe->enabled)
			continue;

		/* Stream ID already used, find a free one. */
		/* TODO: check whether there is no unused stream ID? */
		if (used_stream_ids & BIT(pipe->stream_id))
			pipe->stream_id = ffz(used_stream_ids);

		ret = max96717_init_pipe_stream_id(priv, pipe);
		if (ret)
			return ret;
	}

	return 0;
}

static int max96717_init_lane_config(struct max96717_priv *priv)
{
	struct max_ser_priv *ser_priv = &priv->ser_priv;
	struct max_ser_phy *phy;
	unsigned int i, j;
	int ret;

	for (i = 0; i < priv->info->num_lane_configs; i++) {
		bool matching = true;

		for (j = 0; j < priv->info->num_phys; j++) {
			phy = max_ser_phy_by_id(ser_priv, j);

			if (phy->enabled && phy->mipi.num_data_lanes !=
			    priv->info->lane_configs[i][j]) {
				matching = false;
				break;
			}
		}

		if (matching)
			break;
	}

	if (i == priv->info->num_lane_configs) {
		dev_err(priv->dev, "Invalid lane configuration\n");
		return -EINVAL;
	}

	ret = max96717_update_bits(priv, 0x330, GENMASK(2, 0),
				   priv->info->phy_configs[i]);
	if (ret)
		return ret;

	return 0;
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

	ret = _max96717_set_tunnel_mode(priv, false);
	if (ret)
		return ret;

	/* Disable ports. */
	ret = max96717_update_bits(priv, 0x308, GENMASK(5, 4), 0x00);
	if (ret)
		return ret;

	/* Reset pipe to ports mapping. */
	ret = max96717_update_bits(priv, 0x308, GENMASK(3, 0), 0x00);
	if (ret)
		return ret;

	/* Disable pipes. */
	ret = max96717_write(priv, 0x311, 0x00);
	if (ret)
		return ret;

	ret = max96717_init_lane_config(priv);
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

	ret = max96717_init_pipes_stream_ids(priv);
	if (ret)
		return ret;

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
	.set_pipe_enable = max96717_set_pipe_enable,
	.update_pipe_dts = max96717_update_pipe_dts,
	.init = max96717_init,
	.set_tunnel_mode = max96717_set_tunnel_mode,
	.init_phy = max96717_init_phy,
	.init_pipe = max96717_init_pipe,
	.post_init = max96717_post_init,
};

static int max96717_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct max96717_priv *priv;
	struct max_ser_ops *ops;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	ops = devm_kzalloc(dev, sizeof(*ops), GFP_KERNEL);
	if (!ops)
		return -ENOMEM;

	priv->info = device_get_match_data(dev);
	if (!priv->info) {
		dev_err(dev, "Failed to get match data\n");
		return -ENODEV;
	}

	priv->dev = dev;
	priv->client = client;
	i2c_set_clientdata(client, priv);

	priv->regmap = devm_regmap_init_i2c(client, &max_ser_i2c_regmap);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	*ops = max96717_ops;

	if (!priv->info->has_tunnel_mode)
		ops->set_tunnel_mode = NULL;

	ops->num_pipes = priv->info->num_pipes;
	ops->num_dts_per_pipe = priv->info->num_dts_per_pipe;
	ops->num_phys = priv->info->num_phys;

	priv->ser_priv.dev = &client->dev;
	priv->ser_priv.client = client;
	priv->ser_priv.ops = ops;

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

static const struct max96717_chip_info max96717_info = {
	.has_tunnel_mode = true,
	.num_pipes = 1,
	.num_dts_per_pipe = 8,
	.pipe_hw_ids = { 2 },
	.num_phys = 1,
	.phy_hw_ids = { 1 },
	.num_lane_configs = 2,
	.lane_configs = {
		{ 4 },
		{ 2 },
	},
	.phy_configs = {
		0b000,
		0b000,
	},
};

static const struct max96717_chip_info max9295a_info = {
	.has_tunnel_mode = false,
	.num_pipes = 4,
	.num_dts_per_pipe = 6,
	.pipe_hw_ids = { 0, 1, 2, 3 },
	.num_phys = 1,
	.phy_hw_ids = { 1 },
	.num_lane_configs = 2,
	.lane_configs = {
		{ 4 },
		{ 2 },
	},
	.phy_configs = {
		0b000,
		0b000,
	},
};

static const struct max96717_chip_info max9295b_info = {
	.has_tunnel_mode = false,
	.num_pipes = 4,
	.num_dts_per_pipe = 6,
	.pipe_hw_ids = { 0, 1, 2, 3 },
	.num_phys = 2,
	.phy_hw_ids = { 0, 1 },
	.num_lane_configs = 4,
	.lane_configs = {
		{ 0, 4 },
		{ 2, 0 },
		{ 0, 2 },
		{ 2, 2 },
	},
	.phy_configs = {
		0b000,
		0b001,
		0b010,
		0b011,
	},
};

static const struct of_device_id max96717_of_ids[] = {
	{ .compatible = "maxim,max96717", .data = &max96717_info },
	{ .compatible = "maxim,max9295a", .data = &max9295a_info },
	{ .compatible = "maxim,max9295b", .data = &max9295b_info },
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
