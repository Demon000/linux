// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include "max_des.h"

static int max_des_set_pipe_link(struct max_des_priv *priv,
				 struct max_des_pipe *pipe,
				 struct max_des_link *link)
{
	struct max_des *des = priv->des;
	int ret;

	if (pipe->index != link->index && !des->ops->supports_pipe_link_remap) {
		dev_err(priv->dev, "Pipe link remapping is not supported\n");
		return -EINVAL;
	}

	ret = des->ops->set_pipe_link(des, pipe, link);
	if (ret)
		return ret;

	pipe->link_id = link->index;

	return 0;
}

static int max_des_set_pipe_stream_id(struct max_des_priv *priv,
				      struct max_des_pipe *pipe,
				      unsigned int stream_id)
{
	struct max_des *des = priv->des;
	int ret;

	if (!des->pipe_stream_autoselect) {
		ret = des->ops->set_pipe_stream_id(des, pipe, stream_id);
		if (ret)
			return ret;

		pipe->stream_id = stream_id;
	}

	return 0;
}

static int max_des_link_pipe_xbar_set_routing(struct v4l2_subdev *sd,
					      struct v4l2_subdev_state *state,
					      enum v4l2_subdev_format_whence which,
					      struct v4l2_subdev_krouting *routing)
{
	struct max_component *comp = v4l2_get_subdevdata(sd);
	struct max_des_priv *priv = comp->priv;
	struct max_des *des = priv->des;
	struct v4l2_subdev_route *route;
	unsigned int stream_id;
	unsigned int link_id;
	unsigned int pipe_id;
	int ret;

	ret = max_component_validate_routing(comp, routing);
	if (ret)
		return ret;

	if (which != V4L2_SUBDEV_FORMAT_ACTIVE)
		goto exit;

	for_each_active_route(routing, route) {
		pipe_id = route->source_pad - comp->source_pads_start;

		stream_id = route->sink_pad - comp->sink_pads_start;
		link_id = stream_id / des->num_streams_per_link;
		stream_id = stream_id % des->num_streams_per_link;

		ret = max_des_set_pipe_link(priv, &des->pipes[pipe_id],
					    &des->links[link_id]);
		if (ret)
			return ret;

		ret = max_des_set_pipe_stream_id(priv, &des->pipes[pipe_id],
						 stream_id);
		if (ret)
			return ret;
	}

exit:
	return max_component_set_routing(comp, state, which, routing);
}

static int max_des_link_pipe_xbar_init_routing(struct max_component *comp,
					       struct v4l2_subdev_krouting *routing)
{
	struct max_des_priv *priv = comp->priv;
	struct max_des *des = priv->des;
	unsigned int i;

	routing->num_routes = comp->num_source_pads;

	routing->routes = kcalloc(routing->num_routes, sizeof(*routing->routes),
				  GFP_KERNEL);
	if (!routing->routes)
		return -ENOMEM;

	for (i = 0; i < routing->num_routes; i++) {
		struct v4l2_subdev_route *route = &routing->routes[i];

		route->sink_pad = comp->sink_pads_start + i *
				  des->num_streams_per_link + i;
		route->sink_stream = 0;
		route->source_pad = comp->source_pads_start + i;
		route->source_stream = 0;
		route->flags = V4L2_SUBDEV_ROUTE_FL_ACTIVE;
	}

	return 0;
}

static int max_des_link_pipe_xbar_init_cfg(struct v4l2_subdev *sd,
					   struct v4l2_subdev_state *state)
{
	struct max_component *comp = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_krouting routing;
	int ret;

	ret = max_des_link_pipe_xbar_init_routing(comp, &routing);
	if (ret)
		return ret;

	ret = max_des_link_pipe_xbar_set_routing(sd, state, V4L2_SUBDEV_FORMAT_ACTIVE,
						 &routing);

	kfree(routing.routes);

	return ret;
}

static const struct v4l2_subdev_pad_ops max_des_link_pipe_xbar_pad_ops = {
	.init_cfg = max_des_link_pipe_xbar_init_cfg,
	.set_routing = max_des_link_pipe_xbar_set_routing,
	.enable_streams = max_component_streams_enable,
	.disable_streams = max_component_streams_disable,
	.get_fmt = v4l2_subdev_get_fmt,
};

static const struct v4l2_subdev_ops max_des_link_pipe_xbar_subdev_ops = {
	.pad = &max_des_link_pipe_xbar_pad_ops,
};

static const struct media_entity_operations max_des_link_pipe_xbar_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
	.has_pad_interdep = v4l2_subdev_has_pad_interdep,
};

int max_des_link_pipe_xbar_register_v4l2_sd(struct max_des_priv *priv,
					    struct max_component *comp,
					    struct v4l2_device *v4l2_dev)
{
	struct max_des *des = priv->des;

	comp->priv = priv;
	comp->sd_ops = &max_des_link_pipe_xbar_subdev_ops;
	comp->mc_ops = &max_des_link_pipe_xbar_entity_ops;
	comp->v4l2_dev = v4l2_dev;
	comp->dev = priv->dev;
	comp->num_source_pads = des->ops->num_pipes;
	comp->num_sink_pads = des->ops->num_links * des->num_streams_per_link;
	comp->prefix = des->name;
	comp->name = "link_pipe_xbar";
	comp->index = 0;
	comp->routing_disallow = V4L2_SUBDEV_ROUTING_NO_N_TO_1 |
				 V4L2_SUBDEV_ROUTING_NO_SOURCE_STREAM_MIX;

	return max_component_register_v4l2_sd(comp);
}

void max_des_link_pipe_xbar_unregister_v4l2_sd(struct max_des_priv *priv,
					       struct max_component *comp)
{
	max_component_unregister_v4l2_sd(comp);
}
