// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim MAX96712 Quad GMSL2 Deserializer Driver
 *
 * Copyright (C) 2021 Renesas Electronics Corporation
 * Copyright (C) 2021 Niklas Söderlund
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/i2c-mux.h>
#include <linux/module.h>
#include <linux/of_graph.h>
#include <linux/regmap.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define MAX96712_ID 0x20

#define MAX96712_DPLL_FREQ 1000

#define MAX96712_SOURCE_PAD	0
#define MAX96712_SINK_PAD	1
#define MAX96712_PAD_NUM	2

#define MAX96712_SUBDEVS_NUM	4

#define v4l2_subdev_state v4l2_subdev_pad_config
#define v4l2_subdev_alloc_state v4l2_subdev_alloc_pad_config
#define v4l2_subdev_free_state v4l2_subdev_free_pad_config
#undef v4l2_async_notifier_add_fwnode_subdev
#define v4l2_async_notifier_add_fwnode_subdev(__notifier, __fwnode, __type) \
((__type *)__v4l2_async_notifier_add_fwnode_subdev(__notifier, __fwnode,    \
                           sizeof(__type)))

static const struct regmap_config max96712_i2c_regmap = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0x1f00,
};

struct max96712_asd {
	struct v4l2_async_subdev base;
	struct max96712_subdev_priv *sd_priv;
};

struct max96712_subdev_priv {
	struct v4l2_subdev sd;
	unsigned int index;
	struct fwnode_handle *fwnode;

	struct max96712_priv *priv;

	struct v4l2_subdev *slave_sd;
	struct fwnode_handle *slave_fwnode;
	struct v4l2_subdev_state *slave_sd_state;
	unsigned int slave_sd_pad_id;

	struct v4l2_async_notifier notifier;
	struct media_pad pads[MAX96712_PAD_NUM];

	struct v4l2_fwnode_bus_mipi_csi2 mipi;

	bool active;

	unsigned int tun_dest;
};

struct max96712_priv {
	struct device *dev;
	struct dentry *debugfs_root;
	struct i2c_client *client;
	struct regmap *regmap;
	struct gpio_desc *gpiod_pwdn;

	struct i2c_mux_core *mux;
	unsigned int mux_channel;

	unsigned int lane_config;
	bool skip_subdev_s_stream;
	struct mutex lock;
	bool active;

	struct max96712_subdev_priv sd_privs[MAX96712_SUBDEVS_NUM];

	unsigned			cached_reg_addr;
	char				read_buf[20];
	unsigned int			read_buf_len;
};

static struct max96712_subdev_priv *next_subdev(struct max96712_priv *priv,
						struct max96712_subdev_priv *sd_priv)
{
	if (!sd_priv)
		sd_priv = &priv->sd_privs[0];
	else
		sd_priv++;

	for (; sd_priv < &priv->sd_privs[MAX96712_SUBDEVS_NUM]; sd_priv++) {
		if (sd_priv->fwnode)
			return sd_priv;
	}

	return NULL;
}

#define for_each_subdev(priv, sd_priv) \
	for ((sd_priv) = NULL; ((sd_priv) = next_subdev((priv), (sd_priv))); )

static inline struct max96712_asd *to_max96712_asd(struct v4l2_async_subdev *asd)
{
	return container_of(asd, struct max96712_asd, base);
}

static inline struct max96712_subdev_priv *sd_to_max96712(struct v4l2_subdev *sd)
{
	return container_of(sd, struct max96712_subdev_priv, sd);
}

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
	unsigned int retry = 100;
	int ret;

retry:
	ret = regmap_update_bits(priv->regmap, reg, mask, val);
	if (ret)
		dev_err(priv->dev, "update 0x%04x failed\n", reg);

	if (ret && retry != 0) {
		retry--;
		udelay(1000);
		goto retry;
	}

	return ret;
}

static void max96712_reset(struct max96712_priv *priv)
{
	max96712_update_bits(priv, 0x13, 0x40, 0x40);
	msleep(80);
}

