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

static int __max_des_mipi_update(struct max_des_priv *priv)
{
#if 0
	struct max_des_subdev_priv *sd_priv;
#endif
	bool enable = 0;

#if 0
	for_each_subdev(priv, sd_priv) {
		if (sd_priv->active) {
			enable = 1;
			break;
		}
	}
#endif

	if (enable == priv->active)
		return 0;

	priv->active = enable;

	return priv->ops->mipi_enable(priv, enable);
}

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

static int max_des_update_pipe_remaps(struct max_des_priv *priv,
				      struct max_des_pipe *pipe)
{
	struct max_des_link *link = &priv->links[pipe->data.link_id];
#if 0
	struct max_des_subdev_priv *sd_priv;
	unsigned int i;
#endif

	pipe->data.num_remaps = 0;

	if (link->data.tunnel_mode)
		return 0;

#if 0
	for_each_subdev(priv, sd_priv) {
		unsigned int num_remaps;

		if (sd_priv->pipe_id != pipe->index)
			continue;

		if (!sd_priv->fmt)
			continue;

		if (sd_priv->fmt->dt == MAX_DT_EMB8)
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
				dt = MAX_DT_FS;
			else
				dt = MAX_DT_FE;

			remap->from_dt = dt;
			remap->from_vc = sd_priv->src_vc_id;
			remap->to_dt = dt;
			remap->to_vc = sd_priv->dst_vc_id;
			remap->phy = sd_priv->phy_id;
		}
	}
#endif

	return priv->ops->update_pipe_remaps(priv, &pipe->data);
}
static int max_des_init(struct max_des_priv *priv)
{
	unsigned int i;
	int ret;

	ret = priv->ops->init(priv);
	if (ret)
		return ret;

	for (i = 0; i < priv->ops->num_phys; i++) {
		struct max_des_phy *phy = &priv->phys[i];

		if (!phy->data.enabled)
			continue;

		ret = priv->ops->init_phy(priv, &phy->data);
		if (ret)
			return ret;
	}

	for (i = 0; i < priv->ops->num_pipes; i++) {
		struct max_des_pipe *pipe = &priv->pipes[i];

		ret = priv->ops->init_pipe(priv, &pipe->data);
		if (ret)
			return ret;
	}

	return 0;
}

static int max_des_post_init(struct max_des_priv *priv)
{
	unsigned int i, mask = 0;
	int ret;

	for (i = 0; i < priv->ops->num_links; i++) {
		struct max_des_link *link = &priv->links[i];
		struct max_des_link_data *data = &link->data;

		if (!data->enabled)
			continue;

		mask |= BIT(data->index);
	}

	ret = priv->ops->select_links(priv, mask);
	if (ret)
		return ret;

	if (priv->ops->post_init) {
		ret = priv->ops->post_init(priv);
		if (ret)
			return ret;
	}

	return 0;
}

static int max_des_register_async_v4l2(struct max_des_priv *priv)
{
	bool attach_notifier = true;
	unsigned int i;
	int ret;

	for (i = 0; i < priv->ops->num_phys; i++) {
		struct max_des_phy *phy = &priv->phys[i];

		if (!phy->data.enabled)
			continue;

		ret = max_des_phy_register_v4l2_sd(phy, attach_notifier);
		if (ret)
			return ret;

		attach_notifier = false;
	}

	return 0;
}

static void max_des_unregister_async_v4l2(struct max_des_priv *priv)
{
	unsigned int i;

	for (i = 0; i < priv->ops->num_phys; i++) {
		struct max_des_phy *phy = &priv->phys[i];

		max_des_phy_unregister_v4l2_sd(phy);
	}
}

static int max_des_register_pipe_phy_xbar(struct max_des_priv *priv,
					  struct v4l2_device *v4l2_dev)
{
	struct max_des_pipe_phy_xbar *xbar = &priv->pipe_phy_xbar;
	unsigned int offset;
	unsigned int i;
	int ret;

