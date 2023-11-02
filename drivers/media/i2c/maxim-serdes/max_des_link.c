#include "max_des.h"

static int max_des_link_log_status(struct v4l2_subdev *sd)
{
	struct max_component *comp = v4l2_get_subdevdata(sd);
	struct max_des_link *link = container_of(comp, struct max_des_link, comp);
	struct max_des_link_data *data = &link->data;

	v4l2_info(sd, "index: %u\n", data->index);
	v4l2_info(sd, "enabled: %u\n", data->enabled);
	v4l2_info(sd, "tunnel_mode: %u\n", data->tunnel_mode);
	v4l2_info(sd, "ser_xlate_enabled: %u\n", data->ser_xlate_enabled);
	v4l2_info(sd, "ser_xlate: src: 0x%02x dst: 0x%02x\n",
		  data->ser_xlate.src, data->ser_xlate.dst);

	return 0;
}

static const struct v4l2_subdev_core_ops max_des_link_core_ops = {
	.log_status = max_des_link_log_status,
};

static const struct v4l2_subdev_video_ops max_des_link_video_ops = {
};

static const struct v4l2_subdev_pad_ops max_des_link_pad_ops = {
	.get_fmt = v4l2_subdev_get_fmt,
};

static const struct v4l2_subdev_ops max_des_link_subdev_ops = {
	.core = &max_des_link_core_ops,
	.video = &max_des_link_video_ops,
	.pad = &max_des_link_pad_ops,
};

int max_des_link_register_v4l2_sd(struct max_des_link *link,
				  struct v4l2_device *v4l2_dev)
{
	struct max_des_priv *priv = link->priv;
	struct max_component *comp = &link->comp;

	comp->sd_ops = &max_des_link_subdev_ops;
	comp->v4l2_dev = v4l2_dev;
	comp->client = priv->client;
	comp->num_source_pads = priv->num_streams_per_link;
	comp->num_sink_pads = 1;
	comp->sink_pads_first = true;
	comp->prefix = priv->name;
	comp->name = "link";
	comp->index = link->data.index;

	return max_component_register_v4l2_sd(comp);
}

void max_des_link_unregister_v4l2_sd(struct max_des_link *link)
{
	struct max_component *comp = &link->comp;

	max_component_unregister_v4l2_sd(comp);
}

static int max_des_link_parse_sink_dt_endpoint(struct max_des_priv *priv,
					       struct max_des_link_data *data,
					       struct fwnode_handle *fwnode)
{
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
	if (val && !priv->ops->supports_tunnel_mode) {
		dev_err(priv->dev, "Tunnel mode is not supported\n");
		return -EINVAL;
	}
	data->tunnel_mode = val;

	return 0;
}

int max_des_link_parse_dt(struct max_des_priv *priv, struct max_des_link_data *data,
			  struct fwnode_handle *fwnode)
{
	return max_des_link_parse_sink_dt_endpoint(priv, data, fwnode);
}
