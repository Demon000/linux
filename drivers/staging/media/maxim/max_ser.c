// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim GMSL2 Serializer Driver
 *
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include "max_ser.h"

#include <linux/delay.h>
#include <linux/module.h>

#include "max_ser.h"
#include "max_serdes.h"

const struct regmap_config max_ser_i2c_regmap = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0x1f00,
};
EXPORT_SYMBOL_GPL(max_ser_i2c_regmap);

#define MAX_SER_FMT(_code, _dt)	\
{				\
	.code = (_code),	\
	.dt = (_dt),		\
}

static const struct max_ser_format max_ser_formats[] = {
	MAX_SER_FMT(MEDIA_BUS_FMT_YUYV8_1X16, MAX_SER_DT_YUV422_8B),
	MAX_SER_FMT(MEDIA_BUS_FMT_YUYV10_1X20, MAX_SER_DT_YUV422_10B),
	MAX_SER_FMT(MEDIA_BUS_FMT_RGB565_1X16, MAX_SER_DT_RGB565),
	MAX_SER_FMT(MEDIA_BUS_FMT_RGB666_1X18, MAX_SER_DT_RGB666),
	MAX_SER_FMT(MEDIA_BUS_FMT_RGB888_1X24, MAX_SER_DT_RGB888),
	MAX_SER_FMT(MEDIA_BUS_FMT_SBGGR8_1X8, MAX_SER_DT_RAW8),
	MAX_SER_FMT(MEDIA_BUS_FMT_SGBRG8_1X8, MAX_SER_DT_RAW8),
	MAX_SER_FMT(MEDIA_BUS_FMT_SGRBG8_1X8, MAX_SER_DT_RAW8),
	MAX_SER_FMT(MEDIA_BUS_FMT_SRGGB8_1X8, MAX_SER_DT_RAW8),
	MAX_SER_FMT(MEDIA_BUS_FMT_SBGGR10_1X10, MAX_SER_DT_RAW10),
	MAX_SER_FMT(MEDIA_BUS_FMT_SGBRG10_1X10, MAX_SER_DT_RAW10),
	MAX_SER_FMT(MEDIA_BUS_FMT_SGRBG10_1X10, MAX_SER_DT_RAW10),
	MAX_SER_FMT(MEDIA_BUS_FMT_SRGGB10_1X10, MAX_SER_DT_RAW10),
	MAX_SER_FMT(MEDIA_BUS_FMT_SBGGR12_1X12, MAX_SER_DT_RAW12),
	MAX_SER_FMT(MEDIA_BUS_FMT_SGBRG12_1X12, MAX_SER_DT_RAW12),
	MAX_SER_FMT(MEDIA_BUS_FMT_SGRBG12_1X12, MAX_SER_DT_RAW12),
	MAX_SER_FMT(MEDIA_BUS_FMT_SRGGB12_1X12, MAX_SER_DT_RAW12),
	MAX_SER_FMT(MEDIA_BUS_FMT_SBGGR14_1X14, MAX_SER_DT_RAW14),
	MAX_SER_FMT(MEDIA_BUS_FMT_SGBRG14_1X14, MAX_SER_DT_RAW14),
	MAX_SER_FMT(MEDIA_BUS_FMT_SGRBG14_1X14, MAX_SER_DT_RAW14),
	MAX_SER_FMT(MEDIA_BUS_FMT_SRGGB14_1X14, MAX_SER_DT_RAW14),
	MAX_SER_FMT(MEDIA_BUS_FMT_SBGGR16_1X16, MAX_SER_DT_RAW16),
	MAX_SER_FMT(MEDIA_BUS_FMT_SGBRG16_1X16, MAX_SER_DT_RAW16),
	MAX_SER_FMT(MEDIA_BUS_FMT_SGRBG16_1X16, MAX_SER_DT_RAW16),
	MAX_SER_FMT(MEDIA_BUS_FMT_SRGGB16_1X16, MAX_SER_DT_RAW16),
};

static struct max_ser_subdev_priv *next_subdev(struct max_ser_priv *priv,
						struct max_ser_subdev_priv *sd_priv)
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

static inline struct max_ser_asd *to_max_ser_asd(struct v4l2_async_subdev *asd)
{
	return container_of(asd, struct max_ser_asd, base);
}

