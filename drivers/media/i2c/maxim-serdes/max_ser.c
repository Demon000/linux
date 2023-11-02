// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim GMSL2 Serializer Driver
 *
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include <linux/delay.h>
#include <linux/module.h>

#include "max_ser.h"
#include "max_serdes.h"

#define debug pr_err("%s:%u\n", __func__, __LINE__);

static int max_ser_update_pipe_dts(struct max_ser_priv *priv,
				   struct max_ser_pipe *pipe)
{
#if 0
	struct max_ser_subdev_priv *sd_priv;
#endif

	pipe->data.num_dts = 0;

	if (priv->tunnel_mode)
		return 0;

#if 0
	for_each_subdev(priv, sd_priv) {
		if (sd_priv->pipe_id != pipe->index)
			continue;

		if (pipe->num_dts == priv->ops->num_dts_per_pipe) {
			dev_err(priv->dev, "Too many data types per pipe\n");
			return -EINVAL;
		}

		if (!sd_priv->fmt)
			continue;

		/* TODO: optimize by checking for existing filters. */
		pipe->dts[pipe->num_dts++] = sd_priv->fmt->dt;
	}
#endif

	return priv->ops->update_pipe_dts(priv, &pipe->data);
}

static int max_ser_update_pipe_vcs(struct max_ser_priv *priv,
				   struct max_ser_pipe *pipe)
{
#if 0
	struct max_ser_subdev_priv *sd_priv;
#endif

	pipe->data.vcs = 0;

	if (priv->tunnel_mode)
		return 0;

#if 0
	for_each_subdev(priv, sd_priv) {
		if (sd_priv->pipe_id != pipe->index)
			continue;

		pipe->vcs |= BIT(sd_priv->vc_id);
	}
#endif

	return priv->ops->update_pipe_vcs(priv, &pipe->data);
}

static int max_ser_update_pipe_active(struct max_ser_priv *priv,
				      struct max_ser_pipe *pipe)
{
#if 0
	struct max_ser_subdev_priv *sd_priv;
#endif
	bool enable = 0;

#if 0
	for_each_subdev(priv, sd_priv) {
		if (sd_priv->pipe_id == pipe->index && sd_priv->active) {
			enable = 1;
			break;
		}
	}
#endif

	if (enable == pipe->data.active)
		return 0;

	pipe->data.active = enable;

	return priv->ops->set_pipe_enable(priv, &pipe->data, enable);
}

static int max_ser_init(struct max_ser_priv *priv)
{
	unsigned int i;
	int ret;

	ret = priv->ops->init(priv);
	if (ret)
		return ret;

	for (i = 0; i < priv->ops->num_phys; i++) {
		struct max_ser_phy *phy = &priv->phys[i];

		if (!phy->data.enabled)
			continue;

		ret = priv->ops->init_phy(priv, &phy->data);
		if (ret)
			return ret;
	}

	for (i = 0; i < priv->ops->num_pipes; i++) {
		struct max_ser_pipe *pipe = &priv->pipes[i];

		ret = priv->ops->init_pipe(priv, &pipe->data);
		if (ret)
			return ret;

		ret = max_ser_update_pipe_vcs(priv, pipe);
		if (ret)
			return ret;

		ret = max_ser_update_pipe_dts(priv, pipe);
		if (ret)
			return ret;
	}

	ret = priv->ops->post_init(priv);
	if (ret)
		return ret;

	return 0;
}

static int max_ser_register_async_v4l2(struct max_ser_priv *priv)
{
	struct max_ser_link *link = &priv->links[0];
	int ret;

	ret = max_ser_link_register_v4l2_sd(link);
	if (ret)
		return ret;

	return 0;
}

static void max_ser_unregister_async_v4l2(struct max_ser_priv *priv)
{
	struct max_ser_link *link = &priv->links[0];

	max_ser_link_unregister_v4l2_sd(link);
}

static int max_ser_register_phy_pipe_xbar(struct max_ser_priv *priv,
					  struct v4l2_device *v4l2_dev)
{
	struct max_ser_phy_pipe_xbar *xbar = &priv->phy_pipe_xbar;
	unsigned int offset;
	unsigned int i;
	int ret;

	ret = max_ser_phy_pipe_xbar_register_v4l2_sd(xbar, v4l2_dev);
	if (ret)
		return ret;