	ret = max_des_pipe_phy_xbar_register_v4l2_sd(xbar, v4l2_dev);
	if (ret)
		return ret;

	/* Create xbar->PHY links. */
	offset = 0;
	for (i = 0; i < priv->ops->num_phys; i++) {
		struct max_des_phy *phy = &priv->phys[i];

		ret = max_components_link(&xbar->comp, offset, &phy->comp, 0, phy->data.enabled);
		if (ret < 0)
			return ret;

		offset += ret;
	}

	/* Create pipe->xbar links. */
	offset = 0;
	for (i = 0; i < priv->ops->num_pipes; i++) {
		struct max_des_pipe *pipe = &priv->pipes[i];

		ret = max_components_link(&pipe->comp, 0, &xbar->comp, offset, true);
		if (ret < 0)
			return ret;

		offset += ret;
	}

	return 0;
}

static int max_des_register_link_pipe_xbar(struct max_des_priv *priv,
					   struct v4l2_device *v4l2_dev)
{
	struct max_des_link_pipe_xbar *xbar = &priv->link_pipe_xbar;
	unsigned int offset;
	unsigned int i;
	int ret;

	ret = max_des_link_pipe_xbar_register_v4l2_sd(xbar, v4l2_dev);
	if (ret)
		return ret;

	/* Create xbar->pipe links. */
	offset = 0;
	for (i = 0; i < priv->ops->num_pipes; i++) {
		struct max_des_pipe *pipe = &priv->pipes[i];

		ret = max_components_link(&xbar->comp, offset, &pipe->comp, 0, true);
		if (ret < 0)
			return ret;

		offset += ret;
	}

	/* Create link->xbar links. */
	offset = 0;
	for (i = 0; i < priv->ops->num_links; i++) {
		struct max_des_link *link = &priv->links[i];

		ret = max_components_link(&link->comp, 0, &xbar->comp, offset,
					  link->data.enabled);
		if (ret < 0)
			return ret;

		offset += ret;
	}

	return 0;
}

