// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim MAX96712 Quad GMSL2 Deserializer Driver
 *
 * Copyright (C) 2021 Renesas Electronics Corporation
 * Copyright (C) 2021 Niklas Söderlund
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of_graph.h>
#include <linux/regmap.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define MAX96712_ID 0x20

#define MAX96712_DPLL_FREQ 1000

#define MAX96712_SINK_PAD_START		0
#define MAX96712_SINK_PAD_NUM		4

#define MAX96712_SRC_PAD_START		4
#define MAX96712_SRC_PAD_NUM		4

#define MAX96712_PAD_NUM		(MAX96712_SINK_PAD_NUM + \
					 MAX96712_SRC_PAD_NUM)

struct max96712_source {
	struct v4l2_subdev *sd;
	struct fwnode_handle *fwnode;
};

struct max96712_asd {
	struct v4l2_async_subdev base;
	struct max96712_source *source;
};

enum max96712_pattern {
	MAX96712_PATTERN_NONE = 0,
	MAX96712_PATTERN_CHECKERBOARD,
	MAX96712_PATTERN_GRADIENT,
};

struct max96712_priv {
	struct device *dev;
	struct i2c_client *client;
	struct regmap *regmap;
	struct gpio_desc *gpiod_pwdn;

	unsigned int lane_config;
	struct v4l2_fwnode_bus_mipi_csi2 mipi[MAX96712_SRC_PAD_NUM];
	bool mipi_en[MAX96712_SRC_PAD_NUM];

	struct v4l2_subdev sd;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_async_notifier notifier;
	struct media_pad pads[MAX96712_PAD_NUM];
	struct max96712_source sources[MAX96712_SINK_PAD_NUM];

	unsigned int nsources;
	unsigned int source_mask;
	unsigned int bound_sources;

	enum max96712_pattern pattern;
};

static struct max96712_source *next_source(struct max96712_priv *priv,
					   struct max96712_source *source)
{
	if (!source)
		source = &priv->sources[0];
	else
		source++;

	for (; source < &priv->sources[MAX96712_SINK_PAD_NUM]; source++) {
		if (source->fwnode)
			return source;
	}

	return NULL;
}

#define for_each_source(priv, source) \
	for ((source) = NULL; ((source) = next_source((priv), (source))); )

#define to_index(priv, source) ((source) - &(priv)->sources[0])

static inline struct max96712_asd *to_max96712_asd(struct v4l2_async_subdev *asd)
{
	return container_of(asd, struct max96712_asd, base);
}

static inline struct max96712_priv *sd_to_max96712(struct v4l2_subdev *sd)
{
	return container_of(sd, struct max96712_priv, sd);
}

#if 0
static int max96712_read(struct max96712_priv *priv, int reg)
{
	int ret, val;

	ret = regmap_read(priv->regmap, reg, &val);
	if (ret) {
		dev_err(priv->dev, "read 0x%04x failed\n", reg);
		return ret;
	}

	return val;
}
#endif

static int max96712_write(struct max96712_priv *priv, unsigned int reg, u8 val)
{
	int ret;

	ret = regmap_write(priv->regmap, reg, val);
	if (ret)
		dev_err(priv->dev, "write 0x%04x failed\n", reg);

	return ret;
}

static int max96712_update_bits(struct max96712_priv *priv, unsigned int reg,
				u8 mask, u8 val)
{
	int ret;

	ret = regmap_update_bits(priv->regmap, reg, mask, val);
	if (ret)
		dev_err(priv->dev, "update 0x%04x failed\n", reg);

	return ret;
}

static int max96712_write_bulk(struct max96712_priv *priv, unsigned int reg,
			       const void *val, size_t val_count)
{
	int ret;

	ret = regmap_bulk_write(priv->regmap, reg, val, val_count);
	if (ret)
		dev_err(priv->dev, "bulk write 0x%04x failed\n", reg);

	return ret;
}