	/* Create xbar->pipe links. */
	offset = 0;
	for (i = 0; i < priv->ops->num_pipes; i++) {
		struct max_ser_pipe *pipe = &priv->pipes[i];

		ret = max_components_link(&xbar->comp, offset, &pipe->comp, 0, true);
		if (ret < 0)
			return ret;

		offset += ret;
	}

	/* Create PHY->xbar links. */
	offset = 0;
	for (i = 0; i < priv->ops->num_phys; i++) {
		struct max_ser_phy *phy = &priv->phys[i];

		ret = max_components_link(&phy->comp, 0, &xbar->comp, offset,
					  phy->data.enabled);
		if (ret < 0)
			return ret;

		offset += ret;
	}

	return 0;
}

static int max_ser_register_pipe_link_xbar(struct max_ser_priv *priv,
					   struct v4l2_device *v4l2_dev)
{
	struct max_ser_pipe_link_xbar *xbar = &priv->pipe_link_xbar;
	struct max_ser_link *link = &priv->links[0];
	unsigned int offset;
	unsigned int i;
	int ret;

	ret = max_ser_pipe_link_xbar_register_v4l2_sd(xbar, v4l2_dev);
	if (ret)
		return ret;

	/* Create xbar->link links. */
	ret = max_components_link(&xbar->comp, 0, &link->comp, 0, true);
	if (ret < 0)
		return ret;

	/* Create pipe->xbar links. */
	offset = 0;
	for (i = 0; i < priv->ops->num_pipes; i++) {
		struct max_ser_pipe *pipe = &priv->pipes[i];

		ret = max_components_link(&pipe->comp, 0, &xbar->comp, offset, true);
		if (ret < 0)
			return ret;

		offset += ret;
	}

	return 0;
}

int max_ser_register_v4l2(struct max_ser_priv *priv, struct v4l2_device *v4l2_dev)
{
	unsigned int i;
	int ret;

	for (i = 0; i < priv->ops->num_pipes; i++) {
		struct max_ser_pipe *pipe = &priv->pipes[i];

		ret = max_ser_pipe_register_v4l2_sd(pipe, v4l2_dev);
		if (ret)
			return ret;
	}

	for (i = 0; i < priv->ops->num_phys; i++) {
		struct max_ser_phy *phy = &priv->phys[i];

		if (!phy->data.enabled)
			continue;

		ret = max_ser_phy_register_v4l2_sd(phy, v4l2_dev);
		if (ret)
			return ret;
	}

	ret = max_ser_register_phy_pipe_xbar(priv, v4l2_dev);
	if (ret)
		return ret;

	ret = max_ser_register_pipe_link_xbar(priv, v4l2_dev);
	if (ret)
		return ret;

	return 0;
}

void max_ser_unregister_v4l2(struct max_ser_priv *priv)
{
	unsigned int i;


	max_ser_pipe_link_xbar_unregister_v4l2_sd(&priv->pipe_link_xbar);

	max_ser_phy_pipe_xbar_unregister_v4l2_sd(&priv->phy_pipe_xbar);

	for (i = 0; i < priv->ops->num_phys; i++) {
		struct max_ser_phy *phy = &priv->phys[i];

		if (!phy->data.enabled)
			continue;

		max_ser_phy_unregister_v4l2_sd(phy);
	}

	for (i = 0; i < priv->ops->num_pipes; i++) {
		struct max_ser_pipe *pipe = &priv->pipes[i];

		max_ser_pipe_unregister_v4l2_sd(pipe);
	}
}

