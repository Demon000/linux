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
	struct max_component *comp = v4l2_get_subdevdata(sd);
	struct max_ser_priv *priv = comp->priv;

	return max_ser_register_v4l2(priv, sd->v4l2_dev);
}

static void max_ser_link_unregistered(struct v4l2_subdev *sd)
{
	struct max_component *comp = v4l2_get_subdevdata(sd);
	struct max_ser_priv *priv = comp->priv;

	max_ser_unregister_v4l2(priv);
}

static const struct v4l2_subdev_pad_ops max_ser_link_pad_ops = {
	.init_cfg = max_component_init_cfg,
	.set_routing = max_component_set_validate_routing,
	.enable_streams = max_component_streams_enable,
	.disable_streams = max_component_streams_disable,
	.get_fmt = v4l2_subdev_get_fmt,
};

static const struct v4l2_subdev_ops max_ser_link_subdev_ops = {
	.pad = &max_ser_link_pad_ops,
};

static const struct v4l2_subdev_internal_ops max_ser_link_internal_ops = {
	.registered = max_ser_link_registered,
	.unregistered = max_ser_link_unregistered,
};

static const struct media_entity_operations max_ser_link_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
	.has_pad_interdep = v4l2_subdev_has_pad_interdep,
};

int max_ser_link_register_v4l2_sd(struct max_ser_priv *priv,
				  struct max_ser_link *link,
				  struct max_component *comp)
{
	struct max_ser *ser = priv->ser;

	comp->priv = priv;
	comp->sd_ops = &max_ser_link_subdev_ops;
	comp->internal_ops = &max_ser_link_internal_ops;
	comp->mc_ops = &max_ser_link_entity_ops;
	comp->dev = priv->dev;
	comp->num_source_pads = 1;
	comp->num_sink_pads = MAX_SERDES_STREAMS_NUM;
	comp->prefix = ser->name;
	comp->name = "link";
	comp->index = 0;
	comp->notifier_eps = priv->phys_eps;
	comp->notifier_comps = priv->phys_comps;
	comp->num_notifier_eps = ser->ops->num_phys;
	comp->routing_disallow = V4L2_SUBDEV_ROUTING_ONLY_1_TO_1 |
				 V4L2_SUBDEV_ROUTING_NO_SINK_STREAM_MIX;

	return max_component_register_v4l2_sd(comp);
}

void max_ser_link_unregister_v4l2_sd(struct max_ser_priv *priv,
				     struct max_component *comp)
{
	max_component_unregister_v4l2_sd(comp);
}
