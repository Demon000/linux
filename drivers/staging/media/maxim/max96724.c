// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim MAX96724 Quad GMSL2 Deserializer Driver
 *
 * Copyright (C) 2023 Analog Devices Inc.
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

#define MAX96724_ID 0x20

#define MAX96724_DPLL_FREQ 2500

#define MAX96724_SOURCE_PAD	0
#define MAX96724_SINK_PAD	1
#define MAX96724_PAD_NUM	2

/* TODO: allow infinite subdevs. */
#define MAX96724_SUBDEVS_NUM	4
#define MAX96724_PHYS_NUM	4
#define MAX96724_PIPES_NUM	4
#define MAX96724_STREAMS_NUM	4
#define MAX96724_LINKS_NUM	4
#define MAX96724_REMAP_EL_NUM	5
#define MAX96724_REMAPS_NUM	16

#define MAX96724_MUX_CH_INVALID	-1

#define v4l2_subdev_state v4l2_subdev_pad_config
#define v4l2_subdev_alloc_state v4l2_subdev_alloc_pad_config
#define v4l2_subdev_free_state v4l2_subdev_free_pad_config
#undef v4l2_async_notifier_add_fwnode_subdev
#define v4l2_async_notifier_add_fwnode_subdev(__notifier, __fwnode, __type) \
((__type *)__v4l2_async_notifier_add_fwnode_subdev(__notifier, __fwnode,    \
                           sizeof(__type)))

static const struct regmap_config max96724_i2c_regmap = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0x1f00,
};

struct max96724_asd {
	struct v4l2_async_subdev base;
	struct max96724_subdev_priv *sd_priv;
};

#define MAX96724_DT_VC(dt, vc) (((vc) & 0x3) << 6 | ((dt) & 0x3f))

struct max96724_dt_vc_remap {
	u8 from_dt;
	u8 from_vc;
	u8 to_dt;
	u8 to_vc;
	u8 phy;
};

struct max96724_subdev_priv {
	struct v4l2_subdev sd;
	unsigned int index;
	struct fwnode_handle *fwnode;

	struct max96724_priv *priv;

	struct v4l2_subdev *slave_sd;
	struct fwnode_handle *slave_fwnode;
	struct v4l2_subdev_state *slave_sd_state;
	unsigned int slave_sd_pad_id;

	struct v4l2_async_notifier notifier;
	struct media_pad pads[MAX96724_PAD_NUM];

	bool active;
	unsigned int pipe_id;
	struct max96724_dt_vc_remap remaps[MAX96724_REMAPS_NUM];
	unsigned int num_remaps;
};

struct max96724_pipe {
	unsigned int index;
	unsigned int dest_phy;
	unsigned int src_stream_id;
	unsigned int src_gmsl_link;
	struct max96724_dt_vc_remap remaps[MAX96724_REMAPS_NUM];
	unsigned int num_remaps;
	bool enabled;
};

struct max96724_phy {
	unsigned int index;
	struct v4l2_fwnode_bus_mipi_csi2 mipi;
	bool enabled;
};

struct max96724_priv {
	struct device *dev;
	struct dentry *debugfs_root;
	struct i2c_client *client;
	struct regmap *regmap;
	struct gpio_desc *gpiod_pwdn;

	struct i2c_mux_core *mux;
	int mux_channel;

	unsigned int lane_config;
	struct mutex lock;
	bool active;

	struct max96724_phy phys[MAX96724_PHYS_NUM];
	struct max96724_pipe pipes[MAX96724_PHYS_NUM];
	struct max96724_subdev_priv sd_privs[MAX96724_SUBDEVS_NUM];

	unsigned			cached_reg_addr;
	char				read_buf[20];
	unsigned int			read_buf_len;
};

static struct max96724_subdev_priv *next_subdev(struct max96724_priv *priv,
						struct max96724_subdev_priv *sd_priv)
{
	if (!sd_priv)
		sd_priv = &priv->sd_privs[0];
	else
		sd_priv++;

	for (; sd_priv < &priv->sd_privs[MAX96724_SUBDEVS_NUM]; sd_priv++) {
		if (sd_priv->fwnode)
			return sd_priv;
	}

	return NULL;
}

#define for_each_subdev(priv, sd_priv) \
	for ((sd_priv) = NULL; ((sd_priv) = next_subdev((priv), (sd_priv))); )

