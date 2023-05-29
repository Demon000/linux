// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim GMSL2 Deserializer Driver
 *
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of_graph.h>
#include <linux/regmap.h>

#include "max_des.h"
#include "max_ser.h"
#include "max_serdes.h"

#define MAX_DES_REMAP_EL_NUM		5
#define MAX_DES_I2C_XLATE_EL_NUM	2
#define MAX_DES_MUX_CH_INVALID		-1

const struct regmap_config max_des_i2c_regmap = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0x1f00,
};

static struct max_des_subdev_priv *next_subdev(struct max_des_priv *priv,
					       struct max_des_subdev_priv *sd_priv)
{
	if (!sd_priv)
		sd_priv = &priv->sd_privs[0];
	else
		sd_priv++;

	for (; sd_priv < priv->sd_privs + priv->num_subdevs; sd_priv++) {
		if (sd_priv->fwnode)
			return sd_priv;
	}

	return NULL;
}

#define for_each_subdev(priv, sd_priv) \
	for ((sd_priv) = NULL; ((sd_priv) = next_subdev((priv), (sd_priv))); )

static inline struct max_des_asd *to_max_des_asd(struct v4l2_async_subdev *asd)
{
	return container_of(asd, struct max_des_asd, base);
}

static inline struct max_des_subdev_priv *sd_to_max_des(struct v4l2_subdev *sd)
{
	return container_of(sd, struct max_des_subdev_priv, sd);
}

static int max_des_i2c_mux_select(struct i2c_mux_core *muxc, u32 chan)
{
	struct max_des_priv *priv = i2c_mux_priv(muxc);

	if (priv->mux_channel == chan)
		return 0;

	priv->mux_channel = chan;

	return priv->ops->mux_select(priv, chan);
}

static int max_des_i2c_mux_init(struct max_des_priv *priv)
{
	unsigned int i;
	int ret;

	if (!i2c_check_functionality(priv->client->adapter,
				     I2C_FUNC_SMBUS_WRITE_BYTE_DATA))
		return -ENODEV;

	priv->mux_channel = MAX_DES_MUX_CH_INVALID;

	priv->mux = i2c_mux_alloc(priv->client->adapter, &priv->client->dev,
				  priv->num_subdevs, 0, I2C_MUX_LOCKED,
				  max_des_i2c_mux_select, NULL);
	if (!priv->mux)
		return -ENOMEM;

	priv->mux->priv = priv;

	for (i = 0; i < priv->ops->num_links; i++) {
		struct max_des_link *link = &priv->links[i];

		if (!link->enabled)
			continue;

		ret = i2c_mux_add_adapter(priv->mux, 0, link->index, 0);
		if (ret)
			goto error;
	}

	return 0;

error:
	i2c_mux_del_adapters(priv->mux);

	return ret;
}

static int __max_des_mipi_update(struct max_des_priv *priv)
{
	struct max_des_subdev_priv *sd_priv;
	bool enable = 0;

	for_each_subdev(priv, sd_priv)
		if (sd_priv->active)
			enable = 1;

	if (enable == priv->active)
		return 0;

	priv->active = enable;

	return priv->ops->mipi_enable(priv, enable);
}

static int max_des_mipi_enable(struct max_des_subdev_priv *sd_priv, bool enable)
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

static int max_des_update_pipe_remaps(struct max_des_priv *priv,
				      struct max_des_pipe *pipe)
{
	struct max_des_subdev_priv *sd_priv;
	unsigned int i;

	pipe->num_remaps = 0;

	for_each_subdev(priv, sd_priv) {
		if (sd_priv->pipe_id != pipe->index)
			continue;

		for (i = 0; i < sd_priv->num_remaps; i++) {
			if (pipe->num_remaps == MAX_DES_REMAPS_NUM) {
				dev_err(priv->dev, "Too many remaps\n");
				return -EINVAL;
			}

			pipe->remaps[pipe->num_remaps++] = sd_priv->remaps[i];
		}
	}

	return 0;
}

static int max_des_update_pipes_remaps(struct max_des_priv *priv)
{
	struct max_des_pipe *pipe;
	unsigned int i;
	int ret;

	for (i = 0; i < priv->ops->num_pipes; i++) {
		pipe = &priv->pipes[i];

		if (!pipe->enabled)
			continue;

		ret = max_des_update_pipe_remaps(priv, pipe);
		if (ret)
			return ret;
	}

	return 0;
}

