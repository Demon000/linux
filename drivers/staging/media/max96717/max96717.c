// SPDX-License-Identifier: GPL-2.0+

#include <linux/delay.h>
#include <linux/fwnode.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>

#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define MAX96717_SOURCE_PAD	0
#define MAX96717_SINK_PAD	1

enum max96717_ctrls {
	MAX96717_TEST_PATTERN_CTRL = V4L2_CTRL_CLASS_IMAGE_PROC | 0x1201,
};

enum max96717_pattern {
	MAX96717_PATTERN_NONE = 0,
	MAX96717_PATTERN_CHECKERBOARD,
	MAX96717_PATTERN_GRADIENT,
};

struct max96717_priv {
	struct device *dev;
	struct i2c_client *client;
	struct regmap *regmap;
	struct v4l2_subdev sd;
	struct media_pad pads[2];
	struct v4l2_async_notifier notifier;
	struct v4l2_async_subdev *asd;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_subdev *sensor;
	struct v4l2_subdev_state *sensor_state;
	unsigned int sensor_pad_id;

	enum max96717_pattern pattern;
};

static inline struct max96717_priv *sd_to_max96717(struct v4l2_subdev *sd)
{
	return container_of(sd, struct max96717_priv, sd);
}

static inline struct max96717_priv *i2c_to_max96717(struct i2c_client *client)
{
	return sd_to_max96717(i2c_get_clientdata(client));
}

static inline struct max96717_priv *notifier_to_max96717(struct v4l2_async_notifier *nf)
{
	return container_of(nf, struct max96717_priv, notifier);
}


static int max96717_write(struct max96717_priv *priv, unsigned int reg, u8 val)
{
	int ret;

	ret = regmap_write(priv->regmap, reg, val);
	if (ret)
		dev_err(priv->dev, "write 0x%04x failed\n", reg);

	return ret;
}

static int max96717_update_bits(struct max96717_priv *priv, unsigned int reg,
				u8 mask, u8 val)
{
	unsigned int retry = 100;
	int ret;

retry:
	ret = regmap_update_bits(priv->regmap, reg, mask, val);
	if (ret)
		dev_err(priv->dev, "update 0x%04x failed\n", reg);

	if (ret && retry != 0) {
		retry--;
		udelay(1000);
		goto retry;
	}

	return ret;
}

static int max96717_write_bulk(struct max96717_priv *priv, unsigned int reg,
			       const void *val, size_t val_count)
{
	int ret;

	ret = regmap_bulk_write(priv->regmap, reg, val, val_count);
	if (ret)
		dev_err(priv->dev, "bulk write 0x%04x failed\n", reg);

	return ret;
}

static int max96717_write_bulk_value(struct max96717_priv *priv,
				     unsigned int reg, unsigned int val,
				     size_t val_count)
{
	unsigned int i;
	u8 values[4];

	for (i = 1; i <= val_count; i++)
		values[i - 1] = (val >> ((val_count - i) * 8)) & 0xff;

	return max96717_write_bulk(priv, reg, &values, val_count);
}

