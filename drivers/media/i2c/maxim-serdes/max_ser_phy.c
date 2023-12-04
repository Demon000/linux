// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim GMSL2 Serializer Driver
 *
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include "max_ser.h"
#include "max_serdes.h"

static int max_ser_phy_log_status(struct v4l2_subdev *sd)
{
	struct max_component *comp = v4l2_get_subdevdata(sd);
	struct max_ser_priv *priv = comp->priv;
	struct max_ser *ser = priv->ser;
	struct max_ser_phy *phy = &ser->phys[comp->index];
	int ret;

	v4l2_info(sd, "enabled: %u\n", phy->enabled);
	v4l2_info(sd, "num_data_lanes: %u\n", phy->mipi.num_data_lanes);
	v4l2_info(sd, "clock_lane: %u\n", phy->mipi.clock_lane);
	v4l2_info(sd, "noncontinuous_clock: %u\n",
		  !!(phy->mipi.flags & V4L2_MBUS_CSI2_NONCONTINUOUS_CLOCK));
	if (ser->ops->log_phy_status) {
		ret = ser->ops->log_phy_status(ser, phy, sd->name);
		if (ret)
			return ret;
	}
	v4l2_info(sd, "\n");

	return 0;
}

static int max_ser_set_phy_enable(struct max_ser *ser, struct max_ser_phy *phy,
				  bool enable)
{
	int ret;

	ret = ser->ops->set_phy_enable(ser, phy, true);
	if (ret)
		return ret;

	phy->enabled = true;

	return 0;
}

static int max_ser_phy_enable_streams(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *state, u32 pad,
				      u64 streams_mask)
{
	struct max_component *comp = v4l2_get_subdevdata(sd);
	struct max_ser_priv *priv = comp->priv;
	struct max_ser *ser = priv->ser;
	struct max_ser_phy *phy = &ser->phys[comp->index];
	u64 streams;
	int ret;

	streams = max_component_set_enabled_streams(comp, pad, streams_mask, true);

	if (streams) {
		ret = max_ser_set_phy_enable(ser, phy, true);
		if (ret)
			return ret;
	}

	return max_component_set_remote_streams_enable(comp, state, pad,
						       streams_mask, true);
}

static int max_ser_phy_disable_streams(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *state, u32 pad,
				       u64 streams_mask)
{
	struct max_component *comp = v4l2_get_subdevdata(sd);
	struct max_ser_priv *priv = comp->priv;
	struct max_ser *ser = priv->ser;
	struct max_ser_phy *phy = &ser->phys[comp->index];
	u64 streams;
	int ret;

	streams = max_component_set_enabled_streams(comp, pad, streams_mask, false);

	if (!streams) {
		ret = max_ser_set_phy_enable(ser, phy, false);
		if (ret)
			return ret;
	}

	return max_component_set_remote_streams_enable(comp, state, pad,
						       streams_mask, false);
}

static const struct v4l2_subdev_core_ops max_ser_phy_core_ops = {
	.log_status = max_ser_phy_log_status,
};

static const struct v4l2_subdev_pad_ops max_ser_phy_pad_ops = {
	.init_cfg = max_component_init_cfg,
	.set_routing = max_component_set_validate_routing,
	.enable_streams = max_ser_phy_enable_streams,
	.disable_streams = max_ser_phy_disable_streams,
	.get_fmt = v4l2_subdev_get_fmt,
};

static const struct v4l2_subdev_ops max_ser_phy_subdev_ops = {
	.core = &max_ser_phy_core_ops,
	.pad = &max_ser_phy_pad_ops,
};

static const struct media_entity_operations max_ser_phy_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
	.has_pad_interdep = v4l2_subdev_has_pad_interdep,
};

int max_ser_phy_register_v4l2_sd(struct max_ser_priv *priv,
				 struct max_ser_phy *phy,
				 struct max_component *comp,
				 struct v4l2_device *v4l2_dev)
{
	struct max_ser *ser = priv->ser;

	comp->priv = priv;
	comp->v4l2_dev = v4l2_dev;
	comp->sd_ops = &max_ser_phy_subdev_ops;
	comp->mc_ops = &max_ser_phy_entity_ops;
	comp->dev = priv->dev;
	comp->num_source_pads = 1;
	comp->num_sink_pads = 1;
	comp->sink_pads_first = true;
	comp->prefix = ser->name;
	comp->name = "phy";
	comp->index = phy->index;
	comp->routing_disallow = V4L2_SUBDEV_ROUTING_ONLY_1_TO_1 |
				 V4L2_SUBDEV_ROUTING_NO_STREAM_MIX;

	return max_component_register_v4l2_sd(comp);
}

void max_ser_phy_unregister_v4l2_sd(struct max_ser_priv *priv,
				    struct max_component *comp)
{
	max_component_unregister_v4l2_sd(comp);
}

static int max_ser_phy_parse_sink_dt_endpoint(struct max_ser_priv *priv,
					      struct max_ser_phy *phy,
					      struct fwnode_handle *fwnode)
{
	struct v4l2_fwnode_endpoint v4l2_ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	struct v4l2_mbus_config_mipi_csi2 *mipi = &v4l2_ep.bus.mipi_csi2;
	struct fwnode_handle *ep, *remote_ep;
	struct max_ser *ser = priv->ser;
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(fwnode, 0, 0, 0);
	if (!ep) {
		fwnode_handle_put(ep);
		return 0;
	}

	remote_ep = fwnode_graph_get_remote_endpoint(ep);
	fwnode_handle_put(ep);
	if (!remote_ep) {
		dev_err(priv->dev, "Not connected to subdevice\n");
		return -EINVAL;
	}

	ret = v4l2_fwnode_endpoint_parse(remote_ep, &v4l2_ep);
	fwnode_handle_put(remote_ep);
	if (ret) {
		dev_err(priv->dev, "Could not parse v4l2 endpoint\n");
		return ret;
	}

	if (mipi->flags & V4L2_MBUS_CSI2_NONCONTINUOUS_CLOCK &&
	    !ser->ops->supports_noncontinuous_clock) {
		dev_err(priv->dev, "Clock non-continuous mode is not supported\n");
		return -EINVAL;
	}

	phy->mipi = v4l2_ep.bus.mipi_csi2;

	return 0;
}


int max_ser_phy_parse_dt(struct max_ser_priv *priv, struct max_ser_phy *phy,
			 struct fwnode_handle *fwnode)
{
	return max_ser_phy_parse_sink_dt_endpoint(priv, phy, fwnode);
}
