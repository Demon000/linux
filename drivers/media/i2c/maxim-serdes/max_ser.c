// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim GMSL2 Serializer Driver
 *
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include "max_ser.h"

#include <linux/delay.h>
#include <linux/module.h>

#include <media/mipi-csi2.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include "max_ser.h"
#include "max_serdes.h"

const struct regmap_config max_ser_i2c_regmap = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0x1f00,
};
EXPORT_SYMBOL_GPL(max_ser_i2c_regmap);

struct max_ser_channel {
	struct v4l2_subdev sd;
	unsigned int index;
	struct fwnode_handle *fwnode;

	struct max_ser_priv *priv;
	const struct max_format *fmt;
	struct v4l2_mbus_framefmt framefmt;

	struct media_pad pads[MAX_SER_PAD_NUM];

	bool active;
	unsigned int pipe_id;

	struct v4l2_async_notifier nf;
	struct {
		struct v4l2_subdev *sd;
		unsigned int pad;
		struct fwnode_handle *ep_fwnode;
	} source;
};

struct max_ser_priv {
	struct max_ser *ser;
	struct device *dev;
	struct i2c_client *client;
	struct regmap *regmap;

	struct i2c_atr *atr;

	unsigned int num_channels;
	struct mutex lock;

	struct max_ser_channel *channels;
};

static struct max_ser_channel *next_channel(struct max_ser_priv *priv,
					    struct max_ser_channel *channel)
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

static inline struct max_ser_channel *sd_to_max_ser(struct v4l2_subdev *sd)
{
	return container_of(sd, struct max_ser_channel, sd);
}

static inline struct max_ser_channel *nf_to_max_ser(struct v4l2_async_notifier *nf)
{
	return container_of(nf, struct max_ser_channel, nf);
}

static int max_ser_i2c_atr_attach_client(struct i2c_atr *atr, u32 chan_id,
					 const struct i2c_client *client, u16 alias)
{
	struct max_ser_priv *priv = i2c_atr_get_driver_data(atr);
	struct max_ser *ser = priv->ser;
	struct max_i2c_xlate *xlate;

	if (ser->num_i2c_xlates == ser->ops->num_i2c_xlates) {
		dev_err(priv->dev,
			"Reached maximum number of I2C translations\n");
		return -EINVAL;
	}

	xlate = &ser->i2c_xlates[ser->num_i2c_xlates++];
	xlate->src = alias;
	xlate->dst = client->addr;

	return ser->ops->init_i2c_xlate(ser);
}

static void max_ser_i2c_atr_detach_client(struct i2c_atr *atr, u32 chan_id,
					  const struct i2c_client *client)
{
	struct max_ser_priv *priv = i2c_atr_get_driver_data(atr);
	struct max_ser *ser = priv->ser;
	struct max_i2c_xlate *xlate;
	unsigned int i;

	/* Find index of matching I2C translation. */
	for (i = 0; i < ser->num_i2c_xlates; i++) {
		xlate = &ser->i2c_xlates[i];

		if (xlate->dst == client->addr)
			break;
	}

	WARN_ON(i == ser->num_i2c_xlates);

	/* Starting from index + 1, copy index translation into index - 1. */
	for (i++; i < ser->num_i2c_xlates; i++) {
		ser->i2c_xlates[i - 1].src = ser->i2c_xlates[i].src;
		ser->i2c_xlates[i - 1].dst = ser->i2c_xlates[i].dst;
	}

	/* Zero out last index translation. */
	ser->i2c_xlates[ser->num_i2c_xlates].src = 0;
	ser->i2c_xlates[ser->num_i2c_xlates].dst = 0;

	/* Decrease number of translations. */
	ser->num_i2c_xlates--;

	ser->ops->init_i2c_xlate(ser);
}

static const struct i2c_atr_ops max_ser_i2c_atr_ops = {
	.attach_client = max_ser_i2c_atr_attach_client,
	.detach_client = max_ser_i2c_atr_detach_client,
};

static void max_ser_i2c_atr_deinit(struct max_ser_priv *priv)
{
	/* Deleting adapters that haven't been added does no harm. */
	i2c_atr_del_adapter(priv->atr, 0);

	i2c_atr_delete(priv->atr);
}