static int max96712_wait_for_device(struct max96712_priv *priv)
{
	unsigned int i;
	int ret;

	for (i = 0; i < 100; i++) {
		ret = max96712_read(priv, 0x0);
		if (ret >= 0)
			return 0;

		msleep(10);

		dev_err(priv->dev, "Retry %u waiting for deserializer\n", ret);
	}

	return ret;
}

static int max96712_i2c_mux_select(struct i2c_mux_core *muxc, u32 chan)
{
	struct max96712_priv *priv = i2c_mux_priv(muxc);
	int ret;

	if (priv->mux_channel == chan)
		return 0;

	priv->mux_channel = chan;

	ret = max96712_write(priv, 0x3, (~BIT(chan * 2)) & 0xff);
	if (ret) {
		dev_err(priv->dev, "Failed to write I2C mux config: %d\n", ret);
		return ret;
	}

	usleep_range(3000, 5000);

	return 0;
}

static int max96712_i2c_mux_init(struct max96712_priv *priv)
{
	struct max96712_subdev_priv *sd_priv;
	int ret;

	if (!i2c_check_functionality(priv->client->adapter,
				     I2C_FUNC_SMBUS_WRITE_BYTE_DATA))
		return -ENODEV;

	priv->mux = i2c_mux_alloc(priv->client->adapter, &priv->client->dev,
				  MAX96712_SUBDEVS_NUM, 0, I2C_MUX_LOCKED,
				  max96712_i2c_mux_select, NULL);
	if (!priv->mux)
		return -ENOMEM;

	priv->mux->priv = priv;

	for_each_subdev(priv, sd_priv) {
		ret = i2c_mux_add_adapter(priv->mux, 0, sd_priv->index, 0);
		if (ret < 0)
			goto error;
	}

	return 0;

error:
	i2c_mux_del_adapters(priv->mux);

	return ret;
}

static int __max96712_mipi_update(struct max96712_priv *priv)
{
	struct max96712_subdev_priv *sd_priv;
	bool enable = 0;
	int ret;

	for_each_subdev(priv, sd_priv)
		if (sd_priv->active)
			enable = 1;

	if (enable == priv->active)
		return 0;

	priv->active = enable;

	if (enable) {
		ret = max96712_update_bits(priv, 0x40b, 0x02, 0x02);
		if (ret)
			return ret;

		ret = max96712_update_bits(priv, 0x8a0, 0x80, 0x80);
		if (ret)
			return ret;
	} else {
		ret = max96712_update_bits(priv, 0x8a0, 0x80, 0x00);
		if (ret)
			return ret;

		ret = max96712_update_bits(priv, 0x40b, 0x02, 0x00);
		if (ret)
			return ret;
	}

	return 0;
}

static int max96712_mipi_enable(struct max96712_subdev_priv *sd_priv, bool enable)
{
	struct max96712_priv *priv = sd_priv->priv;
	int ret = 0;

	mutex_lock(&priv->lock);

	if (sd_priv->active == enable)
		goto exit;

	sd_priv->active = enable;

	ret = __max96712_mipi_update(priv);

exit:
	mutex_unlock(&priv->lock);

	return ret;
}

static void max96712_init_phy(struct max96712_subdev_priv *sd_priv)
{
	unsigned int num_data_lanes = sd_priv->mipi.num_data_lanes;
	unsigned int reg, val, shift, mask, clk_bit;
	struct max96712_priv *priv = sd_priv->priv;
	unsigned int index = sd_priv->index;
	unsigned int i;

	/* Configure a lane count. */
	/* TODO: Add support for 1-lane configurations. */
	/* TODO: Add support for 3-lane configurations. */
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
		if (sd_priv->mipi.lane_polarities[i])
			val |= BIT(i == 0 ? clk_bit : i < 3 ? i - 1 : i);
	max96712_update_bits(priv, reg, mask << shift, val << shift);

	/* Set link frequency. */
	max96712_update_bits(priv, 0x415 + 0x3 * index, 0x3f,
			     ((MAX96712_DPLL_FREQ / 100) & 0x1f) | BIT(5));

	/* Set destination controller. */
	shift = index * 2;
	max96712_update_bits(priv, 0x8ca, 0x3 << shift,
			     sd_priv->tun_dest << shift);
	max96712_update_bits(priv, 0x939 + 0x40 * index, 0x30,
			     sd_priv->tun_dest << 4);

	/* Enable. */
	max96712_update_bits(priv, 0x8a2, 0x10 << index, 0x10 << index);
	max96712_update_bits(priv, 0x6, 0x1 << index, 0x1 << index);
}