static void max96717_pattern_enable(struct max96717_priv *priv, bool enable)
{
	const u32 h_active = 1920;
	const u32 h_fp = 88;
	const u32 h_sw = 44;
	const u32 h_bp = 148;
	const u32 h_tot = h_active + h_fp + h_sw + h_bp;

	const u32 v_active = 1080;
	const u32 v_fp = 4;
	const u32 v_sw = 5;
	const u32 v_bp = 36;
	const u32 v_tot = v_active + v_fp + v_sw + v_bp;

	if (!enable || priv->pattern == MAX96717_PATTERN_NONE) {
		max96717_update_bits(priv, 0x026b, 0x03, 0x00);
		return;
	}

	/* PCLK 75MHz. */
	max96717_update_bits(priv, 0x024f, 0x0e, 0x0a);

	/* Configure Video Timing Generator for 1920x1080 @ 30 fps. */
	max96717_write_bulk_value(priv, 0x0250, 0, 3);
	max96717_write_bulk_value(priv, 0x0253, v_sw * h_tot, 3);
	max96717_write_bulk_value(priv, 0x0256,
				  (v_active + v_fp + v_bp) * h_tot, 3);
	max96717_write_bulk_value(priv, 0x0259, 0, 3);
	max96717_write_bulk_value(priv, 0x025c, h_sw, 2);
	max96717_write_bulk_value(priv, 0x025e, h_active + h_fp + h_bp, 2);
	max96717_write_bulk_value(priv, 0x0260, v_tot, 2);
	max96717_write_bulk_value(priv, 0x0262,
				  h_tot * (v_sw + v_bp) + (h_sw + h_bp), 3);
	max96717_write_bulk_value(priv, 0x0265, h_active, 2);
	max96717_write_bulk_value(priv, 0x0267, h_fp + h_sw + h_bp, 2);
	max96717_write_bulk_value(priv, 0x0269, v_active, 2);

	/* Generate VS, HS and DE in free-running mode. */
	max96717_write(priv, 0x024e, 0xf3);

	/* Configure Video Pattern Generator. */
	if (priv->pattern == MAX96717_PATTERN_CHECKERBOARD) {
		/* Set checkerboard pattern size. */
		max96717_write(priv, 0x0273, 0x3c);
		max96717_write(priv, 0x0274, 0x3c);
		max96717_write(priv, 0x0275, 0x3c);

		/* Set checkerboard pattern colors. */
		max96717_write_bulk_value(priv, 0x026d, 0x00df3f, 3);
		max96717_write_bulk_value(priv, 0x0271, 0x000000, 3);

		/* Generate checkerboard pattern. */
		max96717_update_bits(priv, 0x026b, 0x03, 0x1);
	} else {
		/* Set gradient increment. */
		max96717_write(priv, 0x026c, 0x10);

		/* Generate gradient pattern. */
		max96717_update_bits(priv, 0x026b, 0x03, 0x2);
	}
}

static void max96717_tunnel_enable(struct max96717_priv *priv, bool enable)
{
	if (enable && priv->pattern == MAX96717_PATTERN_NONE) {
		dev_err(priv->dev, "configure tunnel\n");
		/* Select tunnel mode. */
		max96717_update_bits(priv, 0x0383, 0x80, 0x80);
	} else {
		dev_err(priv->dev, "configure pixel\n");
		/* Disable Auto BPP mode. */
		max96717_update_bits(priv, 0x0110, 0x08, 0x00);

		/* Select pixel mode. */
		max96717_update_bits(priv, 0x0383, 0x80, 0x00);
	}
}

static int max96717_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct max96717_priv *priv = sd_to_max96717(sd);
	int ret;

	dev_err(priv->dev, "s_stream: %u\n", enable);

	max96717_pattern_enable(priv, enable);
	max96717_tunnel_enable(priv, enable);

	if (priv->pattern == MAX96717_PATTERN_NONE) {
		ret = v4l2_subdev_call(priv->sensor, video, s_stream, enable);
		if (ret)
			dev_err(priv->dev, "Failed to start stream for camera device %d\n", ret);

		return ret;
	}

	return 0;
}

static int max96717_get_selection(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_selection *sel)
{
	struct max96717_priv *priv = sd_to_max96717(sd);
	struct v4l2_subdev_selection sd_sel = *sel;
	int ret;

	if (priv->pattern != MAX96717_PATTERN_NONE) {
		return 0;
	}

	sd_sel.pad = priv->sensor_pad_id;

	ret = v4l2_subdev_call(priv->sensor, pad, get_selection, priv->sensor_state, &sd_sel);
	if (ret)
		return ret;

	sel->r = sd_sel.r;

	return 0;
}

static int max96717_get_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *sd_state,
			    struct v4l2_subdev_format *format)
{
	struct max96717_priv *priv = sd_to_max96717(sd);
	struct v4l2_subdev_format sd_format = *format;
	int ret;

	if (priv->pattern != MAX96717_PATTERN_NONE) {
		format->format.width = 1920;
		format->format.height = 1080;
		format->format.code = MEDIA_BUS_FMT_RGB888_1X24;
		format->format.field = V4L2_FIELD_NONE;

		return 0;
	}

	sd_format.pad = priv->sensor_pad_id;

	ret = v4l2_subdev_call(priv->sensor, pad, get_fmt, priv->sensor_state, &sd_format);
	if (ret)
		return ret;

	format->format = sd_format.format;

	return 0;
}