static int max_ser_i2c_atr_init(struct max_ser_priv *priv)
{
	if (!i2c_check_functionality(priv->client->adapter,
				     I2C_FUNC_SMBUS_WRITE_BYTE_DATA))
		return -ENODEV;

	priv->atr = i2c_atr_new(priv->client->adapter, priv->dev,
				&max_ser_i2c_atr_ops, 1);
	if (IS_ERR(priv->atr))
		return PTR_ERR(priv->atr);

	i2c_atr_set_driver_data(priv->atr, priv);

	return i2c_atr_add_adapter(priv->atr, 0, NULL, NULL);
}

static int max_ser_update_pipe_active(struct max_ser_priv *priv,
				      struct max_ser_pipe *pipe)
{
	struct max_ser_channel *channel;
	struct max_ser *ser = priv->ser;
	bool enable = 0;

	for_each_channel(priv, channel) {
		if (channel->pipe_id == pipe->index && channel->active) {
			enable = 1;
			break;
		}
	}

	if (enable == pipe->active)
		return 0;

	pipe->active = enable;

	return ser->ops->set_pipe_enable(ser, pipe, enable);
}

static int max_ser_ch_enable(struct max_ser_channel *channel, bool enable)
{
	struct max_ser_priv *priv = channel->priv;
	struct max_ser *ser = priv->ser;
	struct max_ser_pipe *pipe = &ser->pipes[channel->pipe_id];
	int ret = 0;

	mutex_lock(&priv->lock);

	if (channel->active == enable)
		goto exit;

	channel->active = enable;

	ret = max_ser_update_pipe_active(priv, pipe);

exit:
	mutex_unlock(&priv->lock);

	return ret;
}

static int max_ser_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct max_ser_channel *channel = sd_to_max_ser(sd);
	int ret;

	ret = v4l2_subdev_call(channel->source.sd, video, s_stream, enable);
	if (ret)
		return ret;

	return max_ser_ch_enable(channel, enable);
}

static int max_ser_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *format)
{
	struct max_ser_channel *channel = v4l2_get_subdevdata(sd);

	if (format->pad == MAX_SER_SOURCE_PAD) {
		format->format.code = MEDIA_BUS_FMT_FIXED;
		return 0;
	}

	if (!channel->fmt)
		return -EINVAL;

	format->format = channel->framefmt;
	format->format.code = channel->fmt->code;

	return 0;
}

static int max_ser_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *format)
{
	struct max_ser_channel *channel = v4l2_get_subdevdata(sd);
	const struct max_format *fmt;

	if (format->pad != MAX_SER_SINK_PAD)
		return -EINVAL;

	fmt = max_format_by_code(format->format.code);
	if (!fmt){
		v4l2_err(sd, "Wrong format requested: %d", format->format.code);
		return -EINVAL;
	}

	channel->fmt = fmt;
	channel->framefmt = format->format;

	return 0;
}

static int max_ser_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	const struct max_format *fmt;

	if (code->pad != MAX_SER_SINK_PAD)
		return -EINVAL;

	fmt = max_format_by_index(code->index);
	if (!fmt)
		return -EINVAL;

	code->code = fmt->code;

	return 0;
}