static int max_ser_parse_dt(struct max_ser_priv *priv)
{
	struct fwnode_handle *fwnode = dev_fwnode(priv->dev);
	struct max_ser_link *link;
	struct max_ser_pipe *pipe;
	struct max_ser_phy *phy;
	const char *label = NULL;
	unsigned int i;
	u32 index;
	u32 val;
	int ret;

	debug

	fwnode_property_read_string(fwnode, "label", &label);
	max_set_priv_name(priv->name, label, priv->client);

	debug

	val = device_property_read_bool(priv->dev, "maxim,tunnel-mode");
	if (val && !priv->ops->supports_tunnel_mode) {
		dev_err(priv->dev, "Tunnel mode is not supported\n");
		return -EINVAL;
	}
	priv->tunnel_mode = val;

	debug

	for (i = 0; i < priv->ops->num_phys; i++) {
		phy = &priv->phys[i];
		phy->priv = priv;
		phy->data.index = i;
		debug
	}

	debug

	priv->phy_pipe_xbar.priv = priv;

	debug

	for (i = 0; i < priv->ops->num_pipes; i++) {
		pipe = &priv->pipes[i];
		pipe->priv = priv;
		pipe->data.index = i;
		pipe->data.phy_id = i % priv->ops->num_phys;
		pipe->data.stream_id = i % MAX_SERDES_STREAMS_NUM;
	}

	debug

	priv->pipe_link_xbar.priv = priv;

	debug

	link = &priv->links[0];
	link->priv = priv;

	debug

	device_for_each_child_node(priv->dev, fwnode) {
		struct device_node *of_node = to_of_node(fwnode);
		struct fwnode_handle *ep;

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

		ep = fwnode_graph_get_endpoint_by_id(fwnode, 0, 0, 0);
		if (!ep) {
			dev_err(priv->dev, "Not connected to subdevice\n");
			fwnode_handle_put(fwnode);
			return -EINVAL;
		}

		phy = &priv->phys[index];
		phy->comp.fwnode = fwnode;
		phy->data.enabled = true;
		priv->phys_eps[index] = ep;
		priv->phys_comps[index] = &phy->comp;

		ret = max_ser_phy_parse_dt(priv, &phy->data, fwnode);
		if (ret) {
			fwnode_handle_put(fwnode);
			return ret;
		}
	}

	debug

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

		ret = max_ser_pipe_parse_dt(priv, &pipe->data, fwnode);
		if (ret) {
			fwnode_handle_put(fwnode);
			return ret;
		}
	}

	debug

	device_for_each_child_node(priv->dev, fwnode) {
		struct device_node *of_node = to_of_node(fwnode);

		if (!of_node_name_eq(of_node, "link"))
			continue;

		link = &priv->links[0];
		link->comp.fwnode = fwnode;
	}

	debug

	return 0;
}

static int max_ser_allocate(struct max_ser_priv *priv)
{
	unsigned int i;

	priv->phys = devm_kcalloc(priv->dev, priv->ops->num_phys,
				  sizeof(*priv->phys), GFP_KERNEL);
	if (!priv->phys)
		return -ENOMEM;

	priv->pipes = devm_kcalloc(priv->dev, priv->ops->num_pipes,
				   sizeof(*priv->pipes), GFP_KERNEL);
	if (!priv->pipes)
		return -ENOMEM;

	priv->links = devm_kcalloc(priv->dev, 1, sizeof(*priv->links), GFP_KERNEL);
	if (!priv->links)
		return -ENOMEM;

	priv->phys_eps = devm_kcalloc(priv->dev, priv->ops->num_phys,
				      sizeof(*priv->phys_eps), GFP_KERNEL);
	if (!priv->phys_eps)
		return -ENOMEM;

	priv->phys_comps = devm_kcalloc(priv->dev, priv->ops->num_phys,
					sizeof(*priv->phys_comps), GFP_KERNEL);
	if (!priv->phys_comps)
		return -ENOMEM;

	priv->i2c_xlates = devm_kcalloc(priv->dev, priv->ops->num_i2c_xlates,
					sizeof(*priv->i2c_xlates), GFP_KERNEL);
	if (!priv->i2c_xlates)
		return -ENOMEM;

	for (i = 0; i < priv->ops->num_pipes; i++) {
		struct max_ser_pipe *pipe = &priv->pipes[i];

		pipe->data.dts = devm_kcalloc(priv->dev, priv->ops->num_dts_per_pipe,
					      sizeof(*pipe->data.dts), GFP_KERNEL);
	}

	return 0;
}

int max_ser_probe(struct max_ser_priv *priv)
{
	int ret;

	debug

	mutex_init(&priv->lock);

	debug

	ret = max_ser_allocate(priv);
	if (ret)
		return ret;

	debug

	ret = max_ser_parse_dt(priv);
	if (ret)
		return ret;

	debug

	ret = max_ser_init(priv);
	if (ret)
		return ret;

	debug

	ret = max_ser_i2c_atr_init(priv);
	if (ret)
		return ret;

	debug

	return max_ser_register_async_v4l2(priv);
}
EXPORT_SYMBOL_GPL(max_ser_probe);

int max_ser_remove(struct max_ser_priv *priv)
{
	max_ser_unregister_async_v4l2(priv);

	max_ser_i2c_atr_deinit(priv);

	return 0;
}
EXPORT_SYMBOL_GPL(max_ser_remove);

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(I2C_ATR);