static void max96712_init(struct max96712_priv *priv)
{
	struct max96712_subdev_priv *sd_priv;

	__max96712_mipi_update(priv);

	/* Select 2x4 or 4x2 mode. */
	max96712_update_bits(priv, 0x8a0, 0x1f, BIT(priv->lane_config));

	for_each_subdev(priv, sd_priv)
		max96712_init_phy(sd_priv);

	/* One-shot reset all PHYs. */
	max96712_write(priv, 0x18, 0x0f);

	/* Disable initial and periodic deskew. */
	max96712_write(priv, 0x983, 0x07);
	max96712_write(priv, 0x984, 0x01);

	/*
	 * Wait for 2ms to allow the link to resynchronize after the
	 * configuration change.
	 */
	usleep_range(2000, 5000);
}

static int max96712_notify_bound(struct v4l2_async_notifier *notifier,
				 struct v4l2_subdev *subdev,
				 struct v4l2_async_subdev *asd)
{
	struct max96712_subdev_priv *sd_priv = sd_to_max96712(notifier->sd);
	struct max96712_priv *priv = sd_priv->priv;
	int ret;

	ret = media_entity_get_fwnode_pad(&subdev->entity,
					  sd_priv->slave_fwnode,
					  MEDIA_PAD_FL_SOURCE);
	if (ret < 0) {
		dev_err(priv->dev,
			"Failed to find pad for %s: %d\n", subdev->name, ret);
		return ret;
	}

	sd_priv->slave_sd = subdev;
	sd_priv->slave_sd_pad_id = ret;

	ret = media_create_pad_link(&sd_priv->slave_sd->entity,
				    sd_priv->slave_sd_pad_id,
				    &sd_priv->sd.entity,
				    MAX96712_SINK_PAD,
				    MEDIA_LNK_FL_ENABLED |
				    MEDIA_LNK_FL_IMMUTABLE);
	if (ret) {
		dev_err(priv->dev,
			"Unable to link %s:%u -> %s:%u\n",
			sd_priv->slave_sd->name,
			sd_priv->slave_sd_pad_id,
			sd_priv->sd.name,
			MAX96712_SINK_PAD);
		return ret;
	}

	dev_err(priv->dev, "Bound %s:%u on %s:%u\n",
		sd_priv->slave_sd->name,
		sd_priv->slave_sd_pad_id,
		sd_priv->sd.name,
		MAX96712_SINK_PAD);

	sd_priv->slave_sd_state = v4l2_subdev_alloc_state(subdev);
	if (IS_ERR(sd_priv->slave_sd_state))
		return PTR_ERR(sd_priv->slave_sd_state);

	/*
	 * Once all cameras have probed, increase the channel amplitude
	 * to compensate for the remote noise immunity threshold and call
	 * the camera post_register operation to complete initialization with
	 * noise immunity enabled.
	 */
	ret = v4l2_subdev_call(sd_priv->slave_sd, core, post_register);
	if (ret) {
		dev_err(priv->dev,
			"Failed to call post register for subdev %s: %d\n",
			sd_priv->sd.name, ret);
		return ret;
	}

	return 0;
}

static void max96712_notify_unbind(struct v4l2_async_notifier *notifier,
				   struct v4l2_subdev *subdev,
				   struct v4l2_async_subdev *asd)
{
	struct max96712_subdev_priv *sd_priv = sd_to_max96712(notifier->sd);

