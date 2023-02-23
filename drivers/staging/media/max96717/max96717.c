// SPDX-License-Identifier: GPL-2.0+

#include <linux/delay.h>
#include <linux/fwnode.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/property.h>

#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

struct max96717_device {
	struct device			*dev;
	struct i2c_client		*client;
	struct v4l2_subdev		sd;
#define MAX96717_SOURCE_PAD	0
#define MAX96717_SINK_PAD	1
	struct media_pad		pads[2];
	struct v4l2_async_notifier	notifier;
	struct v4l2_async_subdev	*asd;
	struct v4l2_ctrl_handler	ctrls;
	struct v4l2_subdev		*sensor;
};

static inline struct max96717_device *sd_to_max96717(struct v4l2_subdev *sd)
{
	return container_of(sd, struct max96717_device, sd);
}

static inline struct max96717_device *i2c_to_max96717(struct i2c_client *client)
{
	return sd_to_max96717(i2c_get_clientdata(client));
}

static inline struct max96717_device *notifier_to_max96717(
						struct v4l2_async_notifier *nf)
{
	return container_of(nf, struct max96717_device, notifier);
}

static int max96717_s_stream(struct v4l2_subdev *sd, int enable)
{
	return 0;
}

static int max96717_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	struct max96717_device *max96717 = sd_to_max96717(sd);

	return v4l2_subdev_call(max96717->sensor, pad, enum_mbus_code, NULL,
				code);
}

static int max96717_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *format)
{
	struct max96717_device *max96717 = sd_to_max96717(sd);

	return v4l2_subdev_call(max96717->sensor, pad, get_fmt, NULL,
				format);
}

static int max96717_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *format)
{
	struct max96717_device *max96717 = sd_to_max96717(sd);

	return v4l2_subdev_call(max96717->sensor, pad, set_fmt, NULL,
				format);
}

static int max96717_post_register(struct v4l2_subdev *sd)
{
	return 0;
}

static const struct v4l2_subdev_video_ops max96717_video_ops = {
	.s_stream	= max96717_s_stream,
};

static const struct v4l2_subdev_pad_ops max96717_subdev_pad_ops = {
	.enum_mbus_code = max96717_enum_mbus_code,
	.get_fmt	= max96717_get_fmt,
	.set_fmt	= max96717_set_fmt,
};

static const struct v4l2_subdev_core_ops max96717_core_ops = {
	.post_register	= max96717_post_register,
};

static const struct v4l2_subdev_ops max96717_subdev_ops = {
	.core		= &max96717_core_ops,
	.video		= &max96717_video_ops,
	.pad		= &max96717_subdev_pad_ops,
};

static int max96717_notify_bound(struct v4l2_async_notifier *notifier,
				struct v4l2_subdev *subdev,
				struct v4l2_async_subdev *asd)
{
	struct max96717_device *max96717 = notifier_to_max96717(notifier);
	int ret, pad;

	/*
	 * Reserve more space than necessary for controls inherited by the
	 * remote subdev.
	 */
	ret = v4l2_ctrl_handler_init(&max96717->ctrls, 16);
	if (ret < 0) {
		dev_err(max96717->dev,
			"Unable to initialize control handler: %d\n", ret);
		return ret;
	}

	ret = v4l2_ctrl_add_handler(&max96717->ctrls, subdev->ctrl_handler,
				    NULL, true);
	if (ret < 0) {
		dev_err(max96717->dev,
			"Unable to add subdev control handler: %d\n", ret);
		goto error_free_handler;
	}
	max96717->sd.ctrl_handler = &max96717->ctrls;

	/* Create media link with the remote sensor source pad. */
	pad = media_entity_get_fwnode_pad(&subdev->entity, asd->match.fwnode,
					  MEDIA_PAD_FL_SOURCE);
	if (pad < 0) {
		dev_err(max96717->dev,
			"Failed to find source pad for %s\n", subdev->name);
		ret = pad;
		goto error_free_handler;
	}

	ret = media_create_pad_link(&subdev->entity, pad,
				    &max96717->sd.entity, MAX96717_SINK_PAD,
				    MEDIA_LNK_FL_ENABLED |
				    MEDIA_LNK_FL_IMMUTABLE);
	if (ret)
		goto error_free_handler;

	max96717->sensor = subdev;

	/*
	 * Call the sensor post_register operation to complete its
	 * initialization.
	 */
	ret = v4l2_subdev_call(max96717->sensor, core, post_register);
	if (ret) {
		dev_err(max96717->dev, "Failed to initialize sensor %u\n", ret);
		goto error_remove_link;
	}

	return 0;

error_remove_link:
	media_entity_remove_links(&max96717->sd.entity);
	max96717->sensor = NULL;

error_free_handler:
	v4l2_ctrl_handler_free(&max96717->ctrls);
	max96717->sd.ctrl_handler = NULL;