static inline struct max96724_asd *to_max96724_asd(struct v4l2_async_subdev *asd)
{
	return container_of(asd, struct max96724_asd, base);
}

static inline struct max96724_subdev_priv *sd_to_max96724(struct v4l2_subdev *sd)
{
	return container_of(sd, struct max96724_subdev_priv, sd);
}

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

static int max96724_wait_for_device(struct max96724_priv *priv)
{
	unsigned int i;
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

static int max96724_reset(struct max96724_priv *priv)
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

static int max96724_i2c_mux_select(struct i2c_mux_core *muxc, u32 chan)
{
	struct max96724_priv *priv = i2c_mux_priv(muxc);
	int ret;

	if (priv->mux_channel == chan)
		return 0;

	priv->mux_channel = chan;

	ret = max96724_write(priv, 0x3, (~BIT(chan * 2)) & 0xff);
	if (ret) {
		dev_err(priv->dev, "Failed to write I2C mux config: %d\n", ret);
		return ret;
	}

	usleep_range(3000, 5000);

	return 0;
}

static int max96724_i2c_mux_init(struct max96724_priv *priv)
{
	struct max96724_subdev_priv *sd_priv;
	int ret;

	if (!i2c_check_functionality(priv->client->adapter,
				     I2C_FUNC_SMBUS_WRITE_BYTE_DATA))
		return -ENODEV;

	priv->mux_channel = MAX96724_MUX_CH_INVALID;

	priv->mux = i2c_mux_alloc(priv->client->adapter, &priv->client->dev,
				  MAX96724_SUBDEVS_NUM, 0, I2C_MUX_LOCKED,
				  max96724_i2c_mux_select, NULL);
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

static int __max96724_mipi_update(struct max96724_priv *priv)
{
	struct max96724_subdev_priv *sd_priv;
	bool enable = 0;
	int ret;

	for_each_subdev(priv, sd_priv)
		if (sd_priv->active)
			enable = 1;

	if (enable == priv->active)
		return 0;

	priv->active = enable;

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

static int max96724_mipi_enable(struct max96724_subdev_priv *sd_priv, bool enable)
{
	struct max96724_priv *priv = sd_priv->priv;
	int ret = 0;

	mutex_lock(&priv->lock);

	if (sd_priv->active == enable)
		goto exit;

	sd_priv->active = enable;

	ret = __max96724_mipi_update(priv);

exit:
	mutex_unlock(&priv->lock);

	return ret;
}

static int max96724_init_phy(struct max96724_priv *priv,
			     struct max96724_phy *phy)
{
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
				   MAX96724_DPLL_FREQ / 100);
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
	max96724_update_bits(priv, 0x8a2, val, val);

	return 0;
}

static int max96724_init_pipe_remap(struct max96724_priv *priv,
				    struct max96724_pipe *pipe,
				    struct max96724_dt_vc_remap *remap,
				    unsigned int i)
{
	unsigned int index = pipe->index;
	unsigned int reg, val, shift, mask;
	int ret;

	/* Set source Data Type and Virtual Channel. */
	/* TODO: implement extended Virtual Channel. */
	reg = 0x90d + 0x40 * index + i * 2;
	ret = max96724_write(priv, reg,
			     MAX96724_DT_VC(remap->from_dt, remap->from_vc));
	if (ret)
		return ret;

	/* Set destination Data Type and Virtual Channel. */
	/* TODO: implement extended Virtual Channel. */
	reg = 0x90e + 0x40 * index + i * 2;
	ret = max96724_write(priv, reg,
			     MAX96724_DT_VC(remap->to_dt, remap->to_vc));
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
				     struct max96724_pipe *pipe)
{
	unsigned int i;
	int ret;

	for (i = 0; i < pipe->num_remaps; i++) {
		struct max96724_dt_vc_remap *remap = &pipe->remaps[i];

		ret = max96724_init_pipe_remap(priv, pipe, remap, i);
		if (ret)
			return ret;
	}

	return 0;
}

static int max96724_update_pipe_remaps(struct max96724_priv *priv,
				       struct max96724_pipe *pipe)
{
	struct max96724_subdev_priv *sd_priv;
	unsigned int i;

	pipe->num_remaps = 0;

	for_each_subdev(priv, sd_priv) {
		if (sd_priv->pipe_id != pipe->index)
			continue;

		for (i = 0; i < sd_priv->num_remaps; i++) {
			if (pipe->num_remaps == MAX96724_REMAPS_NUM) {
				dev_err(priv->dev, "Too many remaps\n");
				return -EINVAL;
			}

			pipe->remaps[pipe->num_remaps++] = sd_priv->remaps[i];
		}
	}

	return 0;
}

static int max96724_update_pipes_remaps(struct max96724_priv *priv)
{
	struct max96724_pipe *pipe;
	unsigned int i;
	int ret;

	for (i = 0; i < MAX96724_PIPES_NUM; i++) {
		pipe = &priv->pipes[i];

		if (!pipe->enabled)
			continue;

		ret = max96724_update_pipe_remaps(priv, pipe);
		if (ret)
			return ret;
	}

	return 0;
}

static int max96724_init_pipe(struct max96724_priv *priv,
			      struct max96724_pipe *pipe)
{
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
	ret = max96724_update_bits(priv, reg, 0x3 << shift, pipe->src_gmsl_link << shift);
	if (ret)
		return ret;

	/* Enable link. */
	val = BIT(pipe->src_gmsl_link);
	ret = max96724_update_bits(priv, 0x6, val, val);
	if (ret)
		return ret;

	return 0;
}

static int max96724_init(struct max96724_priv *priv)
{
	struct max96724_pipe *pipe;
	struct max96724_phy *phy;
	unsigned int i;
	int ret;

	ret = __max96724_mipi_update(priv);
	if (ret)
		return ret;

	/* Select 2x4 or 4x2 mode. */
	ret = max96724_update_bits(priv, 0x8a0, 0x1f, BIT(priv->lane_config));
	if (ret)
		return ret;

	/* Set alternate memory map mode for 12bpp. */
	/* TODO: make dynamic. */
	ret = max96724_write(priv, 0x9b3, 0x01);
	if (ret)
		return ret;

	/* Disable all PHYs. */
	ret = max96724_update_bits(priv, 0x8a2, GENMASK(7, 4), 0x00);
	if (ret)
		return ret;

	/* Disable automatic stream select. */
	ret = max96724_update_bits(priv, 0xf4, BIT(4), 0x00);
	if (ret)
		return ret;

	for (i = 0; i < MAX96724_PHYS_NUM; i++) {
		phy = &priv->phys[i];

		if (!phy->enabled)
			continue;

		ret = max96724_init_phy(priv, phy);
		if (ret)
			return ret;
	}

	/* Disable all pipes. */
	ret = max96724_update_bits(priv, 0xf4, GENMASK(3, 0), 0x00);
	if (ret)
		return ret;

	for (i = 0; i < MAX96724_PIPES_NUM; i++) {
		pipe = &priv->pipes[i];

		if (!pipe->enabled)
			continue;

		ret = max96724_init_pipe(priv, pipe);
		if (ret)
			return ret;

		ret = max96724_init_pipe_remaps(priv, pipe);
		if (ret)
			return ret;
	}

	/* One-shot reset all PHYs. */
	ret = max96724_write(priv, 0x18, 0x0f);
	if (ret)
		return ret;

	/*
	 * Wait for 2ms to allow the link to resynchronize after the
	 * configuration change.
	 */
	usleep_range(2000, 5000);

	return 0;
}

static int max96724_notify_bound(struct v4l2_async_notifier *notifier,
				 struct v4l2_subdev *subdev,
				 struct v4l2_async_subdev *asd)
{
	struct max96724_subdev_priv *sd_priv = sd_to_max96724(notifier->sd);
	struct max96724_priv *priv = sd_priv->priv;
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
				    MAX96724_SINK_PAD,
				    MEDIA_LNK_FL_ENABLED |
				    MEDIA_LNK_FL_IMMUTABLE);
	if (ret) {
		dev_err(priv->dev,
			"Unable to link %s:%u -> %s:%u\n",
			sd_priv->slave_sd->name,
			sd_priv->slave_sd_pad_id,
			sd_priv->sd.name,
			MAX96724_SINK_PAD);
		return ret;
	}

	dev_err(priv->dev, "Bound %s:%u on %s:%u\n",
		sd_priv->slave_sd->name,
		sd_priv->slave_sd_pad_id,
		sd_priv->sd.name,
		MAX96724_SINK_PAD);

	sd_priv->slave_sd_state = v4l2_subdev_alloc_state(subdev);
	if (IS_ERR(sd_priv->slave_sd_state))
		return PTR_ERR(sd_priv->slave_sd_state);

	ret = v4l2_subdev_call(sd_priv->slave_sd, core, post_register);
	if (ret) {
		dev_err(priv->dev,
			"Failed to call post register for subdev %s: %d\n",
			sd_priv->sd.name, ret);
		return ret;
	}

	return 0;
}

static void max96724_notify_unbind(struct v4l2_async_notifier *notifier,
				   struct v4l2_subdev *subdev,
				   struct v4l2_async_subdev *asd)
{
	struct max96724_subdev_priv *sd_priv = sd_to_max96724(notifier->sd);

	sd_priv->slave_sd = NULL;
	v4l2_subdev_free_state(sd_priv->slave_sd_state);
	sd_priv->slave_sd_state = NULL;
}

static const struct v4l2_async_notifier_operations max96724_notify_ops = {
	.bound = max96724_notify_bound,
	.unbind = max96724_notify_unbind,
};

static int max96724_v4l2_notifier_register(struct max96724_subdev_priv *sd_priv)
{
	struct max96724_priv *priv = sd_priv->priv;
	struct max96724_asd *mas;
	int ret;

	v4l2_async_notifier_init(&sd_priv->notifier);

	mas = (struct max96724_asd *)
	      v4l2_async_notifier_add_fwnode_subdev(&sd_priv->notifier,
						    sd_priv->slave_fwnode, struct max96724_asd);
	if (IS_ERR(mas)) {
		ret = PTR_ERR(mas);
		dev_err(priv->dev,
			"Failed to add subdev notifier for subdev %s: %d\n",
			sd_priv->sd.name, ret);
		goto error_cleanup_notifier;
	}

	mas->sd_priv = sd_priv;

	sd_priv->notifier.ops = &max96724_notify_ops;
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

static int max96724_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct max96724_subdev_priv *sd_priv = sd_to_max96724(sd);
	struct max96724_priv *priv = sd_priv->priv;
	int ret;

	max96724_mipi_enable(sd_priv, enable);

	ret = v4l2_subdev_call(sd_priv->slave_sd, video, s_stream, enable);
	if (ret) {
		dev_err(priv->dev, "Failed to start stream for %s: %d\n",
			sd_priv->slave_sd->name, ret);
		return ret;
	}

	return 0;
}

static const struct v4l2_subdev_video_ops max96724_video_ops = {
	.s_stream = max96724_s_stream,
};

static int max96724_get_selection(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_selection *sel)
{
	struct max96724_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_selection sd_sel = *sel;
	int ret;

	if (sel->pad != MAX96724_SOURCE_PAD)
		return -EINVAL;

	sd_sel.pad = sd_priv->slave_sd_pad_id;

	ret = v4l2_subdev_call(sd_priv->slave_sd, pad, get_selection,
			       sd_priv->slave_sd_state, &sd_sel);
	if (ret)
		return ret;

	sel->r = sd_sel.r;

	return 0;
}

static int max96724_get_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *sd_state,
			    struct v4l2_subdev_format *format)
{
	struct max96724_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_format sd_format = *format;
	int ret;