	sd_priv->slave_sd = NULL;
	v4l2_subdev_free_state(sd_priv->slave_sd_state);
	sd_priv->slave_sd_state = NULL;
}

static const struct v4l2_async_notifier_operations max96712_notify_ops = {
	.bound = max96712_notify_bound,
	.unbind = max96712_notify_unbind,
};

static int max96712_v4l2_notifier_register(struct max96712_subdev_priv *sd_priv)
{
	struct max96712_priv *priv = sd_priv->priv;
	struct max96712_asd *mas;
	int ret;

	v4l2_async_notifier_init(&sd_priv->notifier);

	mas = (struct max96712_asd *)
	      v4l2_async_notifier_add_fwnode_subdev(&sd_priv->notifier,
						    sd_priv->slave_fwnode, struct max96712_asd);
	if (IS_ERR(mas)) {
		ret = PTR_ERR(mas);
		dev_err(priv->dev,
			"Failed to add subdev notifier for subdev %s: %d\n",
			sd_priv->sd.name, ret);
		goto error_cleanup_notifier;
	}

	mas->sd_priv = sd_priv;

	sd_priv->notifier.ops = &max96712_notify_ops;
	sd_priv->notifier.flags |= V4L2_ASYNC_NOTIFIER_DEFER_POST_REGISTER;

	ret = v4l2_async_subdev_notifier_register(&sd_priv->sd, &sd_priv->notifier);
	if (ret) {
		dev_err(priv->dev,
			"Failed to register subdev notifier for subdev %s: %d\n",
			sd_priv->sd.name, ret);
		goto error_cleanup_notifier;
	}

	return 0;

error_cleanup_notifier:
	v4l2_async_notifier_cleanup(&sd_priv->notifier);

	return ret;
}

static void max96712_v4l2_notifier_unregister(struct max96712_subdev_priv *sd_priv)
{
	v4l2_async_notifier_unregister(&sd_priv->notifier);
	v4l2_async_notifier_cleanup(&sd_priv->notifier);
}

static int max96712_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct max96712_subdev_priv *sd_priv = sd_to_max96712(sd);
	struct max96712_priv *priv = sd_priv->priv;
	int ret;

	max96712_mipi_enable(sd_priv, enable);

	if (priv->skip_subdev_s_stream)
		return 0;

	ret = v4l2_subdev_call(sd_priv->slave_sd, video, s_stream, enable);
	if (ret) {
		dev_err(priv->dev, "Failed to start stream for %s: %d\n",
			sd_priv->slave_sd->name, ret);
		return ret;
	}

	return 0;
}

static const struct v4l2_subdev_video_ops max96712_video_ops = {
	.s_stream = max96712_s_stream,
};

static int max96712_get_selection(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_selection *sel)
{
	struct max96712_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_selection sd_sel = *sel;
	int ret;

	if (sel->pad != MAX96712_SOURCE_PAD)
		return -EINVAL;

	sd_sel.pad = sd_priv->slave_sd_pad_id;

	ret = v4l2_subdev_call(sd_priv->slave_sd, pad, get_selection,
			       sd_priv->slave_sd_state, &sd_sel);
	if (ret)
		return ret;

	sel->r = sd_sel.r;

	return 0;
}

static int max96712_get_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *sd_state,
			    struct v4l2_subdev_format *format)
{
	struct max96712_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_format sd_format = *format;
	int ret;

	if (format->pad != MAX96712_SOURCE_PAD)
		return -EINVAL;

	sd_format.pad = sd_priv->slave_sd_pad_id;

	ret = v4l2_subdev_call(sd_priv->slave_sd, pad, get_fmt,
			       sd_priv->slave_sd_state, &sd_format);
	if (ret)
		return ret;

	format->format = sd_format.format;

	return 0;
}