static inline struct max_ser_subdev_priv *sd_to_max_ser(struct v4l2_subdev *sd)
{
	return container_of(sd, struct max_ser_subdev_priv, sd);
}

const struct max_ser_format *max_ser_format_by_code(u32 code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(max_ser_formats); i++)
		if (max_ser_formats[i].code == code)
			return &max_ser_formats[i];

	return NULL;
}

static const bool max_ser_format_valid(struct max_ser_priv *priv, u32 code)
{
	if (priv->tunnel_mode)
		return true;

	return max_ser_format_by_code(code);
}

static int max_ser_notify_bound(struct v4l2_async_notifier *notifier,
				 struct v4l2_subdev *subdev,
				 struct v4l2_async_subdev *asd)
{
	struct max_ser_subdev_priv *sd_priv = sd_to_max_ser(notifier->sd);
	struct max_ser_priv *priv = sd_priv->priv;
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
				    MAX_SER_SINK_PAD,
				    MEDIA_LNK_FL_ENABLED |
				    MEDIA_LNK_FL_IMMUTABLE);
	if (ret) {
		dev_err(priv->dev,
			"Unable to link %s:%u -> %s:%u\n",
			sd_priv->slave_sd->name,
			sd_priv->slave_sd_pad_id,
			sd_priv->sd.name,
			MAX_SER_SINK_PAD);
		return ret;
	}

	dev_err(priv->dev, "Bound %s:%u on %s:%u\n",
		sd_priv->slave_sd->name,
		sd_priv->slave_sd_pad_id,
		sd_priv->sd.name,
		MAX_SER_SINK_PAD);

	sd_priv->slave_sd_state = v4l2_subdev_alloc_state(subdev);
	if (IS_ERR(sd_priv->slave_sd_state))
		return PTR_ERR(sd_priv->slave_sd_state);

	/*
	 * Once all cameras have probed, increase the channel amplitude
	 * to compensate for the remote noise immunity threshold and call
	 * the camera post_register operation to complete initialization with
	 * noise immunity enabled.
	 */
	ret = v4l2_subdev_call(sd_priv->slave_sd, core, post_register);
	if (ret) {
		dev_err(priv->dev,
			"Failed to call post register for subdev %s: %d\n",
			sd_priv->slave_sd->name, ret);
		return ret;
	}

	return 0;
}

static void max_ser_notify_unbind(struct v4l2_async_notifier *notifier,
				   struct v4l2_subdev *subdev,
				   struct v4l2_async_subdev *asd)
{
	struct max_ser_subdev_priv *sd_priv = sd_to_max_ser(notifier->sd);

	sd_priv->slave_sd = NULL;
	v4l2_subdev_free_state(sd_priv->slave_sd_state);
	sd_priv->slave_sd_state = NULL;
}

static const struct v4l2_async_notifier_operations max_ser_notify_ops = {
	.bound = max_ser_notify_bound,
	.unbind = max_ser_notify_unbind,
};

static int max_ser_v4l2_notifier_register(struct max_ser_subdev_priv *sd_priv)
{
	struct max_ser_priv *priv = sd_priv->priv;
	struct max_ser_asd *mas;
	int ret;

	v4l2_async_notifier_init(&sd_priv->notifier);

	mas = (struct max_ser_asd *)
	      v4l2_async_notifier_add_fwnode_subdev(&sd_priv->notifier,
						    sd_priv->slave_fwnode, struct max_ser_asd);
	if (IS_ERR(mas)) {
		ret = PTR_ERR(mas);
		dev_err(priv->dev,
			"Failed to add subdev notifier for subdev %s: %d\n",
			sd_priv->sd.name, ret);
		goto error_cleanup_notifier;
	}

	mas->sd_priv = sd_priv;

	sd_priv->notifier.ops = &max_ser_notify_ops;
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

static int max_ser_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct max_ser_subdev_priv *sd_priv = sd_to_max_ser(sd);
	struct max_ser_priv *priv = sd_priv->priv;
	struct max_ser_pipe *pipe = max_ser_pipe_by_id(priv, sd_priv->pipe_id);
	int ret;

	ret = priv->ops->set_pipe_enable(priv, pipe, enable);
	if (ret)
		return ret;

	ret = v4l2_subdev_call(sd_priv->slave_sd, video, s_stream, enable);
	if (ret)
		dev_err(priv->dev, "Failed to start stream for %s: %d\n",
			sd_priv->slave_sd->name, ret);

	return 0;
}