static int max_ser_log_status(struct v4l2_subdev *sd)
{
	struct max_ser_channel *channel = v4l2_get_subdevdata(sd);
	struct max_ser_priv *priv = channel->priv;
	struct max_ser *ser = priv->ser;
	unsigned int i;
	int ret;

	v4l2_info(sd, "tunnel_mode: %u\n", ser->tunnel_mode);
	v4l2_info(sd, "i2c_xlates: %u\n", ser->num_i2c_xlates);
	for (i = 0; i < ser->num_i2c_xlates; i++)
		v4l2_info(sd, "\tsrc: 0x%02x dst: 0x%02x\n",
			  ser->i2c_xlates[i].src, ser->i2c_xlates[i].dst);
	if (ser->ops->log_status) {
		ret = ser->ops->log_status(ser, sd->name);
		if (ret)
			return ret;
	}
	v4l2_info(sd, "\n");

	for_each_channel(priv, channel) {
		v4l2_info(sd, "channel: %u\n", channel->index);
		v4l2_info(sd, "\tfwnode: %pfw\n", channel->fwnode);
		v4l2_info(sd, "\tactive: %u\n", channel->active);
		v4l2_info(sd, "\tfmt: %s\n", channel->fmt ? channel->fmt->name : NULL);
		v4l2_info(sd, "\tdt: 0x%02x\n", channel->fmt ? channel->fmt->dt : 0);
		v4l2_info(sd, "\tpipe_id: %u\n", channel->pipe_id);
		v4l2_info(sd, "\n");
	}

	for (i = 0; i < ser->ops->num_pipes; i++) {
		struct max_ser_pipe *pipe = &ser->pipes[i];

		v4l2_info(sd, "pipe: %u\n", pipe->index);
		v4l2_info(sd, "\tenabled: %u\n", pipe->enabled);
		v4l2_info(sd, "\tactive: %u\n", pipe->enabled);
		v4l2_info(sd, "\tphy_id: %u\n", pipe->phy_id);
		v4l2_info(sd, "\tstream_id: %u\n", pipe->stream_id);
		v4l2_info(sd, "\tdbl8: %u\n", pipe->dbl8);
		v4l2_info(sd, "\tdbl10: %u\n", pipe->dbl10);
		v4l2_info(sd, "\tdbl12: %u\n", pipe->dbl12);
		v4l2_info(sd, "\tsoft_bpp: %u\n", pipe->soft_bpp);
		v4l2_info(sd, "\tbpp: %u\n", pipe->bpp);
		if (ser->ops->log_pipe_status) {
			ret = ser->ops->log_pipe_status(ser, pipe, sd->name);
			if (ret)
				return ret;
		}
		v4l2_info(sd, "\n");
	}

	for (i = 0; i < ser->ops->num_phys; i++) {
		struct max_ser_phy *phy = &ser->phys[i];

		v4l2_info(sd, "phy: %u\n", phy->index);
		v4l2_info(sd, "\tenabled: %u\n", phy->enabled);
		v4l2_info(sd, "\tnum_data_lanes: %u\n", phy->mipi.num_data_lanes);
		v4l2_info(sd, "\tclock_lane: %u\n", phy->mipi.clock_lane);
		v4l2_info(sd, "\tnoncontinuous_clock: %u\n",
			  !!(phy->mipi.flags & V4L2_MBUS_CSI2_NONCONTINUOUS_CLOCK));
		if (ser->ops->log_phy_status) {
			ret = ser->ops->log_phy_status(ser, phy, sd->name);
			if (ret)
				return ret;
		}
		v4l2_info(sd, "\n");
	}

	return 0;
}

#ifdef CONFIG_VIDEO_ADV_DEBUG
static int max_ser_g_register(struct v4l2_subdev *sd, struct v4l2_dbg_register *reg)
{
	struct max_ser_channel *channel = v4l2_get_subdevdata(sd);
	struct max_ser_priv *priv = channel->priv;
	struct max_ser *ser = priv->ser;
	unsigned int val;
	int ret;

	ret = ser->ops->reg_read(ser, reg->reg, &val);
	if (ret)
		return ret;

	reg->val = val;
	reg->size = 1;

	return 0;
}

static int max_ser_s_register(struct v4l2_subdev *sd, const struct v4l2_dbg_register *reg)
{
	struct max_ser_channel *channel = v4l2_get_subdevdata(sd);
	struct max_ser_priv *priv = channel->priv;
	struct max_ser *ser = priv->ser;

	return ser->ops->reg_write(ser, reg->reg, reg->val);
}
#endif

static const struct v4l2_subdev_core_ops max_ser_core_ops = {
	.log_status = max_ser_log_status,
#ifdef CONFIG_VIDEO_ADV_DEBUG
	.g_register = max_ser_g_register,
	.s_register = max_ser_s_register,
#endif
};

static const struct v4l2_subdev_video_ops max_ser_video_ops = {
	.s_stream	= max_ser_s_stream,
};

static const struct v4l2_subdev_pad_ops max_ser_pad_ops = {
	.get_fmt = max_ser_get_fmt,
	.set_fmt = max_ser_set_fmt,
	.enum_mbus_code = max_ser_enum_mbus_code,
};

static const struct v4l2_subdev_ops max_ser_subdev_ops = {
	.core = &max_ser_core_ops,
	.video = &max_ser_video_ops,
	.pad = &max_ser_pad_ops,
};

