// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim GMSL2 Deserializer Driver
 *
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of_graph.h>
#include <linux/regmap.h>

#include <media/mipi-csi2.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include "max_des.h"
#include "max_ser.h"
#include "max_serdes.h"

#define MAX_DES_LINK_FREQUENCY_MIN		100000000ull
#define MAX_DES_LINK_FREQUENCY_DEFAULT		750000000ull
#define MAX_DES_LINK_FREQUENCY_MAX		1250000000ull

struct max_des_channel {
	struct v4l2_subdev sd;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *pixel_rate_ctrl;

	unsigned int index;
	struct fwnode_handle *fwnode;

	struct max_des_priv *priv;
	const struct max_format *fmt;
	struct v4l2_mbus_framefmt framefmt;

	struct media_pad pads[MAX_DES_PAD_NUM];

	bool active;
	unsigned int pipe_id;
	unsigned int phy_id;
	unsigned int src_vc_id;
	unsigned int dst_vc_id;

	struct v4l2_async_notifier nf;
	struct {
		struct v4l2_subdev *sd;
		unsigned int pad;
		struct fwnode_handle *ep_fwnode;
	} source;
};

struct max_des_priv {
	struct max_des *des;

	struct device *dev;
	struct i2c_client *client;

	struct i2c_atr *atr;

	struct max_des_channel *channels;
	unsigned int num_channels;
	struct mutex lock;
};

const struct regmap_config max_des_i2c_regmap = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0x1f00,
};
EXPORT_SYMBOL_GPL(max_des_i2c_regmap);

static struct max_des_channel *next_channel(struct max_des_priv *priv,
					   struct max_des_channel *channel)
{
	if (!channel)
		channel = &priv->channels[0];
	else
		channel++;

	for (; channel < priv->channels + priv->num_channels; channel++) {
		if (channel->fwnode)
			return channel;
	}

	return NULL;
}

#define for_each_channel(priv, channel) \
	for ((channel) = NULL; ((channel) = next_channel((priv), (channel))); )

static inline struct max_des_channel *sd_to_max_des(struct v4l2_subdev *sd)
{
	return container_of(sd, struct max_des_channel, sd);
}

static inline struct max_des_channel *nf_to_max_des(struct v4l2_async_notifier *nf)
{
	return container_of(nf, struct max_des_channel, nf);
}

static int max_des_channel_update(struct max_des_priv *priv)
{
	struct max_des_channel *channel;
	struct max_des *des = priv->des;
	bool enable = 0;

	for_each_channel(priv, channel) {
		if (channel->active) {
			enable = 1;
			break;
		}
	}

	if (enable == des->active)
		return 0;

	des->active = enable;

	return des->ops->set_enable(des, enable);
}

static int max_des_channel_enable(struct max_des_channel *channel, bool enable)
{
	struct max_des_priv *priv = channel->priv;
	int ret = 0;

	mutex_lock(&priv->lock);

	if (channel->active == enable)
		goto exit;

	channel->active = enable;

	ret = max_des_channel_update(priv);

exit:
	mutex_unlock(&priv->lock);

	return ret;
}

static int max_des_pipe_set_remaps(struct max_des_priv *priv,
				   struct max_des_pipe *pipe,
				   struct max_des_dt_vc_remap *remaps,
				   unsigned int num_remaps)
{
	struct max_des *des = priv->des;
	unsigned int i;
	int ret;

	for (i = 0; i < num_remaps; i++) {
		struct max_des_dt_vc_remap *remap = &remaps[i];

		ret = des->ops->set_pipe_remap(des, pipe, i, remap);
		if (ret)
			return ret;

		ret = des->ops->set_pipe_remap_enable(des, pipe, i, true);
		if (ret)
			return ret;
	}

	for (i = num_remaps; i < des->ops->num_remaps_per_pipe; i++) {
		ret = des->ops->set_pipe_remap_enable(des, pipe, i, false);
		if (ret)
			return ret;
	}

	if (pipe->remaps)
		devm_kfree(priv->dev, pipe->remaps);

	pipe->remaps = remaps;
	pipe->num_remaps = num_remaps;

	return 0;
}

static unsigned int max_des_code_num_remaps(u32 code)
{
	u8 dt = max_format_dt_by_code(code);

	if (dt == 0 || dt == MIPI_CSI2_DT_EMBEDDED_8B)
		return 1;

	return 3;
}

static int max_des_pipe_update_phy_tunnel(struct max_des_priv *priv,
					  struct max_des_pipe *pipe)
{
	struct max_des *des = priv->des;
	struct max_des_channel *prev_channel = NULL;
	struct max_des_channel *channel = NULL;


	/*
	 * Check that all channels of a pipe that's routed from a tunnel
	 * link have the same destination phy.
	 */

	for_each_channel(priv, channel) {
		if (channel->pipe_id != pipe->index)
			continue;

		if (prev_channel && prev_channel->phy_id != channel->phy_id)
			return -EINVAL;

		prev_channel = channel;
	}

	if (!channel)
		return 0;

	return des->ops->set_pipe_phy(des, pipe, &des->phys[channel->phy_id]);
}

static int max_des_pipe_update_remaps(struct max_des_priv *priv,
				      struct max_des_pipe *pipe)
{
	struct max_des *des = priv->des;
	struct max_des_link *link = &des->links[pipe->link_id];
	struct max_des_channel *channel;
	struct max_des_dt_vc_remap *remaps;
	unsigned int num_remaps = 0;
	unsigned int num_dt_remaps;
	unsigned int i, j;
	int ret;
	u8 dt;

