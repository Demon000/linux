// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim GMSL2 Deserializer Driver
 *
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include "max_des_priv.h"

#define MAX_DES_LINK_FREQUENCY_MIN		100000000ull
#define MAX_DES_LINK_FREQUENCY_DEFAULT		750000000ull
#define MAX_DES_LINK_FREQUENCY_MAX		1250000000ull

#define V4L2_CID_ALT_MEM_MAP_8			(V4L2_CID_USER_BASE | 0x1001)
#define V4L2_CID_ALT2_MEM_MAP_8			(V4L2_CID_USER_BASE | 0x1002)
#define V4L2_CID_ALT_MEM_MAP_10			(V4L2_CID_USER_BASE | 0x1003)
#define V4L2_CID_ALT_MEM_MAP_12			(V4L2_CID_USER_BASE | 0x1004)

static int max_des_phy_log_status(struct v4l2_subdev *sd)
{
	struct max_component *comp = sd_max_component(sd);
	struct max_des_priv *priv = comp->priv;
	struct max_des *des = priv->des;
	struct max_des_phy *phy = &des->phys[comp->index];
	int ret;

	v4l2_info(sd, "index: %u\n", phy->index);
	v4l2_info(sd, "enabled: %u\n", phy->enabled);
	v4l2_info(sd, "link_frequency: %llu\n", phy->link_frequency);
	v4l2_info(sd, "num_data_lanes: %u\n", phy->mipi.num_data_lanes);
	v4l2_info(sd, "clock_lane: %u\n", phy->mipi.clock_lane);
	v4l2_info(sd, "alt_mem_map8: %u\n", phy->alt_mem_map8);
	v4l2_info(sd, "alt2_mem_map8: %u\n", phy->alt2_mem_map8);
	v4l2_info(sd, "alt_mem_map10: %u\n", phy->alt_mem_map10);
	v4l2_info(sd, "alt_mem_map12: %u\n", phy->alt_mem_map12);

	if (des->ops->log_phy_status) {
		ret = des->ops->log_phy_status(des, phy, sd->name);
		if (ret)
			return ret;
	}

	return 0;
}

static int max_des_phy_registered(struct v4l2_subdev *sd)
{
	struct max_component *comp = sd_max_component(sd);
	struct max_des_priv *priv = comp->priv;

	return max_des_register_v4l2(priv, sd->v4l2_dev);
}

static void max_des_phy_unregistered(struct v4l2_subdev *sd)
{
	struct max_component *comp = sd_max_component(sd);
	struct max_des_priv *priv = comp->priv;

	max_des_unregister_v4l2(priv);
}

static int max_des_phy_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct max_component *comp = ctrl_max_component(ctrl);
	struct max_des_priv *priv = comp->priv;
	struct max_des *des = priv->des;
	struct max_des_phy *phy = &des->phys[comp->index];
	bool enable = ctrl->val;
	int ret;

	switch (ctrl->id) {
	case V4L2_CID_ALT_MEM_MAP_8:
		ret = des->ops->set_phy_alt_mem_map8(des, phy, enable);
		if (ret)
			return ret;

		phy->alt_mem_map8 = enable;

		return 0;
	case V4L2_CID_ALT2_MEM_MAP_8:
		ret = des->ops->set_phy_alt2_mem_map8(des, phy, ctrl->val);
		if (ret)
			return ret;

		phy->alt2_mem_map8 = enable;

		return 0;
	case V4L2_CID_ALT_MEM_MAP_10:
		ret = des->ops->set_phy_alt_mem_map10(des, phy, ctrl->val);
		if (ret)
			return ret;

		phy->alt_mem_map10 = enable;

		return 0;
	case V4L2_CID_ALT_MEM_MAP_12:
		ret = des->ops->set_phy_alt_mem_map12(des, phy, ctrl->val);
		if (ret)
			return ret;

		phy->alt_mem_map10 = enable;

		return 0;
	default:
		return -EINVAL;
	}
}

static const struct v4l2_subdev_core_ops max_des_phy_core_ops = {
	.log_status = max_des_phy_log_status,
};

