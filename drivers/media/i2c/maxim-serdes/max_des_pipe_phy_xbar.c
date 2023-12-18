// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include "max_des_priv.h"

static int max_des_pipe_set_remaps(struct max_des_priv *priv,
				   struct max_des_pipe *pipe,
				   struct max_des_dt_vc_remap *remaps,
				   unsigned int num_remaps)
{
	struct max_des *des = priv->des;
	int ret;

	ret = des->ops->set_pipe_remaps(des, pipe, remaps, num_remaps);
	if (ret)
		return ret;

	if (pipe->remaps)
		devm_kfree(priv->dev, pipe->remaps);

	pipe->remaps = remaps;
	pipe->num_remaps = num_remaps;

	return 0;
}

static u8 max_des_code_dt(u32 code)
{
	const struct max_format *fmt;

	fmt = max_format_by_code(code);
	if (!fmt)
		return 0;

	return fmt->dt;
}

static unsigned int max_des_code_num_remaps(u32 code)
{
	u8 dt = max_des_code_dt(code);

	if (dt == 0 || dt == MIPI_CSI2_DT_EMBEDDED_8B)
		return 1;

	return 3;
}

static int max_des_pipe_update_remaps(struct max_component *comp,
				      struct v4l2_subdev_state *state,
				      struct max_des_pipe *pipe,
				      u64 sink_streams_mask,
				      bool sink_stream_enable)
{
	struct max_des_priv *priv = comp->priv;
	struct max_des *des = priv->des;
	struct v4l2_subdev_stream_config *sink_config, *source_config;
	u8 sink_dt, source_dt;
	struct max_des_dt_vc_remap *remaps;
	struct v4l2_subdev_route *route;
	unsigned int num_remaps = 0;
	unsigned int pipe_id;
	unsigned int idx;
	unsigned int i, j;
	bool enable;
	int ret;

	idx = 0;
	for_each_active_route(&state->routing, route) {
		sink_config = &state->stream_configs.configs[idx];
		pipe_id = sink_config->pad - comp->sink_pads_start;

		idx += 2;

		if (pipe_id != pipe->index)
			continue;

		num_remaps += max_des_code_num_remaps(sink_config->fmt.code);
	}

	if (num_remaps >= des->ops->num_remaps_per_pipe) {
		dev_err(priv->dev, "Too many remaps\n");
		return -EINVAL;
	}

	remaps = devm_kcalloc(priv->dev, num_remaps, sizeof(*remaps), GFP_KERNEL);
	if (!remaps)
		return -ENOMEM;

	idx = 0;
	for_each_active_route(&state->routing, route) {
		unsigned int phy_id = route->source_pad - comp->source_pads_start;
		unsigned int num_dt_remaps;

		sink_config = &state->stream_configs.configs[idx++];
		source_config = &state->stream_configs.configs[idx++];
		pipe_id = sink_config->pad - comp->sink_pads_start;

		if (pipe_id != pipe->index)
			continue;

		if (sink_streams_mask & BIT_ULL(sink_config->stream))
			enable = sink_stream_enable;
		else
			enable = source_config->enabled;

		num_dt_remaps += max_des_code_num_remaps(sink_config->fmt.code);

		sink_dt = max_des_code_dt(sink_config->fmt.code);
		source_dt = max_des_code_dt(source_config->fmt.code);

		for (j = 0; j < num_dt_remaps; j++) {
			struct max_des_dt_vc_remap *remap = &remaps[i + j];

			if (j == 1)
				sink_dt = source_dt = MIPI_CSI2_DT_FS;
			else if (j == 2)
				sink_dt = source_dt = MIPI_CSI2_DT_FE;

			remap->from_dt = sink_dt;
			remap->from_vc = sink_config->vc;
			remap->to_dt = source_dt;
			remap->to_vc = source_config->vc;
			remap->phy = phy_id;
			remap->enabled = enable;
		}

		i += num_dt_remaps;
	}

	ret = max_des_pipe_set_remaps(priv, pipe, remaps, num_remaps);
	if (ret)
		devm_kfree(priv->dev, remaps);

	return ret;
}

static int max_des_pipes_update_remaps(struct max_component *comp,
				       struct v4l2_subdev_state *state)
{
	struct max_des_priv *priv = comp->priv;
	struct max_des *des = priv->des;
	unsigned int i;
	int ret;

	for (i = 0; i < des->ops->num_pipes; i++) {
		struct max_des_pipe *pipe = &des->pipes[i];

		ret = max_des_pipe_update_remaps(comp, state, pipe, 0, false);
		if (ret)
			return ret;
	}

	return 0;
}

static int max_des_pipe_phy_xbar_set_routing(struct v4l2_subdev *sd,
					     struct v4l2_subdev_state *state,
					     enum v4l2_subdev_format_whence which,
					     struct v4l2_subdev_krouting *routing)
{
	struct max_component *comp = v4l2_get_subdevdata(sd);
	int ret;

	ret = max_component_validate_routing(comp, routing);
	if (ret)
		return ret;

	if (which != V4L2_SUBDEV_FORMAT_ACTIVE)
		goto exit;