static int max_des_init_link_i2c_xlate(struct max_des_priv *priv,
				       struct max_des_link *link,
				       struct regmap *regmap)
{
	unsigned int i;
	int ret;

	if (!link->num_i2c_xlates)
		return 0;

	for (i = 0; i < link->num_i2c_xlates; i++) {
		struct max_i2c_xlate *xlate = &link->i2c_xlates[i];

		ret = max_ser_init_i2c_xlate(regmap, i, xlate);
		if (ret)
			return ret;
	}

	return 0;
}

static int max_des_init_link_ser_xlate(struct max_des_priv *priv,
				       struct max_des_link *link)
{
	u8 addrs[] = { link->ser_xlate.src, link->ser_xlate.dst };
	struct i2c_client *client;
	struct regmap *regmap;
	int ret;

	if (!link->ser_xlate_enabled)
		return 0;

	client = i2c_new_dummy_device(priv->client->adapter, addrs[0]);
	if (IS_ERR(client)) {
		ret = PTR_ERR(client);
		dev_err(priv->dev,
			"Failed to create I2C client: %d\n", ret);
		return ret;
	}

	regmap = regmap_init_i2c(client, &max_ser_i2c_regmap);
	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		dev_err(priv->dev,
			"Failed to create I2C regmap: %d\n", ret);
		goto err_unregister_client;
	}

	ret = priv->ops->select_links(priv, BIT(link->index));
	if (ret)
		goto err_regmap_exit;

	ret = max_ser_wait_for_multiple(client, regmap, addrs, 2);
	if (ret) {
		dev_err(priv->dev,
			"Failed waiting for serializer with new or old address: %d\n", ret);
		goto err_regmap_exit;
	}

	ret = max_ser_reset(regmap);
	if (ret) {
		dev_err(priv->dev, "Failed to reset serializer: %d\n", ret);
		goto err_regmap_exit;
	}

	ret = max_ser_wait(client, regmap, addrs[0]);
	if (ret) {
		dev_err(priv->dev,
			"Failed waiting for serializer with new address: %d\n", ret);
		goto err_regmap_exit;
	}

	ret = max_des_init_link_i2c_xlate(priv, link, regmap);
	if (ret)
		goto err_regmap_exit;

	ret = max_ser_change_address(regmap, addrs[1]);
	if (ret) {
		dev_err(priv->dev, "Failed to change serializer address: %d\n", ret);
		goto err_regmap_exit;
	}

err_regmap_exit:
	regmap_exit(regmap);

err_unregister_client:
	i2c_unregister_device(client);

	return ret;
}