static const struct v4l2_subdev_pad_ops max_des_phy_pad_ops = {
	.init_cfg = max_component_init_cfg,
	.set_routing = max_component_set_validate_routing,
	.enable_streams = max_component_streams_enable,
	.disable_streams = max_component_streams_disable,
	.get_fmt = v4l2_subdev_get_fmt,
	.get_frame_desc = max_component_get_frame_desc,
};

static const struct v4l2_subdev_ops max_des_phy_subdev_ops = {
	.core = &max_des_phy_core_ops,
	.pad = &max_des_phy_pad_ops,
};

static const struct v4l2_subdev_internal_ops max_des_phy_internal_ops = {
	.registered = max_des_phy_registered,
	.unregistered = max_des_phy_unregistered,
};

static const struct media_entity_operations max_des_phy_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
	.has_pad_interdep = v4l2_subdev_has_pad_interdep,
};

static const struct v4l2_ctrl_ops max_des_phy_ctrl_ops = {
	.s_ctrl = max_des_phy_set_ctrl,
};

#define CTRL_BOOL(_id, _name) \
	{						\
		.ops = &max_des_phy_ctrl_ops,		\
		.id = (_id),				\
		.name = (_name),			\
		.type = V4L2_CTRL_TYPE_BOOLEAN,		\
		.min = 0,				\
		.max = 1,				\
		.step = 1,				\
		.def = 0,				\
	}

static struct v4l2_ctrl_config max_des_phy_ctrls[] = {
	CTRL_BOOL(V4L2_CID_ALT_MEM_MAP_8, "Alternate mapping for 8-bit DT"),
	CTRL_BOOL(V4L2_CID_ALT2_MEM_MAP_8, "Second alternate mapping for 8-bit DT"),
	CTRL_BOOL(V4L2_CID_ALT_MEM_MAP_10, "Alternate mapping for 10-bit DT"),
	CTRL_BOOL(V4L2_CID_ALT_MEM_MAP_12, "Alternate mapping for 12-bit DT"),
};

int max_des_phy_register_v4l2_sd(struct max_des_priv *priv,
				 struct max_des_phy *phy,
				 struct max_component *comp,
				 bool attach_notifier)
{
	struct max_des *des = priv->des;
	unsigned int i;
	int ret;

	comp->priv = priv;
	comp->sd_ops = &max_des_phy_subdev_ops;
	comp->mc_ops = &max_des_phy_entity_ops;
	comp->internal_ops = &max_des_phy_internal_ops;
	comp->dev = priv->dev;
	comp->num_source_pads = 1;
	comp->num_sink_pads = 1;
	comp->prefix = priv->name;
	comp->name = "phy";
	comp->index = phy->index;
	comp->routing_disallow = V4L2_SUBDEV_ROUTING_ONLY_1_TO_1 |
				 V4L2_SUBDEV_ROUTING_NO_STREAM_MIX;

	if (attach_notifier) {
		comp->notifier_eps = priv->links_eps;
		comp->notifier_comps = priv->links_comps;
		comp->num_notifier_eps = des->ops->num_links;
	}

	ret = v4l2_ctrl_handler_init(&comp->ctrl_handler, ARRAY_SIZE(max_des_phy_ctrls));
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(max_des_phy_ctrls); i++)
		v4l2_ctrl_new_custom(&comp->ctrl_handler,
				     &max_des_phy_ctrls[i], NULL);

	comp->sd.ctrl_handler = &comp->ctrl_handler;
	v4l2_ctrl_handler_setup(&comp->ctrl_handler);

	return max_component_register_v4l2_sd(comp);
}

void max_des_phy_unregister_v4l2_sd(struct max_des_priv *priv,
				    struct max_component *comp)
{
	max_component_unregister_v4l2_sd(comp);
}

static int max_des_phy_parse_src_dt_endpoint(struct max_des_priv *priv,
					     struct max_des_phy *phy,
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

	phy->mipi = v4l2_ep.bus.mipi_csi2;
	phy->link_frequency = link_frequency;

	return 0;
}

int max_des_phy_parse_dt(struct max_des_priv *priv, struct max_des_phy *phy,
			 struct fwnode_handle *fwnode)
{
	return max_des_phy_parse_src_dt_endpoint(priv, phy, fwnode);
}