static int max96712_write_bulk_value(struct max96712_priv *priv,
				     unsigned int reg, unsigned int val,
				     size_t val_count)
{
	unsigned int i;
	u8 values[4];

	for (i = 1; i <= val_count; i++)
		values[i - 1] = (val >> ((val_count - i) * 8)) & 0xff;

	return max96712_write_bulk(priv, reg, &values, val_count);
}

static void max96712_reset(struct max96712_priv *priv)
{
	max96712_update_bits(priv, 0x13, 0x40, 0x40);
	msleep(20);
}

static void max96712_mipi_enable(struct max96712_priv *priv, bool enable)
{
	if (enable) {
		max96712_update_bits(priv, 0x40b, 0x02, 0x02);
		max96712_update_bits(priv, 0x8a0, 0x80, 0x80);
	} else {
		max96712_update_bits(priv, 0x8a0, 0x80, 0x00);
		max96712_update_bits(priv, 0x40b, 0x02, 0x00);
	}
}

static void max96712_mipi_configure_phy(struct max96712_priv *priv, unsigned int index)
{
	unsigned int num_data_lanes = priv->mipi[index].num_data_lanes;
	unsigned int reg, val, shift, mask, clk_bit;
	unsigned int i;

	/* Configure a lane count. */
	/* TODO: Add support for 1-lane configurations. */
	/* TODO: Add support CPHY mode. */
	if (num_data_lanes == 4)
		val = 0xc0;
	else
		val = 0x40;

	max96712_update_bits(priv, 0x90a + 0x40 * index, 0xc0, val);

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
	max96712_update_bits(priv, reg, mask << shift, val << shift);

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
		if (priv->mipi[index].lane_polarities[i])
			val |= BIT(i == 0 ? clk_bit : i < 3 ? i - 1 : i);
	max96712_update_bits(priv, reg, mask << shift, val << shift);

	/* Set link frequency. */
	max96712_update_bits(priv, 0x415 + 0x3 * index, 0x3f,
			     ((MAX96712_DPLL_FREQ / 100) & 0x1f) | BIT(5));

	/* Enable. */
	max96712_update_bits(priv, 0x8a2, 0x10 << index, 0x10 << index);
}

static void max96712_mipi_configure(struct max96712_priv *priv)
{
	unsigned int i;

	max96712_mipi_enable(priv, false);

	/* Select 2x4 or 4x2 mode. */
	max96712_update_bits(priv, 0x8a0, 0x1f, BIT(priv->lane_config));

	for (i = 0; i < MAX96712_SRC_PAD_NUM; i++) {
		if (!priv->mipi_en[i])
			continue;

		max96712_mipi_configure_phy(priv, i);
	}

	/*
	 * Wait for 2ms to allow the link to resynchronize after the
	 * configuration change.
	 */
	usleep_range(2000, 5000);

	/*
	 * Enable forwarding of GPIO 0.
	 */

	dev_err(priv->dev, "enable forwarding gpio 0\n");

	/* GPIO_A GPIO_TX_EN 1 */
	max96712_update_bits(priv, 0x300, 0x02, 0x02);
	/* GPIO_A GPIO_OUT_DIS 0 */
	max96712_update_bits(priv, 0x300, 0x01, 0x00);
}

