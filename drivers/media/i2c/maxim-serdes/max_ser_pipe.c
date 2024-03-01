// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim GMSL2 Serializer Driver
 *
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include "max_ser_priv.h"
#include "max_serdes.h"

#define V4L2_CID_DBL_8			(V4L2_CID_USER_BASE | 0x1001)
#define V4L2_CID_DBL_10			(V4L2_CID_USER_BASE | 0x1002)
#define V4L2_CID_DBL_12			(V4L2_CID_USER_BASE | 0x1003)
#define V4L2_CID_BPP			(V4L2_CID_USER_BASE | 0x1004)
#define V4L2_CID_SOFT_BPP		(V4L2_CID_USER_BASE | 0x1005)

static int max_ser_pipe_log_status(struct v4l2_subdev *sd)
{
	struct max_ser_pipe *pipe = sd_ser_data(sd, pipes);
	struct max_ser *ser = sd_ser(sd);
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
	struct max_component *comp = sd_max_component(sd);
	int ret;

	ret = max_component_validate_routing(comp, which, routing);
	if (ret)
		return ret;

	/* TODO: handle data types and VCs. */

	return max_component_set_routing(comp, state, which, routing);
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
	struct max_ser_pipe *pipe = sd_ser_data(sd, pipes);
	struct max_component *comp = sd_max_component(sd);
	struct max_ser *ser = sd_ser(sd);
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
	struct max_ser_pipe *pipe = sd_ser_data(sd, pipes);
	struct max_component *comp = sd_max_component(sd);
	struct max_ser *ser = sd_ser(sd);
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

static int max_ser_pipe_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct v4l2_subdev *sd = ctrl_sd(ctrl);
	struct max_ser_pipe *pipe = sd_ser_data(sd, pipes);
	struct max_ser *ser = sd_ser(sd);
	bool enable = ctrl->val;
	int ret;

	switch (ctrl->id) {
	case V4L2_CID_DBL_8:
		ret = ser->ops->set_pipe_dbl8_enable(ser, pipe, enable);
		if (ret)
			return ret;

		pipe->dbl8 = enable;

		return 0;
	case V4L2_CID_DBL_10:
		ret = ser->ops->set_pipe_dbl10_enable(ser, pipe, enable);
		if (ret)
			return ret;

		pipe->dbl10 = enable;

		return 0;
	case V4L2_CID_DBL_12:
		ret = ser->ops->set_pipe_dbl12_enable(ser, pipe, enable);
		if (ret)
			return ret;

		pipe->dbl12 = enable;

		return 0;
	case V4L2_CID_BPP:
		ret = ser->ops->set_pipe_bpp(ser, pipe, ctrl->val);
		if (ret)
			return ret;

		pipe->bpp = ctrl->val;

		return 0;
	case V4L2_CID_SOFT_BPP:
		ret = ser->ops->set_pipe_soft_bpp(ser, pipe, ctrl->val);
		if (ret)
			return ret;

		pipe->soft_bpp = ctrl->val;

		return 0;
	default:
		return -EINVAL;
	}
}

static const struct v4l2_subdev_core_ops max_ser_pipe_core_ops = {
	.log_status = max_ser_pipe_log_status,
};

static const struct v4l2_subdev_pad_ops max_ser_pipe_pad_ops = {
	.init_cfg = max_component_init_cfg,
	.set_routing = max_ser_pipe_set_routing,
	.enable_streams = max_ser_pipe_enable_streams,
	.disable_streams = max_ser_pipe_disable_streams,
	.get_fmt = v4l2_subdev_get_fmt,
	.get_frame_desc = max_component_get_frame_desc,
};

static const struct v4l2_subdev_ops max_ser_pipe_subdev_ops = {
	.core = &max_ser_pipe_core_ops,
	.pad = &max_ser_pipe_pad_ops,
};

static const struct media_entity_operations max_ser_pipe_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
	.has_pad_interdep = v4l2_subdev_has_pad_interdep,
};

static const struct v4l2_ctrl_ops max_ser_pipe_ctrl_ops = {
	.s_ctrl = max_ser_pipe_set_ctrl,
};

#define CTRL_BOOL(_id, _name) \
	{						\
		.ops = &max_ser_pipe_ctrl_ops,		\
		.id = (_id),				\
		.name = (_name),			\
		.type = V4L2_CTRL_TYPE_BOOLEAN,		\
		.min = 0,				\
		.max = 1,				\
		.step = 1,				\
		.def = 0,				\
	}

#define CTRL_BPP(_id, _name) \
	{						\
		.ops = &max_ser_pipe_ctrl_ops,		\
		.id = (_id),				\
		.name = (_name),			\
		.type = V4L2_CTRL_TYPE_INTEGER,		\
		.min = 0,				\
		.max = 24,				\
		.step = 1,				\
		.def = 0,				\
	}

static struct v4l2_ctrl_config max_ser_pipe_ctrls[] = {
	CTRL_BOOL(V4L2_CID_DBL_8, "Process BPP = 8 as 16"),
	CTRL_BOOL(V4L2_CID_DBL_10, "Process BPP = 10 as 20"),
	CTRL_BOOL(V4L2_CID_DBL_12, "Process BPP = 12 as 24"),
	CTRL_BPP(V4L2_CID_BPP, "BPP override"),
	CTRL_BPP(V4L2_CID_SOFT_BPP, "Software BPP override"),
};

int max_ser_pipe_register_v4l2_sd(struct max_ser_priv *priv,
				  struct max_ser_pipe *pipe,
				  struct max_component *comp,
				  struct v4l2_device *v4l2_dev)
{
	unsigned int i;
	int ret;

	comp->priv = priv;
	comp->sd_ops = &max_ser_pipe_subdev_ops;
	comp->mc_ops = &max_ser_pipe_entity_ops;
	comp->v4l2_dev = v4l2_dev;
	comp->dev = priv->dev;
	comp->num_source_pads = 1;
	comp->num_sink_pads = 1;
	comp->prefix = priv->name;
	comp->name = "pipe";
	comp->index = pipe->index;
	comp->routing_disallow = V4L2_SUBDEV_ROUTING_ONLY_1_TO_1 |
				 V4L2_SUBDEV_ROUTING_NO_STREAM_MIX;

	ret = v4l2_ctrl_handler_init(&comp->ctrl_handler, ARRAY_SIZE(max_ser_pipe_ctrls));
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(max_ser_pipe_ctrls); i++)
		v4l2_ctrl_new_custom(&comp->ctrl_handler,
				     &max_ser_pipe_ctrls[i], NULL);

	comp->sd.ctrl_handler = &comp->ctrl_handler;
	v4l2_ctrl_handler_setup(&comp->ctrl_handler);

	return max_component_register_v4l2_sd(comp);
}

void max_ser_pipe_unregister_v4l2_sd(struct max_ser_priv *priv,
				     struct max_component *comp)
{
	max_component_unregister_v4l2_sd(comp);
}