static int max_des_init(struct max_des_priv *priv)
{
	unsigned int i, mask;
	int ret;

	ret = __max_des_mipi_update(priv);
	if (ret)
		return ret;

	ret = priv->ops->init(priv);
	if (ret)
		return ret;

	for (i = 0; i < priv->ops->num_phys; i++) {
		struct max_des_phy *phy = &priv->phys[i];

		if (!phy->enabled)
			continue;

		ret = priv->ops->init_phy(priv, phy);
		if (ret)
			return ret;
	}

	for (i = 0; i < priv->ops->num_pipes; i++) {
		struct max_des_pipe *pipe = &priv->pipes[i];

		if (!pipe->enabled)
			continue;

		ret = priv->ops->init_pipe(priv, pipe);
		if (ret)
			return ret;
	}

	for (i = 0; i < priv->ops->num_links; i++) {
		struct max_des_link *link = &priv->links[i];

		if (!link->enabled)
			continue;

		ret = max_des_init_link_ser_xlate(priv, link);
		if (ret)
			return ret;

	}

	mask = 0;
	for (i = 0; i < priv->ops->num_links; i++) {
		struct max_des_link *link = &priv->links[i];

		if (!link->enabled)
			continue;

		mask |= BIT(link->index);
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

static int max_des_notify_bound(struct v4l2_async_notifier *notifier,
				struct v4l2_subdev *subdev,
				struct v4l2_async_subdev *asd)
{
	struct max_des_subdev_priv *sd_priv = sd_to_max_des(notifier->sd);
	struct max_des_priv *priv = sd_priv->priv;
	int ret;

	ret = media_entity_get_fwnode_pad(&subdev->entity,
					  sd_priv->slave_fwnode,
					  MEDIA_PAD_FL_SOURCE);
	if (ret < 0) {
		dev_err(priv->dev,
			"Failed to find pad for %s: %d\n", subdev->name, ret);
		return ret;
	}

	sd_priv->slave_sd = subdev;
	sd_priv->slave_sd_pad_id = ret;

	ret = media_create_pad_link(&sd_priv->slave_sd->entity,
					sd_priv->slave_sd_pad_id,
					&sd_priv->sd.entity,
					MAX_DES_SINK_PAD,
					MEDIA_LNK_FL_ENABLED |
					MEDIA_LNK_FL_IMMUTABLE);
	if (ret) {
		dev_err(priv->dev,
			"Unable to link %s:%u -> %s:%u\n",
			sd_priv->slave_sd->name,
			sd_priv->slave_sd_pad_id,
			sd_priv->sd.name,
			MAX_DES_SINK_PAD);
		return ret;
	}

	dev_err(priv->dev, "Bound %s:%u on %s:%u\n",
		sd_priv->slave_sd->name,
		sd_priv->slave_sd_pad_id,
		sd_priv->sd.name,
		MAX_DES_SINK_PAD);

	sd_priv->slave_sd_state = v4l2_subdev_alloc_state(subdev);
	if (IS_ERR(sd_priv->slave_sd_state))
		return PTR_ERR(sd_priv->slave_sd_state);

	ret = v4l2_subdev_call(sd_priv->slave_sd, core, post_register);
	if (ret) {
		dev_err(priv->dev,
			"Failed to call post register for subdev %s: %d\n",
			sd_priv->sd.name, ret);
		return ret;
	}

	return 0;
}

static void max_des_notify_unbind(struct v4l2_async_notifier *notifier,
				  struct v4l2_subdev *subdev,
				  struct v4l2_async_subdev *asd)
{
	struct max_des_subdev_priv *sd_priv = sd_to_max_des(notifier->sd);

	sd_priv->slave_sd = NULL;
	v4l2_subdev_free_state(sd_priv->slave_sd_state);
	sd_priv->slave_sd_state = NULL;
}

static const struct v4l2_async_notifier_operations max_des_notify_ops = {
	.bound = max_des_notify_bound,
	.unbind = max_des_notify_unbind,
};

static int max_des_v4l2_notifier_register(struct max_des_subdev_priv *sd_priv)
{
	struct max_des_priv *priv = sd_priv->priv;
	struct max_des_asd *mas;
	int ret;

	v4l2_async_notifier_init(&sd_priv->notifier);

	mas = (struct max_des_asd *)
		  v4l2_async_notifier_add_fwnode_subdev(&sd_priv->notifier,
							sd_priv->slave_fwnode, struct max_des_asd);
	if (IS_ERR(mas)) {
		ret = PTR_ERR(mas);
		dev_err(priv->dev,
			"Failed to add subdev notifier for subdev %s: %d\n",
			sd_priv->sd.name, ret);
		goto error_cleanup_notifier;
	}

	mas->sd_priv = sd_priv;

	sd_priv->notifier.ops = &max_des_notify_ops;
	sd_priv->notifier.flags |= V4L2_ASYNC_NOTIFIER_DEFER_POST_REGISTER;

	ret = v4l2_async_subdev_notifier_register(&sd_priv->sd, &sd_priv->notifier);
	if (ret) {
		dev_err(priv->dev,
			"Failed to register subdev notifier for subdev %s: %d\n",
			sd_priv->sd.name, ret);
		goto error_cleanup_notifier;
	}

	return 0;

error_cleanup_notifier:
	v4l2_async_notifier_cleanup(&sd_priv->notifier);

	return ret;
}

static int max_des_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct max_des_subdev_priv *sd_priv = sd_to_max_des(sd);
	struct max_des_priv *priv = sd_priv->priv;
	int ret;

	max_des_mipi_enable(sd_priv, enable);

	ret = v4l2_subdev_call(sd_priv->slave_sd, video, s_stream, enable);
	if (ret) {
		dev_err(priv->dev, "Failed to start stream for %s: %d\n",
			sd_priv->slave_sd->name, ret);
		return ret;
	}

	return 0;
}

static const struct v4l2_subdev_video_ops max_des_video_ops = {
	.s_stream = max_des_s_stream,
};

static int max_des_get_selection(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_selection *sel)
{
	struct max_des_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_selection sd_sel = *sel;
	int ret;

	if (sel->pad != MAX_DES_SOURCE_PAD)
		return -EINVAL;

	sd_sel.pad = sd_priv->slave_sd_pad_id;

	ret = v4l2_subdev_call(sd_priv->slave_sd, pad, get_selection,
				   sd_priv->slave_sd_state, &sd_sel);
	if (ret)
		return ret;

	sel->r = sd_sel.r;

	return 0;
}

static int max_des_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *format)
{
	struct max_des_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_format sd_format = *format;
	int ret;

	if (format->pad != MAX_DES_SOURCE_PAD)
		return -EINVAL;

	sd_format.pad = sd_priv->slave_sd_pad_id;

	ret = v4l2_subdev_call(sd_priv->slave_sd, pad, get_fmt,
				   sd_priv->slave_sd_state, &sd_format);
	if (ret)
		return ret;

	format->format = sd_format.format;

	return 0;
}