static void max96712_pattern_enable(struct max96712_priv *priv, bool enable)
{
	const u32 h_active = 1920;
	const u32 h_fp = 88;
	const u32 h_sw = 44;
	const u32 h_bp = 148;
	const u32 h_tot = h_active + h_fp + h_sw + h_bp;

	const u32 v_active = 1080;
	const u32 v_fp = 4;
	const u32 v_sw = 5;
	const u32 v_bp = 36;
	const u32 v_tot = v_active + v_fp + v_sw + v_bp;

	if (!enable || priv->pattern == MAX96712_PATTERN_NONE) {
		max96712_write(priv, 0x1051, 0x00);
		return;
	}

	/* PCLK 75MHz. */
	max96712_write(priv, 0x0009, 0x01);

	/* Configure Video Timing Generator for 1920x1080 @ 30 fps. */
	max96712_write_bulk_value(priv, 0x1052, 0, 3);
	max96712_write_bulk_value(priv, 0x1055, v_sw * h_tot, 3);
	max96712_write_bulk_value(priv, 0x1058,
				  (v_active + v_fp + v_bp) * h_tot, 3);
	max96712_write_bulk_value(priv, 0x105b, 0, 3);
	max96712_write_bulk_value(priv, 0x105e, h_sw, 2);
	max96712_write_bulk_value(priv, 0x1060, h_active + h_fp + h_bp, 2);
	max96712_write_bulk_value(priv, 0x1062, v_tot, 2);
	max96712_write_bulk_value(priv, 0x1064,
				  h_tot * (v_sw + v_bp) + (h_sw + h_bp), 3);
	max96712_write_bulk_value(priv, 0x1067, h_active, 2);
	max96712_write_bulk_value(priv, 0x1069, h_fp + h_sw + h_bp, 2);
	max96712_write_bulk_value(priv, 0x106b, v_active, 2);

	/* Generate VS, HS and DE in free-running mode. */
	max96712_write(priv, 0x1050, 0xfb);

	/* Configure Video Pattern Generator. */
	if (priv->pattern == MAX96712_PATTERN_CHECKERBOARD) {
		/* Set checkerboard pattern size. */
		max96712_write(priv, 0x1074, 0x3c);
		max96712_write(priv, 0x1075, 0x3c);
		max96712_write(priv, 0x1076, 0x3c);

		/* Set checkerboard pattern colors. */
		max96712_write_bulk_value(priv, 0x106e, 0xfecc00, 3);
		max96712_write_bulk_value(priv, 0x1071, 0x006aa7, 3);

		/* Generate checkerboard pattern. */
		max96712_write(priv, 0x1051, 0x10);
	} else {
		/* Set gradient increment. */
		max96712_write(priv, 0x106d, 0x10);

		/* Generate gradient pattern. */
		max96712_write(priv, 0x1051, 0x20);
	}
}

static int max96712_notify_bound(struct v4l2_async_notifier *notifier,
				 struct v4l2_subdev *subdev,
				 struct v4l2_async_subdev *asd)
{
	struct max96712_priv *priv = sd_to_max96712(notifier->sd);
	struct max96712_source *source = to_max96712_asd(asd)->source;
	unsigned int index = to_index(priv, source);
	unsigned int src_pad;
	int ret;

	ret = media_entity_get_fwnode_pad(&subdev->entity,
					  source->fwnode,
					  MEDIA_PAD_FL_SOURCE);
	if (ret < 0) {
		dev_err(priv->dev, "Failed to find pad for %s\n", subdev->name);
		return ret;
	}

	priv->bound_sources |= BIT(index);
	source->sd = subdev;
	src_pad = ret;

	ret = media_create_pad_link(&source->sd->entity, src_pad,
				    &priv->sd.entity, index,
				    MEDIA_LNK_FL_ENABLED |
				    MEDIA_LNK_FL_IMMUTABLE);
	if (ret) {
		dev_err(priv->dev,
			"Unable to link %s:%u -> %s:%u\n",
			source->sd->name, src_pad, priv->sd.name, index);
		return ret;
	}

	dev_err(priv->dev, "Bound %s pad: %u on index %u\n",
		subdev->name, src_pad, index);

	/*
	 * As we register a subdev notifiers we won't get a .complete() callback
	 * here, so we have to use bound_sources to identify when all remote
	 * serializers have probed.
	 */
	if (priv->bound_sources != priv->source_mask)
		return 0;

	/*
	 * Once all cameras have probed, increase the channel amplitude
	 * to compensate for the remote noise immunity threshold and call
	 * the camera post_register operation to complete initialization with
	 * noise immunity enabled.
	 */
	dev_err(priv->dev, "Calling post_register\n");
	for_each_source(priv, source) {
		ret = v4l2_subdev_call(source->sd, core, post_register);
		if (ret) {
			dev_err(priv->dev, "Failed to initialize camera device %u\n",
				index);
			return ret;
		}
	}

