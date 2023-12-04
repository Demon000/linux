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

static int max_ser_init(struct max_ser_priv *priv)
{
	struct max_ser *ser = priv->ser;
	unsigned int i;
	int ret;

	ret = ser->ops->init(ser);
	if (ret)
		return ret;

	for (i = 0; i < ser->ops->num_phys; i++) {
		struct max_ser_phy *phy = &ser->phys[i];

		if (!phy->enabled)
			continue;

		ret = ser->ops->init_phy(ser, phy);
		if (ret)
			return ret;
	}

	for (i = 0; i < ser->ops->num_pipes; i++) {
		struct max_ser_pipe *pipe = &ser->pipes[i];

		ret = ser->ops->init_pipe(ser, pipe);
		if (ret)
			return ret;
	}

	if (ser->ops->post_init) {
		ret = ser->ops->post_init(ser);
		if (ret)
			return ret;
	}

	return 0;
}

static int max_ser_register_async_v4l2(struct max_ser_priv *priv)
{
	struct max_ser *ser = priv->ser;
	struct max_ser_link *link = &ser->links[0];
	int ret;

	ret = max_ser_link_register_v4l2_sd(priv, link, &priv->links_comp[0]);
	if (ret)
		return ret;

	return 0;
}

static void max_ser_unregister_async_v4l2(struct max_ser_priv *priv)
{
	max_ser_link_unregister_v4l2_sd(priv, &priv->links_comp[0]);
}

static int max_ser_register_phy_pipe_xbar(struct max_ser_priv *priv,
					  struct v4l2_device *v4l2_dev)
{
	struct max_component *comp = &priv->phy_pipe_xbar_comp;
	struct max_component *other_comp;
	struct max_ser *ser = priv->ser;
	unsigned int offset;
	unsigned int i;
	int ret;

	ret = max_ser_phy_pipe_xbar_register_v4l2_sd(priv, comp, v4l2_dev);
	if (ret)
		return ret;

	/* Create xbar->pipe links. */
	offset = 0;
	for (i = 0; i < ser->ops->num_pipes; i++) {
		other_comp = &priv->pipes_comp[i];

		ret = max_components_link(comp, offset, other_comp, 0);
		if (ret < 0)
			return ret;

		offset += ret;
	}

	/* Create PHY->xbar links. */
	offset = 0;
	for (i = 0; i < ser->ops->num_phys; i++) {
		struct max_ser_phy *phy = &ser->phys[i];

		if (!phy->enabled) {
			offset++;
			continue;
		}

		other_comp = &priv->phys_comp[i];

		ret = max_components_link(other_comp, 0, comp, offset);
		if (ret < 0)
			return ret;

		offset += ret;
	}

	return 0;
}

static int max_ser_register_pipe_link_xbar(struct max_ser_priv *priv,
					   struct v4l2_device *v4l2_dev)
{
	struct max_component *comp = &priv->pipe_link_xbar_comp;
	struct max_component *other_comp = &priv->links_comp[0];
	struct max_ser *ser = priv->ser;
	unsigned int offset;
	unsigned int i;
	int ret;

	ret = max_ser_pipe_link_xbar_register_v4l2_sd(priv, comp, v4l2_dev);
	if (ret)
		return ret;

	/* Create xbar->link links. */
	ret = max_components_link(comp, 0, other_comp, 0);
	if (ret < 0)
		return ret;

	/* Create pipe->xbar links. */
	offset = 0;
	for (i = 0; i < ser->ops->num_pipes; i++) {
		other_comp = &priv->pipes_comp[i];

		ret = max_components_link(other_comp, 0, comp, offset);
		if (ret < 0)
			return ret;

		offset += ret;
	}

	return 0;
}