	if (format->pad != MAX96724_SOURCE_PAD)
		return -EINVAL;

	sd_format.pad = sd_priv->slave_sd_pad_id;

	ret = v4l2_subdev_call(sd_priv->slave_sd, pad, get_fmt,
			       sd_priv->slave_sd_state, &sd_format);
	if (ret)
		return ret;

	format->format = sd_format.format;

	return 0;
}

static int max96724_set_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *sd_state,
			    struct v4l2_subdev_format *format)
{
	struct max96724_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_format sd_format = *format;
	int ret;

	if (format->pad != MAX96724_SOURCE_PAD)
		return -EINVAL;

	sd_format.pad = sd_priv->slave_sd_pad_id;

	ret = v4l2_subdev_call(sd_priv->slave_sd, pad, set_fmt,
			       sd_priv->slave_sd_state, &sd_format);
	if (ret)
		return ret;

	format->format = sd_format.format;

	return 0;
}

static int max96724_enum_mbus_code(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_mbus_code_enum *code)
{
	struct max96724_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_mbus_code_enum sd_code = *code;
	int ret;

	if (code->pad != MAX96724_SOURCE_PAD)
		return -EINVAL;

	sd_code.pad = sd_priv->slave_sd_pad_id;

	ret = v4l2_subdev_call(sd_priv->slave_sd, pad, enum_mbus_code,
			       sd_priv->slave_sd_state, &sd_code);
	if (ret)
		return ret;

	code->code = sd_code.code;

	return 0;
}