static const struct media_entity_operations max_ser_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int max_ser_init(struct max_ser_priv *priv)
{
	struct max_ser *ser = priv->ser;
	unsigned int i;
	int ret;

	ret = ser->ops->init(ser);
	if (ret)
		return ret;

	for (i = 0; i < ser->ops->num_phys; i++) {
		struct max_ser_phy *phy = &ser->phys[i];

		if (phy->enabled) {
			if (!phy->bus_config_parsed) {
				dev_err(priv->dev, "Cannot turn on unconfigured PHY\n");
				return -EINVAL;
			}

			ret = ser->ops->init_phy(ser, phy);
			if (ret)
				return ret;
		}

		ret = ser->ops->set_phy_enable(ser, phy, phy->enabled);
		if (ret)
			return ret;
	}

	for (i = 0; i < ser->ops->num_pipes; i++) {
		struct max_ser_pipe *pipe = &ser->pipes[i];
		struct max_ser_phy *phy = &ser->phys[pipe->phy_id];

		ret = ser->ops->set_pipe_enable(ser, pipe, false);
		if (ret)
			return ret;

		ret = ser->ops->set_pipe_stream_id(ser, pipe, pipe->stream_id);
		if (ret)
			return ret;

		ret = ser->ops->set_pipe_phy(ser, pipe, phy);
		if (ret)
			return ret;

		if (!pipe->enabled)
			continue;

		ret = ser->ops->init_pipe(ser, pipe);
		if (ret)
			return ret;
	}

	return 0;
}

static int max_ser_notify_bound(struct v4l2_async_notifier *nf,
			        struct v4l2_subdev *subdev,
			        struct v4l2_async_connection *asd)
{
	struct max_ser_channel *channel = nf_to_max_ser(nf);
	struct max_ser_priv *priv = channel->priv;
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
				    &channel->sd.entity, MAX_SER_SINK_PAD,
				    MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
	if (ret) {
		dev_err(priv->dev, "Unable to link %s:%u -> %s:%u\n",
			channel->source.sd->name, channel->source.pad,
			channel->sd.name, MAX_SER_SINK_PAD);
		return ret;
	}

	return 0;
}

static void max_ser_notify_unbind(struct v4l2_async_notifier *nf,
				  struct v4l2_subdev *subdev,
				  struct v4l2_async_connection *asd)
{
	struct max_ser_channel *channel = nf_to_max_ser(nf);

	channel->source.sd = NULL;
}

static const struct v4l2_async_notifier_operations max_ser_notify_ops = {
	.bound = max_ser_notify_bound,
	.unbind = max_ser_notify_unbind,
};

static int max_ser_v4l2_notifier_register(struct max_ser_channel *channel)
{
	struct max_ser_priv *priv = channel->priv;
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

	channel->nf.ops = &max_ser_notify_ops;

	ret = v4l2_async_nf_register(&channel->nf);
	if (ret) {
		dev_err(priv->dev, "Failed to register subdev notifier");
		v4l2_async_nf_cleanup(&channel->nf);
		return ret;
	}

	return 0;
}

static void max_ser_v4l2_notifier_unregister(struct max_ser_channel *channel)
{
	v4l2_async_nf_unregister(&channel->nf);
	v4l2_async_nf_cleanup(&channel->nf);
}

static int max_ser_v4l2_register_sd(struct max_ser_channel *channel)
{
	struct max_ser_priv *priv = channel->priv;
	struct i2c_client *client = priv->client;
	int ret;

	v4l2_subdev_init(&channel->sd, &max_ser_subdev_ops);
	channel->sd.owner = priv->dev->driver->owner;
	channel->sd.dev = priv->dev;
	channel->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	channel->sd.entity.ops = &max_ser_media_ops;
	channel->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	channel->sd.fwnode = channel->fwnode;
	fwnode_handle_get(channel->sd.fwnode);

	snprintf(channel->sd.name, sizeof(channel->sd.name), "%s %d-%04x:%u",
		 client->dev.driver->name, i2c_adapter_id(client->adapter),
		 client->addr, channel->index);

	channel->pads[MAX_SER_SOURCE_PAD].flags = MEDIA_PAD_FL_SOURCE;
	channel->pads[MAX_SER_SINK_PAD].flags = MEDIA_PAD_FL_SINK;

	v4l2_set_subdevdata(&channel->sd, channel);

	ret = media_entity_pads_init(&channel->sd.entity, MAX_SER_PAD_NUM, channel->pads);
	if (ret)
		goto error;

	ret = max_ser_v4l2_notifier_register(channel);
	if (ret) {
		dev_err_probe(priv->dev, ret,
			      "v4l2 subdev notifier register failed\n");
		goto error;
	}

	ret = v4l2_async_register_subdev(&channel->sd);
	if (ret)
		goto error;

	return 0;

error:
	media_entity_cleanup(&channel->sd.entity);
	fwnode_handle_put(channel->sd.fwnode);

	return ret;
}