	if (link->tunnel_mode)
		return max_des_pipe_update_phy_tunnel(priv, pipe);

	for_each_channel(priv, channel) {
		if (channel->pipe_id != pipe->index)
			continue;

		if (!channel->fmt)
			continue;

		num_dt_remaps = max_des_code_num_remaps(channel->fmt->code);

		num_remaps += num_dt_remaps;
	}

	if (num_remaps >= des->ops->num_remaps_per_pipe) {
		dev_err(priv->dev, "Too many remaps\n");
		return -EINVAL;
	}

	remaps = devm_kcalloc(priv->dev, num_remaps, sizeof(*remaps), GFP_KERNEL);
	if (!remaps)
		return -ENOMEM;

	i = 0;
	for_each_channel(priv, channel) {
		if (channel->pipe_id != pipe->index)
			continue;

		if (!channel->fmt)
			continue;

		num_dt_remaps = max_des_code_num_remaps(channel->fmt->code);

		for (j = 0; j < num_dt_remaps; j++) {
			struct max_des_dt_vc_remap *remap = &remaps[i + j];

			if (j == 0)
				dt = channel->fmt->dt;
			else if (j == 1)
				dt = MIPI_CSI2_DT_FS;
			else if (j == 2)
				dt = MIPI_CSI2_DT_FE;

			remap->from_dt = dt;
			remap->from_vc = channel->src_vc_id;
			remap->to_dt = dt;
			remap->to_vc = channel->dst_vc_id;
			remap->phy = channel->phy_id;
		}

		i += num_dt_remaps;
	}

	ret = max_des_pipe_set_remaps(priv, pipe, remaps, num_remaps);
	if (ret)
		devm_kfree(priv->dev, remaps);

	return ret;
}

static int max_des_init_link_ser_xlate(struct max_des_priv *priv,
				       struct max_des_link *link,
				       u8 power_up_addr, u8 new_addr)
{
	u8 addrs[] = { power_up_addr, new_addr };
	struct max_des *des = priv->des;
	struct i2c_client *client;
	struct regmap *regmap;
	int ret;

	client = i2c_new_dummy_device(priv->client->adapter, power_up_addr);
	if (IS_ERR(client)) {
		ret = PTR_ERR(client);
		dev_err(priv->dev,
			"Failed to create I2C client: %d\n", ret);
		return ret;
	}

	regmap = regmap_init_i2c(client, &max_ser_i2c_regmap);
	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		dev_err(priv->dev,
			"Failed to create I2C regmap: %d\n", ret);
		goto err_unregister_client;
	}

	ret = des->ops->select_links(des, BIT(link->index));
	if (ret)
		goto err_regmap_exit;

	ret = max_ser_wait_for_multiple(client, regmap, addrs, ARRAY_SIZE(addrs));
	if (ret) {
		dev_err(priv->dev,
			"Failed waiting for serializer with new or old address: %d\n", ret);
		goto err_regmap_exit;
	}

	ret = max_ser_reset(regmap);
	if (ret) {
		dev_err(priv->dev, "Failed to reset serializer: %d\n", ret);
		goto err_regmap_exit;
	}

	ret = max_ser_wait(client, regmap, power_up_addr);
	if (ret) {
		dev_err(priv->dev,
			"Failed waiting for serializer with new address: %d\n", ret);
		goto err_regmap_exit;
	}

	ret = max_ser_change_address(client, regmap, new_addr, des->ops->fix_tx_ids);
	if (ret) {
		dev_err(priv->dev, "Failed to change serializer address: %d\n", ret);
		goto err_regmap_exit;
	}

err_regmap_exit:
	regmap_exit(regmap);

err_unregister_client:
	i2c_unregister_device(client);

	return ret;
}