static int max96724_enum_frame_size(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *sd_state,
				    struct v4l2_subdev_frame_size_enum *fse)
{
	struct max96724_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_frame_size_enum sd_fse = *fse;
	int ret;

	if (fse->pad != MAX96724_SOURCE_PAD)
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

static int max96724_enum_frame_interval(struct v4l2_subdev *sd,
					struct v4l2_subdev_state *sd_state,
					struct v4l2_subdev_frame_interval_enum *fie)
{
	struct max96724_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_frame_interval_enum sd_fie = *fie;
	int ret;

	if (fie->pad != MAX96724_SOURCE_PAD)
		return -EINVAL;

	sd_fie.pad = sd_priv->slave_sd_pad_id;

	ret = v4l2_subdev_call(sd_priv->slave_sd, pad, enum_frame_interval,
			       sd_priv->slave_sd_state, &sd_fie);
	if (ret)
		return ret;

	fie->code = sd_fie.code;
	fie->width = sd_fie.width;
	fie->height = sd_fie.height;
	fie->interval = sd_fie.interval;

	return 0;
}

static const struct v4l2_subdev_pad_ops max96724_pad_ops = {
	.get_selection = max96724_get_selection,
	.get_fmt = max96724_get_fmt,
	.set_fmt = max96724_set_fmt,
	.enum_mbus_code = max96724_enum_mbus_code,
	.enum_frame_size = max96724_enum_frame_size,
	.enum_frame_interval = max96724_enum_frame_interval,
};

static const struct v4l2_subdev_ops max96724_subdev_ops = {
	.video = &max96724_video_ops,
	.pad = &max96724_pad_ops,
};

static int max96724_v4l2_register_sd(struct max96724_subdev_priv *sd_priv)
{
	struct max96724_priv *priv = sd_priv->priv;
	unsigned int index = sd_priv->index;
	char postfix[3];
	int ret;

	ret = max96724_v4l2_notifier_register(sd_priv);
	if (ret)
		return ret;

	snprintf(postfix, sizeof(postfix), ":%d", index);

	v4l2_i2c_subdev_init(&sd_priv->sd, priv->client, &max96724_subdev_ops);
	v4l2_i2c_subdev_set_name(&sd_priv->sd, priv->client, NULL, postfix);
	sd_priv->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	sd_priv->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sd_priv->sd.fwnode = sd_priv->fwnode;

	sd_priv->pads[MAX96724_SOURCE_PAD].flags = MEDIA_PAD_FL_SOURCE;
	sd_priv->pads[MAX96724_SINK_PAD].flags = MEDIA_PAD_FL_SINK;

	ret = media_entity_pads_init(&sd_priv->sd.entity, MAX96724_PAD_NUM, sd_priv->pads);
	if (ret)
		goto error;

	v4l2_set_subdevdata(&sd_priv->sd, sd_priv);

	return v4l2_async_register_subdev(&sd_priv->sd);

error:
	v4l2_async_notifier_unregister(&sd_priv->notifier);
	v4l2_async_notifier_cleanup(&sd_priv->notifier);
	media_entity_cleanup(&sd_priv->sd.entity);
	fwnode_handle_put(sd_priv->sd.fwnode);

	return ret;
}

static void max96724_v4l2_unregister_sd(struct max96724_subdev_priv *sd_priv)
{
	v4l2_async_notifier_unregister(&sd_priv->notifier);
	v4l2_async_notifier_cleanup(&sd_priv->notifier);
	v4l2_async_unregister_subdev(&sd_priv->sd);
	media_entity_cleanup(&sd_priv->sd.entity);
	fwnode_handle_put(sd_priv->sd.fwnode);
}

static int max96724_v4l2_register(struct max96724_priv *priv)
{
	struct max96724_subdev_priv *sd_priv;
	int ret;

	for_each_subdev(priv, sd_priv) {
		ret = max96724_v4l2_register_sd(sd_priv);
		if (ret)
			return ret;
	}

	return 0;
}

static void max96724_v4l2_unregister(struct max96724_priv *priv)
{
	struct max96724_subdev_priv *sd_priv;

	for_each_subdev(priv, sd_priv)
		max96724_v4l2_unregister_sd(sd_priv);
}

static int max96724_parse_ch_remap_dt(struct max96724_subdev_priv *sd_priv,
				      struct fwnode_handle *fwnode)
{
	const char *prop_name = "max,dt-vc-phy-remap";
	struct max96724_priv *priv = sd_priv->priv;
	unsigned int i, count;
	u32 *remaps_arr;
	int ret;