static int max_des_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *format)
{
	struct max_des_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_format sd_format = *format;
	int ret;

	if (format->pad != MAX_DES_SOURCE_PAD)
		return -EINVAL;

	sd_format.pad = sd_priv->slave_sd_pad_id;

	ret = v4l2_subdev_call(sd_priv->slave_sd, pad, set_fmt,
				   sd_priv->slave_sd_state, &sd_format);
	if (ret)
		return ret;

	format->format = sd_format.format;

	return 0;
}

static int max_des_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	struct max_des_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_mbus_code_enum sd_code = *code;
	int ret;

	if (code->pad != MAX_DES_SOURCE_PAD)
		return -EINVAL;

	sd_code.pad = sd_priv->slave_sd_pad_id;

	ret = v4l2_subdev_call(sd_priv->slave_sd, pad, enum_mbus_code,
				   sd_priv->slave_sd_state, &sd_code);
	if (ret)
		return ret;

	code->code = sd_code.code;

	return 0;
}

static int max_des_enum_frame_size(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	struct max_des_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_frame_size_enum sd_fse = *fse;
	int ret;

	if (fse->pad != MAX_DES_SOURCE_PAD)
		return -EINVAL;

	sd_fse.pad = sd_priv->slave_sd_pad_id;

	ret = v4l2_subdev_call(sd_priv->slave_sd, pad, enum_frame_size,
				   sd_priv->slave_sd_state, &sd_fse);
	if (ret)
		return ret;

	fse->code = sd_fse.code;
	fse->min_width = sd_fse.min_width;
	fse->max_width = sd_fse.max_width;
	fse->min_height = sd_fse.min_height;
	fse->max_height = sd_fse.max_height;

	return 0;
}

static int max_des_enum_frame_interval(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *sd_state,
				       struct v4l2_subdev_frame_interval_enum *fie)
{
	struct max_des_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_frame_interval_enum sd_fie = *fie;
	int ret;

	if (fie->pad != MAX_DES_SOURCE_PAD)
		return -EINVAL;

	sd_fie.pad = sd_priv->slave_sd_pad_id;

	ret = v4l2_subdev_call(sd_priv->slave_sd, pad, enum_frame_interval,
				   sd_priv->slave_sd_state, &sd_fie);
	if (ret)
		return ret;

	fie->code = sd_fie.code;
	fie->width = sd_fie.width;
	fie->height = sd_fie.height;
	fie->interval = sd_fie.interval;

	return 0;
}

static const struct v4l2_subdev_pad_ops max_des_pad_ops = {
	.get_selection = max_des_get_selection,
	.get_fmt = max_des_get_fmt,
	.set_fmt = max_des_set_fmt,
	.enum_mbus_code = max_des_enum_mbus_code,
	.enum_frame_size = max_des_enum_frame_size,
	.enum_frame_interval = max_des_enum_frame_interval,
};