static int max_des_init(struct max_des_priv *priv)
{
	struct max_des *des = priv->des;
	unsigned int i;
	int ret;

	ret = des->ops->init(des);
	if (ret)
		return ret;

	ret = max_des_channel_update(priv);
	if (ret)
		return ret;

	for (i = 0; i < des->ops->num_phys; i++) {
		struct max_des_phy *phy = &des->phys[i];

		if (phy->enabled) {
			if (!phy->bus_config_parsed) {
				dev_err(priv->dev, "Cannot turn on unconfigured PHY\n");
				return -EINVAL;
			}

			ret = des->ops->init_phy(des, phy);
			if (ret)
				return ret;
		}

		ret = des->ops->set_phy_enable(des, phy, phy->enabled);
		if (ret)
			return ret;
	}

	for (i = 0; i < des->ops->num_pipes; i++) {
		struct max_des_pipe *pipe = &des->pipes[i];

		ret = des->ops->init_pipe(des, pipe);
		if (ret)
			return ret;

		ret = des->ops->set_pipe_enable(des, pipe, pipe->enabled);
		if (ret)
			return ret;

		if (!pipe->enabled)
			continue;

		ret = des->ops->set_pipe_stream_id(des, pipe, pipe->stream_id);
		if (ret)
			return ret;

		ret = max_des_pipe_update_remaps(priv, pipe);
		if (ret)
			return ret;
	}

	if (des->ops->init_link) {
		for (i = 0; i < des->ops->num_links; i++) {
			struct max_des_link *link = &des->links[i];

			if (!link->enabled)
				continue;

			ret = des->ops->init_link(des, link);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int max_des_post_init(struct max_des_priv *priv)
{
	struct max_des *des = priv->des;
	unsigned int i, mask = 0;
	int ret;

	for (i = 0; i < des->ops->num_links; i++) {
		struct max_des_link *link = &des->links[i];

		if (!link->enabled)
			continue;

		mask |= BIT(link->index);
	}

	ret = des->ops->select_links(des, mask);
	if (ret)
		return ret;

	if (des->ops->post_init) {
		ret = des->ops->post_init(des);
		if (ret)
			return ret;
	}

	return 0;
}

static int max_des_ser_atr_attach_client(struct i2c_atr *atr, u32 chan_id,
					 const struct i2c_client *client, u16 alias)
{
	struct max_des_priv *priv = i2c_atr_get_driver_data(atr);
	struct max_des *des = priv->des;
	struct max_des_link *link = &des->links[chan_id];

	if (link->ser_xlate_enabled) {
		dev_err(priv->dev, "Serializer for link %u already bound\n", link->index);
		return -EINVAL;
	}

	link->ser_xlate.src = alias;
	link->ser_xlate.dst = client->addr;
	link->ser_xlate_enabled = true;

	return max_des_init_link_ser_xlate(priv, link, client->addr, alias);
}

static void max_des_ser_atr_detach_client(struct i2c_atr *atr, u32 chan_id,
					  const struct i2c_client *client)
{
	/* Don't do anything. */
}

static const struct i2c_atr_ops max_des_i2c_atr_ops = {
	.attach_client = max_des_ser_atr_attach_client,
	.detach_client = max_des_ser_atr_detach_client,
};

static void max_des_i2c_atr_deinit(struct max_des_priv *priv)
{
	struct max_des *des = priv->des;
	unsigned int i;

	for (i = 0; i < des->ops->num_links; i++) {
		struct max_des_link *link = &des->links[i];

		/* Deleting adapters that haven't been added does no harm. */
		i2c_atr_del_adapter(priv->atr, link->index);
	}

	i2c_atr_delete(priv->atr);
}

static int max_des_i2c_atr_init(struct max_des_priv *priv)
{
	struct max_des *des = priv->des;
	unsigned int i;
	int ret;

	if (!i2c_check_functionality(priv->client->adapter,
				     I2C_FUNC_SMBUS_WRITE_BYTE_DATA))
		return -ENODEV;

	priv->atr = i2c_atr_new(priv->client->adapter, priv->dev,
				&max_des_i2c_atr_ops, des->ops->num_links);
	if (IS_ERR(priv->atr))
		return PTR_ERR(priv->atr);

	i2c_atr_set_driver_data(priv->atr, priv);

	for (i = 0; i < des->ops->num_links; i++) {
		struct max_des_link *link = &des->links[i];

		if (!link->enabled)
			continue;

		ret = i2c_atr_add_adapter(priv->atr, link->index, NULL, NULL);
		if (ret)
			goto err_add_adapters;
	}

	return 0;

err_add_adapters:
	max_des_i2c_atr_deinit(priv);

	return ret;
}

static int max_des_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct max_des_channel *channel = sd_to_max_des(sd);
	int ret;

	ret = v4l2_subdev_call(channel->source.sd, video, s_stream, enable);
	if (ret)
		return ret;

	return max_des_channel_enable(channel, enable);
}

static int max_des_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *format)
{
	struct max_des_channel *channel = v4l2_get_subdevdata(sd);

	if (format->pad == MAX_DES_SINK_PAD) {
		format->format.code = MEDIA_BUS_FMT_FIXED;
		return 0;
	}

	if (!channel->fmt)
		return -EINVAL;

	format->format = channel->framefmt;
	format->format.code = channel->fmt->code;

	return 0;
}

static u64 max_des_get_pixel_rate(struct max_des_channel *channel)
{
	struct max_des_priv *priv = channel->priv;
	struct max_des *des = priv->des;
	struct max_des_phy *phy = &des->phys[channel->phy_id];
	u8 bpp = 8;

	if (channel->fmt)
		bpp = channel->fmt->bpp;

	return phy->link_frequency * 2 * phy->mipi.num_data_lanes / bpp;
}

static int max_des_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *format)
{
	struct max_des_channel *channel = v4l2_get_subdevdata(sd);
	struct max_des_priv *priv = channel->priv;
	struct max_des *des = priv->des;
	struct max_des_pipe *pipe = &des->pipes[channel->pipe_id];
	const struct max_format *fmt;
	int ret;

	if (format->pad != MAX_DES_SOURCE_PAD)
		return -EINVAL;

	fmt = max_format_by_code(format->format.code);
	if (!fmt){
		v4l2_err(sd, "Wrong format requested: %d", format->format.code);
		return -EINVAL;
	}

	channel->fmt = fmt;
	channel->framefmt = format->format;

	v4l2_ctrl_s_ctrl_int64(channel->pixel_rate_ctrl,
			       max_des_get_pixel_rate(channel));

	mutex_lock(&priv->lock);

	ret = max_des_pipe_update_remaps(priv, pipe);

	mutex_unlock(&priv->lock);

	return ret;
}

static int max_des_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	const struct max_format *fmt;

	if (code->pad != MAX_DES_SOURCE_PAD)
		return -EINVAL;

	fmt = max_format_by_index(code->index);
	if (!fmt)
		return -EINVAL;

	code->code = fmt->code;

	return 0;
}