	ret = fwnode_property_count_u32(fwnode, prop_name);
	if (ret <= 0)
		return 0;

	count = ret;

	if (count % MAX96724_REMAP_EL_NUM != 0 ||
	    count / MAX96724_REMAP_EL_NUM > MAX96724_REMAPS_NUM) {
		dev_err(priv->dev, "Invalid remap element number %u\n", count);
		return -EINVAL;
	}

	remaps_arr = kcalloc(count, sizeof(u32), GFP_KERNEL);
	if (!remaps_arr)
		return -ENOMEM;

	ret = fwnode_property_read_u32_array(fwnode, prop_name, remaps_arr, count);
	if (ret)
		goto exit;

	for (i = 0; i < count; i += MAX96724_REMAP_EL_NUM) {
		unsigned int index = i / MAX96724_REMAP_EL_NUM;

		sd_priv->remaps[index].from_dt = remaps_arr[i + 0];
		sd_priv->remaps[index].from_vc = remaps_arr[i + 1];
		sd_priv->remaps[index].to_dt = remaps_arr[i + 2];
		sd_priv->remaps[index].to_vc = remaps_arr[i + 3];
		sd_priv->remaps[index].phy = remaps_arr[i + 4];

		if (remaps_arr[i + 4] > MAX96724_PHYS_NUM) {
			dev_err(priv->dev, "Invalid remap PHY %u\n",
				remaps_arr[i + 4]);
			ret = -EINVAL;
			goto exit;
		}

		sd_priv->num_remaps++;
	}

exit:
	kfree(remaps_arr);