static int max_ser_get_selection(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_selection *sel)
{
	struct max_ser_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_selection sd_sel = *sel;
	int ret;

	if (sel->pad != MAX_SER_SOURCE_PAD)
		return -EINVAL;

	sd_sel.pad = sd_priv->slave_sd_pad_id;

	ret = v4l2_subdev_call(sd_priv->slave_sd, pad, get_selection,
			       sd_priv->slave_sd_state, &sd_sel);
	if (ret)
		return ret;

	sel->r = sd_sel.r;

	return 0;
}

static int max_ser_fix_fmt_code(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_format *format)
{
	struct v4l2_subdev_mbus_code_enum code = {
		.pad = MAX_SER_SOURCE_PAD,
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};
	int ret;

	ret = v4l2_subdev_call(sd, pad, enum_mbus_code, sd_state, &code);
	if (ret)
		return ret;

	format->format.code = code.code;

	return 0;
}

static int max_ser_check_fmt_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_format *format)
{
	struct max_ser_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct max_ser_priv *priv = sd_priv->priv;
	struct max_ser_pipe *pipe = max_ser_pipe_by_id(priv, sd_priv->pipe_id);
	int ret;

	if (max_ser_format_valid(priv, format->format.code))
		goto set_data_type;

	ret = max_ser_fix_fmt_code(sd, sd_state, format);
	if (ret)
		return ret;

	ret = v4l2_subdev_call(sd_priv->slave_sd, pad, set_fmt,
			       sd_priv->slave_sd_state, format);
	if (ret)
		return ret;

	if (!max_ser_format_valid(priv, format->format.code))
		return -EINVAL;

set_data_type:
	/*
	 * TODO: figure out how to handle multiple DTs per pipe from multiple
	 * channels per pipe.
	 */
	ret = priv->ops->set_pipe_dt(priv, pipe, format->format.code);
	if (ret)
		return ret;

	return 0;
}

static int max_ser_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *format)
{
	struct max_ser_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_format sd_format = *format;
	int ret;

	if (format->pad != MAX_SER_SOURCE_PAD)
		return -EINVAL;

	sd_format.pad = sd_priv->slave_sd_pad_id;

	ret = v4l2_subdev_call(sd_priv->slave_sd, pad, get_fmt,
			       sd_priv->slave_sd_state, &sd_format);
	if (ret)
		return ret;

	ret = max_ser_check_fmt_code(sd, sd_state, &sd_format);
	if (ret)
		return ret;

	format->format = sd_format.format;

	return 0;
}

static int max_ser_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *format)
{
	struct max_ser_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_format sd_format = *format;
	int ret;

	if (format->pad != MAX_SER_SOURCE_PAD)
		return -EINVAL;

	sd_format.pad = sd_priv->slave_sd_pad_id;

	ret = v4l2_subdev_call(sd_priv->slave_sd, pad, set_fmt,
			       sd_priv->slave_sd_state, &sd_format);
	if (ret)
		return ret;

	ret = max_ser_check_fmt_code(sd, sd_state, &sd_format);
	if (ret)
		return ret;

	format->format = sd_format.format;

	return 0;
}

static int max_ser_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	struct max_ser_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct max_ser_priv *priv = sd_priv->priv;
	struct v4l2_subdev_mbus_code_enum sd_code = *code;
	int ret;

	if (code->pad != MAX_SER_SOURCE_PAD)
		return -EINVAL;

	sd_code.pad = sd_priv->slave_sd_pad_id;

	while (true) {
		ret = v4l2_subdev_call(sd_priv->slave_sd, pad, enum_mbus_code,
				       sd_priv->slave_sd_state, &sd_code);
		if (ret)
			return ret;

		if (max_ser_format_valid(priv, sd_code.code))
			break;

		sd_code.index++;
	}

	code->code = sd_code.code;

	return 0;
}