int max_des_register_v4l2(struct max_des_priv *priv, struct v4l2_device *v4l2_dev)
{
	unsigned int i;
	int ret;

	priv->num_bound_phys++;

	if (priv->num_bound_phys != priv->num_enabled_phys)
		return 0;

	for (i = 0; i < priv->ops->num_pipes; i++) {
		struct max_des_pipe *pipe = &priv->pipes[i];

		ret = max_des_pipe_register_v4l2_sd(pipe, v4l2_dev);
		if (ret)
			return ret;
	}

	for (i = 0; i < priv->ops->num_links; i++) {
		struct max_des_link *link = &priv->links[i];

		if (!link->data.enabled)
			continue;

		ret = max_des_link_register_v4l2_sd(link, v4l2_dev);
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
	unsigned int i;

	priv->num_bound_phys--;

	if (priv->num_bound_phys != 0)
		return;

	max_des_link_pipe_xbar_unregister_v4l2_sd(&priv->link_pipe_xbar);

	max_des_pipe_phy_xbar_unregister_v4l2_sd(&priv->pipe_phy_xbar);

	for (i = 0; i < priv->ops->num_links; i++) {
		struct max_des_link *link = &priv->links[i];

		if (!link->data.enabled)
			continue;

		max_des_link_unregister_v4l2_sd(link);
	}

	for (i = 0; i < priv->ops->num_pipes; i++) {
		struct max_des_pipe *pipe = &priv->pipes[i];

		max_des_pipe_unregister_v4l2_sd(pipe);
	}
}

static int max_des_parse_dt(struct max_des_priv *priv)
{
	struct fwnode_handle *fwnode = dev_fwnode(priv->dev);
	struct max_des_link *link;
	struct max_des_pipe *pipe;
	struct max_des_phy *phy;
	const char *label = NULL;
	unsigned int i;
	u32 index;
	u32 val;
	int ret;

	fwnode_property_read_string(fwnode, "label", &label);
	max_set_priv_name(priv->name, label, priv->client);

	val = device_property_read_bool(priv->dev, "maxim,pipe-stream-autoselect");
	if (val && !priv->ops->supports_pipe_stream_autoselect) {
		dev_err(priv->dev, "Pipe stream autoselect is not supported\n");
		return -EINVAL;
	}
	priv->pipe_stream_autoselect = val;
	priv->num_streams_per_link = priv->pipe_stream_autoselect ? 1 : MAX_SERDES_STREAMS_NUM;

	for (i = 0; i < priv->ops->num_phys; i++) {
		phy = &priv->phys[i];
		phy->priv = priv;
		phy->data.index = i;
	}

	priv->pipe_phy_xbar.priv = priv;

	for (i = 0; i < priv->ops->num_pipes; i++) {
		pipe = &priv->pipes[i];
		pipe->priv = priv;
		pipe->data.index = i;
		pipe->data.phy_id = i % priv->ops->num_phys;
		pipe->data.stream_id = i % MAX_SERDES_STREAMS_NUM;
		pipe->data.link_id = i;
	}

	priv->link_pipe_xbar.priv = priv;

	for (i = 0; i < priv->ops->num_links; i++) {
		link = &priv->links[i];
		link->priv = priv;
		link->data.index = i;
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

		if (index >= priv->ops->num_phys) {
			dev_err(priv->dev, "Invalid PHY %u\n", index);
			fwnode_handle_put(fwnode);
			return -EINVAL;
		}

		phy = &priv->phys[index];
		phy->comp.fwnode = fwnode;
		phy->data.enabled = true;
		priv->num_enabled_phys++;

		ret = max_des_phy_parse_dt(priv, &phy->data, fwnode);
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

		if (index >= priv->ops->num_links) {
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

		link = &priv->links[index];
		link->comp.fwnode = fwnode;
		link->data.enabled = true;
		priv->links_eps[index] = ep;
		priv->links_comps[index] = &link->comp;

		ret = max_des_link_parse_dt(priv, &link->data, fwnode);
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

		if (index >= priv->ops->num_pipes) {
			dev_err(priv->dev, "Invalid pipe %u\n", index);
			fwnode_handle_put(fwnode);
			return -EINVAL;
		}

		pipe = &priv->pipes[index];

		ret = max_des_pipe_parse_dt(priv, &pipe->data, fwnode);
		if (ret) {
			fwnode_handle_put(fwnode);
			return ret;
		}
	}

	return 0;
}

static int max_des_allocate(struct max_des_priv *priv)
{
	priv->phys = devm_kcalloc(priv->dev, priv->ops->num_phys,
				  sizeof(*priv->phys), GFP_KERNEL);
	if (!priv->phys)
		return -ENOMEM;

	priv->pipes = devm_kcalloc(priv->dev, priv->ops->num_pipes,
				   sizeof(*priv->pipes), GFP_KERNEL);
	if (!priv->pipes)
		return -ENOMEM;

	priv->links = devm_kcalloc(priv->dev, priv->ops->num_links,
				   sizeof(*priv->links), GFP_KERNEL);
	if (!priv->links)
		return -ENOMEM;

	priv->links_eps = devm_kcalloc(priv->dev, priv->ops->num_links,
				       sizeof(*priv->links_eps), GFP_KERNEL);
	if (!priv->links_eps)
		return -ENOMEM;

	priv->links_comps = devm_kcalloc(priv->dev, priv->ops->num_links,
					 sizeof(*priv->links_comps), GFP_KERNEL);
	if (!priv->links_comps)
		return -ENOMEM;

	return 0;
}

int max_des_probe(struct max_des_priv *priv)
{
	int ret;

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

int max_des_remove(struct max_des_priv *priv)
{
	max_des_unregister_async_v4l2(priv);

	max_des_i2c_atr_deinit(priv);

	return 0;
}
EXPORT_SYMBOL_GPL(max_des_remove);

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(I2C_ATR);