static void max_ser_v4l2_unregister_sd(struct max_ser_channel *channel)
{
	max_ser_v4l2_notifier_unregister(channel);
	v4l2_async_unregister_subdev(&channel->sd);
	media_entity_cleanup(&channel->sd.entity);
	fwnode_handle_put(channel->sd.fwnode);
}

static int max_ser_v4l2_register(struct max_ser_priv *priv)
{
	struct max_ser_channel *channel;
	int ret;

	for_each_channel(priv, channel) {
		ret = max_ser_v4l2_register_sd(channel);
		if (ret)
			return ret;
	}

	return 0;
}

static void max_ser_v4l2_unregister(struct max_ser_priv *priv)
{
	struct max_ser_channel *channel;

	for_each_channel(priv, channel)
		max_ser_v4l2_unregister_sd(channel);
}

static int max_ser_parse_pipe_dt(struct max_ser_priv *priv,
				 struct max_ser_pipe *pipe,
				 struct fwnode_handle *fwnode)
{
	unsigned int val;

	val = 0;
	fwnode_property_read_u32(fwnode, "maxim,soft-bpp", &val);
	if (val > 24) {
		dev_err(priv->dev, "Invalid soft bpp %u\n", val);
		return -EINVAL;
	}
	pipe->soft_bpp = val;

	val = 0;
	fwnode_property_read_u32(fwnode, "maxim,bpp", &val);
	if (val > 24) {
		dev_err(priv->dev, "Invalid bpp %u\n", val);
		return -EINVAL;
	}
	pipe->bpp = val;

	pipe->dbl8 = fwnode_property_read_bool(fwnode, "maxim,dbl8");
	pipe->dbl10 = fwnode_property_read_bool(fwnode, "maxim,dbl10");
	pipe->dbl12 = fwnode_property_read_bool(fwnode, "maxim,dbl12");

	return 0;
}

static int max_ser_parse_ch_dt(struct max_ser_channel *channel,
			       struct fwnode_handle *fwnode)
{
	struct max_ser_priv *priv = channel->priv;
	unsigned int index = channel->index;
	struct max_ser *ser = priv->ser;
	struct max_ser_pipe *pipe;
	struct max_ser_phy *phy;
	u32 val;

	val = index % ser->ops->num_pipes;
	fwnode_property_read_u32(fwnode, "maxim,pipe-id", &val);
	if (val >= ser->ops->num_pipes) {
		dev_err(priv->dev, "Invalid pipe %u\n", val);
		return -EINVAL;
	}
	channel->pipe_id = val;

	if (fwnode_property_read_bool(fwnode, "maxim,embedded-data"))
		channel->fmt = max_format_by_dt(MIPI_CSI2_DT_EMBEDDED_8B);

	pipe = &ser->pipes[val];
	pipe->enabled = true;

	phy = &ser->phys[pipe->phy_id];
	phy->enabled = true;

	return 0;
}

static int max_ser_parse_sink_dt_endpoint(struct max_ser_channel *channel,
					  struct fwnode_handle *fwnode)
{
	struct max_ser_priv *priv = channel->priv;
	struct max_ser *ser = priv->ser;
	struct max_ser_pipe *pipe = &ser->pipes[channel->pipe_id];
	struct max_ser_phy *phy = &ser->phys[pipe->phy_id];
	struct v4l2_fwnode_endpoint v4l2_ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	struct v4l2_mbus_config_mipi_csi2 *mipi = &v4l2_ep.bus.mipi_csi2;
	struct fwnode_handle *ep, *remote_ep;
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(fwnode, MAX_SER_SINK_PAD, 0, 0);
	if (!ep) {
		dev_err(priv->dev, "Failed to get local endpoint\n");
		return 0;
	}

	remote_ep = fwnode_graph_get_remote_endpoint(ep);
	fwnode_handle_put(ep);
	if (!remote_ep) {
		dev_err(priv->dev, "Failed to get remote endpoint\n");
		return -EINVAL;
	}
	channel->source.ep_fwnode = remote_ep;