static int max_des_log_status(struct v4l2_subdev *sd)
{
	struct max_des_channel *channel = v4l2_get_subdevdata(sd);
	struct max_des_priv *priv = channel->priv;
	struct max_des *des = priv->des;
	unsigned int i, j;
	int ret;

	v4l2_info(sd, "active: %u\n", des->active);
	if (des->ops->log_status) {
		ret = des->ops->log_status(des, sd->name);
		if (ret)
			return ret;
	}
	v4l2_info(sd, "\n");

	for (i = 0; i < des->ops->num_links; i++) {
		struct max_des_link *link = &des->links[i];

		v4l2_info(sd, "link: %u\n", link->index);
		v4l2_info(sd, "\tenabled: %u\n", link->enabled);
		v4l2_info(sd, "\ttunnel_mode: %u\n", link->tunnel_mode);
		v4l2_info(sd, "\tser_xlate_enabled: %u\n", link->ser_xlate_enabled);
		v4l2_info(sd, "\tser_xlate: src: 0x%02x dst: 0x%02x\n",
			  link->ser_xlate.src, link->ser_xlate.dst);
		v4l2_info(sd, "\n");
	}

	for_each_channel(priv, channel) {
		v4l2_info(sd, "channel: %u\n", channel->index);
		v4l2_info(sd, "\tfwnode: %pfw\n", channel->fwnode);
		v4l2_info(sd, "\tactive: %u\n", channel->active);
		v4l2_info(sd, "\tfmt: %s\n", channel->fmt ? channel->fmt->name : NULL);
		v4l2_info(sd, "\tdt: 0x%02x\n", channel->fmt ? channel->fmt->dt : 0);
		v4l2_info(sd, "\tpipe_id: %u\n", channel->pipe_id);
		v4l2_info(sd, "\tphy_id: %u\n", channel->phy_id);
		v4l2_info(sd, "\tsrc_vc_id: %u\n", channel->src_vc_id);
		v4l2_info(sd, "\tdst_vc_id: %u\n", channel->dst_vc_id);
		v4l2_info(sd, "\n");
	}

	for (i = 0; i < des->ops->num_pipes; i++) {
		struct max_des_pipe *pipe = &des->pipes[i];

		v4l2_info(sd, "pipe: %u\n", pipe->index);
		v4l2_info(sd, "\tenabled: %u\n", pipe->enabled);
		v4l2_info(sd, "\tstream_id: %u\n", pipe->stream_id);
		v4l2_info(sd, "\tlink_id: %u\n", pipe->link_id);
		v4l2_info(sd, "\tdbl8: %u\n", pipe->dbl8);
		v4l2_info(sd, "\tdbl8mode: %u\n", pipe->dbl8mode);
		v4l2_info(sd, "\tdbl10: %u\n", pipe->dbl10);
		v4l2_info(sd, "\tdbl10mode: %u\n", pipe->dbl10mode);
		v4l2_info(sd, "\tdbl12: %u\n", pipe->dbl12);
		v4l2_info(sd, "\tremaps: %u\n", pipe->num_remaps);
		for (j = 0; j < pipe->num_remaps; j++) {
			struct max_des_dt_vc_remap *remap = &pipe->remaps[j];

			v4l2_info(sd, "\t\tremap: from: vc: %u, dt: 0x%02x\n",
				  remap->from_vc, remap->from_dt);
			v4l2_info(sd, "\t\t       to:   vc: %u, dt: 0x%02x, phy: %u\n",
				  remap->to_vc, remap->to_dt, remap->phy);
		}
		if (des->ops->log_pipe_status) {
			ret = des->ops->log_pipe_status(des, pipe, sd->name);
			if (ret)
				return ret;
		}
		v4l2_info(sd, "\n");
	}

	for (i = 0; i < des->ops->num_phys; i++) {
		struct max_des_phy *phy = &des->phys[i];

		v4l2_info(sd, "phy: %u\n", phy->index);
		v4l2_info(sd, "\tenabled: %u\n", phy->enabled);
		v4l2_info(sd, "\tlink_frequency: %llu\n", phy->link_frequency);
		v4l2_info(sd, "\tnum_data_lanes: %u\n", phy->mipi.num_data_lanes);
		v4l2_info(sd, "\tclock_lane: %u\n", phy->mipi.clock_lane);
		v4l2_info(sd, "\talt_mem_map8: %u\n", phy->alt_mem_map8);
		v4l2_info(sd, "\talt2_mem_map8: %u\n", phy->alt2_mem_map8);
		v4l2_info(sd, "\talt_mem_map10: %u\n", phy->alt_mem_map10);
		v4l2_info(sd, "\talt_mem_map12: %u\n", phy->alt_mem_map12);
		if (des->ops->log_phy_status) {
			ret = des->ops->log_phy_status(des, phy, sd->name);
			if (ret)
				return ret;
		}
		v4l2_info(sd, "\n");
	}

	return 0;
}

#ifdef CONFIG_VIDEO_ADV_DEBUG
static int max_des_g_register(struct v4l2_subdev *sd, struct v4l2_dbg_register *reg)
{
	struct max_des_channel *channel = v4l2_get_subdevdata(sd);
	struct max_des_priv *priv = channel->priv;
	struct max_des *des = priv->des;
	unsigned int val;
	int ret;

	ret = des->ops->reg_read(des, reg->reg, &val);
	if (ret)
		return ret;

	reg->val = val;
	reg->size = 1;

	return 0;
}

static int max_des_s_register(struct v4l2_subdev *sd, const struct v4l2_dbg_register *reg)
{
	struct max_des_channel *channel = v4l2_get_subdevdata(sd);
	struct max_des_priv *priv = channel->priv;
	struct max_des *des = priv->des;

	return des->ops->reg_write(des, reg->reg, reg->val);
}
#endif

static const struct v4l2_subdev_core_ops max_des_core_ops = {
	.log_status = max_des_log_status,
#ifdef CONFIG_VIDEO_ADV_DEBUG
	.g_register = max_des_g_register,
	.s_register = max_des_s_register,
#endif
};