static int max96717_set_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *sd_state,
			    struct v4l2_subdev_format *format)
{
	struct max96717_priv *priv = sd_to_max96717(sd);
	struct v4l2_subdev_format sd_format = *format;
	int ret;

	if (priv->pattern != MAX96717_PATTERN_NONE) {
		format->format.width = 1920;
		format->format.height = 1080;
		format->format.code = MEDIA_BUS_FMT_RGB888_1X24;
		format->format.field = V4L2_FIELD_NONE;

		return 0;
	}

	sd_format.pad = priv->sensor_pad_id;

	ret = v4l2_subdev_call(priv->sensor, pad, set_fmt, priv->sensor_state, &sd_format);
	if (ret)
		return ret;

	return 0;
}

static int max96717_enum_mbus_code(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_mbus_code_enum *code)
{
	struct max96717_priv *priv = sd_to_max96717(sd);
	struct v4l2_subdev_mbus_code_enum sd_code = *code;
	int ret;

	if (priv->pattern != MAX96717_PATTERN_NONE) {
		code->code = MEDIA_BUS_FMT_RGB888_1X24;

		return 0;
	}

	sd_code.pad = priv->sensor_pad_id;

	ret = v4l2_subdev_call(priv->sensor, pad, enum_mbus_code, priv->sensor_state, &sd_code);
	if (ret)
		return ret;

	code->code = sd_code.code;

	return 0;
}

static int max96717_enum_frame_size(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *sd_state,
				    struct v4l2_subdev_frame_size_enum *fse)
{
	struct max96717_priv *priv = sd_to_max96717(sd);
	struct v4l2_subdev_frame_size_enum sd_fse = *fse;
	int ret;

	if (priv->pattern != MAX96717_PATTERN_NONE) {
		fse->code = MEDIA_BUS_FMT_RGB888_1X24;

		return 0;
	}

	sd_fse.pad = priv->sensor_pad_id;

	ret = v4l2_subdev_call(priv->sensor, pad, enum_frame_size, priv->sensor_state, &sd_fse);
	if (ret)
		return ret;

	fse->code = sd_fse.code;
	fse->min_width = sd_fse.min_width;
	fse->max_width = sd_fse.max_width;
	fse->min_height = sd_fse.min_height;
	fse->max_height = sd_fse.max_height;

	return 0;
}

static int max96717_post_register(struct v4l2_subdev *sd)
{
	return 0;
}

static const char * const max96717_test_pattern[] = {
	"None",
	"Checkerboard",
	"Gradient",
};

static int max96717_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct max96717_priv *priv =
		container_of(ctrl->handler, struct max96717_priv, ctrl_handler);

	switch (ctrl->id) {
	case MAX96717_TEST_PATTERN_CTRL:
		priv->pattern = ctrl->val;
		break;
	}
	return 0;
}

static const struct v4l2_ctrl_ops max96717_ctrl_ops = {
	.s_ctrl = max96717_s_ctrl,
};

static const struct v4l2_subdev_video_ops max96717_video_ops = {
	.s_stream	= max96717_s_stream,
};

static const struct v4l2_subdev_pad_ops max96717_pad_ops = {
	.get_selection = max96717_get_selection,
	.get_fmt = max96717_get_fmt,
	.set_fmt = max96717_set_fmt,
	.enum_mbus_code = max96717_enum_mbus_code,
	.enum_frame_size = max96717_enum_frame_size,
};

static const struct v4l2_subdev_core_ops max96717_core_ops = {
	.post_register	= max96717_post_register,
};

static const struct v4l2_subdev_ops max96717_subdev_ops = {
	.core		= &max96717_core_ops,
	.video		= &max96717_video_ops,
	.pad		= &max96717_pad_ops,
};

static int max96717_notify_bound(struct v4l2_async_notifier *notifier,
				 struct v4l2_subdev *subdev,
				 struct v4l2_async_subdev *asd)
{
	struct max96717_priv *priv = notifier_to_max96717(notifier);
	int ret, pad;

	/* Create media link with the remote sensor source pad. */
	pad = media_entity_get_fwnode_pad(&subdev->entity, asd->match.fwnode,
					  MEDIA_PAD_FL_SOURCE);
	if (pad < 0) {
		dev_err(priv->dev,
			"Failed to find source pad for %s\n", subdev->name);
		return pad;
	}