	ret = max_des_pipes_update_remaps(comp, state);
	if (ret)
		return ret;

exit:
	return max_component_set_routing(comp, state, which, routing);
}

static int max_des_pipe_phy_xbar_init_routing(struct max_component *comp,
					      struct v4l2_subdev_krouting *routing)
{
	struct max_des_priv *priv = comp->priv;
	struct max_des *des = priv->des;
	struct max_des_phy *phy;
	u64 *pads_streams;
	unsigned int i;
	int ret;

	routing->num_routes = max(comp->num_source_pads, des->num_enabled_phys);

	routing->routes = kcalloc(routing->num_routes, sizeof(*routing->routes),
				  GFP_KERNEL);
	if (!routing->routes)
		return -ENOMEM;

	pads_streams = kcalloc(comp->num_pads, sizeof(*pads_streams), GFP_KERNEL);
	if (!pads_streams) {
		kfree(routing->routes);
		return -ENOMEM;
	}

	pr_err("comp: %s, sink_pads_start: %u, num_sink_pads: %u\n",
		comp->sd.name, comp->sink_pads_start, comp->num_sink_pads);
	pr_err("comp: %s, source_pads_start: %u, num_source_pads: %u\n",
		comp->sd.name, comp->source_pads_start, comp->num_source_pads);
	pr_err("comp: %s, num_enabled_phys: %u\n",
		comp->sd.name, des->num_enabled_phys);

	for (i = 0; i < routing->num_routes; i++) {
		struct v4l2_subdev_route *route = &routing->routes[i];

		route->sink_pad = comp->sink_pads_start + i % comp->num_sink_pads;
		route->sink_stream = ffz(pads_streams[route->sink_pad]);

		phy = des->enabled_phys[i % des->num_enabled_phys];

		route->source_pad = comp->source_pads_start + phy->index;
		route->source_stream = ffz(pads_streams[route->source_pad]);
		route->flags = V4L2_SUBDEV_ROUTE_FL_ACTIVE;

		pads_streams[route->sink_pad] |= BIT_ULL(route->sink_stream);
		pads_streams[route->source_pad] |= BIT_ULL(route->source_stream);

		pr_err("comp: %s, i: %u, %u/%u -> %u/%u\n",
			comp->sd.name, i,
			route->sink_pad, route->sink_stream,
			route->source_pad, route->source_stream);
	}

	kfree(pads_streams);

	return ret;
}

static int max_des_pipe_phy_xbar_init_cfg(struct v4l2_subdev *sd,
					  struct v4l2_subdev_state *state)
{
	struct max_component *comp = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_krouting routing;
	int ret;

	ret = max_des_pipe_phy_xbar_init_routing(comp, &routing);
	if (ret)
		return ret;

	ret = max_des_pipe_phy_xbar_set_routing(sd, state, V4L2_SUBDEV_FORMAT_ACTIVE,
						&routing);

	kfree(routing.routes);

	return ret;
}

static struct max_des_pipe *
max_des_find_pipe_by_pad_stream(struct max_component *comp,
				struct v4l2_subdev_state *state,
				u32 pad, u32 stream)
{
	struct max_des_priv *priv = comp->priv;
	struct max_des *des = priv->des;
	unsigned int pipe_id;
	u32 sink_pad;
	int ret;

	if (max_component_is_pad_source(comp, pad)) {
		ret = v4l2_subdev_routing_find_opposite_end(&state->routing,
							    pad, stream,
							    &sink_pad, NULL);
		if (ret)
			return NULL;
	} else {
		sink_pad = pad;
	}

	pipe_id = sink_pad - comp->sink_pads_start;;

	return &des->pipes[pipe_id];
}

static int max_des_pipe_phy_xbar_set_vc(struct v4l2_subdev *sd,
					struct v4l2_subdev_state *state,
					struct v4l2_subdev_vc *vc)
{
	struct max_component *comp = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_stream_config *config;
	struct max_des_pipe *pipe;
	u32 old_vc;
	int ret;

	config = max_find_stream_config(state, vc->pad, vc->stream);
	if (!config)
		return -EINVAL;

	old_vc = config->vc;
	config->vc = vc->vc;

	if (vc->which != V4L2_SUBDEV_FORMAT_ACTIVE)
		return 0;

	pipe = max_des_find_pipe_by_pad_stream(comp, state, vc->pad, vc->stream);
	if (!pipe)
		return -EINVAL;

	ret = max_des_pipe_update_remaps(comp, state, pipe, 0, false);
	if (ret)
		config->vc = old_vc;

	return ret;
}

static int max_des_pipe_phy_xbar_set_fmt(struct v4l2_subdev *sd,
					 struct v4l2_subdev_state *state,
					 struct v4l2_subdev_format *format)
{
	struct max_component *comp = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_stream_config *config;
	struct v4l2_mbus_framefmt old_fmt;
	struct max_des_pipe *pipe;
	int ret;

	config = max_find_stream_config(state, format->pad, format->stream);
	if (!config)
		return -EINVAL;

	old_fmt = config->fmt;
	config->fmt = format->format;