static const struct v4l2_subdev_video_ops max_des_video_ops = {
	.s_stream = max_des_s_stream,
};

static const struct v4l2_subdev_pad_ops max_des_pad_ops = {
	.get_fmt = max_des_get_fmt,
	.set_fmt = max_des_set_fmt,
	.enum_mbus_code = max_des_enum_mbus_code,
};

static const struct v4l2_subdev_ops max_des_subdev_ops = {
	.core = &max_des_core_ops,
	.video = &max_des_video_ops,
	.pad = &max_des_pad_ops,
};

static const struct media_entity_operations max_des_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int max_des_notify_bound(struct v4l2_async_notifier *nf,
			        struct v4l2_subdev *subdev,
			        struct v4l2_async_connection *asd)
{
	struct max_des_channel *channel = nf_to_max_des(nf);
	struct max_des_priv *priv = channel->priv;
	int ret;

	ret = media_entity_get_fwnode_pad(&subdev->entity,
					  channel->source.ep_fwnode,
					  MEDIA_PAD_FL_SOURCE);
	if (ret < 0) {
		dev_err(priv->dev, "Failed to find pad for %s\n", subdev->name);
		return ret;
	}

	channel->source.sd = subdev;
	channel->source.pad = ret;

	ret = media_create_pad_link(&channel->source.sd->entity, channel->source.pad,
				    &channel->sd.entity, MAX_DES_SINK_PAD,
				    MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
	if (ret) {
		dev_err(priv->dev, "Unable to link %s:%u -> %s:%u\n",
			channel->source.sd->name, channel->source.pad,
			channel->sd.name, MAX_DES_SINK_PAD);
		return ret;
	}

	return 0;
}

static void max_des_notify_unbind(struct v4l2_async_notifier *nf,
				  struct v4l2_subdev *subdev,
				  struct v4l2_async_connection *asd)
{
	struct max_des_channel *channel = nf_to_max_des(nf);

	channel->source.sd = NULL;
}

static const struct v4l2_async_notifier_operations max_des_notify_ops = {
	.bound = max_des_notify_bound,
	.unbind = max_des_notify_unbind,
};

static int max_des_v4l2_notifier_register(struct max_des_channel *channel)
{
	struct max_des_priv *priv = channel->priv;
	struct v4l2_async_connection *asc;
	int ret;

	v4l2_async_subdev_nf_init(&channel->nf, &channel->sd);

	asc = v4l2_async_nf_add_fwnode(&channel->nf,
				       channel->source.ep_fwnode,
				       struct v4l2_async_connection);
	if (IS_ERR(asc)) {
		dev_err(priv->dev, "Failed to add subdev for source %u: %pe",
			channel->index, asc);
		v4l2_async_nf_cleanup(&channel->nf);
		return PTR_ERR(asc);
	}

	channel->nf.ops = &max_des_notify_ops;

	ret = v4l2_async_nf_register(&channel->nf);
	if (ret) {
		dev_err(priv->dev, "Failed to register subdev notifier");
		v4l2_async_nf_cleanup(&channel->nf);
		return ret;
	}

	return 0;
}

static void max_des_v4l2_notifier_unregister(struct max_des_channel *channel)
{
	v4l2_async_nf_unregister(&channel->nf);
	v4l2_async_nf_cleanup(&channel->nf);
}

static int max_des_v4l2_register_sd(struct max_des_channel *channel)
{
	struct v4l2_ctrl_handler *hdl = &channel->ctrl_handler;
	u64 max_pixel_rate = max_des_get_pixel_rate(channel);
	struct max_des_priv *priv = channel->priv;
	struct i2c_client *client = priv->client;
	struct max_des *des = priv->des;
	struct max_des_phy *phy = &des->phys[channel->phy_id];
	int ret;

	v4l2_subdev_init(&channel->sd, &max_des_subdev_ops);
	channel->sd.owner = priv->dev->driver->owner;
	channel->sd.dev = priv->dev;
	channel->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	channel->sd.entity.ops = &max_des_media_ops;
	channel->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	channel->sd.fwnode = channel->fwnode;
	fwnode_handle_get(channel->sd.fwnode);

	snprintf(channel->sd.name, sizeof(channel->sd.name), "%s %d-%04x:%u",
		 client->dev.driver->name, i2c_adapter_id(client->adapter),
		 client->addr, channel->index);

	channel->sd.ctrl_handler = hdl;
	ret = v4l2_ctrl_handler_init(hdl, 1);
	if (ret)
		goto error;

	v4l2_ctrl_new_int_menu(hdl, NULL, V4L2_CID_LINK_FREQ, 0, 0, &phy->link_frequency);
	channel->pixel_rate_ctrl = v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_PIXEL_RATE,
						     0, max_pixel_rate,
						     1, max_pixel_rate);

	channel->pads[MAX_DES_SOURCE_PAD].flags = MEDIA_PAD_FL_SOURCE;
	channel->pads[MAX_DES_SINK_PAD].flags = MEDIA_PAD_FL_SINK;

	v4l2_set_subdevdata(&channel->sd, channel);

	ret = media_entity_pads_init(&channel->sd.entity, MAX_DES_PAD_NUM, channel->pads);
	if (ret)
		goto error;

	ret = max_des_v4l2_notifier_register(channel);
	if (ret) {
		dev_err(priv->dev, "v4l2 subdev notifier register failed: %d\n", ret);
		goto error;
	}

	ret = v4l2_async_register_subdev(&channel->sd);
	if (ret)
		goto error;

	return 0;

error:
	media_entity_cleanup(&channel->sd.entity);
	v4l2_ctrl_handler_free(&channel->ctrl_handler);
	fwnode_handle_put(channel->sd.fwnode);