	return 0;
}

static void max96712_notify_unbind(struct v4l2_async_notifier *notifier,
				   struct v4l2_subdev *subdev,
				   struct v4l2_async_subdev *asd)
{
	struct max96712_priv *priv = sd_to_max96712(notifier->sd);
	struct max96712_source *source = to_max96712_asd(asd)->source;
	unsigned int index = to_index(priv, source);

	source->sd = NULL;
	priv->bound_sources &= ~BIT(index);
}

static const struct v4l2_async_notifier_operations max96712_notify_ops = {
	.bound = max96712_notify_bound,
	.unbind = max96712_notify_unbind,
};

static int max96712_v4l2_notifier_register(struct max96712_priv *priv)
{
	struct max96712_source *source = NULL;
	int ret;

	if (!priv->nsources)
		return 0;

	v4l2_async_notifier_init(&priv->notifier);

	for_each_source(priv, source) {
		unsigned int i = to_index(priv, source);
		struct max96712_asd *mas;

		mas = (struct max96712_asd *)
		      v4l2_async_notifier_add_fwnode_subdev(&priv->notifier,
							    source->fwnode, struct max96712_asd);
		if (IS_ERR(mas)) {
			dev_err(priv->dev, "Failed to add subdev for source %u: %ld",
				i, PTR_ERR(mas));
			v4l2_async_notifier_cleanup(&priv->notifier);
			return PTR_ERR(mas);
		}

		mas->source = source;
	}

	priv->notifier.ops = &max96712_notify_ops;
	priv->notifier.flags |= V4L2_ASYNC_NOTIFIER_DEFER_POST_REGISTER;
	ret = v4l2_async_subdev_notifier_register(&priv->sd, &priv->notifier);
	if (ret) {
		dev_err(priv->dev, "Failed to register subdev_notifier");
		v4l2_async_notifier_cleanup(&priv->notifier);
		return ret;
	}

	return 0;
}

static void max96712_v4l2_notifier_unregister(struct max96712_priv *priv)
{
	if (!priv->nsources)
		return;

	v4l2_async_notifier_unregister(&priv->notifier);
	v4l2_async_notifier_cleanup(&priv->notifier);
}

static int max96712_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct max96712_priv *priv = v4l2_get_subdevdata(sd);
	struct max96712_source *source = NULL;
	int ret;

	dev_err(priv->dev, "s_stream: %u\n", enable);

	for_each_source(priv, source) {
		ret = v4l2_subdev_call(source->sd, video, s_stream, enable);
		if (ret)
			dev_err(priv->dev, "Failed to initialize camera device\n");
	}

	if (enable) {
		max96712_pattern_enable(priv, true);
		max96712_mipi_enable(priv, true);
	} else {
		max96712_mipi_enable(priv, false);
		max96712_pattern_enable(priv, false);
	}

	return 0;
}

static const struct v4l2_subdev_video_ops max96712_video_ops = {
	.s_stream = max96712_s_stream,
};

static int max96712_get_pad_format(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_format *format)
{
	format->format.width = 1920;
	format->format.height = 1080;
	format->format.code = MEDIA_BUS_FMT_RGB888_1X24;
	format->format.field = V4L2_FIELD_NONE;

	return 0;
}

static const struct v4l2_subdev_pad_ops max96712_pad_ops = {
	.get_fmt = max96712_get_pad_format,
	.set_fmt = max96712_get_pad_format,
};

static const struct v4l2_subdev_ops max96712_subdev_ops = {
	.video = &max96712_video_ops,
	.pad = &max96712_pad_ops,
};

static const char * const max96712_test_pattern[] = {
	"None",
	"Checkerboard",
	"Gradient",
};

static int max96712_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct max96712_priv *priv =
		container_of(ctrl->handler, struct max96712_priv, ctrl_handler);

	switch (ctrl->id) {
	case V4L2_CID_TEST_PATTERN:
		priv->pattern = ctrl->val;
		break;
	}
	return 0;
}