static int max96712_set_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *sd_state,
			    struct v4l2_subdev_format *format)
{
	struct max96712_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_format sd_format = *format;
	int ret;

	if (format->pad != MAX96712_SOURCE_PAD)
		return -EINVAL;

	sd_format.pad = sd_priv->slave_sd_pad_id;

	ret = v4l2_subdev_call(sd_priv->slave_sd, pad, set_fmt,
			       sd_priv->slave_sd_state, &sd_format);
	if (ret)
		return ret;

	format->format = sd_format.format;

	return 0;
}

static int max96712_enum_mbus_code(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_mbus_code_enum *code)
{
	struct max96712_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_mbus_code_enum sd_code = *code;
	int ret;

	if (code->pad != MAX96712_SOURCE_PAD)
		return -EINVAL;

	sd_code.pad = sd_priv->slave_sd_pad_id;

	ret = v4l2_subdev_call(sd_priv->slave_sd, pad, enum_mbus_code,
			       sd_priv->slave_sd_state, &sd_code);
	if (ret)
		return ret;

	code->code = sd_code.code;

	return 0;
}

static int max96712_enum_frame_size(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *sd_state,
				    struct v4l2_subdev_frame_size_enum *fse)
{
	struct max96712_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_frame_size_enum sd_fse = *fse;
	int ret;

	if (fse->pad != MAX96712_SOURCE_PAD)
		return -EINVAL;

	sd_fse.pad = sd_priv->slave_sd_pad_id;

	ret = v4l2_subdev_call(sd_priv->slave_sd, pad, enum_frame_size,
			       sd_priv->slave_sd_state, &sd_fse);
	if (ret)
		return ret;

	fse->code = sd_fse.code;
	fse->min_width = sd_fse.min_width;
	fse->max_width = sd_fse.max_width;
	fse->min_height = sd_fse.min_height;
	fse->max_height = sd_fse.max_height;

	return 0;
}

static const struct v4l2_subdev_pad_ops max96712_pad_ops = {
	.get_selection = max96712_get_selection,
	.get_fmt = max96712_get_fmt,
	.set_fmt = max96712_set_fmt,
	.enum_mbus_code = max96712_enum_mbus_code,
	.enum_frame_size = max96712_enum_frame_size,
};

static const struct v4l2_subdev_ops max96712_subdev_ops = {
	.video = &max96712_video_ops,
	.pad = &max96712_pad_ops,
};

static const char *max96712_subdev_names[] = {
	":0", ":1", ":2", ":3"
};

static int max96712_v4l2_register_sd(struct max96712_subdev_priv *sd_priv)
{
	struct max96712_priv *priv = sd_priv->priv;
	unsigned int index = sd_priv->index;
	int ret;

	if (index >= ARRAY_SIZE(max96712_subdev_names))
		return -EINVAL;

	ret = max96712_v4l2_notifier_register(sd_priv);
	if (ret)
		return ret;

	v4l2_i2c_subdev_init(&sd_priv->sd, priv->client, &max96712_subdev_ops);
	/* TODO: format name */
	v4l2_i2c_subdev_set_name(&sd_priv->sd, priv->client, NULL, max96712_subdev_names[index]);
	sd_priv->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	sd_priv->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sd_priv->sd.fwnode = sd_priv->fwnode;

	sd_priv->pads[MAX96712_SOURCE_PAD].flags = MEDIA_PAD_FL_SOURCE;
	sd_priv->pads[MAX96712_SINK_PAD].flags = MEDIA_PAD_FL_SINK;

	ret = media_entity_pads_init(&sd_priv->sd.entity, MAX96712_PAD_NUM, sd_priv->pads);
	if (ret)
		goto error;

	v4l2_set_subdevdata(&sd_priv->sd, sd_priv);

	return v4l2_async_register_subdev(&sd_priv->sd);

error:
	max96712_v4l2_notifier_unregister(sd_priv);

	return ret;
}