	return ret;
}

static int max96724_parse_pipe_dt(struct max96724_priv *priv,
				  struct max96724_pipe *pipe,
				  struct fwnode_handle *fwnode)
{
	u32 val;

	val = pipe->index;
	fwnode_property_read_u32(fwnode, "max,dest-phy", &val);
	if (val > MAX96724_PHYS_NUM) {
		dev_err(priv->dev, "Invalid destination PHY %u\n", val);
		return -EINVAL;
	}
	pipe->dest_phy = val;

	val = pipe->src_stream_id;
	fwnode_property_read_u32(fwnode, "max,src-stream-id", &val);
	if (val > MAX96724_STREAMS_NUM) {
		dev_err(priv->dev, "Invalid source stream %u\n", val);
		return -EINVAL;
	}
	pipe->src_stream_id = val;

	val = pipe->src_gmsl_link;
	fwnode_property_read_u32(fwnode, "max,src-gmsl-link", &val);
	if (val > MAX96724_LINKS_NUM) {
		dev_err(priv->dev, "Invalid source link %u\n", val);
		return -EINVAL;
	}
	pipe->src_gmsl_link = val;

	return 0;
}

static int max96724_parse_ch_dt(struct max96724_subdev_priv *sd_priv,
				struct fwnode_handle *fwnode)
{
	struct max96724_priv *priv = sd_priv->priv;
	struct max96724_pipe *pipe;
	struct max96724_phy *phy;
	u32 val;

	val = sd_priv->index;
	fwnode_property_read_u32(fwnode, "max,pipe-id", &val);
	if (val > MAX96724_PHYS_NUM) {
		dev_err(priv->dev, "Invalid destination PHY %u\n", val);
		return -EINVAL;
	}
	sd_priv->pipe_id = val;

	pipe = &priv->pipes[val];
	pipe->enabled = true;

	val = pipe->index;
	fwnode_property_read_u32(fwnode, "max,dest-phy", &val);
	if (val > MAX96724_PHYS_NUM) {
		dev_err(priv->dev, "Invalid destination PHY %u\n", val);
		return -EINVAL;
	}
	pipe->dest_phy = val;

	phy = &priv->phys[pipe->dest_phy];
	phy->enabled = true;

