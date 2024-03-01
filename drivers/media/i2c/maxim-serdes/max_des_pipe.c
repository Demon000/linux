// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim GMSL2 Deserializer Driver
 *
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include "max_des_priv.h"

#define V4L2_CID_DBL_8			(V4L2_CID_USER_BASE | 0x1001)
#define V4L2_CID_DBL_8_MODE		(V4L2_CID_USER_BASE | 0x1002)
#define V4L2_CID_DBL_10			(V4L2_CID_USER_BASE | 0x1003)
#define V4L2_CID_DBL_10_MODE		(V4L2_CID_USER_BASE | 0x1004)
#define V4L2_CID_DBL_12			(V4L2_CID_USER_BASE | 0x1005)

static int max_des_pipe_log_status(struct v4l2_subdev *sd)
{
	struct max_component *comp = sd_max_component(sd);
	struct max_des_priv *priv = comp->priv;
	struct max_des *des = priv->des;
	struct max_des_pipe *pipe = &des->pipes[comp->index];
	unsigned int i;
	int ret;

	v4l2_info(sd, "index: %u\n", pipe->index);
	v4l2_info(sd, "phy_id: %u\n", pipe->phy_id);
	if (des->pipe_stream_autoselect)
		v4l2_info(sd, "stream_id: autoselect\n");
	else
		v4l2_info(sd, "stream_id: %u\n", pipe->stream_id);
	v4l2_info(sd, "link_id: %u\n", pipe->link_id);
	v4l2_info(sd, "dbl8: %u\n", pipe->dbl8);
	v4l2_info(sd, "dbl8mode: %u\n", pipe->dbl8mode);
	v4l2_info(sd, "dbl10: %u\n", pipe->dbl10);
	v4l2_info(sd, "dbl10mode: %u\n", pipe->dbl10mode);
	v4l2_info(sd, "dbl12: %u\n", pipe->dbl12);
	v4l2_info(sd, "remaps: %u\n", pipe->num_remaps);

	for (i = 0; i < pipe->num_remaps; i++) {
		struct max_des_dt_vc_remap *remap = &pipe->remaps[i];

		v4l2_info(sd, "\tremap: from: vc: %u, dt: 0x%02x\n",
			  remap->from_vc, remap->from_dt);
		v4l2_info(sd, "\t       to:   vc: %u, dt: 0x%02x, pipe: %u\n",
			  remap->to_vc, remap->to_dt, remap->phy);
	}

	if (des->ops->log_pipe_status) {
		ret = des->ops->log_pipe_status(des, pipe, sd->name);
		if (ret)
			return ret;
	}
	v4l2_info(sd, "\n");

	return 0;
}

static int max_des_set_pipe_enable(struct max_des *des, struct max_des_pipe *pipe,
				   bool enable)
{
	int ret;

	ret = des->ops->set_pipe_enable(des, pipe, true);
	if (ret)
		return ret;

	pipe->enabled = true;

	return 0;
}

static int max_des_pipe_enable_streams(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *state, u32 pad,
				       u64 streams_mask)
{
	struct max_component *comp = sd_max_component(sd);
	struct max_des_priv *priv = comp->priv;
	struct max_des *des = priv->des;
	struct max_des_pipe *pipe = &des->pipes[comp->index];
	u64 streams;
	int ret;

	streams = max_component_set_enabled_streams(comp, pad, streams_mask, true);

	if (streams) {
		ret = max_des_set_pipe_enable(des, pipe, true);
		if (ret)
			return ret;
	}

	return max_component_set_remote_streams_enable(comp, state, pad,
						       streams_mask, true);
}

static int max_des_pipe_disable_streams(struct v4l2_subdev *sd,
					struct v4l2_subdev_state *state, u32 pad,
					u64 streams_mask)
{
	struct max_component *comp = sd_max_component(sd);
	struct max_des_priv *priv = comp->priv;
	struct max_des *des = priv->des;
	struct max_des_pipe *pipe = &des->pipes[comp->index];
	u64 streams;
	int ret;

	streams = max_component_set_enabled_streams(comp, pad, streams_mask, true);

	if (!streams) {
		ret = max_des_set_pipe_enable(des, pipe, false);
		if (ret)
			return ret;
	}

	return max_component_set_remote_streams_enable(comp, state, pad,
						       streams_mask, false);
}

