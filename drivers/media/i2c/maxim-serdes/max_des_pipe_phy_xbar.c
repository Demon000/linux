// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include "max_des_priv.h"

#define pad_to_pipe_id(comp, pad) (pad - comp->sink_pads_start)
#define pad_to_phy_id(comp, pad) (pad - comp->source_pads_start)
#define pipe_id_to_pad(comp, pipe_id) (pipe_id + comp->sink_pads_start)
#define phy_id_to_pad(comp, phy_id) (phy_id + comp->source_pads_start)

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

		ret = des->ops->set_pipe_remap_enable(des, pipe, i, remap->enabled);
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

static int max_des_get_stream_group(struct v4l2_subdev *sd,
				    struct v4l2_subdev_krouting *routing,
				    unsigned int pad, unsigned int stream,
				    unsigned int *stream_group)
{
	struct v4l2_subdev_route *route;
	bool found;


	for_each_active_route(routing, route) {
		if (route->source_pad == pad && route->source_stream == stream) {
			*stream_group = route->stream_group;
			found = true;
			break;
		}
	}

	if (!found)
		return -EINVAL;

	return 0;
}

static int max_des_get_frame_desc(struct v4l2_subdev *sd, unsigned int pad,
				  struct v4l2_mbus_frame_desc *fd)
{
	struct v4l2_subdev_state *state;
	unsigned int stream_groups[MAX_SERDES_VC_ID_NUM] = { 0 };
	unsigned int vc_ids[MAX_SERDES_VC_ID_NUM] = { 0 };
	unsigned int stream_group_index = 0;
	u64 stream_groups_mask = 0;
	u64 vc_ids_mask = 0;
	unsigned int i, j;
	int ret;

	ret = max_component_get_frame_desc(sd, pad, fd);
	if (ret)
		return ret;

	state = v4l2_subdev_lock_and_get_active_state(sd);

	for (i = 0; i < fd->num_entries; i++) {
		unsigned int stream_group;
		unsigned int vc_id;

		if (fd->type != V4L2_MBUS_FRAME_DESC_TYPE_CSI2)
			continue;

		ret = max_des_get_stream_group(sd, &state->routing, pad,
					       fd->entry[i].stream,
					       &stream_group);
		if (ret)
			goto exit;

		if (stream_groups_mask & BIT(stream_group)) {
			for (j = 0; j < MAX_SERDES_VC_ID_NUM; j++)
				if (stream_groups[j] == stream_group)
					vc_id = vc_ids[j];

		} else {
			if (stream_group_index == MAX_SERDES_VC_ID_NUM) {
				ret = -E2BIG;
				goto exit;
			}

			stream_groups_mask |= BIT(stream_group);

			stream_groups[stream_group_index] = stream_group;
			vc_id = ffz(vc_ids_mask);
			vc_ids[stream_group_index] = vc_id;
			vc_ids_mask |= BIT(vc_id);

			stream_group_index++;
		}

		fd->entry[fd->num_entries].bus.csi2.vc = vc_id;
	}

exit:
	v4l2_subdev_unlock_state(state);

	return ret;
}

static unsigned int max_des_code_num_remaps(u32 code)
{
	u8 dt = max_format_dt_by_code(code);

	if (dt == 0 || dt == MIPI_CSI2_DT_EMBEDDED_8B)
		return 1;

	return 3;
}