static const struct v4l2_subdev_ops max_des_subdev_ops = {
	.video = &max_des_video_ops,
	.pad = &max_des_pad_ops,
};

static int max_des_v4l2_register_sd(struct max_des_subdev_priv *sd_priv)
{
	struct max_des_priv *priv = sd_priv->priv;
	unsigned int index = sd_priv->index;
	char postfix[3];
	int ret;

	ret = max_des_v4l2_notifier_register(sd_priv);
	if (ret)
		return ret;

	snprintf(postfix, sizeof(postfix), ":%d", index);

	v4l2_i2c_subdev_init(&sd_priv->sd, priv->client, &max_des_subdev_ops);
	v4l2_i2c_subdev_set_name(&sd_priv->sd, priv->client, NULL, postfix);
	sd_priv->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	sd_priv->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sd_priv->sd.fwnode = sd_priv->fwnode;

	sd_priv->pads[MAX_DES_SOURCE_PAD].flags = MEDIA_PAD_FL_SOURCE;
	sd_priv->pads[MAX_DES_SINK_PAD].flags = MEDIA_PAD_FL_SINK;

	ret = media_entity_pads_init(&sd_priv->sd.entity, MAX_DES_PAD_NUM, sd_priv->pads);
	if (ret)
		goto error;

	v4l2_set_subdevdata(&sd_priv->sd, sd_priv);

	return v4l2_async_register_subdev(&sd_priv->sd);

error:
	v4l2_async_notifier_unregister(&sd_priv->notifier);
	v4l2_async_notifier_cleanup(&sd_priv->notifier);
	media_entity_cleanup(&sd_priv->sd.entity);
	fwnode_handle_put(sd_priv->sd.fwnode);

	return ret;
}

static void max_des_v4l2_unregister_sd(struct max_des_subdev_priv *sd_priv)
{
	v4l2_async_notifier_unregister(&sd_priv->notifier);
	v4l2_async_notifier_cleanup(&sd_priv->notifier);
	v4l2_async_unregister_subdev(&sd_priv->sd);
	media_entity_cleanup(&sd_priv->sd.entity);
	fwnode_handle_put(sd_priv->sd.fwnode);
}

static int max_des_v4l2_register(struct max_des_priv *priv)
{
	struct max_des_subdev_priv *sd_priv;
	int ret;

	for_each_subdev(priv, sd_priv) {
		ret = max_des_v4l2_register_sd(sd_priv);
		if (ret)
			return ret;
	}

	return 0;
}

static void max_des_v4l2_unregister(struct max_des_priv *priv)
{
	struct max_des_subdev_priv *sd_priv;

	for_each_subdev(priv, sd_priv)
		max_des_v4l2_unregister_sd(sd_priv);
}

static int max_des_parse_link_i2c_xlate_dt(struct max_des_priv *priv,
					   struct max_des_link *link,
					   struct fwnode_handle *fwnode)
{
	const char *prop_name = "max,i2c-addr-translate";
	u32 vals[MAX_DES_I2C_XLATE_EL_NUM * MAX_DES_I2C_XLATES_NUM];
	unsigned int count, i;
	int ret;

	ret = fwnode_property_count_u32(fwnode, prop_name);
	if (ret <= 0)
		return 0;

	count = ret;

	if (ret % MAX_DES_I2C_XLATE_EL_NUM != 0 ||
	    count / MAX_DES_I2C_XLATE_EL_NUM > MAX_DES_I2C_XLATES_NUM) {
		dev_err(priv->dev,
			"Invalid I2C addr translate element number %u\n", ret);
		return -EINVAL;
	}

	ret = fwnode_property_read_u32_array(fwnode, prop_name, vals, count);
	if (ret)
		return ret;

	for (i = 0; i < count; i += MAX_DES_I2C_XLATE_EL_NUM) {
		unsigned int index = i / MAX_DES_I2C_XLATE_EL_NUM;
		struct max_i2c_xlate *xlate = &link->i2c_xlates[index];

		xlate->src = vals[0];
		xlate->dst = vals[1];
		link->num_i2c_xlates++;
	}

	return 0;
}