static const struct v4l2_ctrl_ops max96712_ctrl_ops = {
	.s_ctrl = max96712_s_ctrl,
};

static int max96712_v4l2_register(struct max96712_priv *priv)
{
	long pixel_rate;
	unsigned int i;
	int ret;

	/* Register v4l2 async notifiers for connected Camera subdevices */
	ret = max96712_v4l2_notifier_register(priv);
	if (ret) {
		dev_err(priv->dev, "Unable to register V4L2 async notifiers\n");
		return ret;
	}

	v4l2_i2c_subdev_init(&priv->sd, priv->client, &max96712_subdev_ops);
	priv->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	priv->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;

	v4l2_ctrl_handler_init(&priv->ctrl_handler, 2);

	/*
	 * TODO: Once V4L2_CID_LINK_FREQ is changed from a menu control to an
	 * INT64 control it should be used here instead of V4L2_CID_PIXEL_RATE.
	 */
	pixel_rate = MAX96712_DPLL_FREQ / priv->mipi[0].num_data_lanes * 1000000;
	v4l2_ctrl_new_std(&priv->ctrl_handler, NULL, V4L2_CID_PIXEL_RATE,
			  pixel_rate, pixel_rate, 1, pixel_rate);

	v4l2_ctrl_new_std_menu_items(&priv->ctrl_handler, &max96712_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(max96712_test_pattern) - 1,
				     0, 0, max96712_test_pattern);

	priv->sd.ctrl_handler = &priv->ctrl_handler;
	ret = priv->ctrl_handler.error;
	if (ret)
		goto error;

	for (i = 0; i < MAX96712_PAD_NUM; i++)
		priv->pads[i].flags = i < MAX96712_SRC_PAD_START ? MEDIA_PAD_FL_SINK
								 : MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&priv->sd.entity, MAX96712_PAD_NUM, priv->pads);
	if (ret)
		goto error;

	v4l2_set_subdevdata(&priv->sd, priv);

	ret = v4l2_async_register_subdev(&priv->sd);
	if (ret < 0) {
		dev_err(priv->dev, "Unable to register subdevice\n");
		goto error;
	}

	return 0;
error:
	v4l2_ctrl_handler_free(&priv->ctrl_handler);
	max96712_v4l2_notifier_unregister(priv);

	return ret;
}

static void max96712_v4l2_unregister(struct max96712_priv *priv)
{
	max96712_v4l2_notifier_unregister(priv);
	v4l2_async_unregister_subdev(&priv->sd);
}

static int max96712_parse_src_dt_endpoint(struct max96712_priv *priv,
					  unsigned int index,
					  unsigned int port_index)
{
	struct fwnode_handle *ep;
	struct v4l2_fwnode_endpoint v4l2_ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(priv->dev),
					     port_index, 0, 0);
	if (!ep) {
		dev_err(priv->dev, "Not connected to subdevice\n");
		return -EINVAL;
	}

	dev_err(priv->dev, "%s:%u: subdevice: %pfw\n", __func__, __LINE__, ep);

	ret = v4l2_fwnode_endpoint_parse(ep, &v4l2_ep);
	fwnode_handle_put(ep);
	if (ret) {
		dev_err(priv->dev, "Could not parse v4l2 endpoint\n");
		return ret;
	}

	priv->mipi[index] = v4l2_ep.bus.mipi_csi2;
	priv->mipi_en[index] = true;

	return 0;
}

static int max96712_parse_sink_dt_endpoint(struct max96712_priv *priv,
					   unsigned int index)
{
	struct max96712_source *source;
	struct fwnode_handle *ep;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(priv->dev),
					     index, 0, 0);
	if (!ep) {
		dev_err(priv->dev, "Not connected to subdevice\n");
		return -EINVAL;
	}

	dev_err(priv->dev, "%s:%u: subdevice: %pfw\n", __func__, __LINE__, ep);

	source = &priv->sources[index];
	source->fwnode = fwnode_graph_get_remote_endpoint(ep);
	if (!source->fwnode) {
		dev_err(priv->dev, "Not connected to remote endpoint\n");

		return -EINVAL;
	}

	dev_err(priv->dev, "%s:%u: subdevice remote ep: %pfw\n", __func__, __LINE__, source->fwnode);

	priv->source_mask |= BIT(index);
	priv->nsources++;

	return 0;
}