static void max96712_v4l2_unregister_sd(struct max96712_subdev_priv *sd_priv)
{
	max96712_v4l2_notifier_unregister(sd_priv);
	v4l2_async_unregister_subdev(&sd_priv->sd);
}

static int max96712_v4l2_register(struct max96712_priv *priv)
{
	struct max96712_subdev_priv *sd_priv;
	int ret;

	for_each_subdev(priv, sd_priv) {
		ret = max96712_v4l2_register_sd(sd_priv);
		if (ret)
			return ret;
	}

	return 0;
}

static void max96712_v4l2_unregister(struct max96712_priv *priv)
{
	struct max96712_subdev_priv *sd_priv;

	for_each_subdev(priv, sd_priv)
		max96712_v4l2_unregister_sd(sd_priv);
}

static int max96712_parse_ch_dt(struct max96712_subdev_priv *sd_priv,
				struct fwnode_handle *fwnode)
{
	int ret;
	u32 val;

	sd_priv->tun_dest = sd_priv->index;
	ret = fwnode_property_read_u32(fwnode, "max,tunnel-destination", &val);
	if (!ret) {
		if (val > MAX96712_SUBDEVS_NUM)
			return -EINVAL;

		sd_priv->tun_dest = val;
	}

	return 0;
}

static int max96712_parse_src_dt_endpoint(struct max96712_subdev_priv *sd_priv,
					  struct fwnode_handle *fwnode)
{
	struct max96712_priv *priv = sd_priv->priv;
	struct v4l2_fwnode_endpoint v4l2_ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	struct fwnode_handle *ep;
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(fwnode, MAX96712_SOURCE_PAD, 0, 0);
	if (!ep) {
		dev_err(priv->dev, "Not connected to subdevice\n");
		return -EINVAL;
	}

	ret = v4l2_fwnode_endpoint_parse(ep, &v4l2_ep);
	fwnode_handle_put(ep);
	if (ret) {
		dev_err(priv->dev, "Could not parse v4l2 endpoint\n");
		return ret;
	}

	sd_priv->mipi = v4l2_ep.bus.mipi_csi2;

	return 0;
}

static int max96712_parse_sink_dt_endpoint(struct max96712_subdev_priv *sd_priv,
					   struct fwnode_handle *fwnode)
{
	struct max96712_priv *priv = sd_priv->priv;
	struct fwnode_handle *ep;

	ep = fwnode_graph_get_endpoint_by_id(fwnode, MAX96712_SINK_PAD, 0, 0);
	if (!ep) {
		dev_err(priv->dev, "Not connected to subdevice\n");
		return -EINVAL;
	}

	sd_priv->slave_fwnode = fwnode_graph_get_remote_endpoint(ep);
	if (!sd_priv->slave_fwnode) {
		dev_err(priv->dev, "Not connected to remote endpoint\n");

		return -EINVAL;
	}

	return 0;
}

static const unsigned int max96712_lane_configs[][MAX96712_SUBDEVS_NUM] = {
	{ 2, 2, 2, 2 },
	{ 0, 0, 0, 0 },
	{ 0, 4, 0, 4 },
	{ 0, 4, 2, 2 },
	{ 2, 2, 0, 4 },
};