static int max_ser_enum_frame_size(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	struct max_ser_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct max_ser_priv *priv = sd_priv->priv;
	struct v4l2_subdev_frame_size_enum sd_fse = *fse;
	int ret;

	if (fse->pad != MAX_SER_SOURCE_PAD)
		return -EINVAL;

	sd_fse.pad = sd_priv->slave_sd_pad_id;

	while (true) {
		ret = v4l2_subdev_call(sd_priv->slave_sd, pad, enum_frame_size,
				       sd_priv->slave_sd_state, &sd_fse);
		if (ret)
			return ret;

		if (max_ser_format_valid(priv, sd_fse.code))
			break;

		sd_fse.index++;
	}

	fse->code = sd_fse.code;
	fse->min_width = sd_fse.min_width;
	fse->max_width = sd_fse.max_width;
	fse->min_height = sd_fse.min_height;
	fse->max_height = sd_fse.max_height;

	return 0;
}

static int max_ser_enum_frame_interval(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *sd_state,
				       struct v4l2_subdev_frame_interval_enum *fie)
{
	struct max_ser_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct max_ser_priv *priv = sd_priv->priv;
	struct v4l2_subdev_frame_interval_enum sd_fie = *fie;
	int ret;

	if (fie->pad != MAX_SER_SOURCE_PAD)
		return -EINVAL;

	sd_fie.pad = sd_priv->slave_sd_pad_id;

	while (true) {
		ret = v4l2_subdev_call(sd_priv->slave_sd, pad, enum_frame_interval,
				       sd_priv->slave_sd_state, &sd_fie);
		if (ret)
			return ret;

		if (max_ser_format_valid(priv, sd_fie.code))
			break;

		sd_fie.index++;
	}

	fie->code = sd_fie.code;
	fie->width = sd_fie.width;
	fie->height = sd_fie.height;
	fie->interval = sd_fie.interval;

	return 0;
}

static int max_ser_post_register(struct v4l2_subdev *sd)
{
	return 0;
}

static const struct v4l2_subdev_video_ops max_ser_video_ops = {
	.s_stream	= max_ser_s_stream,
};

static const struct v4l2_subdev_pad_ops max_ser_pad_ops = {
	.get_selection = max_ser_get_selection,
	.get_fmt = max_ser_get_fmt,
	.set_fmt = max_ser_set_fmt,
	.enum_mbus_code = max_ser_enum_mbus_code,
	.enum_frame_size = max_ser_enum_frame_size,
	.enum_frame_interval = max_ser_enum_frame_interval,
};

static const struct v4l2_subdev_core_ops max_ser_core_ops = {
	.post_register	= max_ser_post_register,
};

static const struct v4l2_subdev_ops max_ser_subdev_ops = {
	.core		= &max_ser_core_ops,
	.video		= &max_ser_video_ops,
	.pad		= &max_ser_pad_ops,
};

static int max_ser_init(struct max_ser_priv *priv)
{
	unsigned int i;
	int ret;

	ret = priv->ops->init(priv);
	if (ret)
		return ret;

	if (priv->ops->set_tunnel_mode && priv->tunnel_mode) {
		ret = priv->ops->set_tunnel_mode(priv);
		if (ret)
			return ret;
	}

	for (i = 0; i < priv->ops->num_phys; i++) {
		struct max_ser_phy *phy = &priv->phys[i];

		if (!phy->enabled)
			continue;

		ret = priv->ops->init_phy(priv, phy);
		if (ret)
			return ret;
	}

	for (i = 0; i < priv->ops->num_pipes; i++) {
		struct max_ser_pipe *pipe = &priv->pipes[i];

		if (!pipe->enabled)
			continue;

		ret = priv->ops->init_pipe(priv, pipe);
		if (ret)
			return ret;
	}

	ret = priv->ops->post_init(priv);
	if (ret)
		return ret;

	return 0;
}

