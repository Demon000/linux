// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim GMSL2 Serializer Driver
 *
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include "max_ser.h"
#include "max_serdes.h"

static int max_ser_set_pipe_phy(struct max_ser_priv *priv,
				struct max_ser_pipe *pipe,
				struct max_ser_phy *phy)
{
	struct max_ser *ser = priv->ser;
	int ret;

	ret = ser->ops->set_pipe_phy(ser, pipe, phy);
	if (ret)
		return ret;

	pipe->phy_id = phy->index;

	return 0;
}

static int max_ser_phy_pipe_xbar_set_routing(struct v4l2_subdev *sd,
					     struct v4l2_subdev_state *state,
					     enum v4l2_subdev_format_whence which,
					     struct v4l2_subdev_krouting *routing)
{
	struct max_component *comp = v4l2_get_subdevdata(sd);
	struct max_ser_priv *priv = comp->priv;
	struct max_ser *ser = priv->ser;
	struct v4l2_subdev_route *route;
	unsigned int pipe_id;
	unsigned int phy_id;
	int ret;

	ret = max_component_validate_routing(comp, routing);
	if (ret)
		return ret;

	if (which != V4L2_SUBDEV_FORMAT_ACTIVE)
		goto exit;

	for_each_active_route(routing, route) {
		phy_id = route->sink_pad - comp->sink_pads_start;
		pipe_id = route->source_pad - comp->source_pads_start;

		ret = max_ser_set_pipe_phy(priv, &ser->pipes[pipe_id],
					   &ser->phys[phy_id]);
		if (ret)
			return ret;
	}

exit:
	return max_component_set_routing(comp, state, which, routing);
}

static int max_ser_phy_pipe_xbar_init_cfg(struct v4l2_subdev *sd,
					  struct v4l2_subdev_state *state)
{
	struct max_component *comp = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_krouting routing;
	int ret;

	ret = max_component_init_routing(comp, &routing);
	if (ret)
		return ret;

	ret = max_ser_phy_pipe_xbar_set_routing(sd, state, V4L2_SUBDEV_FORMAT_ACTIVE,
						&routing);

	kfree(routing.routes);

	return ret;
}

static const struct v4l2_subdev_pad_ops max_ser_phy_pipe_xbar_pad_ops = {
	.init_cfg = max_ser_phy_pipe_xbar_init_cfg,
	.set_routing = max_ser_phy_pipe_xbar_set_routing,
	.enable_streams = max_component_streams_enable,
	.disable_streams = max_component_streams_disable,
	.get_fmt = v4l2_subdev_get_fmt,
};

static const struct v4l2_subdev_ops max_ser_phy_pipe_xbar_subdev_ops = {
	.pad = &max_ser_phy_pipe_xbar_pad_ops,
};

static const struct media_entity_operations max_ser_phy_pipe_xbar_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
	.has_pad_interdep = v4l2_subdev_has_pad_interdep,
};

int max_ser_phy_pipe_xbar_register_v4l2_sd(struct max_ser_priv *priv,
					   struct max_component *comp,
					   struct v4l2_device *v4l2_dev)
{
	struct max_ser *ser = priv->ser;

	comp->priv = priv;
	comp->sd_ops = &max_ser_phy_pipe_xbar_subdev_ops;
	comp->mc_ops = &max_ser_phy_pipe_xbar_entity_ops;
	comp->v4l2_dev = v4l2_dev;
	comp->dev = priv->dev;
	comp->num_source_pads = ser->ops->num_pipes;
	comp->num_sink_pads = ser->ops->num_phys;
	comp->prefix = ser->name;
	comp->name = "phy_pipe_xbar";
	comp->index = 0;
	comp->routing_disallow = V4L2_SUBDEV_ROUTING_NO_N_TO_1 |
				 V4L2_SUBDEV_ROUTING_NO_SOURCE_STREAM_MIX;

	return max_component_register_v4l2_sd(comp);
}

void max_ser_phy_pipe_xbar_unregister_v4l2_sd(struct max_ser_priv *priv,
					      struct max_component *comp)
{
	max_component_unregister_v4l2_sd(comp);
}