	ret = media_create_pad_link(&subdev->entity, pad,
				    &priv->sd.entity, MAX96717_SINK_PAD,
				    MEDIA_LNK_FL_ENABLED |
				    MEDIA_LNK_FL_IMMUTABLE);
	if (ret)
		return ret;

	priv->sensor_pad_id = pad;
	priv->sensor = subdev;

	priv->sensor_state = v4l2_subdev_alloc_state(subdev);
	if (IS_ERR(priv->sensor_state))
		return PTR_ERR(priv->sensor_state);

	dev_err(priv->dev, "Calling post_register\n");

	/*
	 * Call the sensor post_register operation to complete its
	 * initialization.
	 */
	ret = v4l2_subdev_call(priv->sensor, core, post_register);
	if (ret) {
		dev_err(priv->dev, "Failed to initialize sensor %d\n", ret);
		goto error_remove_link;
	}

	/* Add controls from the subdevice */
	ret = v4l2_ctrl_add_handler(&priv->ctrl_handler,
				    priv->sensor->ctrl_handler,
				    NULL, true);
	if (ret) {
		dev_err(priv->dev, "Failed to add subdevice controls %d\n", ret);
		goto error_remove_link;
	}

	return 0;

error_remove_link:
	media_entity_remove_links(&priv->sd.entity);
	priv->sensor = NULL;

	return ret;
}

static void max96717_notify_unbind(struct v4l2_async_notifier *notifier,
				   struct v4l2_subdev *subdev,
				   struct v4l2_async_subdev *asd)
{
	struct max96717_priv *priv = notifier_to_max96717(notifier);

	media_entity_remove_links(&priv->sd.entity);
	priv->sensor = NULL;
	v4l2_subdev_free_state(priv->sensor_state);
}

static const struct v4l2_async_notifier_operations max96717_notifier_ops = {
	.bound = max96717_notify_bound,
	.unbind = max96717_notify_unbind,
};

static int max96717_parse_dt(struct max96717_priv *priv)
{
	struct fwnode_handle *ep, *remote;
	struct v4l2_fwnode_endpoint vep = {
		.bus_type = V4L2_MBUS_PARALLEL,
	};
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(priv->dev), 1, 0, 0);
	if (!ep) {
		dev_err(priv->dev, "Unable to get sensor endpoint: %pOF\n",
			priv->dev->of_node);
		return -ENOENT;
	}

	dev_err(priv->dev, "%s:%u: sensor endpoint: %pfw\n", __func__, __LINE__, ep);

	remote = fwnode_graph_get_remote_endpoint(ep);
	if (!remote) {
		dev_err(priv->dev, "Unable to get remote endpoint: %pOF\n",
			priv->dev->of_node);
		return -ENOENT;
	}

	dev_err(priv->dev, "%s:%u: remote endpoint: %pfw\n", __func__, __LINE__, remote);

	ret = v4l2_fwnode_endpoint_parse(ep, &vep);
	fwnode_handle_put(ep);
	if (ret) {
		fwnode_handle_put(remote);
		dev_err(priv->dev, "Unable to parse endpoint: %pOF\n",
			to_of_node(ep));
		return ret;
	}

	v4l2_async_notifier_init(&priv->notifier);
	priv->asd = v4l2_async_notifier_add_fwnode_subdev(&priv->notifier,
					      remote, struct v4l2_async_subdev);
	fwnode_handle_put(remote);
	if (IS_ERR(priv->asd))
		return PTR_ERR(priv->asd);

	priv->notifier.ops = &max96717_notifier_ops;
	priv->notifier.flags = V4L2_ASYNC_NOTIFIER_DEFER_POST_REGISTER;
	ret = v4l2_async_subdev_notifier_register(&priv->sd,
						  &priv->notifier);
	if (ret < 0) {
		v4l2_async_notifier_cleanup(&priv->notifier);
		return ret;
	}

	return 0;
}