static int max96712_parse_dt(struct max96712_priv *priv)
{
	struct max96712_subdev_priv *sd_priv;
	struct fwnode_handle *fwnode;
	unsigned int i, j;
	u32 index;
	int ret;

	priv->skip_subdev_s_stream = device_property_read_bool(priv->dev,
			"max,skip-subdev-s-stream");

	fwnode_for_each_child_node(dev_fwnode(priv->dev), fwnode) {
		struct device_node *of_node = to_of_node(fwnode);

		if (!of_node_name_eq(of_node, "channel"))
			continue;

		ret = fwnode_property_read_u32(fwnode, "reg", &index);
		if (ret)
			continue;

		sd_priv = &priv->sd_privs[index];
		sd_priv->fwnode = fwnode;
		sd_priv->priv = priv;
		sd_priv->index = index;

		ret = max96712_parse_ch_dt(sd_priv, fwnode);
		if (ret)
			continue;

		ret = max96712_parse_sink_dt_endpoint(sd_priv, fwnode);
		if (ret)
			continue;

		ret = max96712_parse_src_dt_endpoint(sd_priv, fwnode);
		if (ret)
			continue;
	}

	for (i = 0; i < ARRAY_SIZE(max96712_lane_configs); i++) {
		bool matching = true;

		for (j = 0; j < MAX96712_SUBDEVS_NUM; j++) {
			sd_priv = &priv->sd_privs[j];

			if (sd_priv->fwnode && sd_priv->mipi.num_data_lanes !=
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

static int max96712_dump_regs(struct max96712_priv *priv, struct seq_file *m)
{
	static const struct {
		unsigned int start;
		unsigned int end;
	} registers[] = {
		{0x0, 0x1271},
	};
	unsigned int i, j;
	int val;

	for (i = 0; i < ARRAY_SIZE(registers); i++) {
		for (j = registers[i].start; j <= registers[i].end; j++) {
			val = max96712_read(priv, j);
			if (val < 0)
				return -EINVAL;

			seq_printf(m, "0x%04x: 0x%02x\n", j, val);
		}
	}

	return 0;
}

static int max96712_dump_regs_show(struct seq_file *m, void *private)
{
	struct max96712_priv *priv = m->private;

	return max96712_dump_regs(priv, m);
}
DEFINE_SHOW_ATTRIBUTE(max96712_dump_regs);

static ssize_t max96712_debugfs_read_reg(struct file *file, char __user *userbuf,
				      size_t count, loff_t *ppos)
{
	struct max96712_priv *priv = file->private_data;
	int ret;

	if (*ppos > 0)
		return simple_read_from_buffer(userbuf, count, ppos,
					       priv->read_buf,
					       priv->read_buf_len);

	ret = max96712_read(priv, priv->cached_reg_addr);
	if (ret < 0) {
		dev_err(priv->dev, "%s: read failed\n", __func__);
		return ret;
	}

	priv->read_buf_len = snprintf(priv->read_buf,
				      sizeof(priv->read_buf),
				      "0x%02X\n", ret);

	return simple_read_from_buffer(userbuf, count, ppos,
				       priv->read_buf,
				       priv->read_buf_len);
}

static ssize_t max96712_debugfs_write_reg(struct file *file,
				       const char __user *userbuf,
				       size_t count, loff_t *ppos)
{
	struct max96712_priv *priv = file->private_data;
	unsigned reg, val;
	char buf[80];
	int ret;

	count = min_t(size_t, count, (sizeof(buf)-1));
	if (copy_from_user(buf, userbuf, count))
		return -EFAULT;

	buf[count] = 0;

	ret = sscanf(buf, "%i %i", &reg, &val);

	if (ret != 1 && ret != 2)
		return -EINVAL;

	priv->cached_reg_addr = reg;

	if (ret == 1)
		return count;

	ret = max96712_write(priv, reg, val);
	if (ret) {
		dev_err(priv->dev, "%s: write failed\n", __func__);
		return ret;
	}

	return count;
}

static const struct file_operations max96712_reg_fops = {
	.open = simple_open,
	.read = max96712_debugfs_read_reg,
	.write = max96712_debugfs_write_reg,
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

	priv->debugfs_root = debugfs_create_dir(dev_name(priv->dev), NULL);
	debugfs_create_file("dump_regs", 0600, priv->debugfs_root, priv,
			    &max96712_dump_regs_fops);
	debugfs_create_file("reg", 0600, priv->debugfs_root, priv,
			    &max96712_reg_fops);

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

	ret = max96712_wait_for_device(priv);
	if (ret)
		return ret;

	ret = max96712_parse_dt(priv);
	if (ret)
		return ret;

	ret = max96712_i2c_mux_init(priv);
	if (ret)
		return ret;

	max96712_init(priv);

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