	return ret;
}

static void max_des_v4l2_unregister_sd(struct max_des_channel *channel)
{
	max_des_v4l2_notifier_unregister(channel);
	v4l2_async_unregister_subdev(&channel->sd);
	media_entity_cleanup(&channel->sd.entity);
	v4l2_ctrl_handler_free(&channel->ctrl_handler);
	fwnode_handle_put(channel->sd.fwnode);
}

static int max_des_v4l2_register(struct max_des_priv *priv)
{
	struct max_des_channel *channel;
	int ret;

	for_each_channel(priv, channel) {
		ret = max_des_v4l2_register_sd(channel);
		if (ret)
			return ret;
	}

	return 0;
}

static void max_des_v4l2_unregister(struct max_des_priv *priv)
{
	struct max_des_channel *channel;

	for_each_channel(priv, channel)
		max_des_v4l2_unregister_sd(channel);
}

static int max_des_parse_phy_dt(struct max_des_priv *priv,
				struct max_des_phy *phy,
				struct fwnode_handle *fwnode)
{
	phy->alt_mem_map8 = fwnode_property_read_bool(fwnode, "maxim,alt-mem-map8");
	phy->alt2_mem_map8 = fwnode_property_read_bool(fwnode, "maxim,alt2-mem-map8");
	phy->alt_mem_map10 = fwnode_property_read_bool(fwnode, "maxim,alt-mem-map10");
	phy->alt_mem_map12 = fwnode_property_read_bool(fwnode, "maxim,alt-mem-map12");

	return 0;
}

static int max_des_parse_pipe_dt(struct max_des_priv *priv,
				 struct max_des_pipe *pipe,
				 struct fwnode_handle *fwnode)
{
	pipe->dbl8 = fwnode_property_read_bool(fwnode, "maxim,dbl8");
	pipe->dbl10 = fwnode_property_read_bool(fwnode, "maxim,dbl10");
	pipe->dbl12 = fwnode_property_read_bool(fwnode, "maxim,dbl12");

	pipe->dbl8mode = fwnode_property_read_bool(fwnode, "maxim,dbl8-mode");
	pipe->dbl10mode = fwnode_property_read_bool(fwnode, "maxim,dbl10-mode");

	return 0;
}

static int max_des_parse_ch_dt(struct max_des_channel *channel,
			       struct fwnode_handle *fwnode)
{
	struct max_des_priv *priv = channel->priv;
	unsigned int index = channel->index;
	struct max_des *des = priv->des;
	struct max_des_pipe *pipe;
	struct max_des_link *link;
	struct max_des_phy *phy;
	u32 val;

	/* TODO: implement extended Virtual Channel. */
	val = 0;
	fwnode_property_read_u32(fwnode, "maxim,src-vc-id", &val);
	if (val >= MAX_SERDES_VC_ID_NUM) {
		dev_err(priv->dev, "Invalid source virtual channel %u\n", val);
		return -EINVAL;
	}
	channel->src_vc_id = val;

	/* TODO: implement extended Virtual Channel. */
	val = index % MAX_SERDES_VC_ID_NUM;
	fwnode_property_read_u32(fwnode, "maxim,dst-vc-id", &val);
	if (val >= MAX_SERDES_VC_ID_NUM) {
		dev_err(priv->dev, "Invalid destination virtual channel %u\n", val);
		return -EINVAL;
	}
	channel->dst_vc_id = val;

	val = index % des->ops->num_pipes;
	fwnode_property_read_u32(fwnode, "maxim,pipe-id", &val);
	if (val >= des->ops->num_pipes) {
		dev_err(priv->dev, "Invalid pipe %u\n", val);
		return -EINVAL;
	}
	channel->pipe_id = val;

	pipe = &des->pipes[val];
	pipe->enabled = true;

	val = channel->pipe_id % des->ops->num_phys;
	fwnode_property_read_u32(fwnode, "maxim,phy-id", &val);
	if (val >= des->ops->num_phys) {
		dev_err(priv->dev, "Invalid PHY %u\n", val);
		return -EINVAL;
	}
	channel->phy_id = val;

	if (fwnode_property_read_bool(fwnode, "maxim,embedded-data"))
		channel->fmt = max_format_by_dt(MIPI_CSI2_DT_EMBEDDED_8B);

	phy = &des->phys[val];
	phy->enabled = true;

	link = &des->links[pipe->link_id];
	link->enabled = true;

	return 0;
}

static int max_des_parse_sink_dt_endpoint(struct max_des_channel *channel,
					  struct fwnode_handle *fwnode)
{
	struct max_des_priv *priv = channel->priv;
	struct max_des *des = priv->des;
	struct max_des_pipe *pipe = &des->pipes[channel->pipe_id];
	struct max_des_link *link = &des->links[pipe->link_id];
	struct fwnode_handle *ep, *channel_fwnode, *device_fwnode;
	u32 val;

	ep = fwnode_graph_get_endpoint_by_id(fwnode, MAX_DES_SINK_PAD, 0, 0);
	if (!ep) {
		dev_err(priv->dev, "Not connected to subdevice\n");
		return 0;
	}

	channel->source.ep_fwnode = fwnode_graph_get_remote_endpoint(ep);
	if (!channel->source.ep_fwnode) {
		dev_err(priv->dev, "no remote endpoint\n");
		return -ENODEV;
	}

	channel_fwnode = fwnode_graph_get_remote_port_parent(ep);
	fwnode_handle_put(ep);
	if (!channel_fwnode) {
		dev_err(priv->dev, "Not connected to remote subdevice\n");
		return -EINVAL;
	}