static int max_des_pipe_update_remaps(struct max_component *comp,
				      struct v4l2_subdev_krouting *routing,
				      struct v4l2_subdev_stream_configs *stream_configs,
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
	unsigned int num_dt_remaps;
	unsigned int pipe_id;
	unsigned int phy_id;
	unsigned int i = 0;
	unsigned int j;
	u32 source_code;
	u32 sink_code;
	bool enable;
	int ret;

	for_each_active_route(routing, route) {
		sink_code = 0;

		sink_config = max_find_stream_config(stream_configs, route->sink_pad,
						     route->sink_stream);
		if (sink_config)
			sink_code = sink_config->fmt.code;

		pipe_id = pad_to_pipe_id(comp, route->sink_pad);
		if (pipe_id != pipe->index)
			continue;

		num_dt_remaps = max_des_code_num_remaps(sink_code);

		num_remaps += num_dt_remaps;
	}

	if (num_remaps >= des->ops->num_remaps_per_pipe) {
		dev_err(priv->dev, "Too many remaps\n");
		return -EINVAL;
	}

	remaps = devm_kcalloc(priv->dev, num_remaps, sizeof(*remaps), GFP_KERNEL);
	if (!remaps)
		return -ENOMEM;

	for_each_active_route(routing, route) {
		sink_code = 0;
		source_code = 0;

		sink_config = max_find_stream_config(stream_configs, route->sink_pad,
						     route->sink_stream);
		if (sink_config)
			sink_code = sink_config->fmt.code;

		source_config = max_find_stream_config(stream_configs, route->source_pad,
						       route->source_stream);
		if (source_config)
			source_code = source_config->fmt.code;

		phy_id = pad_to_phy_id(comp, route->source_pad);
		pipe_id = pad_to_pipe_id(comp, route->sink_pad);

		if (pipe_id != pipe->index)
			continue;

		if (sink_streams_mask & BIT_ULL(route->sink_stream))
			enable = sink_stream_enable;
		else if (source_config)
			enable = source_config->enabled;
		else
			enable = false;

		num_dt_remaps = max_des_code_num_remaps(sink_code);

		sink_dt = max_format_dt_by_code(sink_code);
		source_dt = max_format_dt_by_code(source_code);

		for (j = 0; j < num_dt_remaps; j++) {
			struct max_des_dt_vc_remap *remap = &remaps[i + j];
			u32 from_vc = 0;
			u32 to_vc = 0;

			/*
			 * TODO:
			 * Find original vc and dt by running
			 * max_component_get_sink_stream_frame_entry.
			 * Find remapped vc by evaluating each stream's
			 * stream_group and assigning the first unused VC to it.
			 */

			if (sink_config)
				from_vc = 0;

			if (source_config)
				to_vc = 0;

			if (j == 1)
				sink_dt = source_dt = MIPI_CSI2_DT_FS;
			else if (j == 2)
				sink_dt = source_dt = MIPI_CSI2_DT_FE;

			remap->from_dt = sink_dt;
			remap->from_vc = from_vc;
			remap->to_dt = source_dt;
			remap->to_vc = to_vc;
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
				       struct v4l2_subdev_krouting *routing)
{
	struct max_des_priv *priv = comp->priv;
	struct max_des *des = priv->des;
	unsigned int i;
	int ret;

	for (i = 0; i < des->ops->num_pipes; i++) {
		struct max_des_pipe *pipe = &des->pipes[i];

		ret = max_des_pipe_update_remaps(comp, routing, NULL, pipe, 0, false);
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

	ret = max_component_validate_routing(comp, which, routing);
	if (ret)
		return ret;

	if (which != V4L2_SUBDEV_FORMAT_ACTIVE)
		goto exit;

	ret = max_des_pipes_update_remaps(comp, routing);
	if (ret)
		return ret;

exit:
	return max_component_set_routing(comp, state, which, routing);
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

	pipe_id = pad_to_pipe_id(comp, sink_pad);

	return &des->pipes[pipe_id];
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

	config = max_find_stream_config(&state->stream_configs, format->pad, format->stream);
	if (!config)
		return -EINVAL;

	old_fmt = config->fmt;
	config->fmt = format->format;

	if (format->which != V4L2_SUBDEV_FORMAT_ACTIVE)
		return 0;

	pipe = max_des_find_pipe_by_pad_stream(comp, state, format->pad, format->stream);
	if (!pipe)
		return -EINVAL;

	ret = max_des_pipe_update_remaps(comp, &state->routing, &state->stream_configs,
					 pipe, 0, false);
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
		u64 source_streams = streams_mask;

		if (!(pipes & BIT_ULL(i)))
			continue;

		pipe = &des->pipes[i];

		sink_pad = pipe_id_to_pad(comp, pipe->index);
		sink_streams_mask = v4l2_subdev_state_xlate_streams(state, pad,
								    sink_pad,
								    &source_streams);

		ret = max_des_pipe_update_remaps(comp, &state->routing, &state->stream_configs,
						 pipe, sink_streams_mask, enable);
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
	.init_cfg = max_component_init_cfg,
	.set_routing = max_des_pipe_phy_xbar_set_routing,
	.enable_streams = max_des_pipe_phy_xbar_enable_streams,
	.disable_streams = max_des_pipe_phy_xbar_disable_streams,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = max_des_pipe_phy_xbar_set_fmt,
	.get_frame_desc = max_des_get_frame_desc,
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
	comp->unique = true;
	comp->routing_disallow = V4L2_SUBDEV_ROUTING_ONLY_1_TO_1;

	return max_component_register_v4l2_sd(comp);
}

void max_des_pipe_phy_xbar_unregister_v4l2_sd(struct max_des_priv *priv,
					      struct max_component *comp)
{
	max_component_unregister_v4l2_sd(comp);
}
