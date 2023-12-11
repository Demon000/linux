// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim GMSL2 Deserializer Driver
 *
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of_graph.h>
#include <media/v4l2-mc.h>

#include "max_des.h"
#include "max_serdes.h"

#if 0
static int __max_des_mipi_update(struct max_des_priv *priv)
{
	struct max_des *des = priv->des;
	struct max_des_subdev_priv *sd_priv;
	bool enable = 0;

	for_each_subdev(priv, sd_priv) {
		if (sd_priv->active) {
			enable = 1;
			break;
		}
	}

	if (enable == priv->active)
		return 0;

	priv->active = enable;

	return des->ops->mipi_enable(des, enable);
}
#endif

#if 0
static int max_des_ch_enable(struct max_des_subdev_priv *sd_priv, bool enable)
{
	struct max_des_priv *priv = sd_priv->priv;
	int ret = 0;

	mutex_lock(&priv->lock);

	if (sd_priv->active == enable)
		goto exit;

	sd_priv->active = enable;

	ret = __max_des_mipi_update(priv);

exit:
	mutex_unlock(&priv->lock);

	return ret;
}
#endif

#if 0
static int max_des_update_pipe_remaps(struct max_des_priv *priv,
				      struct max_des_pipe *pipe)
{
	struct max_des *des = priv->des;
	struct max_des_link *link = &des->links[pipe->link_id];
	struct max_des_subdev_priv *sd_priv;
	unsigned int i;

	pipe->num_remaps = 0;

	if (link->tunnel_mode)
		return 0;

	for_each_subdev(priv, sd_priv) {
		unsigned int num_remaps;

		if (sd_priv->pipe_id != pipe->index)
			continue;

		if (!sd_priv->fmt)
			continue;

		if (sd_priv->fmt->dt == MIPI_CSI2_DT_EMBEDDED_8B)
			num_remaps = 1;
		else
			num_remaps = 3;

		for (i = 0; i < num_remaps; i++) {
			struct max_des_dt_vc_remap *remap;
			unsigned int dt;

			if (pipe->num_remaps == MAX_DES_REMAPS_NUM) {
				dev_err(priv->dev, "Too many remaps\n");
				return -EINVAL;
			}

			remap = &pipe->remaps[pipe->num_remaps++];

			if (i == 0)
				dt = sd_priv->fmt->dt;
			else if (i == 1)
				dt = MIPI_CSI2_DT_FS;
			else
				dt = MIPI_CSI2_DT_FE;

			remap->from_dt = dt;
			remap->from_vc = sd_priv->src_vc_id;
			remap->to_dt = dt;
			remap->to_vc = sd_priv->dst_vc_id;
			remap->phy = sd_priv->phy_id;
		}
	}

	return des->ops->set_pipe_remaps(des, pipe);
}
#endif

static int max_des_init(struct max_des_priv *priv)
{
	struct max_des *des = priv->des;
	unsigned int i;
	int ret;

	ret = des->ops->init(des);
	if (ret)
		return ret;

	for (i = 0; i < des->ops->num_phys; i++) {
		struct max_des_phy *phy = &des->phys[i];

		if (phy->enabled) {
			ret = des->ops->init_phy(des, phy);
			if (ret)
				return ret;
		}

		ret = des->ops->set_phy_enable(des, phy, phy->enabled);
		if (ret)
			return ret;
	}

	for (i = 0; i < des->ops->num_pipes; i++) {
		struct max_des_pipe *pipe = &des->pipes[i];

		ret = des->ops->init_pipe(des, pipe);
		if (ret)
			return ret;

		/*
		 * Pipes will be enabled dynamically.
		 */
		ret = des->ops->set_pipe_enable(des, pipe, false);
		if (ret)
			return ret;
	}

	return 0;
}

static int max_des_post_init(struct max_des_priv *priv)
{
	struct max_des *des = priv->des;
	unsigned int i, mask = 0;
	int ret;

	for (i = 0; i < des->ops->num_links; i++) {
		struct max_des_link *link = &des->links[i];

		if (!link->enabled)
			continue;

		mask |= BIT(link->index);
	}

	ret = des->ops->select_links(des, mask);
	if (ret)
		return ret;

	if (des->ops->post_init) {
		ret = des->ops->post_init(des);
		if (ret)
			return ret;
	}

	return 0;
}