static const unsigned int max96712_lane_configs[][MAX96712_SRC_PAD_NUM] = {
	{ 2, 2, 2, 2 },
	{ 0, 0, 0, 0 },
	{ 0, 4, 0, 4 },
	{ 0, 4, 2, 2 },
	{ 2, 2, 0, 4 },
};

static int max96712_parse_dt(struct max96712_priv *priv)
{
	unsigned int i, j;
	int ret;

	for (i = 0; i < MAX96712_SINK_PAD_NUM; i++) {
		ret = max96712_parse_sink_dt_endpoint(priv, i);
		if (ret)
			continue;
	}

	for (i = 0; i < MAX96712_SRC_PAD_NUM; i++) {
		ret = max96712_parse_src_dt_endpoint(priv, i, i + MAX96712_SRC_PAD_START);
		if (ret)
			continue;
	}

	for (i = 0; i < ARRAY_SIZE(max96712_lane_configs); i++) {
		bool matching = true;

		for (j = 0; j < MAX96712_SRC_PAD_NUM; j++) {
			if (priv->mipi_en[j] && priv->mipi[j].num_data_lanes !=
			    max96712_lane_configs[i][j]) {
				matching = false;
				break;
			}
		}

		if (matching)
			break;
	}

	if (i == ARRAY_SIZE(max96712_lane_configs)) {
		dev_err(priv->dev, "Invalid lane configuration\n");
		return -EINVAL;
	}

	priv->lane_config = i;

	return 0;
}

static const struct regmap_config max96712_i2c_regmap = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0x1f00,
};

static int max96712_probe(struct i2c_client *client)
{
	struct max96712_priv *priv;
	int ret;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &client->dev;
	priv->client = client;
	i2c_set_clientdata(client, priv);

	priv->regmap = devm_regmap_init_i2c(client, &max96712_i2c_regmap);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	priv->gpiod_pwdn = devm_gpiod_get_optional(&client->dev, "enable",
						   GPIOD_OUT_HIGH);
	if (IS_ERR(priv->gpiod_pwdn))
		return PTR_ERR(priv->gpiod_pwdn);

	gpiod_set_consumer_name(priv->gpiod_pwdn, "max96712-pwdn");
	gpiod_set_value_cansleep(priv->gpiod_pwdn, 1);

	if (priv->gpiod_pwdn)
		usleep_range(4000, 5000);

#if 0
	if (max96712_read(priv, 0x4a) != MAX96712_ID)
		return -ENODEV;
#endif

	max96712_reset(priv);

	ret = max96712_parse_dt(priv);
	if (ret)
		return ret;

	max96712_mipi_configure(priv);

	return max96712_v4l2_register(priv);
}

static int max96712_remove(struct i2c_client *client)
{
	struct max96712_priv *priv = i2c_get_clientdata(client);

	max96712_v4l2_unregister(priv);

	gpiod_set_value_cansleep(priv->gpiod_pwdn, 0);

	return 0;
}

static const struct of_device_id max96712_of_table[] = {
	{ .compatible = "maxim,max96712" },
	{ .compatible = "maxim,max96724" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, max96712_of_table);

static struct i2c_driver max96712_i2c_driver = {
	.driver	= {
		.name = "max96712",
		.of_match_table	= of_match_ptr(max96712_of_table),
	},
	.probe_new = max96712_probe,
	.remove = max96712_remove,
};

module_i2c_driver(max96712_i2c_driver);

MODULE_DESCRIPTION("Maxim MAX96712 Quad GMSL2 Deserializer Driver");
MODULE_AUTHOR("Niklas Söderlund <niklas.soderlund@ragnatech.se>");
MODULE_LICENSE("GPL");