int max_ser_register_v4l2(struct max_ser_priv *priv, struct v4l2_device *v4l2_dev)
{
	struct max_ser *ser = priv->ser;
	struct max_component *comp;
	unsigned int i;
	int ret;

	for (i = 0; i < ser->ops->num_phys; i++) {
		struct max_ser_phy *phy = &ser->phys[i];

		if (!phy->enabled)
			continue;

		comp = &priv->phys_comp[i];

		ret = max_ser_phy_register_v4l2_sd(priv, phy, comp, v4l2_dev);
		if (ret)
			return ret;
	}

	for (i = 0; i < ser->ops->num_pipes; i++) {
		struct max_ser_pipe *pipe = &ser->pipes[i];

		comp = &priv->pipes_comp[i];

		ret = max_ser_pipe_register_v4l2_sd(priv, pipe, comp, v4l2_dev);
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
	struct max_ser *ser = priv->ser;
	struct max_component *comp;
	unsigned int i;

	max_ser_pipe_link_xbar_unregister_v4l2_sd(priv, &priv->pipe_link_xbar_comp);

	max_ser_phy_pipe_xbar_unregister_v4l2_sd(priv, &priv->phy_pipe_xbar_comp);

	for (i = 0; i < ser->ops->num_pipes; i++) {
		comp = &priv->pipes_comp[i];

		max_ser_pipe_unregister_v4l2_sd(priv, comp);
	}

	for (i = 0; i < ser->ops->num_phys; i++) {
		struct max_ser_phy *phy = &ser->phys[i];

		if (!phy->enabled)
			continue;

		comp = &priv->phys_comp[i];

		max_ser_phy_unregister_v4l2_sd(priv, comp);
	}
}

static int max_ser_parse_dt(struct max_ser_priv *priv)
{
	struct max_ser *ser = priv->ser;
	struct fwnode_handle *fwnode = dev_fwnode(priv->dev);
	struct max_component *comp;
	struct max_ser_pipe *pipe;
	struct max_ser_phy *phy;
	const char *label = NULL;
	unsigned int i;
	u32 index;
	u32 val;
	int ret;

	fwnode_property_read_string(fwnode, "label", &label);
	max_set_priv_name(ser->name, label, priv->client);

	val = device_property_read_bool(priv->dev, "maxim,tunnel-mode");
	if (val && !ser->ops->supports_tunnel_mode) {
		dev_err(priv->dev, "Tunnel mode is not supported\n");
		return -EINVAL;
	}
	ser->tunnel_mode = val;

	for (i = 0; i < ser->ops->num_phys; i++) {
		phy = &ser->phys[i];
		phy->index = i;
	}

	for (i = 0; i < ser->ops->num_pipes; i++) {
		pipe = &ser->pipes[i];
		pipe->index = i;
	}

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

		if (index >= ser->ops->num_phys) {
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

		phy = &ser->phys[index];
		phy->enabled = true;

		comp = &priv->phys_comp[index];
		comp->fwnode = fwnode;

		priv->phys_eps[index] = ep;
		priv->phys_comps[index] = comp;

		ret = max_ser_phy_parse_dt(priv, phy, fwnode);
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

		if (index >= ser->ops->num_pipes) {
			dev_err(priv->dev, "Invalid pipe %u\n", index);
			fwnode_handle_put(fwnode);
			return -EINVAL;
		}

		pipe = &ser->pipes[index];

		ret = max_ser_pipe_parse_dt(priv, pipe, fwnode);
		if (ret) {
			fwnode_handle_put(fwnode);
			return ret;
		}
	}

	device_for_each_child_node(priv->dev, fwnode) {
		struct device_node *of_node = to_of_node(fwnode);

		if (!of_node_name_eq(of_node, "link"))
			continue;

		comp = &priv->links_comp[0];
		comp->fwnode = fwnode;
	}

	return 0;
}

static int max_ser_allocate(struct max_ser_priv *priv)
{
	struct max_ser *ser = priv->ser;
	unsigned int i;

	ser->phys = devm_kcalloc(priv->dev, ser->ops->num_phys,
				 sizeof(*ser->phys), GFP_KERNEL);
	if (!ser->phys)
		return -ENOMEM;

	ser->pipes = devm_kcalloc(priv->dev, ser->ops->num_pipes,
				  sizeof(*ser->pipes), GFP_KERNEL);
	if (!ser->pipes)
		return -ENOMEM;

	ser->links = devm_kcalloc(priv->dev, 1, sizeof(*ser->links), GFP_KERNEL);
	if (!ser->links)
		return -ENOMEM;

	ser->i2c_xlates = devm_kcalloc(priv->dev, ser->ops->num_i2c_xlates,
				       sizeof(*ser->i2c_xlates), GFP_KERNEL);
	if (!ser->i2c_xlates)
		return -ENOMEM;

	for (i = 0; i < ser->ops->num_pipes; i++) {
		struct max_ser_pipe *pipe = &ser->pipes[i];

		pipe->dts = devm_kcalloc(priv->dev, ser->ops->num_dts_per_pipe,
					      sizeof(*pipe->dts), GFP_KERNEL);
		if (!pipe->dts)
			return -ENOMEM;
	}

	priv->phys_eps = devm_kcalloc(priv->dev, ser->ops->num_phys,
				      sizeof(*priv->phys_eps), GFP_KERNEL);
	if (!priv->phys_eps)
		return -ENOMEM;

	priv->phys_comps = devm_kcalloc(priv->dev, ser->ops->num_phys,
					sizeof(*priv->phys_comps), GFP_KERNEL);
	if (!priv->phys_comps)
		return -ENOMEM;

	priv->phys_comp = devm_kcalloc(priv->dev, ser->ops->num_phys,
				       sizeof(*priv->phys_comp), GFP_KERNEL);
	if (!priv->phys_comp)
		return -ENOMEM;

	priv->pipes_comp = devm_kcalloc(priv->dev, ser->ops->num_pipes,
					sizeof(*priv->pipes_comp), GFP_KERNEL);
	if (!priv->pipes_comp)
		return -ENOMEM;

	priv->links_comp = devm_kcalloc(priv->dev, 1, sizeof(*priv->links_comp), GFP_KERNEL);
	if (!priv->links_comp)
		return -ENOMEM;

	return 0;
}

int max_ser_probe(struct i2c_client *client, struct max_ser *ser)
{
	struct device *dev = &client->dev;
	struct max_ser_priv *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;
	priv->dev = dev;
	priv->ser = ser;
	ser->priv = priv;

	mutex_init(&priv->lock);

	ret = max_ser_allocate(priv);
	if (ret)
		return ret;

	ret = max_ser_parse_dt(priv);
	if (ret)
		return ret;

	ret = max_ser_init(priv);
	if (ret)
		return ret;

	ret = max_ser_i2c_atr_init(priv);
	if (ret)
		return ret;

	return max_ser_register_async_v4l2(priv);
}
EXPORT_SYMBOL_GPL(max_ser_probe);

int max_ser_remove(struct max_ser *ser)
{
	struct max_ser_priv *priv = ser->priv;

	max_ser_unregister_async_v4l2(priv);

	max_ser_i2c_atr_deinit(priv);

	return 0;
}
EXPORT_SYMBOL_GPL(max_ser_remove);

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(I2C_ATR);
