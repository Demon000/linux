#include "max_des.h"

static int max_des_link_log_status(struct v4l2_subdev *sd)
{
	struct max_component *comp = v4l2_get_subdevdata(sd);
	struct max_des_priv *priv = comp->priv;
	struct max_des *des = priv->des;
	struct max_des_link *link = &des->links[comp->index];

	v4l2_info(sd, "index: %u\n", link->index);
	v4l2_info(sd, "enabled: %u\n", link->enabled);
	v4l2_info(sd, "tunnel_mode: %u\n", link->tunnel_mode);
	v4l2_info(sd, "ser_xlate_enabled: %u\n", link->ser_xlate_enabled);
	v4l2_info(sd, "ser_xlate: src: 0x%02x dst: 0x%02x\n",
		  link->ser_xlate.src, link->ser_xlate.dst);

	return 0;
}

static const struct v4l2_subdev_core_ops max_des_link_core_ops = {
	.log_status = max_des_link_log_status,
};

static const struct v4l2_subdev_pad_ops max_des_link_pad_ops = {
	.init_cfg = max_component_init_cfg,
	.set_routing = max_component_set_validate_routing,
	.enable_streams = max_component_streams_enable,
	.disable_streams = max_component_streams_disable,
	.get_fmt = v4l2_subdev_get_fmt,
};

static const struct v4l2_subdev_ops max_des_link_subdev_ops = {
	.core = &max_des_link_core_ops,
	.pad = &max_des_link_pad_ops,
};

static const struct media_entity_operations max_des_link_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
	.has_pad_interdep = v4l2_subdev_has_pad_interdep,
};

int max_des_link_register_v4l2_sd(struct max_des_priv *priv,
				  struct max_des_link *link,
				  struct max_component *comp,
				  struct v4l2_device *v4l2_dev)
{
	struct max_des *des = priv->des;

	comp->priv = priv;
	comp->sd_ops = &max_des_link_subdev_ops;
	comp->mc_ops = &max_des_link_entity_ops;
	comp->v4l2_dev = v4l2_dev;
	comp->dev = priv->dev;
	comp->num_source_pads = des->num_streams_per_link;
	comp->num_sink_pads = 1;
	comp->sink_pads_first = true;
	comp->prefix = des->name;
	comp->name = "link";
	comp->index = link->index;
	comp->routing_disallow = V4L2_SUBDEV_ROUTING_ONLY_1_TO_1 |
				 V4L2_SUBDEV_ROUTING_NO_SOURCE_STREAM_MIX;

	return max_component_register_v4l2_sd(comp);
}

void max_des_link_unregister_v4l2_sd(struct max_des_priv *priv,
				     struct max_component *comp)
{
	max_component_unregister_v4l2_sd(comp);
}

static int max_des_link_parse_sink_dt_endpoint(struct max_des_priv *priv,
					       struct max_des_link *link,
					       struct fwnode_handle *fwnode)
{
	struct max_des *des = priv->des;
	struct fwnode_handle *ep, *device_fwnode;
	u32 val;

	ep = fwnode_graph_get_endpoint_by_id(fwnode, 0, 0, 0);
	if (!ep) {
		dev_err(priv->dev, "Not connected to subdevice\n");
		return -EINVAL;
	}

	device_fwnode = fwnode_graph_get_remote_port_parent(ep);
	fwnode_handle_put(ep);
	if (!device_fwnode) {
		dev_err(priv->dev, "Not connected to remote subdevice\n");
		return -EINVAL;
	}

	val = fwnode_property_read_bool(device_fwnode, "maxim,tunnel-mode");
	fwnode_handle_put(device_fwnode);
	if (val && !des->ops->supports_tunnel_mode) {
		dev_err(priv->dev, "Tunnel mode is not supported\n");
		return -EINVAL;
	}
	link->tunnel_mode = val;

	return 0;
}

int max_des_link_parse_dt(struct max_des_priv *priv, struct max_des_link *link,
			  struct fwnode_handle *fwnode)
{
	return max_des_link_parse_sink_dt_endpoint(priv, link, fwnode);
}