static int max_ser_v4l2_register_sd(struct max_ser_subdev_priv *sd_priv)
{
	struct max_ser_priv *priv = sd_priv->priv;
	unsigned int index = sd_priv->index;
	char postfix[3];
	int ret;

	ret = max_ser_v4l2_notifier_register(sd_priv);
	if (ret)
		return ret;

	snprintf(postfix, sizeof(postfix), ":%d", index);

	v4l2_i2c_subdev_init(&sd_priv->sd, priv->client, &max_ser_subdev_ops);
	v4l2_i2c_subdev_set_name(&sd_priv->sd, priv->client, NULL, postfix);
	sd_priv->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	sd_priv->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sd_priv->sd.fwnode = sd_priv->fwnode;

	sd_priv->pads[MAX_SER_SOURCE_PAD].flags = MEDIA_PAD_FL_SOURCE;
	sd_priv->pads[MAX_SER_SINK_PAD].flags = MEDIA_PAD_FL_SINK;

	ret = media_entity_pads_init(&sd_priv->sd.entity, MAX_SER_PAD_NUM, sd_priv->pads);
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

static void max_ser_v4l2_unregister_sd(struct max_ser_subdev_priv *sd_priv)
{
	v4l2_async_notifier_unregister(&sd_priv->notifier);
	v4l2_async_notifier_cleanup(&sd_priv->notifier);
	v4l2_async_unregister_subdev(&sd_priv->sd);
	media_entity_cleanup(&sd_priv->sd.entity);
	fwnode_handle_put(sd_priv->sd.fwnode);
}

static int max_ser_v4l2_register(struct max_ser_priv *priv)
{
	struct max_ser_subdev_priv *sd_priv;
	int ret;

	for_each_subdev(priv, sd_priv) {
		ret = max_ser_v4l2_register_sd(sd_priv);
		if (ret)
			return ret;
	}

	return 0;
}

static void max_ser_v4l2_unregister(struct max_ser_priv *priv)
{
	struct max_ser_subdev_priv *sd_priv;

	for_each_subdev(priv, sd_priv)
		max_ser_v4l2_unregister_sd(sd_priv);
}

static int max_ser_parse_pipe_dt(struct max_ser_priv *priv,
				 struct max_ser_pipe *pipe,
				 struct fwnode_handle *fwnode)
{
	unsigned int val;

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

	return 0;
}

static int max_ser_parse_ch_dt(struct max_ser_subdev_priv *sd_priv,
			       struct fwnode_handle *fwnode)
{
	struct max_ser_priv *priv = sd_priv->priv;
	struct max_ser_pipe *pipe;
	struct max_ser_phy *phy;
	u32 val;

	val = sd_priv->index;
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

	return 0;
}

static int max_ser_parse_src_dt_endpoint(struct max_ser_subdev_priv *sd_priv,
					  struct fwnode_handle *fwnode)
{
	struct max_ser_priv *priv = sd_priv->priv;
	struct fwnode_handle *ep;

	ep = fwnode_graph_get_endpoint_by_id(fwnode, MAX_SER_SOURCE_PAD, 0, 0);
	if (!ep) {
		dev_err(priv->dev, "Not connected to subdevice\n");
		return -EINVAL;
	}

	return 0;
}

static int max_ser_parse_sink_dt_endpoint(struct max_ser_subdev_priv *sd_priv,
					  struct fwnode_handle *fwnode)
{
	struct max_ser_priv *priv = sd_priv->priv;
	struct max_ser_pipe *pipe = &priv->pipes[sd_priv->pipe_id];
	struct max_ser_phy *phy = &priv->phys[pipe->phy_id];
	struct v4l2_fwnode_endpoint v4l2_ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	struct fwnode_handle *ep, *remote_ep;
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(fwnode, MAX_SER_SINK_PAD, 0, 0);
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

	sd_priv->slave_fwnode = remote_ep;

	return 0;
}

static int max_ser_parse_dt(struct max_ser_priv *priv)
{
	const char *channel_node_name = "channel";
	const char *pipe_node_name = "pipe";
	struct max_ser_subdev_priv *sd_priv;
	struct fwnode_handle *fwnode;
	struct max_ser_pipe *pipe;
	struct max_ser_phy *phy;
	unsigned int i;
	u32 index;
	int ret;

	if (priv->ops->set_tunnel_mode)
		priv->tunnel_mode = device_property_read_bool(priv->dev, "max,tunnel-mode");

	for (i = 0; i < priv->ops->num_phys; i++) {
		phy = &priv->phys[i];
		phy->index = i;
	}

	for (i = 0; i < priv->ops->num_pipes; i++) {
		pipe = &priv->pipes[i];
		pipe->index = i;
		/*
		 * Serializer chips usually have more pipes than PHYs,
		 * make sure each pipe gets a valid PHY.
		 */
		pipe->phy_id = i / priv->ops->num_phys;
		pipe->stream_id = i;
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
			dev_err(priv->dev, "Invalid pipe number %u\n", index);
			return -EINVAL;
		}

		pipe = &priv->pipes[index];

		ret = max_ser_parse_pipe_dt(priv, pipe, fwnode);
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

		ret = max_ser_parse_ch_dt(sd_priv, fwnode);
		if (ret)
			return ret;

		ret = max_ser_parse_sink_dt_endpoint(sd_priv, fwnode);
		if (ret)
			return ret;

		ret = max_ser_parse_src_dt_endpoint(sd_priv, fwnode);
		if (ret)
			return ret;
	}

	return 0;
}

