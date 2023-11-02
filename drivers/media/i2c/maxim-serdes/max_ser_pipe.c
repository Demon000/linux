// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim GMSL2 Serializer Driver
 *
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include "max_ser.h"
#include "max_serdes.h"

static int max_ser_pipe_log_status(struct v4l2_subdev *sd)
{
	struct max_component *comp = v4l2_get_subdevdata(sd);
	struct max_ser_pipe *pipe = container_of(comp, struct max_ser_pipe, comp);
	struct max_ser_pipe_data *data = &pipe->data;
	struct max_ser_priv *priv = pipe->priv;
	unsigned int i;
	int ret;

	v4l2_info(sd, "phy_id: %u\n", data->phy_id);
	v4l2_info(sd, "stream_id: %u\n", data->stream_id);
	v4l2_info(sd, "dts: %u\n", data->num_dts);
	for (i = 0; i < data->num_dts; i++)
		v4l2_info(sd, "\tdt: 0x%02x\n", data->dts[i]);
	v4l2_info(sd, "vcs: 0x%08x\n", data->vcs);
	v4l2_info(sd, "dbl8: %u\n", data->dbl8);
	v4l2_info(sd, "dbl10: %u\n", data->dbl10);
	v4l2_info(sd, "dbl12: %u\n", data->dbl12);
	v4l2_info(sd, "soft_bpp: %u\n", data->soft_bpp);
	v4l2_info(sd, "bpp: %u\n", data->bpp);
	if (priv->ops->log_pipe_status) {
		ret = priv->ops->log_pipe_status(priv, data, sd->name);
		if (ret)
			return ret;
	}
	v4l2_info(sd, "\n");

	return 0;
}

static const struct v4l2_subdev_core_ops max_ser_pipe_core_ops = {
	.log_status = max_ser_pipe_log_status,
};

static const struct v4l2_subdev_video_ops max_ser_pipe_video_ops = {
};

static const struct v4l2_subdev_pad_ops max_ser_pipe_pad_ops = {
	.get_fmt = v4l2_subdev_get_fmt,
};

static const struct v4l2_subdev_ops max_ser_pipe_subdev_ops = {
	.core = &max_ser_pipe_core_ops,
	.video = &max_ser_pipe_video_ops,
	.pad = &max_ser_pipe_pad_ops,
};

int max_ser_pipe_register_v4l2_sd(struct max_ser_pipe *pipe,
				  struct v4l2_device *v4l2_dev)
{
	struct max_ser_priv *priv = pipe->priv;
	struct max_component *comp = &pipe->comp;

	comp->sd_ops = &max_ser_pipe_subdev_ops;
	comp->v4l2_dev = v4l2_dev;
	comp->client = priv->client;
	comp->num_source_pads = 1;
	comp->num_sink_pads = 1;
	comp->prefix = priv->name;
	comp->name = "pipe";
	comp->index = pipe->data.index;

	return max_component_register_v4l2_sd(comp);
}

void max_ser_pipe_unregister_v4l2_sd(struct max_ser_pipe *pipe)
{
	struct max_component *comp = &pipe->comp;

	max_component_unregister_v4l2_sd(comp);
}

int max_ser_pipe_parse_dt(struct max_ser_priv *priv, struct max_ser_pipe_data *data,
			  struct fwnode_handle *fwnode)
{
	unsigned int val;

	val = data->phy_id;
	fwnode_property_read_u32(fwnode, "maxim,phy-id", &val);
	if (val >= priv->ops->num_phys) {
		dev_err(priv->dev, "Invalid PHY %u\n", val);
		return -EINVAL;
	}
	data->phy_id = val;

	val = data->stream_id;
	fwnode_property_read_u32(fwnode, "maxim,stream-id", &val);
	if (val >= MAX_SERDES_STREAMS_NUM) {
		dev_err(priv->dev, "Invalid stream %u\n", val);
		return -EINVAL;
	}
	data->stream_id = val;

	val = 0;
	fwnode_property_read_u32(fwnode, "maxim,soft-bpp", &val);
	if (val > 24) {
		dev_err(priv->dev, "Invalid soft bpp %u\n", val);
		return -EINVAL;
	}
	data->soft_bpp = val;

	val = 0;
	fwnode_property_read_u32(fwnode, "maxim,bpp", &val);
	if (val > 24) {
		dev_err(priv->dev, "Invalid bpp %u\n", val);
		return -EINVAL;
	}
	data->bpp = val;

	data->dbl8 = fwnode_property_read_bool(fwnode, "maxim,dbl8");
	data->dbl10 = fwnode_property_read_bool(fwnode, "maxim,dbl10");
	data->dbl12 = fwnode_property_read_bool(fwnode, "maxim,dbl12");

	return 0;
}