static int max_des_register_async_v4l2(struct max_des_priv *priv)
{
	struct max_des *des = priv->des;
	bool attach_notifier = true;
	struct max_component *comp;
	unsigned int i;
	int ret;

	for (i = 0; i < des->ops->num_phys; i++) {
		struct max_des_phy *phy = &des->phys[i];

		if (!phy->enabled)
			continue;

		comp = &priv->phys_comp[i];

		ret = max_des_phy_register_v4l2_sd(priv, phy, comp, attach_notifier);
		if (ret)
			return ret;

		attach_notifier = false;
	}

	return 0;
}

static void max_des_unregister_async_v4l2(struct max_des_priv *priv)
{
	struct max_des *des = priv->des;
	struct max_component *comp;
	unsigned int i;

	for (i = 0; i < des->ops->num_phys; i++) {
		struct max_des_phy *phy = &des->phys[i];

		if (!phy->enabled)
			continue;

		comp = &priv->phys_comp[i];

		max_des_phy_unregister_v4l2_sd(priv, comp);
	}
}

static int max_des_register_pipe_phy_xbar(struct max_des_priv *priv,
					  struct v4l2_device *v4l2_dev)
{
	struct max_component *comp = &priv->pipe_phy_xbar_comp;
	struct max_component *other_comp;
	struct max_des *des = priv->des;
	unsigned int offset;
	unsigned int i;
	int ret;

	ret = max_des_pipe_phy_xbar_register_v4l2_sd(priv, comp, v4l2_dev);
	if (ret)
		return ret;

	/* Create xbar->PHY links. */
	offset = 0;
	for (i = 0; i < des->ops->num_phys; i++) {
		struct max_des_phy *phy = &des->phys[i];

		if (!phy->enabled) {
			offset++;
			continue;
		}

		other_comp = &priv->phys_comp[i];

		ret = max_components_link(comp, offset, other_comp, 0);
		if (ret < 0)
			return ret;

		offset += ret;
	}

	/* Create pipe->xbar links. */
	offset = 0;
	for (i = 0; i < des->ops->num_pipes; i++) {
		other_comp = &priv->pipes_comp[i];

		ret = max_components_link(other_comp, 0, comp, offset);
		if (ret < 0)
			return ret;

		offset += ret;
	}

	return 0;
}

static int max_des_register_link_pipe_xbar(struct max_des_priv *priv,
					   struct v4l2_device *v4l2_dev)
{
	struct max_component *comp = &priv->link_pipe_xbar_comp;
	struct max_component *other_comp;
	struct max_des *des = priv->des;
	unsigned int offset;
	unsigned int i;
	int ret;

	ret = max_des_link_pipe_xbar_register_v4l2_sd(priv, comp, v4l2_dev);
	if (ret)
		return ret;

	/* Create xbar->pipe links. */
	offset = 0;
	for (i = 0; i < des->ops->num_pipes; i++) {
		other_comp = &priv->pipes_comp[i];

		ret = max_components_link(comp, offset, other_comp, 0);
		if (ret < 0)
			return ret;

		offset += ret;
	}

	/* Create link->xbar links. */
	offset = 0;
	for (i = 0; i < des->ops->num_links; i++) {
		struct max_des_link *link = &des->links[i];

		if (!link->enabled) {
			offset += des->num_streams_per_link;
			continue;
		}

		other_comp = &priv->links_comp[i];

		ret = max_components_link(other_comp, 0, comp, offset);
		if (ret < 0)
			return ret;

		offset += ret;
	}

	return 0;
}

int max_des_register_v4l2(struct max_des_priv *priv, struct v4l2_device *v4l2_dev)
{
	struct max_des *des = priv->des;
	struct max_component *comp;
	unsigned int i;
	int ret;

	priv->num_bound_phys++;

	if (priv->num_bound_phys != des->num_enabled_phys)
		return 0;

	for (i = 0; i < des->ops->num_pipes; i++) {
		struct max_des_pipe *pipe = &des->pipes[i];

		comp = &priv->pipes_comp[i];

		ret = max_des_pipe_register_v4l2_sd(priv, pipe, comp, v4l2_dev);
		if (ret)
			return ret;
	}

	for (i = 0; i < des->ops->num_links; i++) {
		struct max_des_link *link = &des->links[i];

		if (!link->enabled)
			continue;

		comp = &priv->links_comp[i];

		ret = max_des_link_register_v4l2_sd(priv, link, comp, v4l2_dev);
		if (ret)
			return ret;
	}

	ret = max_des_register_pipe_phy_xbar(priv, v4l2_dev);
	if (ret)
		return ret;

	ret = max_des_register_link_pipe_xbar(priv, v4l2_dev);
	if (ret)
		return ret;

	return 0;
}