	ret = v4l2_fwnode_endpoint_parse(remote_ep, &v4l2_ep);
	fwnode_handle_put(remote_ep);
	if (ret) {
		dev_err(priv->dev, "Could not parse v4l2 endpoint\n");
		return ret;
	}

	if (mipi->flags & V4L2_MBUS_CSI2_NONCONTINUOUS_CLOCK &&
	    !ser->ops->supports_noncontinuous_clock) {
		dev_err(priv->dev, "Clock non-continuous mode is not supported\n");
		return -EINVAL;
	}

	if (!phy->bus_config_parsed) {
		phy->mipi = v4l2_ep.bus.mipi_csi2;
		phy->bus_config_parsed = true;

		return 0;
	}

	if (phy->mipi.num_data_lanes != mipi->num_data_lanes) {
		dev_err(priv->dev, "PHY configured with differing number of data lanes\n");
		return -EINVAL;
	}

	if ((phy->mipi.flags & V4L2_MBUS_CSI2_NONCONTINUOUS_CLOCK) !=
	    (mipi->flags & V4L2_MBUS_CSI2_NONCONTINUOUS_CLOCK)) {
		dev_err(priv->dev, "PHY configured with differing clock continuity\n");
		return -EINVAL;
	}

	return 0;
}

static int max_ser_find_phys_config(struct max_ser_priv *priv)
{
	struct max_ser *ser = priv->ser;
	const struct max_phy_configs *configs = ser->ops->phys_configs.configs;
	unsigned int num_phys_configs = ser->ops->phys_configs.num_configs;
	struct max_ser_phy *phy;
	unsigned int i, j;

	if (!num_phys_configs)
		return 0;

	for (i = 0; i < num_phys_configs; i++) {
		bool matching = true;

		for (j = 0; j < ser->ops->num_phys; j++) {
			phy = &ser->phys[j];

			if (!phy->enabled)
				continue;

			if (phy->mipi.num_data_lanes == configs[i].lanes[j])
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

	ser->phys_config = i;

	return 0;
}

static int max_ser_parse_dt(struct max_ser_priv *priv)
{
	struct max_ser *ser = priv->ser;
	const char *channel_node_name = "channel";
	const char *pipe_node_name = "pipe";
	struct max_ser_channel *channel;
	struct fwnode_handle *fwnode;
	struct max_ser_pipe *pipe;
	struct max_ser_phy *phy;
	unsigned int i;
	u32 index;
	u32 val;
	int ret;

	val = device_property_read_bool(priv->dev, "maxim,tunnel-mode");
	if (val && !ser->ops->supports_tunnel_mode) {
		dev_err(priv->dev, "Tunnel mode is not supported\n");
		return -EINVAL;
	}
	ser->tunnel_mode = val;

	for (i = 0; i < ser->ops->num_phys; i++) {
		phy = &ser->phys[i];
		phy->index = i;
	}

	for (i = 0; i < ser->ops->num_pipes; i++) {
		pipe = &ser->pipes[i];
		pipe->index = i;
		pipe->phy_id = i % ser->ops->num_phys;
		pipe->stream_id = i % MAX_SERDES_STREAMS_NUM;
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

		if (index >= ser->ops->num_pipes) {
			dev_err(priv->dev, "Invalid pipe number %u\n", index);
			fwnode_handle_put(fwnode);
			return -EINVAL;
		}

		pipe = &ser->pipes[index];

		ret = max_ser_parse_pipe_dt(priv, pipe, fwnode);
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

	i = 0;
	device_for_each_child_node(priv->dev, fwnode) {
		struct device_node *of_node = to_of_node(fwnode);

		if (!of_node_name_eq(of_node, channel_node_name))
			continue;

		ret = fwnode_property_read_u32(fwnode, "reg", &index);
		if (ret) {
			dev_err(priv->dev, "Failed to read reg: %d\n", ret);
			continue;
		}

		channel = &priv->channels[i++];
		channel->fwnode = fwnode;
		channel->priv = priv;
		channel->index = index;

		ret = max_ser_parse_ch_dt(channel, fwnode);
		if (ret) {
			fwnode_handle_put(fwnode);
			return ret;
		}

		ret = max_ser_parse_sink_dt_endpoint(channel, fwnode);
		if (ret) {
			fwnode_handle_put(fwnode);
			return ret;
		}
	}

	return max_ser_find_phys_config(priv);
}

static int max_ser_allocate(struct max_ser_priv *priv)
{
	struct max_ser *ser = priv->ser;

	ser->phys = devm_kcalloc(priv->dev, ser->ops->num_phys,
				 sizeof(*ser->phys), GFP_KERNEL);
	if (!ser->phys)
		return -ENOMEM;

	ser->pipes = devm_kcalloc(priv->dev, ser->ops->num_pipes,
				  sizeof(*ser->pipes), GFP_KERNEL);
	if (!ser->pipes)
		return -ENOMEM;

	ser->i2c_xlates = devm_kcalloc(priv->dev, ser->ops->num_i2c_xlates,
				       sizeof(*ser->i2c_xlates), GFP_KERNEL);
	if (!ser->i2c_xlates)
		return -ENOMEM;

	return 0;
}

int max_ser_probe(struct i2c_client *client, struct max_ser *ser)
{
	struct device *dev = &client->dev;
	struct max_ser_priv *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;
	priv->dev = dev;
	priv->ser = ser;
	ser->priv = priv;

	mutex_init(&priv->lock);

	ret = max_ser_allocate(priv);
	if (ret)
		return ret;

	ret = max_ser_parse_dt(priv);
	if (ret)
		return ret;

	ret = max_ser_init(priv);
	if (ret)
		return ret;

	ret = max_ser_i2c_atr_init(priv);
	if (ret)
		return ret;

	return max_ser_v4l2_register(priv);
}
EXPORT_SYMBOL_GPL(max_ser_probe);

int max_ser_remove(struct max_ser *ser)
{
	struct max_ser_priv *priv = ser->priv;

	max_ser_v4l2_unregister(priv);

	max_ser_i2c_atr_deinit(priv);

	return 0;
}
EXPORT_SYMBOL_GPL(max_ser_remove);

int max_ser_reset(struct regmap *regmap)
{
	int ret;

	ret = regmap_update_bits(regmap, 0x10, 0x80, 0x80);
	if (ret)
		return ret;

	msleep(50);

	return 0;
}
EXPORT_SYMBOL_GPL(max_ser_reset);

int max_ser_wait_for_multiple(struct i2c_client *client, struct regmap *regmap,
			      u8 *addrs, unsigned int num_addrs)
{
	unsigned int i, j, val;
	int ret;

	for (i = 0; i < 10; i++) {
		for (j = 0; j < num_addrs; j++) {
			client->addr = addrs[j];

			ret = regmap_read(regmap, 0x0, &val);
			if (ret >= 0)
				return 0;
		}

		msleep(100);

		dev_err(&client->dev, "Retry %u waiting for serializer: %d\n", i, ret);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(max_ser_wait_for_multiple);

int max_ser_wait(struct i2c_client *client, struct regmap *regmap, u8 addr)
{
	return max_ser_wait_for_multiple(client, regmap, &addr, 1);
}
EXPORT_SYMBOL_GPL(max_ser_wait);

static int max_ser_get_dev_id(struct regmap *regmap, unsigned int *dev_id)
{
	return regmap_read(regmap, 0xd, dev_id);
}

static int max_ser_fix_tx_ids(struct regmap *regmap, u8 addr)
{
	unsigned int addr_regs[] = { 0x7b, 0x83, 0x8b, 0x93, 0xa3, 0xab };
	unsigned int dev_id;
	unsigned int i;
	int ret;

	ret = max_ser_get_dev_id(regmap, &dev_id);
	if (ret)
		return ret;

	switch (dev_id) {
	case MAX_SER_MAX9265A_DEV_ID:
		for (i = 0; i < ARRAY_SIZE(addr_regs); i++) {
			ret = regmap_write(regmap, addr_regs[i], addr);
			if (ret)
				return ret;
		}

		break;
	default:
		return 0;
	}

	return 0;
}

int max_ser_change_address(struct i2c_client *client, struct regmap *regmap, u8 addr,
			   bool fix_tx_ids)
{
	int ret;

	ret = regmap_write(regmap, 0x0, addr << 1);
	if (ret)
		return ret;

	client->addr = addr;

	if (fix_tx_ids) {
		ret = max_ser_fix_tx_ids(regmap, addr);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(max_ser_change_address);

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(I2C_ATR);