	device_fwnode = fwnode_get_parent(channel_fwnode);
	fwnode_handle_put(channel_fwnode);
	if (!device_fwnode) {
		dev_err(priv->dev, "Not connected to remote subdevice\n");
		return -EINVAL;
	}

	val = fwnode_property_read_bool(device_fwnode, "maxim,tunnel-mode");
	fwnode_handle_put(device_fwnode);
	if (val && !des->ops->supports_tunnel_mode) {
		dev_err(priv->dev, "Tunnel mode is not supported\n");
		return -EINVAL;
	}
	link->tunnel_mode = val;

	return 0;
}

static int max_des_parse_src_dt_endpoint(struct max_des_channel *channel,
					 struct fwnode_handle *fwnode)
{
	struct max_des_priv *priv = channel->priv;
	struct max_des *des = priv->des;
	struct max_des_phy *phy = &des->phys[channel->phy_id];
	struct v4l2_fwnode_endpoint v4l2_ep = {
		.bus_type = V4L2_MBUS_UNKNOWN
	};
	struct v4l2_mbus_config_mipi_csi2 *mipi = &v4l2_ep.bus.mipi_csi2;
	enum v4l2_mbus_type bus_type;
	struct fwnode_handle *ep;
	u64 link_frequency;
	unsigned int i;
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(fwnode, MAX_DES_SOURCE_PAD, 0, 0);
	if (!ep) {
		return 0;
	}

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &v4l2_ep);
	fwnode_handle_put(ep);
	if (ret) {
		dev_err(priv->dev, "Could not parse v4l2 endpoint\n");
		return ret;
	}

	bus_type = v4l2_ep.bus_type;
	if (bus_type != V4L2_MBUS_CSI2_DPHY && bus_type != V4L2_MBUS_CSI2_CPHY) {
		v4l2_fwnode_endpoint_free(&v4l2_ep);
		dev_err(priv->dev, "Unsupported bus-type %u\n", bus_type);
		return -EINVAL;
	}

	ret = 0;
	if (v4l2_ep.nr_of_link_frequencies == 0)
		link_frequency = MAX_DES_LINK_FREQUENCY_DEFAULT;
	else if (v4l2_ep.nr_of_link_frequencies == 1)
		link_frequency = v4l2_ep.link_frequencies[0];
	else
		ret = -EINVAL;

	v4l2_fwnode_endpoint_free(&v4l2_ep);

	if (ret) {
		dev_err(priv->dev, "PHY configured with invalid number of link frequencies\n");
		return -EINVAL;
	}

	if (link_frequency < MAX_DES_LINK_FREQUENCY_MIN ||
	    link_frequency > MAX_DES_LINK_FREQUENCY_MAX) {
		dev_err(priv->dev, "PHY configured with out of range link frequency\n");
		return -EINVAL;
	}

	for (i = 0; i < mipi->num_data_lanes; i++) {
		if (mipi->data_lanes[i] > mipi->num_data_lanes) {
			dev_err(priv->dev, "PHY configured with data lanes out of range\n");
			return -EINVAL;
		}
	}

	if (!phy->bus_config_parsed) {
		phy->bus_type = bus_type;
		phy->mipi = *mipi;
		phy->link_frequency = link_frequency;
		phy->bus_config_parsed = true;

		return 0;
	}

	if (phy->bus_type != bus_type) {
		dev_err(priv->dev, "PHY configured with differing bus type\n");
		return -EINVAL;
	}

	if (phy->link_frequency != link_frequency) {
		dev_err(priv->dev, "PHY configured with differing link frequency\n");
		return -EINVAL;
	}

	if (phy->mipi.num_data_lanes != mipi->num_data_lanes) {
		dev_err(priv->dev, "PHY configured with differing number of data lanes\n");
		return -EINVAL;
	}

	for (i = 0; i < mipi->num_data_lanes; i++) {
		if (phy->mipi.data_lanes[i] != mipi->data_lanes[i]) {
			dev_err(priv->dev, "PHY configured with differing data lanes\n");
			return -EINVAL;
		}
	}

	if (phy->mipi.clock_lane != mipi->clock_lane) {
		dev_err(priv->dev, "PHY configured with differing clock lane\n");
		return -EINVAL;
	}

	return 0;
}

static int max_des_find_phys_config(struct max_des_priv *priv)
{
	struct max_des *des = priv->des;
	const struct max_phy_configs *configs = des->ops->phys_configs.configs;
	unsigned int num_phys_configs = des->ops->phys_configs.num_configs;
	struct max_des_phy *phy;
	unsigned int i, j;

	if (!num_phys_configs)
		return 0;

	for (i = 0; i < num_phys_configs; i++) {
		bool matching = true;

		for (j = 0; j < des->ops->num_phys; j++) {
			phy = &des->phys[j];

			if (!phy->enabled)
				continue;

			if (phy->mipi.num_data_lanes == configs[i].lanes[j] &&
			    phy->mipi.clock_lane == configs[i].clock_lane[j])
				continue;

			matching = false;

			break;
		}

		if (matching)
			break;
	}

	if (i == num_phys_configs) {
		dev_err(priv->dev, "Invalid lane configuration\n");
		return -EINVAL;
	}

	des->phys_config = i;

	return 0;
}

