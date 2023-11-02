#include "max_des.h"

#define MAX_DES_LINK_FREQUENCY_MIN		100000000ull
#define MAX_DES_LINK_FREQUENCY_DEFAULT		750000000ull
#define MAX_DES_LINK_FREQUENCY_MAX		1250000000ull

static int max_des_phy_log_status(struct v4l2_subdev *sd)
{
	struct max_component *comp = v4l2_get_subdevdata(sd);
	struct max_des_phy *phy = container_of(comp, struct max_des_phy, comp);
	struct max_des_phy_data *data = &phy->data;
	struct max_des_priv *priv = phy->priv;
	int ret;

	v4l2_info(sd, "index: %u\n", data->index);
	v4l2_info(sd, "enabled: %u\n", data->enabled);
	v4l2_info(sd, "link_frequency: %llu\n", data->link_frequency);
	v4l2_info(sd, "num_data_lanes: %u\n", data->mipi.num_data_lanes);
	v4l2_info(sd, "clock_lane: %u\n", data->mipi.clock_lane);
	v4l2_info(sd, "alt_mem_map8: %u\n", data->alt_mem_map8);
	v4l2_info(sd, "alt2_mem_map8: %u\n", data->alt2_mem_map8);
	v4l2_info(sd, "alt_mem_map10: %u\n", data->alt_mem_map10);
	v4l2_info(sd, "alt_mem_map12: %u\n", data->alt_mem_map12);

	if (priv->ops->log_phy_status) {
		ret = priv->ops->log_phy_status(priv, data, sd->name);
		if (ret)
			return ret;
	}

	return 0;
}

static int max_des_phy_registered(struct v4l2_subdev *sd)
{
	struct max_des_phy *phy = v4l2_get_subdevdata(sd);
	struct max_des_priv *priv = phy->priv;

	return max_des_register_v4l2(priv, sd->v4l2_dev);
}

static void max_des_phy_unregistered(struct v4l2_subdev *sd)
{
	struct max_des_phy *phy = v4l2_get_subdevdata(sd);
	struct max_des_priv *priv = phy->priv;

	max_des_unregister_v4l2(priv);
}

static const struct v4l2_subdev_core_ops max_des_phy_core_ops = {
	.log_status = max_des_phy_log_status,
};

static const struct v4l2_subdev_video_ops max_des_phy_video_ops = {
};

static const struct v4l2_subdev_pad_ops max_des_phy_pad_ops = {
	.get_fmt = v4l2_subdev_get_fmt,
};

static const struct v4l2_subdev_ops max_des_phy_subdev_ops = {
	.core = &max_des_phy_core_ops,
	.video = &max_des_phy_video_ops,
	.pad = &max_des_phy_pad_ops,
};

static const struct v4l2_subdev_internal_ops max_des_phy_internal_ops = {
	.registered = max_des_phy_registered,
	.unregistered = max_des_phy_unregistered,
};

int max_des_phy_register_v4l2_sd(struct max_des_phy *phy, bool attach_notifier)
{
	struct max_des_priv *priv = phy->priv;
	struct max_component *comp = &phy->comp;

	comp->sd_ops = &max_des_phy_subdev_ops;
	comp->internal_ops = &max_des_phy_internal_ops;
	comp->client = priv->client;
	comp->num_source_pads = 1;
	comp->num_sink_pads = 1;
	comp->prefix = priv->name;
	comp->name = "phy";
	comp->index = phy->data.index;

	if (attach_notifier) {
		comp->notifier_eps = priv->links_eps;
		comp->notifier_comps = priv->links_comps;
		comp->num_notifier_eps = priv->ops->num_links;
	}

	return max_component_register_v4l2_sd(comp);
}

void max_des_phy_unregister_v4l2_sd(struct max_des_phy *phy)
{
	struct max_component *comp = &phy->comp;

	max_component_unregister_v4l2_sd(comp);
}

static int max_des_phy_parse_src_dt_endpoint(struct max_des_priv *priv,
					     struct max_des_phy_data *data,
					     struct fwnode_handle *fwnode)
{
	struct v4l2_fwnode_endpoint v4l2_ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	struct v4l2_mbus_config_mipi_csi2 *mipi = &v4l2_ep.bus.mipi_csi2;
	struct fwnode_handle *ep;
	u64 link_frequency;
	unsigned int i;
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(fwnode, 0, 0, 0);
	if (!ep) {
		fwnode_handle_put(ep);
		return 0;
	}

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &v4l2_ep);
	fwnode_handle_put(ep);
	if (ret) {
		dev_err(priv->dev, "Could not parse v4l2 endpoint\n");
		return ret;
	}

	ret = 0;
	if (v4l2_ep.nr_of_link_frequencies == 0)
		link_frequency = MAX_DES_LINK_FREQUENCY_DEFAULT;
	else if (v4l2_ep.nr_of_link_frequencies == 1)
		link_frequency = v4l2_ep.link_frequencies[0];
	else
		ret = -EINVAL;

	v4l2_fwnode_endpoint_free(&v4l2_ep);

	if (ret) {
		dev_err(priv->dev, "PHY configured with invalid number of link frequencies\n");
		return -EINVAL;
	}

	if (link_frequency < MAX_DES_LINK_FREQUENCY_MIN ||
	    link_frequency > MAX_DES_LINK_FREQUENCY_MAX) {
		dev_err(priv->dev, "PHY configured with out of range link frequency\n");
		return -EINVAL;
	}

	for (i = 0; i < mipi->num_data_lanes; i++) {
		if (mipi->data_lanes[i] > mipi->num_data_lanes) {
			dev_err(priv->dev, "PHY configured with data lanes out of range\n");
			return -EINVAL;
		}
	}

	data->mipi = v4l2_ep.bus.mipi_csi2;
	data->link_frequency = link_frequency;

	return 0;
}

int max_des_phy_parse_dt(struct max_des_priv *priv, struct max_des_phy_data *data,
			 struct fwnode_handle *fwnode)
{
	data->alt_mem_map8 = fwnode_property_read_bool(fwnode, "maxim,alt-mem-map8");
	data->alt2_mem_map8 = fwnode_property_read_bool(fwnode, "maxim,alt2-mem-map8");
	data->alt_mem_map10 = fwnode_property_read_bool(fwnode, "maxim,alt-mem-map10");
	data->alt_mem_map12 = fwnode_property_read_bool(fwnode, "maxim,alt-mem-map12");

	return max_des_phy_parse_src_dt_endpoint(priv, data, fwnode);
}