static int max96717_init(struct max96717_priv *priv)
{
	int ret;

	ret = max96717_update_bits(priv, 0x0302, 0x70, 0x10);
	if (ret)
		return ret;

	ret = max96717_update_bits(priv, 0x0331, 0x30, 0x10);
	if (ret)
		return ret;

	/*
	 * Enable forwarding of GPIO 0.
	 */

	dev_err(priv->dev, "enable forwarding gpio 0\n");

	/* GPIO_A GPIO_RX_EN 1 */
	max96717_update_bits(priv, 0x2be, 0x4, 0x4);
	/* GPIO_A GPIO_OUT_DIS 0 */
	max96717_update_bits(priv, 0x2be, 0x1, 0x0);

	return 0;
}

static const struct regmap_config max96717_i2c_regmap = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0x1f00,
};

static const struct v4l2_ctrl_config max96717_test_pattern_ctrl = {
	.ops = &max96717_ctrl_ops,
	.id = MAX96717_TEST_PATTERN_CTRL,
	.name = "Serializer test pattern",
	.type = V4L2_CTRL_TYPE_MENU,
	.min = 0,
	.max = ARRAY_SIZE(max96717_test_pattern) - 1,
	.def = 0,
	.qmenu = max96717_test_pattern,
};

static int max96717_probe(struct i2c_client *client)
{
	struct max96717_priv *priv;
	struct fwnode_handle *ep;
	int ret;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = &client->dev;
	priv->client = client;

	priv->regmap = devm_regmap_init_i2c(client, &max96717_i2c_regmap);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	/* Initialize and register the subdevice. */
	v4l2_i2c_subdev_init(&priv->sd, client, &max96717_subdev_ops);
	priv->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	priv->pads[MAX96717_SOURCE_PAD].flags = MEDIA_PAD_FL_SOURCE;
	priv->pads[MAX96717_SINK_PAD].flags = MEDIA_PAD_FL_SINK;
	priv->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	priv->sd.entity.flags |= MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER;

	v4l2_ctrl_handler_init(&priv->ctrl_handler, 1);

	v4l2_ctrl_new_custom(&priv->ctrl_handler, &max96717_test_pattern_ctrl, NULL);

	priv->sd.ctrl_handler = &priv->ctrl_handler;
	ret = priv->ctrl_handler.error;
	if (ret)
		return ret;

	ret = media_entity_pads_init(&priv->sd.entity, 2, priv->pads);
	if (ret < 0)
		goto error_handler_free;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(priv->dev), 0, 0, 0);
	if (!ep) {
		dev_err(priv->dev, "Unable to get endpoint 0: %pOF\n",
			priv->dev->of_node);
		ret = -ENODEV;
		goto error_media_entity;
	}

	dev_err(priv->dev, "%s:%u: endpoint: %pfw\n", __func__, __LINE__, ep);

	ret = max96717_init(priv);
	if (ret)
		goto error_put_node;

	ret = max96717_parse_dt(priv);
	if (ret)
		goto error_put_node;

	priv->sd.fwnode = ep;
	ret = v4l2_async_register_subdev(&priv->sd);
	if (ret)
		goto error_put_node;

	return 0;

error_put_node:
	fwnode_handle_put(priv->sd.fwnode);
error_media_entity:
	media_entity_cleanup(&priv->sd.entity);
error_handler_free:
	v4l2_ctrl_handler_free(&priv->ctrl_handler);

	return ret;
}

static int max96717_remove(struct i2c_client *client)
{
	struct max96717_priv *priv = i2c_to_max96717(client);

	v4l2_async_notifier_cleanup(&priv->notifier);
	v4l2_async_unregister_subdev(&priv->sd);
	fwnode_handle_put(priv->sd.fwnode);
	media_entity_cleanup(&priv->sd.entity);

	return 0;
}

static const struct of_device_id max96717_of_ids[] = {
	{ .compatible = "maxim,max96717", },
	{ }
};
MODULE_DEVICE_TABLE(of, max96717_of_ids);

static struct i2c_driver max96717_i2c_driver = {
	.driver	= {
		.name	= "max96717",
		.of_match_table = max96717_of_ids,
	},
	.probe_new	= max96717_probe,
	.remove		= max96717_remove,
};

module_i2c_driver(max96717_i2c_driver);

MODULE_DESCRIPTION("MAX96717 GMSL serializer subdevice driver");
MODULE_AUTHOR("Cosmin Tanislav <cosmin.tanislav@analog.com>");
MODULE_LICENSE("GPL");