static int max_des_pipe_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct max_component *comp = ctrl_max_component(ctrl);
	struct max_des_priv *priv = comp->priv;
	struct max_des *des = priv->des;
	struct max_des_pipe *pipe = &des->pipes[comp->index];
	bool enable = ctrl->val;
	int ret;

	switch (ctrl->id) {
	case V4L2_CID_DBL_8:
		ret = des->ops->set_pipe_dbl8_enable(des, pipe, enable);
		if (ret)
			return ret;

		pipe->dbl8 = enable;

		return 0;
	case V4L2_CID_DBL_8_MODE:
		ret = des->ops->set_pipe_dbl8_mode_enable(des, pipe, enable);
		if (ret)
			return ret;

		pipe->dbl8mode = enable;

		return 0;
	case V4L2_CID_DBL_10:
		ret = des->ops->set_pipe_dbl10_enable(des, pipe, enable);
		if (ret)
			return ret;

		pipe->dbl10 = enable;

		return 0;
	case V4L2_CID_DBL_10_MODE:
		ret = des->ops->set_pipe_dbl10_mode_enable(des, pipe, enable);
		if (ret)
			return ret;

		pipe->dbl10mode = enable;

		return 0;
	case V4L2_CID_DBL_12:
		ret = des->ops->set_pipe_dbl12_enable(des, pipe, enable);
		if (ret)
			return ret;

		pipe->dbl12 = enable;

		return 0;
	default:
		return -EINVAL;
	}
}

static const struct v4l2_subdev_core_ops max_des_pipe_core_ops = {
	.log_status = max_des_pipe_log_status,
};

static const struct v4l2_subdev_pad_ops max_des_pipe_pad_ops = {
	.init_cfg = max_component_init_cfg,
	.set_routing = max_component_set_validate_routing,
	.enable_streams = max_des_pipe_enable_streams,
	.disable_streams = max_des_pipe_disable_streams,
	.get_fmt = v4l2_subdev_get_fmt,
	.get_frame_desc = max_component_get_frame_desc,
};

static const struct v4l2_subdev_ops max_des_pipe_subdev_ops = {
	.core = &max_des_pipe_core_ops,
	.pad = &max_des_pipe_pad_ops,
};

static const struct media_entity_operations max_des_link_pipe_xbar_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
	.has_pad_interdep = v4l2_subdev_has_pad_interdep,
};

static const struct v4l2_ctrl_ops max_des_pipe_ctrl_ops = {
	.s_ctrl = max_des_pipe_set_ctrl,
};

#define CTRL_BOOL(_id, _name) \
	{						\
		.ops = &max_des_pipe_ctrl_ops,		\
		.id = (_id),				\
		.name = (_name),			\
		.type = V4L2_CTRL_TYPE_BOOLEAN,		\
		.min = 0,				\
		.max = 1,				\
		.step = 1,				\
		.def = 0,				\
	}

static struct v4l2_ctrl_config max_des_pipe_ctrls[] = {
	CTRL_BOOL(V4L2_CID_DBL_8, "Process BPP = 8 as 16"),
	CTRL_BOOL(V4L2_CID_DBL_8_MODE, "Alternate BPP = 8 as 16"),
	CTRL_BOOL(V4L2_CID_DBL_10, "Process BPP = 10 as 20"),
	CTRL_BOOL(V4L2_CID_DBL_10_MODE, "Alternate BPP = 10 as 20"),
	CTRL_BOOL(V4L2_CID_DBL_12, "Process BPP = 12 as 24"),
};

int max_des_pipe_register_v4l2_sd(struct max_des_priv *priv,
				  struct max_des_pipe *pipe,
				  struct max_component *comp,
				  struct v4l2_device *v4l2_dev)
{
	unsigned int i;
	int ret;

	comp->priv = priv;
	comp->sd_ops = &max_des_pipe_subdev_ops;
	comp->mc_ops = &max_des_link_pipe_xbar_entity_ops;
	comp->v4l2_dev = v4l2_dev;
	comp->dev = priv->dev;
	comp->num_source_pads = 1;
	comp->num_sink_pads = 1;
	comp->prefix = priv->name;
	comp->name = "pipe";
	comp->index = pipe->index;
	comp->routing_disallow = V4L2_SUBDEV_ROUTING_ONLY_1_TO_1 |
				 V4L2_SUBDEV_ROUTING_NO_STREAM_MIX;

	ret = v4l2_ctrl_handler_init(&comp->ctrl_handler, ARRAY_SIZE(max_des_pipe_ctrls));
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(max_des_pipe_ctrls); i++)
		v4l2_ctrl_new_custom(&comp->ctrl_handler,
				     &max_des_pipe_ctrls[i], NULL);

	comp->sd.ctrl_handler = &comp->ctrl_handler;
	v4l2_ctrl_handler_setup(&comp->ctrl_handler);

	return max_component_register_v4l2_sd(comp);
}

void max_des_pipe_unregister_v4l2_sd(struct max_des_priv *priv,
				     struct max_component *comp)
{
	max_component_unregister_v4l2_sd(comp);
}