static int max_des_parse_dt(struct max_des_priv *priv)
{
	struct max_des *des = priv->des;
	const char *channel_node_name = "channel";
	const char *pipe_node_name = "pipe";
	const char *phy_node_name = "phy";
	struct max_des_channel *channel;
	struct fwnode_handle *fwnode;
	struct max_des_link *link;
	struct max_des_pipe *pipe;
	struct max_des_phy *phy;
	unsigned int i;
	u32 index;
	int ret;

	for (i = 0; i < des->ops->num_phys; i++) {
		phy = &des->phys[i];
		phy->index = i;
	}

	for (i = 0; i < des->ops->num_pipes; i++) {
		pipe = &des->pipes[i];
		pipe->index = i;
		pipe->stream_id = 0;
		pipe->link_id = i % des->ops->num_links;
	}

	for (i = 0; i < des->ops->num_links; i++) {
		link = &des->links[i];
		link->index = i;
	}

	device_for_each_child_node(priv->dev, fwnode) {
		struct device_node *of_node = to_of_node(fwnode);

		if (!of_node_name_eq(of_node, phy_node_name))
			continue;

		ret = fwnode_property_read_u32(fwnode, "reg", &index);
		if (ret) {
			dev_err(priv->dev, "Failed to read reg: %d\n", ret);
			continue;
		}

		if (index >= des->ops->num_phys) {
			dev_err(priv->dev, "Invalid PHY %u\n", index);
			fwnode_handle_put(fwnode);
			return -EINVAL;
		}

		phy = &des->phys[index];

		ret = max_des_parse_phy_dt(priv, phy, fwnode);
		if (ret) {
			fwnode_handle_put(fwnode);
			return ret;
		}
	}

	device_for_each_child_node(priv->dev, fwnode) {
		struct device_node *of_node = to_of_node(fwnode);

		if (!of_node_name_eq(of_node, pipe_node_name))
			continue;

		ret = fwnode_property_read_u32(fwnode, "reg", &index);
		if (ret) {
			dev_err(priv->dev, "Failed to read reg: %d\n", ret);
			continue;
		}

		if (index >= des->ops->num_pipes) {
			dev_err(priv->dev, "Invalid pipe %u\n", index);
			fwnode_handle_put(fwnode);
			return -EINVAL;
		}

		pipe = &des->pipes[index];

		ret = max_des_parse_pipe_dt(priv, pipe, fwnode);
		if (ret) {
			fwnode_handle_put(fwnode);
			return ret;
		}
	}

	device_for_each_child_node(priv->dev, fwnode) {
		struct device_node *of_node = to_of_node(fwnode);

		if (!of_node_name_eq(of_node, channel_node_name))
			continue;

		ret = fwnode_property_read_u32(fwnode, "reg", &index);
		if (ret) {
			dev_err(priv->dev, "Failed to read reg: %d\n", ret);
			continue;
		}

		priv->num_channels++;
	}

	priv->channels = devm_kcalloc(priv->dev, priv->num_channels,
				      sizeof(*priv->channels), GFP_KERNEL);
	if (!priv->channels)
		return -ENOMEM;

	channel = priv->channels;
	device_for_each_child_node(priv->dev, fwnode) {
		struct device_node *of_node = to_of_node(fwnode);

		if (!of_node_name_eq(of_node, channel_node_name))
			continue;

		ret = fwnode_property_read_u32(fwnode, "reg", &index);
		if (ret) {
			dev_err(priv->dev, "Failed to read reg: %d\n", ret);
			continue;
		}

		channel->fwnode = fwnode;
		channel->priv = priv;
		channel->index = index;

		ret = max_des_parse_ch_dt(channel, fwnode);
		if (ret) {
			fwnode_handle_put(fwnode);
			return ret;
		}

		ret = max_des_parse_sink_dt_endpoint(channel, fwnode);
		if (ret) {
			fwnode_handle_put(fwnode);
			return ret;
		}

		ret = max_des_parse_src_dt_endpoint(channel, fwnode);
		if (ret) {
			fwnode_handle_put(fwnode);
			return ret;
		}

		channel++;
	}

	return max_des_find_phys_config(priv);
}

static int max_des_allocate(struct max_des_priv *priv)
{
	struct max_des *des = priv->des;

	des->phys = devm_kcalloc(priv->dev, des->ops->num_phys,
				 sizeof(*des->phys), GFP_KERNEL);
	if (!des->phys)
		return -ENOMEM;

	des->pipes = devm_kcalloc(priv->dev, des->ops->num_pipes,
				  sizeof(*des->pipes), GFP_KERNEL);
	if (!des->pipes)
		return -ENOMEM;

	des->links = devm_kcalloc(priv->dev, des->ops->num_links,
				  sizeof(*des->links), GFP_KERNEL);
	if (!des->links)
		return -ENOMEM;

	return 0;
}

int max_des_probe(struct i2c_client *client, struct max_des *des)
{
	struct device *dev = &client->dev;
	struct max_des_priv *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;
	priv->dev = dev;
	priv->des = des;
	des->priv = priv;

	mutex_init(&priv->lock);

	ret = max_des_allocate(priv);
	if (ret)
		return ret;

	ret = max_des_parse_dt(priv);
	if (ret)
		return ret;

	ret = max_des_init(priv);
	if (ret)
		return ret;

	ret = max_des_i2c_atr_init(priv);
	if (ret)
		return ret;

	ret = max_des_post_init(priv);
	if (ret)
		return ret;

	return max_des_v4l2_register(priv);
}
EXPORT_SYMBOL_GPL(max_des_probe);

int max_des_remove(struct max_des *des)
{
	struct max_des_priv *priv = des->priv;

	max_des_v4l2_unregister(priv);

	max_des_i2c_atr_deinit(priv);

	return 0;
}
EXPORT_SYMBOL_GPL(max_des_remove);

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(I2C_ATR);