static int max_des_parse_link_ser_xlate_dt(struct max_des_priv *priv,
					   struct max_des_link *link,
					   struct fwnode_handle *fwnode)
{
	const char *prop_name = "max,ser-addr-translate";
	u32 vals[MAX_DES_I2C_XLATE_EL_NUM];
	int ret;

	ret = fwnode_property_count_u32(fwnode, prop_name);
	if (ret <= 0)
		return 0;

	if (ret != MAX_DES_I2C_XLATE_EL_NUM) {
		dev_err(priv->dev,
			"Invalid serializer addr translate element number %u\n", ret);
		return -EINVAL;
	}

	ret = fwnode_property_read_u32_array(fwnode, prop_name,
					     vals, ARRAY_SIZE(vals));
	if (ret)
		return ret;

	link->ser_xlate.src = vals[0];
	link->ser_xlate.dst = vals[1];
	link->ser_xlate_enabled = true;

	return 0;
}

static int max_des_parse_ch_remap_dt(struct max_des_subdev_priv *sd_priv,
				     struct fwnode_handle *fwnode)
{
	const char *prop_name = "max,dt-vc-phy-remap";
	struct max_des_priv *priv = sd_priv->priv;
	unsigned int i, count;
	u32 *remaps_arr;
	int ret;

	ret = fwnode_property_count_u32(fwnode, prop_name);
	if (ret <= 0)
		return 0;

	count = ret;

	if (count % MAX_DES_REMAP_EL_NUM != 0 ||
		count / MAX_DES_REMAP_EL_NUM > MAX_DES_REMAPS_NUM) {
		dev_err(priv->dev, "Invalid remap element number %u\n", count);
		return -EINVAL;
	}

	remaps_arr = kcalloc(count, sizeof(u32), GFP_KERNEL);
	if (!remaps_arr)
		return -ENOMEM;

	ret = fwnode_property_read_u32_array(fwnode, prop_name, remaps_arr, count);
	if (ret)
		goto exit;

	for (i = 0; i < count; i += MAX_DES_REMAP_EL_NUM) {
		unsigned int index = i / MAX_DES_REMAP_EL_NUM;
		struct max_des_dt_vc_remap *remap = &sd_priv->remaps[index];

		remap->from_dt = remaps_arr[i + 0];
		remap->from_vc = remaps_arr[i + 1];
		remap->to_dt = remaps_arr[i + 2];
		remap->to_vc = remaps_arr[i + 3];
		remap->phy = remaps_arr[i + 4];

		if (remap->phy >= priv->ops->num_phys) {
			dev_err(priv->dev, "Invalid remap PHY %u\n",
				remaps_arr[i + 4]);
			ret = -EINVAL;
			goto exit;
		}

		sd_priv->num_remaps++;
	}

exit:
	kfree(remaps_arr);

	return ret;
}

static int max_des_parse_pipe_link_remap_dt(struct max_des_priv *priv,
					    struct max_des_pipe *pipe,
					    struct fwnode_handle *fwnode)
{
	u32 val;
	int ret;

	val = pipe->link_id;
	ret = fwnode_property_read_u32(fwnode, "max,link-id", &val);
	if (!ret && priv->ops->supports_pipe_link_remap) {
		dev_err(priv->dev, "Pipe link remapping is not supported\n");
		return -EINVAL;
	}

	if (val >= priv->ops->num_links) {
		dev_err(priv->dev, "Invalid link %u\n", val);
		return -EINVAL;
	}

	pipe->link_id = val;

	return 0;
}

static int max_des_parse_pipe_dt(struct max_des_priv *priv,
				 struct max_des_pipe *pipe,
				 struct fwnode_handle *fwnode)
{
	u32 val;
	int ret;

	val = pipe->phy_id;
	fwnode_property_read_u32(fwnode, "max,phy-id", &val);
	if (val >= priv->ops->num_phys) {
		dev_err(priv->dev, "Invalid PHY %u\n", val);
		return -EINVAL;
	}
	pipe->phy_id = val;

	val = pipe->stream_id;
	fwnode_property_read_u32(fwnode, "max,stream-id", &val);
	if (val >= MAX_SERDES_STREAMS_NUM) {
		dev_err(priv->dev, "Invalid stream %u\n", val);
		return -EINVAL;
	}
	pipe->stream_id = val;

