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
	struct max_ser_priv *priv = comp->priv;
	struct max_ser *ser = priv->ser;
	struct max_ser_pipe *pipe = &ser->pipes[comp->index];
	unsigned int i;
	int ret;

	v4l2_info(sd, "phy_id: %u\n", pipe->phy_id);
	v4l2_info(sd, "stream_id: %u\n", pipe->stream_id);
	v4l2_info(sd, "dts: %u\n", pipe->num_dts);
	for (i = 0; i < pipe->num_dts; i++)
		v4l2_info(sd, "\tdt: 0x%02x\n", pipe->dts[i]);
	v4l2_info(sd, "vcs: 0x%08x\n", pipe->vcs);
	v4l2_info(sd, "dbl8: %u\n", pipe->dbl8);
	v4l2_info(sd, "dbl10: %u\n", pipe->dbl10);
	v4l2_info(sd, "dbl12: %u\n", pipe->dbl12);
	v4l2_info(sd, "soft_bpp: %u\n", pipe->soft_bpp);
	v4l2_info(sd, "bpp: %u\n", pipe->bpp);
	if (ser->ops->log_pipe_status) {
		ret = ser->ops->log_pipe_status(ser, pipe, sd->name);
		if (ret)
			return ret;
	}
	v4l2_info(sd, "\n");

	return 0;
}

static int max_ser_pipe_set_routing(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *state,
				    enum v4l2_subdev_format_whence which,
				    struct v4l2_subdev_krouting *routing)
{
	struct max_component *comp = v4l2_get_subdevdata(sd);
	int ret;

	ret = max_component_validate_routing(comp, routing);
	if (ret)
		return ret;

	/* TODO: handle data types and VCs. */

	return max_component_set_routing(comp, state, which, routing);
}

static int max_ser_pipe_init_cfg(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state)
{
	struct max_component *comp = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_krouting routing;
	int ret;

	ret = max_component_init_routing(comp, &routing);
	if (ret)
		return ret;

	ret = max_ser_pipe_set_routing(sd, state, V4L2_SUBDEV_FORMAT_ACTIVE, &routing);

	kfree(routing.routes);

	return ret;
}

static int max_ser_set_pipe_enable(struct max_ser *ser, struct max_ser_pipe *pipe,
				   bool enable)
{
	int ret;

	ret = ser->ops->set_pipe_enable(ser, pipe, true);
	if (ret)
		return ret;

	pipe->enabled = true;

	return 0;
}

static int max_ser_pipe_enable_streams(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *state, u32 pad,
				       u64 streams_mask)
{
	struct max_component *comp = v4l2_get_subdevdata(sd);
	struct max_ser_priv *priv = comp->priv;
	struct max_ser *ser = priv->ser;
	struct max_ser_pipe *pipe = &ser->pipes[comp->index];
	u64 streams;
	int ret;

	streams = max_component_set_enabled_streams(comp, pad, streams_mask, true);

	if (streams) {
		ret = max_ser_set_pipe_enable(ser, pipe, true);
		if (ret)
			return ret;
	}

	return max_component_set_remote_streams_enable(comp, state, pad,
						       streams_mask, true);
}

static int max_ser_pipe_disable_streams(struct v4l2_subdev *sd,
					struct v4l2_subdev_state *state, u32 pad,
					u64 streams_mask)
{
	struct max_component *comp = v4l2_get_subdevdata(sd);
	struct max_ser_priv *priv = comp->priv;
	struct max_ser *ser = priv->ser;
	struct max_ser_pipe *pipe = &ser->pipes[comp->index];
	u64 streams;
	int ret;

	streams = max_component_set_enabled_streams(comp, pad, streams_mask, true);

	if (!streams) {
		ret = max_ser_set_pipe_enable(ser, pipe, false);
		if (ret)
			return ret;
	}

	return max_component_set_remote_streams_enable(comp, state, pad,
						       streams_mask, false);
}

static const struct v4l2_subdev_core_ops max_ser_pipe_core_ops = {
	.log_status = max_ser_pipe_log_status,
};

static const struct v4l2_subdev_pad_ops max_ser_pipe_pad_ops = {
	.init_cfg = max_ser_pipe_init_cfg,
	.set_routing = max_ser_pipe_set_routing,
	.enable_streams = max_ser_pipe_enable_streams,
	.disable_streams = max_ser_pipe_disable_streams,
	.get_fmt = v4l2_subdev_get_fmt,
};

static const struct v4l2_subdev_ops max_ser_pipe_subdev_ops = {
	.core = &max_ser_pipe_core_ops,
	.pad = &max_ser_pipe_pad_ops,
};

static const struct media_entity_operations max_ser_pipe_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
	.has_pad_interdep = v4l2_subdev_has_pad_interdep,
};

int max_ser_pipe_register_v4l2_sd(struct max_ser_priv *priv,
				  struct max_ser_pipe *pipe,
				  struct max_component *comp,
				  struct v4l2_device *v4l2_dev)
{
	struct max_ser *ser = priv->ser;

	comp->priv = priv;
	comp->sd_ops = &max_ser_pipe_subdev_ops;
	comp->mc_ops = &max_ser_pipe_entity_ops;
	comp->v4l2_dev = v4l2_dev;
	comp->dev = priv->dev;
	comp->num_source_pads = 1;
	comp->num_sink_pads = 1;
	comp->prefix = ser->name;
	comp->name = "pipe";
	comp->index = pipe->index;
	comp->routing_disallow = V4L2_SUBDEV_ROUTING_ONLY_1_TO_1 |
				 V4L2_SUBDEV_ROUTING_NO_STREAM_MIX;

	return max_component_register_v4l2_sd(comp);
}

void max_ser_pipe_unregister_v4l2_sd(struct max_ser_priv *priv,
				     struct max_component *comp)
{
	max_component_unregister_v4l2_sd(comp);
}

int max_ser_pipe_parse_dt(struct max_ser_priv *priv, struct max_ser_pipe *pipe,
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
