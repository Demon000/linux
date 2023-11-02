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
	struct max_ser_phy *phy = container_of(comp, struct max_ser_phy, comp);
	struct max_ser_phy_data *data = &phy->data;
	struct max_ser_priv *priv = phy->priv;
	int ret;

	v4l2_info(sd, "enabled: %u\n", data->enabled);
	v4l2_info(sd, "num_data_lanes: %u\n", data->mipi.num_data_lanes);
	v4l2_info(sd, "clock_lane: %u\n", data->mipi.clock_lane);
	v4l2_info(sd, "noncontinuous_clock: %u\n",
		  !!(data->mipi.flags & V4L2_MBUS_CSI2_NONCONTINUOUS_CLOCK));
	if (priv->ops->log_phy_status) {
		ret = priv->ops->log_phy_status(priv, data, sd->name);
		if (ret)
			return ret;
	}
	v4l2_info(sd, "\n");

	return 0;
}

static const struct v4l2_subdev_core_ops max_ser_phy_core_ops = {
	.log_status = max_ser_phy_log_status,
};

static const struct v4l2_subdev_video_ops max_ser_phy_video_ops = {
};

static const struct v4l2_subdev_pad_ops max_ser_phy_pad_ops = {
	.get_fmt = v4l2_subdev_get_fmt,
};

static const struct v4l2_subdev_ops max_ser_phy_subdev_ops = {
	.core = &max_ser_phy_core_ops,
	.video = &max_ser_phy_video_ops,
	.pad = &max_ser_phy_pad_ops,
};

int max_ser_phy_register_v4l2_sd(struct max_ser_phy *phy,
				 struct v4l2_device *v4l2_dev)
{
	struct max_ser_priv *priv = phy->priv;
	struct max_component *comp = &phy->comp;

	comp->v4l2_dev = v4l2_dev;
	comp->sd_ops = &max_ser_phy_subdev_ops;
	comp->client = priv->client;
	comp->num_source_pads = 1;
	comp->num_sink_pads = 1;
	comp->sink_pads_first = true;
	comp->prefix = priv->name;
	comp->name = "phy";
	comp->index = phy->data.index;

	return max_component_register_v4l2_sd(comp);
}

void max_ser_phy_unregister_v4l2_sd(struct max_ser_phy *phy)
{
	struct max_component *comp = &phy->comp;

	max_component_unregister_v4l2_sd(comp);
}

static int max_ser_phy_parse_sink_dt_endpoint(struct max_ser_priv *priv,
					      struct max_ser_phy_data *data,
					      struct fwnode_handle *fwnode)
{
	struct v4l2_fwnode_endpoint v4l2_ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	struct v4l2_mbus_config_mipi_csi2 *mipi = &v4l2_ep.bus.mipi_csi2;
	struct fwnode_handle *ep, *remote_ep;
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
	    !priv->ops->supports_noncontinuous_clock) {
		dev_err(priv->dev, "Clock non-continuous mode is not supported\n");
		return -EINVAL;
	}

	data->mipi = v4l2_ep.bus.mipi_csi2;

	return 0;
}


int max_ser_phy_parse_dt(struct max_ser_priv *priv, struct max_ser_phy_data *data,
			 struct fwnode_handle *fwnode)
{
	return max_ser_phy_parse_sink_dt_endpoint(priv, data, fwnode);
}