	return ret;
}

static void max96717_notify_unbind(struct v4l2_async_notifier *notifier,
				  struct v4l2_subdev *subdev,
				  struct v4l2_async_subdev *asd)
{
	struct max96717_device *max96717 = notifier_to_max96717(notifier);

	media_entity_remove_links(&max96717->sd.entity);
	max96717->sensor = NULL;
}

static const struct v4l2_async_notifier_operations max96717_notifier_ops = {
	.bound = max96717_notify_bound,
	.unbind = max96717_notify_unbind,
};

static int max96717_parse_dt(struct max96717_device *max96717)
{
	struct fwnode_handle *ep, *remote;
	struct v4l2_fwnode_endpoint vep = {
		.bus_type = V4L2_MBUS_PARALLEL,
	};
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(max96717->dev), 1, 0, 0);
	if (!ep) {
		dev_err(max96717->dev, "Unable to get sensor endpoint: %pOF\n",
			max96717->dev->of_node);
		return -ENOENT;
	}

	remote = fwnode_graph_get_remote_endpoint(ep);
	if (!remote) {
		dev_err(max96717->dev, "Unable to get remote endpoint: %pOF\n",
			max96717->dev->of_node);
		return -ENOENT;
	}

	ret = v4l2_fwnode_endpoint_parse(ep, &vep);
	fwnode_handle_put(ep);
	if (ret) {
		fwnode_handle_put(remote);
		dev_err(max96717->dev, "Unable to parse endpoint: %pOF\n",
			to_of_node(ep));
		return ret;
	}

	v4l2_async_notifier_init(&max96717->notifier);
	max96717->asd = v4l2_async_notifier_add_fwnode_subdev(&max96717->notifier,
					      remote, sizeof(struct v4l2_async_subdev));
	fwnode_handle_put(remote);
	if (IS_ERR(max96717->asd))
		return PTR_ERR(max96717->asd);

	max96717->notifier.ops = &max96717_notifier_ops;
	max96717->notifier.flags = V4L2_ASYNC_NOTIFIER_DEFER_POST_REGISTER;
	ret = v4l2_async_subdev_notifier_register(&max96717->sd,
						  &max96717->notifier);
	if (ret < 0) {
		v4l2_async_notifier_cleanup(&max96717->notifier);
		return ret;
	}

	return 0;
}

static int max96717_init(struct max96717_device *max96717)
{
	return 0;
}

static int max96717_probe(struct i2c_client *client)
{
	struct max96717_device *max96717;
	struct fwnode_handle *ep;
	int ret;

	max96717 = devm_kzalloc(&client->dev, sizeof(*max96717), GFP_KERNEL);
	if (!max96717)
		return -ENOMEM;
	max96717->dev = &client->dev;
	max96717->client = client;

	/* Initialize and register the subdevice. */
	v4l2_i2c_subdev_init(&max96717->sd, client, &max96717_subdev_ops);
	max96717->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	max96717->pads[MAX96717_SOURCE_PAD].flags = MEDIA_PAD_FL_SOURCE;
	max96717->pads[MAX96717_SINK_PAD].flags = MEDIA_PAD_FL_SINK;
	max96717->sd.entity.flags |= MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER;
	ret = media_entity_pads_init(&max96717->sd.entity, 2, max96717->pads);
	if (ret < 0)
		return ret;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(max96717->dev), 0, 0, 0);
	if (!ep) {
		dev_err(max96717->dev, "Unable to get endpoint 0: %pOF\n",
			max96717->dev->of_node);
		ret = -ENODEV;
		goto error_media_entity;
	}

	max96717->sd.fwnode = ep;
	ret = v4l2_async_register_subdev(&max96717->sd);
	if (ret)
		goto error_put_node;

	ret = max96717_parse_dt(max96717);
	if (ret)
		goto error_unregister_subdev;

	ret = max96717_init(max96717);
	if (ret)
		goto error_unregister_subdev;

	return 0;

error_unregister_subdev:
	v4l2_async_unregister_subdev(&max96717->sd);
error_put_node:
	fwnode_handle_put(max96717->sd.fwnode);
error_media_entity:
	media_entity_cleanup(&max96717->sd.entity);

	return ret;
}

static int max96717_remove(struct i2c_client *client)
{
	struct max96717_device *max96717 = i2c_to_max96717(client);

	v4l2_ctrl_handler_free(&max96717->ctrls);
	v4l2_async_notifier_cleanup(&max96717->notifier);
	v4l2_async_unregister_subdev(&max96717->sd);
	fwnode_handle_put(max96717->sd.fwnode);
	media_entity_cleanup(&max96717->sd.entity);

	return 0;
}

static const struct of_device_id max96717_of_ids[] = {
	{ .compatible = "imi,max96717", },
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