void max_des_unregister_v4l2(struct max_des_priv *priv)
{
	struct max_des *des = priv->des;
	struct max_component *comp;
	unsigned int i;

	priv->num_bound_phys--;

	if (priv->num_bound_phys != 0)
		return;

	max_des_link_pipe_xbar_unregister_v4l2_sd(priv, &priv->link_pipe_xbar_comp);

	max_des_pipe_phy_xbar_unregister_v4l2_sd(priv, &priv->pipe_phy_xbar_comp);

	for (i = 0; i < des->ops->num_links; i++) {
		struct max_des_link *link = &des->links[i];

		if (!link->enabled)
			continue;

		comp = &priv->links_comp[i];

		max_des_link_unregister_v4l2_sd(priv, comp);
	}

	for (i = 0; i < des->ops->num_pipes; i++) {
		comp = &priv->pipes_comp[i];

		max_des_pipe_unregister_v4l2_sd(priv, comp);
	}
}

static int max_des_find_phys_config(struct max_des_priv *priv)
{
	struct max_des *des = priv->des;
	const struct max_serdes_phy_configs *configs = des->ops->phys_configs.configs;
	unsigned int num_phys_configs = des->ops->phys_configs.num_configs;
	struct max_des_phy *phy;
	unsigned int i, j;

	for (i = 0; i < num_phys_configs; i++) {
		bool matching = true;

		for (j = 0; j < des->ops->num_phys; j++) {
			phy = &des->phys[j];

			if (!phy->enabled)
				continue;

			if (phy->mipi.num_data_lanes == configs[i].lanes[j] &&
			    phy->mipi.clock_lane == configs[i].clock_lane[j])
				continue;

			matching = false;

			break;
		}

		if (matching)
			break;
	}

	if (i == num_phys_configs) {
		dev_err(priv->dev, "Invalid lane configuration\n");
		return -EINVAL;
	}

	des->phys_config = i;

	return 0;
}

static int max_des_parse_dt(struct max_des_priv *priv)
{
	struct max_des *des = priv->des;
	struct fwnode_handle *fwnode = dev_fwnode(priv->dev);
	struct max_component *comp;
	struct max_des_link *link;
	struct max_des_pipe *pipe;
	struct max_des_phy *phy;
	const char *label = NULL;
	unsigned int i;
	u32 index;
	u32 val;
	int ret;

	fwnode_property_read_string(fwnode, "label", &label);
	max_set_priv_name(des->name, label, priv->client);

	val = device_property_read_bool(priv->dev, "maxim,pipe-stream-autoselect");
	if (val && !des->ops->supports_pipe_stream_autoselect) {
		dev_err(priv->dev, "Pipe stream autoselect is not supported\n");
		return -EINVAL;
	}
	des->pipe_stream_autoselect = val;
	des->num_streams_per_link = des->pipe_stream_autoselect ? 1 : MAX_SERDES_STREAMS_NUM;

	for (i = 0; i < des->ops->num_phys; i++) {
		phy = &des->phys[i];
		phy->index = i;
	}

	for (i = 0; i < des->ops->num_pipes; i++) {
		pipe = &des->pipes[i];
		pipe->index = i;
		pipe->phy_id = i % des->ops->num_phys;
		pipe->stream_id = i % MAX_SERDES_STREAMS_NUM;
		pipe->link_id = i;
	}

	for (i = 0; i < des->ops->num_links; i++) {
		link = &des->links[i];
		link->index = i;
	}

	device_for_each_child_node(priv->dev, fwnode) {
		struct device_node *of_node = to_of_node(fwnode);

		if (!of_node_name_eq(of_node, "phy"))
			continue;

		ret = fwnode_property_read_u32(fwnode, "reg", &index);
		if (ret) {
			dev_err(priv->dev, "Failed to read reg: %d\n", ret);
			continue;
		}

		if (index >= des->ops->num_phys) {
			dev_err(priv->dev, "Invalid PHY %u\n", index);
			fwnode_handle_put(fwnode);
			return -EINVAL;
		}

		phy = &des->phys[index];
		phy->enabled = true;

		comp = &priv->phys_comp[index];
		comp->fwnode = fwnode;

		des->enabled_phys[des->num_enabled_phys++] = phy;

		ret = max_des_phy_parse_dt(priv, phy, fwnode);
		if (ret) {
			fwnode_handle_put(fwnode);
			return ret;
		}
	}

	device_for_each_child_node(priv->dev, fwnode) {
		struct device_node *of_node = to_of_node(fwnode);
		struct fwnode_handle *ep;

		if (!of_node_name_eq(of_node, "link"))
			continue;

		ret = fwnode_property_read_u32(fwnode, "reg", &index);
		if (ret) {
			dev_err(priv->dev, "Failed to read reg: %d\n", ret);
			continue;
		}

		if (index >= des->ops->num_links) {
			dev_err(priv->dev, "Invalid link %u\n", index);
			fwnode_handle_put(fwnode);
			return -EINVAL;
		}

		ep = fwnode_graph_get_endpoint_by_id(fwnode, 0, 0, 0);
		if (!ep) {
			dev_err(priv->dev, "Not connected to subdevice\n");
			fwnode_handle_put(fwnode);
			return -EINVAL;
		}

		link = &des->links[index];
		link->enabled = true;

		comp = &priv->links_comp[index];
		comp->fwnode = fwnode;

		priv->links_eps[index] = ep;
		priv->links_comps[index] = comp;

		ret = max_des_link_parse_dt(priv, link, fwnode);
		if (ret) {
			fwnode_handle_put(fwnode);
			return ret;
		}
	}

	device_for_each_child_node(priv->dev, fwnode) {
		struct device_node *of_node = to_of_node(fwnode);

		if (!of_node_name_eq(of_node, "pipe"))
			continue;

		ret = fwnode_property_read_u32(fwnode, "reg", &index);
		if (ret) {
			dev_err(priv->dev, "Failed to read reg: %d\n", ret);
			continue;
		}

		if (index >= des->ops->num_pipes) {
			dev_err(priv->dev, "Invalid pipe %u\n", index);
			fwnode_handle_put(fwnode);
			return -EINVAL;
		}

		pipe = &des->pipes[index];

		ret = max_des_pipe_parse_dt(priv, pipe, fwnode);
		if (ret) {
			fwnode_handle_put(fwnode);
			return ret;
		}
	}

	ret = max_des_find_phys_config(priv);
	if (ret)
		return ret;

	return 0;
}