	ret = max_des_parse_pipe_link_remap_dt(priv, pipe, fwnode);
	if (ret)
		return ret;

	return 0;
}

static int max_des_parse_ch_dt(struct max_des_subdev_priv *sd_priv,
			       struct fwnode_handle *fwnode)
{
	struct max_des_priv *priv = sd_priv->priv;
	struct max_des_pipe *pipe;
	struct max_des_link *link;
	struct max_des_phy *phy;

	u32 val;

	val = sd_priv->pipe_id;
	fwnode_property_read_u32(fwnode, "max,pipe-id", &val);
	if (val >= priv->ops->num_pipes) {
		dev_err(priv->dev, "Invalid pipe %u\n", val);
		return -EINVAL;
	}
	sd_priv->pipe_id = val;

	pipe = &priv->pipes[val];
	pipe->enabled = true;

	phy = &priv->phys[pipe->phy_id];
	phy->enabled = true;

	link = &priv->links[pipe->link_id];
	link->enabled = true;

	return 0;
}

static int max_des_parse_src_dt_endpoint(struct max_des_subdev_priv *sd_priv,
					 struct fwnode_handle *fwnode)
{
	struct max_des_priv *priv = sd_priv->priv;
	struct max_des_pipe *pipe = &priv->pipes[sd_priv->pipe_id];
	struct max_des_phy *phy = &priv->phys[pipe->phy_id];
	struct v4l2_fwnode_endpoint v4l2_ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	struct fwnode_handle *ep, *remote_ep;
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(fwnode, MAX_DES_SOURCE_PAD, 0, 0);
	if (!ep) {
		dev_err(priv->dev, "Not connected to subdevice\n");
		return -EINVAL;
	}

	remote_ep = fwnode_graph_get_remote_endpoint(ep);
	fwnode_handle_put(ep);
	if (!remote_ep) {
		dev_err(priv->dev, "Not connected to remote endpoint\n");
		return -EINVAL;
	}

	ret = v4l2_fwnode_endpoint_parse(remote_ep, &v4l2_ep);
	fwnode_handle_put(remote_ep);
	if (ret) {
		dev_err(priv->dev, "Could not parse v4l2 endpoint\n");
		return ret;
	}

	/* TODO: check the rest of the MIPI configuration. */
	if (phy->mipi.num_data_lanes && phy->mipi.num_data_lanes !=
	    v4l2_ep.bus.mipi_csi2.num_data_lanes) {
		dev_err(priv->dev, "PHY configured with differing number of data lanes\n");
		return -EINVAL;
	}

	phy->mipi = v4l2_ep.bus.mipi_csi2;

	return 0;
}

static int max_des_parse_sink_dt_endpoint(struct max_des_subdev_priv *sd_priv,
					  struct fwnode_handle *fwnode)
{
	struct max_des_priv *priv = sd_priv->priv;
	struct fwnode_handle *ep;

	ep = fwnode_graph_get_endpoint_by_id(fwnode, MAX_DES_SINK_PAD, 0, 0);
	if (!ep) {
		dev_err(priv->dev, "Not connected to subdevice\n");
		return -EINVAL;
	}

	sd_priv->slave_fwnode = fwnode_graph_get_remote_endpoint(ep);
	if (!sd_priv->slave_fwnode) {
		dev_err(priv->dev, "Not connected to remote endpoint\n");

		return -EINVAL;
	}

	return 0;
}