	return 0;
}

static int max96724_parse_src_dt_endpoint(struct max96724_subdev_priv *sd_priv,
					  struct fwnode_handle *fwnode)
{
	struct max96724_priv *priv = sd_priv->priv;
	struct max96724_pipe *pipe = &priv->pipes[sd_priv->pipe_id];
	struct max96724_phy *phy = &priv->phys[pipe->dest_phy];
	struct v4l2_fwnode_endpoint v4l2_ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	struct fwnode_handle *ep, *remote_ep;
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(fwnode, MAX96724_SOURCE_PAD, 0, 0);
	if (!ep) {
		dev_err(priv->dev, "Not connected to subdevice\n");
		return -EINVAL;
	}

	remote_ep = fwnode_graph_get_remote_endpoint(ep);
	fwnode_handle_put(ep);
	if (!remote_ep) {
		dev_err(priv->dev, "Not connected to remote endpoint\n");
		return -EINVAL;
	}

	ret = v4l2_fwnode_endpoint_parse(remote_ep, &v4l2_ep);
	fwnode_handle_put(remote_ep);
	if (ret) {
		dev_err(priv->dev, "Could not parse v4l2 endpoint\n");
		return ret;
	}

	/* TODO: check the rest of the MIPI configuration. */
	if (phy->mipi.num_data_lanes && phy->mipi.num_data_lanes !=
	    v4l2_ep.bus.mipi_csi2.num_data_lanes) {
		dev_err(priv->dev, "PHY configured with differing number of data lanes\n");
		return -EINVAL;
	}

	phy->mipi = v4l2_ep.bus.mipi_csi2;

	return 0;
}

static int max96724_parse_sink_dt_endpoint(struct max96724_subdev_priv *sd_priv,
					   struct fwnode_handle *fwnode)
{
	struct max96724_priv *priv = sd_priv->priv;
	struct fwnode_handle *ep;

	ep = fwnode_graph_get_endpoint_by_id(fwnode, MAX96724_SINK_PAD, 0, 0);
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

static const unsigned int max96724_lane_configs[][MAX96724_SUBDEVS_NUM] = {
	{ 2, 2, 2, 2 },
	{ 0, 0, 0, 0 },
	{ 0, 4, 4, 0 },
	{ 0, 4, 2, 2 },
	{ 2, 2, 4, 0 },
};

static int max96724_parse_dt(struct max96724_priv *priv)
{
	struct max96724_subdev_priv *sd_priv;
	struct fwnode_handle *fwnode;
	struct max96724_pipe *pipe;
	struct max96724_phy *phy;
	unsigned int i, j;
	u32 index;
	int ret;

	for (i = 0; i < MAX96724_PHYS_NUM; i++) {
		phy = &priv->phys[i];
		phy->index = i;
	}

	for (i = 0; i < MAX96724_PIPES_NUM; i++) {
		pipe = &priv->pipes[i];
		pipe->index = i;
		pipe->src_gmsl_link = i;
	}

	device_for_each_child_node(priv->dev, fwnode) {
		struct device_node *of_node = to_of_node(fwnode);

		if (!of_node_name_eq(of_node, "pipe"))
			continue;

		ret = fwnode_property_read_u32(fwnode, "reg", &index);
		if (ret) {
			dev_err(priv->dev, "Failed to read reg: %d\n", ret);
			continue;
		}

		if (index >= MAX96724_PIPES_NUM) {
			dev_err(priv->dev, "Invalid pipe number %u\n", index);
			return -EINVAL;
		}

		pipe = &priv->pipes[index];

		ret = max96724_parse_pipe_dt(priv, pipe, fwnode);
		if (ret)
			return ret;
	}

	device_for_each_child_node(priv->dev, fwnode) {
		struct device_node *of_node = to_of_node(fwnode);

		if (!of_node_name_eq(of_node, "channel"))
			continue;

		ret = fwnode_property_read_u32(fwnode, "reg", &index);
		if (ret) {
			dev_err(priv->dev, "Failed to read reg: %d\n", ret);
			continue;
		}

		if (index >= MAX96724_SUBDEVS_NUM) {
			dev_err(priv->dev, "Invalid channel number %u\n", index);
			return -EINVAL;
		}

		sd_priv = &priv->sd_privs[index];
		sd_priv->fwnode = fwnode;
		sd_priv->priv = priv;
		sd_priv->index = index;

		ret = max96724_parse_ch_dt(sd_priv, fwnode);
		if (ret)
			return ret;

		ret = max96724_parse_ch_remap_dt(sd_priv, fwnode);
		if (ret)
			return ret;

		ret = max96724_parse_sink_dt_endpoint(sd_priv, fwnode);
		if (ret)
			return ret;

		ret = max96724_parse_src_dt_endpoint(sd_priv, fwnode);
		if (ret)
			return ret;
	}

	ret = max96724_update_pipes_remaps(priv);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(max96724_lane_configs); i++) {
		bool matching = true;

		for (j = 0; j < MAX96724_PHYS_NUM; j++) {
			phy = &priv->phys[j];

			if (phy->enabled && phy->mipi.num_data_lanes !=
			    max96724_lane_configs[i][j]) {
				matching = false;
				break;
			}
		}

		if (matching)
			break;
	}