static int max_des_allocate(struct max_des_priv *priv)
{
	struct max_des *des = priv->des;
	unsigned int i;

	des->phys = devm_kcalloc(priv->dev, des->ops->num_phys,
				 sizeof(*des->phys), GFP_KERNEL);
	if (!des->phys)
		return -ENOMEM;

	des->pipes = devm_kcalloc(priv->dev, des->ops->num_pipes,
				  sizeof(*des->pipes), GFP_KERNEL);
	if (!des->pipes)
		return -ENOMEM;

	des->links = devm_kcalloc(priv->dev, des->ops->num_links,
				  sizeof(*des->links), GFP_KERNEL);
	if (!des->links)
		return -ENOMEM;

	priv->links_eps = devm_kcalloc(priv->dev, des->ops->num_links,
				       sizeof(*priv->links_eps), GFP_KERNEL);
	if (!priv->links_eps)
		return -ENOMEM;

	priv->links_comps = devm_kcalloc(priv->dev, des->ops->num_links,
					 sizeof(*priv->links_comps), GFP_KERNEL);
	if (!priv->links_comps)
		return -ENOMEM;

	priv->phys_comp = devm_kcalloc(priv->dev, des->ops->num_phys,
				       sizeof(*priv->phys_comp), GFP_KERNEL);
	if (!priv->phys_comp)
		return -ENOMEM;

	priv->pipes_comp = devm_kcalloc(priv->dev, des->ops->num_pipes,
					sizeof(*priv->pipes_comp), GFP_KERNEL);
	if (!priv->pipes_comp)
		return -ENOMEM;

	priv->links_comp = devm_kcalloc(priv->dev, des->ops->num_links,
					sizeof(*priv->links_comp), GFP_KERNEL);
	if (!priv->links_comp)
		return -ENOMEM;

	return 0;
}

int max_des_probe(struct i2c_client *client, struct max_des *des)
{
	struct device *dev = &client->dev;
	struct max_des_priv *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;
	priv->dev = dev;
	priv->des = des;
	des->priv = priv;

	mutex_init(&priv->lock);

	ret = max_des_allocate(priv);
	if (ret)
		return ret;

	ret = max_des_parse_dt(priv);
	if (ret)
		return ret;

	ret = max_des_init(priv);
	if (ret)
		return ret;

	ret = max_des_i2c_atr_init(priv);
	if (ret)
		return ret;

	ret = max_des_post_init(priv);
	if (ret)
		return ret;

	return max_des_register_async_v4l2(priv);
}
EXPORT_SYMBOL_GPL(max_des_probe);

int max_des_remove(struct max_des *des)
{
	struct max_des_priv *priv = des->priv;

	max_des_unregister_async_v4l2(priv);

	max_des_i2c_atr_deinit(priv);

	return 0;
}
EXPORT_SYMBOL_GPL(max_des_remove);

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(I2C_ATR);
