// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include "max_des.h"

static const struct v4l2_subdev_video_ops max_des_pipe_phy_xbar_video_ops = {
};

static const struct v4l2_subdev_pad_ops max_des_pipe_phy_xbar_pad_ops = {
	.get_fmt = v4l2_subdev_get_fmt,
};

static const struct v4l2_subdev_ops max_des_pipe_phy_xbar_subdev_ops = {
	.video = &max_des_pipe_phy_xbar_video_ops,
	.pad = &max_des_pipe_phy_xbar_pad_ops,
};

int max_des_pipe_phy_xbar_register_v4l2_sd(struct max_des_pipe_phy_xbar *xbar,
					   struct v4l2_device *v4l2_dev)
{
	struct max_des_priv *priv = xbar->priv;
	struct max_component *comp = &xbar->comp;

	comp->sd_ops = &max_des_pipe_phy_xbar_subdev_ops;
	comp->v4l2_dev = v4l2_dev;
	comp->client = priv->client;
	comp->num_source_pads = priv->ops->num_phys;
	comp->num_sink_pads = priv->ops->num_pipes;
	comp->prefix = priv->name;
	comp->name = "pipe_phy_xbar";
	comp->index = 0;

	return max_component_register_v4l2_sd(comp);
}

void max_des_pipe_phy_xbar_unregister_v4l2_sd(struct max_des_pipe_phy_xbar *xbar)
{
	struct max_component *comp = &xbar->comp;

	max_component_unregister_v4l2_sd(comp);
}