static int max_des_parse_dt(struct max_des_priv *priv)
{
	const char *channel_node_name = "channel";
	const char *link_node_name = "link";
	const char *pipe_node_name = "pipe";
	struct max_des_subdev_priv *sd_priv;
	struct fwnode_handle *fwnode;
	struct max_des_link *link;
	struct max_des_pipe *pipe;
	struct max_des_phy *phy;
	unsigned int i;
	u32 index;
	int ret;

	for (i = 0; i < priv->ops->num_phys; i++) {
		phy = &priv->phys[i];
		phy->index = i;
	}

	for (i = 0; i < priv->ops->num_pipes; i++) {
		pipe = &priv->pipes[i];
		pipe->index = i;
		pipe->phy_id = i;
		pipe->stream_id = i;
		pipe->link_id = i;
	}

	for (i = 0; i < priv->ops->num_links; i++) {
		link = &priv->links[i];
		link->index = i;
	}

	device_for_each_child_node(priv->dev, fwnode) {
		struct device_node *of_node = to_of_node(fwnode);

		if (!of_node_name_eq(of_node, link_node_name))
			continue;

		ret = fwnode_property_read_u32(fwnode, "reg", &index);
		if (ret) {
			dev_err(priv->dev, "Failed to read reg: %d\n", ret);
			continue;
		}

		if (index >= priv->ops->num_links) {
			dev_err(priv->dev, "Invalid link %u\n", index);
			return -EINVAL;
		}

		link = &priv->links[index];

		ret = max_des_parse_link_ser_xlate_dt(priv, link, fwnode);
		if (ret)
			return ret;

		ret = max_des_parse_link_i2c_xlate_dt(priv, link, fwnode);
		if (ret)
			return ret;
	}

	device_for_each_child_node(priv->dev, fwnode) {
		struct device_node *of_node = to_of_node(fwnode);

		if (!of_node_name_eq(of_node, pipe_node_name))
			continue;

		ret = fwnode_property_read_u32(fwnode, "reg", &index);
		if (ret) {
			dev_err(priv->dev, "Failed to read reg: %d\n", ret);
			continue;
		}

		if (index >= priv->ops->num_pipes) {
			dev_err(priv->dev, "Invalid pipe %u\n", index);
			return -EINVAL;
		}

		pipe = &priv->pipes[index];

		ret = max_des_parse_pipe_dt(priv, pipe, fwnode);
		if (ret)
			return ret;
	}

	device_for_each_child_node(priv->dev, fwnode) {
		struct device_node *of_node = to_of_node(fwnode);

		if (!of_node_name_eq(of_node, channel_node_name))
			continue;

		ret = fwnode_property_read_u32(fwnode, "reg", &index);
		if (ret) {
			dev_err(priv->dev, "Failed to read reg: %d\n", ret);
			continue;
		}

		if (index + 1 < priv->num_subdevs)
			continue;

		priv->num_subdevs = index + 1;
	}

	priv->sd_privs = devm_kcalloc(priv->dev, priv->num_subdevs,
				      sizeof(*priv->sd_privs), GFP_KERNEL);
	if (!priv->sd_privs)
		return -ENOMEM;

	device_for_each_child_node(priv->dev, fwnode) {
		struct device_node *of_node = to_of_node(fwnode);

		if (!of_node_name_eq(of_node, channel_node_name))
			continue;

		ret = fwnode_property_read_u32(fwnode, "reg", &index);
		if (ret) {
			dev_err(priv->dev, "Failed to read reg: %d\n", ret);
			continue;
		}

		sd_priv = &priv->sd_privs[index];
		sd_priv->fwnode = fwnode;
		sd_priv->priv = priv;
		sd_priv->index = index;
		sd_priv->pipe_id = index;

		ret = max_des_parse_ch_dt(sd_priv, fwnode);
		if (ret)
			return ret;

		ret = max_des_parse_ch_remap_dt(sd_priv, fwnode);
		if (ret)
			return ret;

		ret = max_des_parse_sink_dt_endpoint(sd_priv, fwnode);
		if (ret)
			return ret;

		ret = max_des_parse_src_dt_endpoint(sd_priv, fwnode);
		if (ret)
			return ret;
	}

	ret = max_des_update_pipes_remaps(priv);
	if (ret)
		return ret;

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

	return 0;
}

int max_des_probe(struct max_des_priv *priv)
{
	int ret;

	ret = max_des_allocate(priv);
	if (ret)
		return ret;

	ret = max_des_parse_dt(priv);
	if (ret)
		return ret;

	ret = max_des_init(priv);
	if (ret)
		return ret;

	ret = max_des_i2c_mux_init(priv);
	if (ret)
		return ret;

	return max_des_v4l2_register(priv);
}
EXPORT_SYMBOL_GPL(max_des_probe);

int max_des_remove(struct max_des_priv *priv)
{
	max_des_v4l2_unregister(priv);

	return 0;
}
EXPORT_SYMBOL_GPL(max_des_remove);
