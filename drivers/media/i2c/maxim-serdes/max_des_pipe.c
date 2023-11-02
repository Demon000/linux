#include "max_des.h"

static int max_des_pipe_log_status(struct v4l2_subdev *sd)
{
	struct max_component *comp = v4l2_get_subdevdata(sd);
	struct max_des_pipe *pipe = container_of(comp, struct max_des_pipe, comp);
	struct max_des_pipe_data *data = &pipe->data;
	struct max_des_priv *priv = pipe->priv;
	unsigned int i;
	int ret;

	v4l2_info(sd, "index: %u\n", data->index);
	v4l2_info(sd, "phy_id: %u\n", data->phy_id);
	v4l2_info(sd, "stream_id: %u\n", data->stream_id);
	v4l2_info(sd, "link_id: %u\n", data->link_id);
	v4l2_info(sd, "dbl8: %u\n", data->dbl8);
	v4l2_info(sd, "dbl8mode: %u\n", data->dbl8mode);
	v4l2_info(sd, "dbl10: %u\n", data->dbl10);
	v4l2_info(sd, "dbl10mode: %u\n", data->dbl10mode);
	v4l2_info(sd, "dbl12: %u\n", data->dbl12);
	v4l2_info(sd, "remaps: %u\n", data->num_remaps);

	for (i = 0; i < data->num_remaps; i++) {
		struct max_des_dt_vc_remap *remap = &data->remaps[i];

		v4l2_info(sd, "\tremap: from: vc: %u, dt: 0x%02x\n",
			  remap->from_vc, remap->from_dt);
		v4l2_info(sd, "\t       to:   vc: %u, dt: 0x%02x, pipe: %u\n",
			  remap->to_vc, remap->to_dt, remap->phy);
	}

	if (priv->ops->log_pipe_status) {
		ret = priv->ops->log_pipe_status(priv, data, sd->name);
		if (ret)
			return ret;
	}
	v4l2_info(sd, "\n");

	return 0;
}

static const struct v4l2_subdev_core_ops max_des_pipe_core_ops = {
	.log_status = max_des_pipe_log_status,
};

static const struct v4l2_subdev_video_ops max_des_pipe_video_ops = {
};

static const struct v4l2_subdev_pad_ops max_des_pipe_pad_ops = {
	.get_fmt = v4l2_subdev_get_fmt,
};

static const struct v4l2_subdev_ops max_des_pipe_subdev_ops = {
	.core = &max_des_pipe_core_ops,
	.video = &max_des_pipe_video_ops,
	.pad = &max_des_pipe_pad_ops,
};

int max_des_pipe_register_v4l2_sd(struct max_des_pipe *pipe,
				  struct v4l2_device *v4l2_dev)
{
	struct max_des_priv *priv = pipe->priv;
	struct max_component *comp = &pipe->comp;

	comp->sd_ops = &max_des_pipe_subdev_ops;
	comp->v4l2_dev = v4l2_dev;
	comp->client = priv->client;
	comp->num_source_pads = 1;
	comp->num_sink_pads = 1;
	comp->prefix = priv->name;
	comp->name = "pipe";
	comp->index = pipe->data.index;

	return max_component_register_v4l2_sd(comp);
}

void max_des_pipe_unregister_v4l2_sd(struct max_des_pipe *pipe)
{
	struct max_component *comp = &pipe->comp;

	max_component_unregister_v4l2_sd(comp);
}

static int max_des_pipe_parse_link_remap_dt(struct max_des_priv *priv,
					    struct max_des_pipe_data *data,
					    struct fwnode_handle *fwnode)
{
	u32 val;
	int ret;

	val = data->link_id;
	ret = fwnode_property_read_u32(fwnode, "maxim,link-id", &val);
	if (!ret && !priv->ops->supports_pipe_link_remap) {
		dev_err(priv->dev, "Pipe link remapping is not supported\n");
		return -EINVAL;
	}

	if (val >= priv->ops->num_links) {
		dev_err(priv->dev, "Invalid link %u\n", val);
		return -EINVAL;
	}

	data->link_id = val;

	return 0;
}

int max_des_pipe_parse_dt(struct max_des_priv *priv, struct max_des_pipe_data *data,
			  struct fwnode_handle *fwnode)
{
	u32 val;
	int ret;

	val = data->phy_id;
	fwnode_property_read_u32(fwnode, "maxim,phy-id", &val);
	if (val >= priv->ops->num_pipes) {
		dev_err(priv->dev, "Invalid PHY %u\n", val);
		return -EINVAL;
	}
	data->phy_id = val;

	val = data->stream_id;
	ret = fwnode_property_read_u32(fwnode, "maxim,stream-id", &val);
	if (!ret && priv->pipe_stream_autoselect) {
		dev_err(priv->dev, "Cannot select stream when using autoselect\n");
		return -EINVAL;
	}

	if (val >= MAX_SERDES_STREAMS_NUM) {
		dev_err(priv->dev, "Invalid stream %u\n", val);
		return -EINVAL;
	}

	data->stream_id = val;

	data->dbl8 = fwnode_property_read_bool(fwnode, "maxim,dbl8");
	data->dbl10 = fwnode_property_read_bool(fwnode, "maxim,dbl10");
	data->dbl12 = fwnode_property_read_bool(fwnode, "maxim,dbl12");

	data->dbl8mode = fwnode_property_read_bool(fwnode, "maxim,dbl8-mode");
	data->dbl10mode = fwnode_property_read_bool(fwnode, "maxim,dbl10-mode");

	ret = max_des_pipe_parse_link_remap_dt(priv, data, fwnode);
	if (ret)
		return ret;

	return 0;
}