	if (i == ARRAY_SIZE(max96724_lane_configs)) {
		dev_err(priv->dev, "Invalid lane configuration\n");
		return -EINVAL;
	}

	priv->lane_config = i;

	return 0;
}

static int max96724_dump_regs(struct max96724_priv *priv, struct seq_file *m)
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
			val = max96724_read(priv, j);
			if (val < 0)
				return -EINVAL;

			seq_printf(m, "0x%04x: 0x%02x\n", j, val);
		}
	}

	return 0;
}

static int max96724_dump_regs_show(struct seq_file *m, void *private)
{
	struct max96724_priv *priv = m->private;

	return max96724_dump_regs(priv, m);
}
DEFINE_SHOW_ATTRIBUTE(max96724_dump_regs);

static ssize_t max96724_debugfs_read_reg(struct file *file, char __user *userbuf,
				      size_t count, loff_t *ppos)
{
	struct max96724_priv *priv = file->private_data;
	int ret;

	if (*ppos > 0)
		return simple_read_from_buffer(userbuf, count, ppos,
					       priv->read_buf,
					       priv->read_buf_len);

	ret = max96724_read(priv, priv->cached_reg_addr);
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

static ssize_t max96724_debugfs_write_reg(struct file *file,
				       const char __user *userbuf,
				       size_t count, loff_t *ppos)
{
	struct max96724_priv *priv = file->private_data;
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

	ret = max96724_write(priv, reg, val);
	if (ret) {
		dev_err(priv->dev, "%s: write failed\n", __func__);
		return ret;
	}

	return count;
}

static const struct file_operations max96724_reg_fops = {
	.open = simple_open,
	.read = max96724_debugfs_read_reg,
	.write = max96724_debugfs_write_reg,
};

static int max96724_probe(struct i2c_client *client)
{
	struct max96724_priv *priv;
	int ret;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &client->dev;
	priv->client = client;
	i2c_set_clientdata(client, priv);

	priv->regmap = devm_regmap_init_i2c(client, &max96724_i2c_regmap);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	priv->debugfs_root = debugfs_create_dir(dev_name(priv->dev), NULL);
	debugfs_create_file("dump_regs", 0600, priv->debugfs_root, priv,
			    &max96724_dump_regs_fops);
	debugfs_create_file("reg", 0600, priv->debugfs_root, priv,
			    &max96724_reg_fops);

	priv->gpiod_pwdn = devm_gpiod_get_optional(&client->dev, "enable",
						   GPIOD_OUT_HIGH);
	if (IS_ERR(priv->gpiod_pwdn))
		return PTR_ERR(priv->gpiod_pwdn);

	gpiod_set_consumer_name(priv->gpiod_pwdn, "max96724-pwdn");
	gpiod_set_value_cansleep(priv->gpiod_pwdn, 1);

	if (priv->gpiod_pwdn)
		usleep_range(4000, 5000);

	ret = max96724_reset(priv);
	if (ret)
		return ret;

	ret = max96724_parse_dt(priv);
	if (ret)
		return ret;

	ret = max96724_init(priv);
	if (ret)
		return ret;

	ret = max96724_i2c_mux_init(priv);
	if (ret)
		return ret;

	return max96724_v4l2_register(priv);
}

static int max96724_remove(struct i2c_client *client)
{
	struct max96724_priv *priv = i2c_get_clientdata(client);

	max96724_v4l2_unregister(priv);

	gpiod_set_value_cansleep(priv->gpiod_pwdn, 0);

	return 0;
}

static const struct of_device_id max96724_of_table[] = {
	{ .compatible = "maxim,max96724" },
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