	if (format->which != V4L2_SUBDEV_FORMAT_ACTIVE)
		return 0;

	pipe = max_des_find_pipe_by_pad_stream(comp, state, format->pad, format->stream);
	if (!pipe)
		return -EINVAL;

	ret = max_des_pipe_update_remaps(comp, state, pipe, 0, false);
	if (ret)
		config->fmt = old_fmt;

	return ret;
}

static int max_des_enable_pipe_remaps_for_source_streams(struct max_component *comp,
							 struct v4l2_subdev_state *state,
							 u32 pad, u64 streams_mask,
							 bool enable)
{
	struct max_des_priv *priv = comp->priv;
	struct max_des *des = priv->des;
	struct max_des_pipe *pipe;
	unsigned int i;
	u64 sink_streams_mask;
	u32 sink_pad;
	u64 pipes = 0;
	u32 stream;
	int ret;

	/* Gather all affected pipes. */
	for (stream = 0; stream < sizeof(streams_mask) * 8; stream++) {
		if (!(streams_mask & BIT_ULL(stream)))
			continue;

		pipe = max_des_find_pipe_by_pad_stream(comp, state, pad, stream);
		if (!pipe)
			return -EINVAL;

		pipes |= BIT(pipe->index);
	}

	/*
	 * For each affected pipe, translate the source streams to sink streams
	 * for it, and update the remaps.
	 */
	for (i = 0; i < des->ops->num_pipes; i++) {
		if (!(pipes & BIT_ULL(i)))
			continue;

		pipe = &des->pipes[i];

		sink_pad = comp->sink_pads_start + pipe->index;
		sink_streams_mask = v4l2_subdev_state_xlate_streams(state, pad,
								    sink_pad,
								    &streams_mask);

		ret = max_des_pipe_update_remaps(comp, state, pipe,
						 sink_streams_mask, enable);
		if (ret)
			return ret;
	}

	return 0;
}

static int max_des_pipe_phy_xbar_enable_streams(struct v4l2_subdev *sd,
						struct v4l2_subdev_state *state,
						u32 pad, u64 streams_mask)
{
	struct max_component *comp = v4l2_get_subdevdata(sd);
	u64 streams;
	int ret;

	streams = max_component_set_enabled_streams(comp, pad, streams_mask, true);

	ret = max_des_enable_pipe_remaps_for_source_streams(comp, state, pad,
							    streams_mask, true);
	if (ret)
		return ret;

	return max_component_set_remote_streams_enable(comp, state, pad,
						       streams_mask, true);
}

static int max_des_pipe_phy_xbar_disable_streams(struct v4l2_subdev *sd,
						 struct v4l2_subdev_state *state,
						 u32 pad, u64 streams_mask)
{
	struct max_component *comp = v4l2_get_subdevdata(sd);
	u64 streams;
	int ret;

	streams = max_component_set_enabled_streams(comp, pad, streams_mask, false);

	ret = max_des_enable_pipe_remaps_for_source_streams(comp, state, pad,
							    streams_mask, false);
	if (ret)
		return ret;

	return max_component_set_remote_streams_enable(comp, state, pad,
						       streams_mask, false);
}

static const struct v4l2_subdev_pad_ops max_des_pipe_phy_xbar_pad_ops = {
	.init_cfg = max_des_pipe_phy_xbar_init_cfg,
	.set_routing = max_des_pipe_phy_xbar_set_routing,
	.enable_streams = max_des_pipe_phy_xbar_enable_streams,
	.disable_streams = max_des_pipe_phy_xbar_disable_streams,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = max_des_pipe_phy_xbar_set_fmt,
	.get_vc = v4l2_subdev_get_vc,
	.set_vc = max_des_pipe_phy_xbar_set_vc,
};

static const struct v4l2_subdev_ops max_des_pipe_phy_xbar_subdev_ops = {
	.pad = &max_des_pipe_phy_xbar_pad_ops,
};

static const struct media_entity_operations max_des_pipe_phy_xbar_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
	.has_pad_interdep = v4l2_subdev_has_pad_interdep,
};

int max_des_pipe_phy_xbar_register_v4l2_sd(struct max_des_priv *priv,
					   struct max_component *comp,
					   struct v4l2_device *v4l2_dev)
{
	struct max_des *des = priv->des;

	comp->priv = priv;
	comp->sd_ops = &max_des_pipe_phy_xbar_subdev_ops;
	comp->mc_ops = &max_des_pipe_phy_xbar_entity_ops;
	comp->v4l2_dev = v4l2_dev;
	comp->dev = priv->dev;
	comp->num_source_pads = des->ops->num_phys;
	comp->num_sink_pads = des->ops->num_pipes;
	comp->prefix = priv->name;
	comp->name = "pipe_phy_xbar";
	comp->index = 0;
	comp->routing_disallow = V4L2_SUBDEV_ROUTING_ONLY_1_TO_1;

	return max_component_register_v4l2_sd(comp);
}

void max_des_pipe_phy_xbar_unregister_v4l2_sd(struct max_des_priv *priv,
					      struct max_component *comp)
{
	max_component_unregister_v4l2_sd(comp);
}