static int max_ser_allocate(struct max_ser_priv *priv)
{
	priv->phys = devm_kcalloc(priv->dev, priv->ops->num_phys,
				  sizeof(*priv->phys), GFP_KERNEL);
	if (!priv->phys)
		return -ENOMEM;

	priv->pipes = devm_kcalloc(priv->dev, priv->ops->num_pipes,
				   sizeof(*priv->pipes), GFP_KERNEL);
	if (!priv->pipes)
		return -ENOMEM;

	return 0;
}

int max_ser_probe(struct max_ser_priv *priv)
{
	int ret;

	ret = max_ser_allocate(priv);
	if (ret)
		return ret;

	ret = max_ser_parse_dt(priv);
	if (ret)
		return ret;

	ret = max_ser_init(priv);
	if (ret)
		return ret;

	return max_ser_v4l2_register(priv);
}
EXPORT_SYMBOL_GPL(max_ser_probe);

int max_ser_remove(struct max_ser_priv *priv)
{
	max_ser_v4l2_unregister(priv);

	return 0;
}
EXPORT_SYMBOL_GPL(max_ser_remove);

int max_ser_reset(struct regmap *regmap)
{
	return regmap_update_bits(regmap, 0x10, 0x80, 0x80);
}
EXPORT_SYMBOL_GPL(max_ser_reset);

int max_ser_wait_for_multiple(struct i2c_client *client, struct regmap *regmap,
			      u8 *addrs, unsigned int num_addrs)
{
	unsigned int i, j, val;
	int ret;

	for (i = 0; i < 100; i++) {
		for (j = 0; j < num_addrs; j++) {
			client->addr = addrs[j];

			ret = regmap_read(regmap, 0x0, &val);
			if (ret >= 0)
				return 0;
		}

		msleep(10);

		dev_err(&client->dev, "Retry %u waiting for serializer: %d\n", i, ret);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(max_ser_wait_for_multiple);

int max_ser_wait(struct i2c_client *client, struct regmap *regmap, u8 addr)
{
	return max_ser_wait_for_multiple(client, regmap, &addr, 1);
}
EXPORT_SYMBOL_GPL(max_ser_wait);

int max_ser_change_address(struct i2c_client *client, struct regmap *regmap, u8 addr)
{
	unsigned int dev_id;
	int ret;

	ret = regmap_read(regmap, 0xd, &dev_id);
	if (ret)
		return ret;

	ret = regmap_write(regmap, 0x0, addr << 1);
	if (ret)
		return ret;

	client->addr = addr;

	switch (dev_id) {
	case MAX_SER_MAX96717_DEV_ID:
		return 0;
	case MAX_SER_MAX9265A_DEV_ID: {
		unsigned int addr_regs[] = { 0x7b, 0x83, 0x8b, 0x93, 0xa3, 0xab };
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(addr_regs); i++) {
			ret = regmap_write(regmap, addr_regs[i], addr);
			if (ret)
				return ret;
		}

		break;
	}
	default:
		return 0;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(max_ser_change_address);

int max_ser_init_i2c_xlate(struct regmap *regmap, unsigned int i,
			   struct max_i2c_xlate *i2c_xlate)
{
	unsigned int addr = 0x42 + 0x2 * i;
	int ret;

	ret = regmap_write(regmap, addr, i2c_xlate->dst << 1);
	if (ret)
		return ret;

	ret = regmap_write(regmap, addr + 0x1, i2c_xlate->src << 1);
	if (ret)
		return ret;

	return 0;
}
EXPORT_SYMBOL_GPL(max_ser_init_i2c_xlate);
