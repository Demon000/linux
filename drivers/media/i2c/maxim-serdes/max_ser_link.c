// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim GMSL2 Serializer Driver
 *
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include "max_ser.h"
#include "max_serdes.h"

static int max_ser_link_registered(struct v4l2_subdev *sd)
{
	struct max_ser_link *link = v4l2_get_subdevdata(sd);
	struct max_ser_priv *priv = link->priv;

	return max_ser_register_v4l2(priv, sd->v4l2_dev);
}

static void max_ser_link_unregistered(struct v4l2_subdev *sd)
{
	struct max_ser_link *link = v4l2_get_subdevdata(sd);
	struct max_ser_priv *priv = link->priv;

	max_ser_unregister_v4l2(priv);
}

static const struct v4l2_subdev_core_ops max_ser_link_core_ops = {
};

static const struct v4l2_subdev_video_ops max_ser_link_video_ops = {
};

static const struct v4l2_subdev_pad_ops max_ser_link_pad_ops = {
	.get_fmt = v4l2_subdev_get_fmt,
};

static const struct v4l2_subdev_ops max_ser_link_subdev_ops = {
	.core = &max_ser_link_core_ops,
	.video = &max_ser_link_video_ops,
	.pad = &max_ser_link_pad_ops,
};

static const struct v4l2_subdev_internal_ops max_ser_link_internal_ops = {
	.registered = max_ser_link_registered,
	.unregistered = max_ser_link_unregistered,
};

int max_ser_link_register_v4l2_sd(struct max_ser_link *link)
{
	struct max_ser_priv *priv = link->priv;
	struct max_component *comp = &link->comp;

	comp->sd_ops = &max_ser_link_subdev_ops;
	comp->internal_ops = &max_ser_link_internal_ops;
	comp->client = priv->client;
	comp->num_source_pads = 1;
	comp->num_sink_pads = MAX_SERDES_STREAMS_NUM;
	comp->prefix = priv->name;
	comp->name = "link";
	comp->index = 0;
	comp->notifier_eps = priv->phys_eps;
	comp->notifier_comps = priv->phys_comps;
	comp->num_notifier_eps = priv->ops->num_phys;

	return max_component_register_v4l2_sd(comp);
}

void max_ser_link_unregister_v4l2_sd(struct max_ser_link *link)
{
	struct max_component *comp = &link->comp;

	max_component_unregister_v4l2_sd(comp);
}
