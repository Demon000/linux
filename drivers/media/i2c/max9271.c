// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2017-2021 Jacopo Mondi
 * Copyright (C) 2017-2020 Kieran Bingham
 * Copyright (C) 2017-2019 Laurent Pinchart
 * Copyright (C) 2017-2019 Niklas Söderlund
 * Copyright (C) 2016 Renesas Electronics Corporation
 * Copyright (C) 2015 Cogent Embedded, Inc.
 */

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

#define MAX9271_DEFAULT_ADDR	0x40

/* Register 0x02 */
#define MAX9271_SPREAD_SPECT_0		(0 << 5)
#define MAX9271_SPREAD_SPECT_05		(1 << 5)
#define MAX9271_SPREAD_SPECT_15		(2 << 5)
#define MAX9271_SPREAD_SPECT_1		(5 << 5)
#define MAX9271_SPREAD_SPECT_2		(3 << 5)
#define MAX9271_SPREAD_SPECT_3		(6 << 5)
#define MAX9271_SPREAD_SPECT_4		(7 << 5)
#define MAX9271_R02_RES			BIT(4)
#define MAX9271_PCLK_AUTODETECT		(3 << 2)
#define MAX9271_SERIAL_AUTODETECT	(0x03)
/* Register 0x04 */
#define MAX9271_SEREN			BIT(7)
#define MAX9271_CLINKEN			BIT(6)
#define MAX9271_PRBSEN			BIT(5)
#define MAX9271_SLEEP			BIT(4)
#define MAX9271_INTTYPE_I2C		(0 << 2)
#define MAX9271_INTTYPE_UART		(1 << 2)
#define MAX9271_INTTYPE_NONE		(2 << 2)
#define MAX9271_REVCCEN			BIT(1)
#define MAX9271_FWDCCEN			BIT(0)
/* Register 0x07 */
#define MAX9271_DBL			BIT(7)
#define MAX9271_DRS			BIT(6)
#define MAX9271_BWS			BIT(5)
#define MAX9271_ES			BIT(4)
#define MAX9271_HVEN			BIT(2)
#define MAX9271_EDC_1BIT_PARITY		(0 << 0)
#define MAX9271_EDC_6BIT_CRC		(1 << 0)
#define MAX9271_EDC_6BIT_HAMMING	(2 << 0)
/* Register 0x08 */
#define MAX9271_INVVS			BIT(7)
#define MAX9271_INVHS			BIT(6)
#define MAX9271_REV_LOGAIN		BIT(3)
#define MAX9271_REV_HIVTH		BIT(0)
/* Register 0x09 */
#define MAX9271_ID			0x09
/* Register 0x0d */
#define MAX9271_I2CLOCACK		BIT(7)
#define MAX9271_I2CSLVSH_1046NS_469NS	(3 << 5)
#define MAX9271_I2CSLVSH_938NS_352NS	(2 << 5)
#define MAX9271_I2CSLVSH_469NS_234NS	(1 << 5)
#define MAX9271_I2CSLVSH_352NS_117NS	(0 << 5)
#define MAX9271_I2CMSTBT_837KBPS	(7 << 2)
#define MAX9271_I2CMSTBT_533KBPS	(6 << 2)
#define MAX9271_I2CMSTBT_339KBPS	(5 << 2)
#define MAX9271_I2CMSTBT_173KBPS	(4 << 2)
#define MAX9271_I2CMSTBT_105KBPS	(3 << 2)
#define MAX9271_I2CMSTBT_84KBPS		(2 << 2)
#define MAX9271_I2CMSTBT_28KBPS		(1 << 2)
#define MAX9271_I2CMSTBT_8KBPS		(0 << 2)
#define MAX9271_I2CSLVTO_NONE		(3 << 0)
#define MAX9271_I2CSLVTO_1024US		(2 << 0)
#define MAX9271_I2CSLVTO_256US		(1 << 0)
#define MAX9271_I2CSLVTO_64US		(0 << 0)
/* Register 0x0f */
#define MAX9271_GPIO5OUT		BIT(5)
#define MAX9271_GPIO4OUT		BIT(4)
#define MAX9271_GPIO3OUT		BIT(3)
#define MAX9271_GPIO2OUT		BIT(2)
#define MAX9271_GPIO1OUT		BIT(1)
#define MAX9271_GPO			BIT(0)
/* Register 0x15 */
#define MAX9271_PCLKDET			BIT(0)

struct max9271_device {
	struct device			*dev;
	struct i2c_client		*client;
	struct v4l2_subdev		sd;
#define MAX9271_SOURCE_PAD	0
#define MAX9271_SINK_PAD	1
	struct media_pad		pads[2];
	struct v4l2_async_notifier	notifier;
	struct v4l2_async_subdev	*asd;
	struct v4l2_ctrl_handler	ctrls;
	struct v4l2_subdev		*sensor;
};

static inline struct max9271_device *sd_to_max9271(struct v4l2_subdev *sd)
{
	return container_of(sd, struct max9271_device, sd);
}

static inline struct max9271_device *i2c_to_max9271(struct i2c_client *client)
{
	return sd_to_max9271(i2c_get_clientdata(client));
}

static inline struct max9271_device *notifier_to_max9271(
						struct v4l2_async_notifier *nf)
{
	return container_of(nf, struct max9271_device, notifier);
}

/* --- MAX9271 hardware operations --- */

static int max9271_read(struct max9271_device *dev, u8 reg)
{
	int ret;

	dev_dbg(&dev->client->dev, "%s(0x%02x)\n", __func__, reg);

	ret = i2c_smbus_read_byte_data(dev->client, reg);
	if (ret < 0)
		dev_dbg(&dev->client->dev,
			"%s: register 0x%02x read failed (%d)\n",
			__func__, reg, ret);

	return ret;
}

static int max9271_write(struct max9271_device *dev, u8 reg, u8 val)
{
	int ret;

	dev_dbg(&dev->client->dev, "%s(0x%02x, 0x%02x)\n", __func__, reg, val);

	ret = i2c_smbus_write_byte_data(dev->client, reg, val);
	if (ret < 0)
		dev_err(&dev->client->dev,
			"%s: register 0x%02x write failed (%d)\n",
			__func__, reg, ret);

	return ret;
}

/*
 * max9271_pclk_detect() - Detect valid pixel clock from image sensor
 *
 * Wait up to 10ms for a valid pixel clock.
 *
 * Returns 0 for success, < 0 for pixel clock not properly detected
 */
static int max9271_pclk_detect(struct max9271_device *dev)
{
	unsigned int i;
	int ret;

	for (i = 0; i < 100; i++) {
		ret = max9271_read(dev, 0x15);
		if (ret < 0)
			return ret;

		if (ret & MAX9271_PCLKDET)
			return 0;

		usleep_range(50, 100);
	}

	dev_err(&dev->client->dev, "Unable to detect valid pixel clock\n");

	return -EIO;
}

static void max9271_wake_up(struct max9271_device *dev)
{
	/*
	 * Use the chip default address as this function has to be called
	 * before any other one.
	 */
	dev->client->addr = MAX9271_DEFAULT_ADDR;
	i2c_smbus_read_byte(dev->client);
	usleep_range(5000, 8000);
}

static int max9271_set_serial_link(struct max9271_device *dev, bool enable)
{
	int ret;
	u8 val = MAX9271_REVCCEN | MAX9271_FWDCCEN;

	if (enable) {
		ret = max9271_pclk_detect(dev);
		if (ret)
			return ret;

		val |= MAX9271_SEREN;
	} else {
		val |= MAX9271_CLINKEN;
	}

	/*
	 * The serializer temporarily disables the reverse control channel for
	 * 350µs after starting/stopping the forward serial link, but the
	 * deserializer synchronization time isn't clearly documented.
	 *
	 * According to the serializer datasheet we should wait 3ms, while
	 * according to the deserializer datasheet we should wait 5ms.
	 *
	 * Short delays here appear to show bit-errors in the writes following.
	 * Therefore a conservative delay seems best here.
	 */
	ret = max9271_write(dev, 0x04, val);
	if (ret < 0)
		return ret;

	usleep_range(5000, 8000);

	return 0;
}

static int max9271_configure_i2c(struct max9271_device *dev, u8 i2c_config)
{
	int ret;

	ret = max9271_write(dev, 0x0d, i2c_config);
	if (ret < 0)
		return ret;

	/* The delay required after an I2C bus configuration change is not
	 * characterized in the serializer manual. Sleep up to 5msec to
	 * stay safe.
	 */
	usleep_range(3500, 5000);

	return 0;
}

static int max9271_set_high_threshold(struct max9271_device *dev, bool enable)
{
	int ret;

	ret = max9271_read(dev, 0x08);
	if (ret < 0)
		return ret;

	/*
	 * Enable or disable reverse channel high threshold to increase
	 * immunity to power supply noise.
	 */
	ret = max9271_write(dev, 0x08, enable ? ret | BIT(0) : ret & ~BIT(0));
	if (ret < 0)
		return ret;

	usleep_range(2000, 2500);

	return 0;
}

static int max9271_configure_gmsl_link(struct max9271_device *dev)
{
	int ret;

	/*
	 * Configure the GMSL link:
	 *
	 * - Double input mode, high data rate, 24-bit mode
	 * - Latch input data on PCLKIN rising edge
	 * - Enable HS/VS encoding
	 * - 1-bit parity error detection
	 *
	 * TODO: Make the GMSL link configuration parametric.
	 */
	ret = max9271_write(dev, 0x07, MAX9271_DBL | MAX9271_HVEN |
			    MAX9271_EDC_1BIT_PARITY);
	if (ret < 0)
		return ret;

	usleep_range(5000, 8000);

	/*
	 * Adjust spread spectrum to +4% and auto-detect pixel clock
	 * and serial link rate.
	 */
	ret = max9271_write(dev, 0x02,
			    MAX9271_SPREAD_SPECT_4 | MAX9271_R02_RES |
			    MAX9271_PCLK_AUTODETECT |
			    MAX9271_SERIAL_AUTODETECT);
	if (ret < 0)
		return ret;

	usleep_range(5000, 8000);

	return 0;
}

static int max9271_set_gpios(struct max9271_device *dev, u8 gpio_mask)
{
	int ret;

	ret = max9271_read(dev, 0x0f);
	if (ret < 0)
		return 0;

	ret |= gpio_mask;
	ret = max9271_write(dev, 0x0f, ret);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Failed to set gpio (%d)\n", ret);
		return ret;
	}

	usleep_range(3500, 5000);

	return 0;
}

static int max9271_clear_gpios(struct max9271_device *dev, u8 gpio_mask)
{
	int ret;

	ret = max9271_read(dev, 0x0f);
	if (ret < 0)
		return 0;

	ret &= ~gpio_mask;
	ret = max9271_write(dev, 0x0f, ret);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Failed to clear gpio (%d)\n", ret);
		return ret;
	}

	usleep_range(3500, 5000);

	return 0;
}

static int max9271_enable_gpios(struct max9271_device *dev, u8 gpio_mask)
{
	int ret;

	ret = max9271_read(dev, 0x0e);
	if (ret < 0)
		return 0;

	/* BIT(0) reserved: GPO is always enabled. */
	ret |= (gpio_mask & ~BIT(0));
	ret = max9271_write(dev, 0x0e, ret);
	if (ret < 0) {
		dev_err(&dev->client->dev, "Failed to enable gpio (%d)\n", ret);
		return ret;
	}

	usleep_range(3500, 5000);

	return 0;
}

static int max9271_verify_id(struct max9271_device *dev)
{
	int ret;

	ret = max9271_read(dev, 0x1e);
	if (ret < 0) {
		dev_err(&dev->client->dev, "MAX9271 ID read failed (%d)\n",
			ret);
		return ret;
	}

	if (ret != MAX9271_ID) {
		dev_err(&dev->client->dev, "MAX9271 ID mismatch (0x%02x)\n",
			ret);
		return -ENXIO;
	}

	return 0;
}

static int max9271_set_address(struct max9271_device *dev, u8 addr)
{
	int ret;

	ret = max9271_write(dev, 0x00, addr << 1);
	if (ret < 0) {
		dev_err(&dev->client->dev,
			"MAX9271 I2C address change failed (%d)\n", ret);
		return ret;
	}
	usleep_range(3500, 5000);

	return 0;
}

/* --- V4L2 Subdev Ops --- */

static int max9271_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct max9271_device *max9271 = sd_to_max9271(sd);

	return max9271_set_serial_link(max9271, enable);
}

static int max9271_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	struct max9271_device *max9271 = sd_to_max9271(sd);

	return v4l2_subdev_call(max9271->sensor, pad, enum_mbus_code, NULL,
				code);
}

static int max9271_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *format)
{
	struct max9271_device *max9271 = sd_to_max9271(sd);

	return v4l2_subdev_call(max9271->sensor, pad, get_fmt, NULL,
				format);
}

static int max9271_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_format *format)
{
	struct max9271_device *max9271 = sd_to_max9271(sd);

	return v4l2_subdev_call(max9271->sensor, pad, set_fmt, NULL,
				format);
}

static int max9271_post_register(struct v4l2_subdev *sd)
{
	struct max9271_device *max9271 = sd_to_max9271(sd);
	int ret;

	ret = max9271_verify_id(max9271);
	if (ret < 0)
		return ret;

	ret = max9271_enable_gpios(max9271, MAX9271_GPIO1OUT);
	if (ret)
		return ret;

	ret = max9271_configure_gmsl_link(max9271);
	if (ret)
		return ret;

	return 0;
}

static const struct v4l2_subdev_video_ops max9271_video_ops = {
	.s_stream	= max9271_s_stream,
};

static const struct v4l2_subdev_pad_ops max9271_subdev_pad_ops = {
	.enum_mbus_code = max9271_enum_mbus_code,
	.get_fmt	= max9271_get_fmt,
	.set_fmt	= max9271_set_fmt,
};

static const struct v4l2_subdev_core_ops max9271_core_ops = {
	.post_register	= max9271_post_register,
};

static const struct v4l2_subdev_ops max9271_subdev_ops = {
	.core		= &max9271_core_ops,
	.video		= &max9271_video_ops,
	.pad		= &max9271_subdev_pad_ops,
};

/* --- V4L2 Async Notifier --- */

static int max9271_notify_bound(struct v4l2_async_notifier *notifier,
				struct v4l2_subdev *subdev,
				struct v4l2_async_subdev *asd)
{
	struct max9271_device *max9271 = notifier_to_max9271(notifier);
	int ret, pad;

	/*
	 * Reserve more space than necessary for controls inherited by the
	 * remote subdev.
	 */
	ret = v4l2_ctrl_handler_init(&max9271->ctrls, 16);
	if (ret < 0) {
		dev_err(max9271->dev,
			"Unable to initialize control handler: %d\n", ret);
		return ret;
	}

	ret = v4l2_ctrl_add_handler(&max9271->ctrls, subdev->ctrl_handler,
				    NULL, true);
	if (ret < 0) {
		dev_err(max9271->dev,
			"Unable to add subdev control handler: %d\n", ret);
		goto error_free_handler;
	}
	max9271->sd.ctrl_handler = &max9271->ctrls;

	/* Create media link with the remote sensor source pad. */
	pad = media_entity_get_fwnode_pad(&subdev->entity, asd->match.fwnode,
					  MEDIA_PAD_FL_SOURCE);
	if (pad < 0) {
		dev_err(max9271->dev,
			"Failed to find source pad for %s\n", subdev->name);
		ret = pad;
		goto error_free_handler;
	}

	ret = media_create_pad_link(&subdev->entity, pad,
				    &max9271->sd.entity, MAX9271_SINK_PAD,
				    MEDIA_LNK_FL_ENABLED |
				    MEDIA_LNK_FL_IMMUTABLE);
	if (ret)
		goto error_free_handler;

	max9271->sensor = subdev;

	/*
	 * Hold OV10635 in reset during max9271 configuration. The reset signal
	 * has to be asserted for at least 200 microseconds.
	 */
	ret = max9271_clear_gpios(max9271, MAX9271_GPIO1OUT);
	if (ret)
		return ret;
	usleep_range(200, 500);

	/*
	 * Release ov10635 from reset and initialize it. The image sensor
	 * requires at least 2048 XVCLK cycles (85 micro-seconds at 24MHz)
	 * before being available. Stay safe and wait up to 500 micro-seconds.
	 */
	ret = max9271_set_gpios(max9271, MAX9271_GPIO1OUT);
	if (ret)
		return ret;
	usleep_range(100, 500);

	/*
	 * Call the sensor post_register operation to complete its
	 * initialization.
	 */
	ret = v4l2_subdev_call(max9271->sensor, core, post_register);
	if (ret) {
		dev_err(max9271->dev, "Failed to initialize sensor %u\n", ret);
		goto error_remove_link;
	}

	return 0;

error_remove_link:
	media_entity_remove_links(&max9271->sd.entity);
	max9271->sensor = NULL;

error_free_handler:
	v4l2_ctrl_handler_free(&max9271->ctrls);
	max9271->sd.ctrl_handler = NULL;

	return ret;
}

static void max9271_notify_unbind(struct v4l2_async_notifier *notifier,
				  struct v4l2_subdev *subdev,
				  struct v4l2_async_subdev *asd)
{
	struct max9271_device *max9271 = notifier_to_max9271(notifier);

	media_entity_remove_links(&max9271->sd.entity);
	max9271->sensor = NULL;
}

static const struct v4l2_async_notifier_operations max9271_notifier_ops = {
	.bound = max9271_notify_bound,
	.unbind = max9271_notify_unbind,
};

static int max9271_parse_dt(struct max9271_device *max9271)
{
	struct fwnode_handle *ep, *remote;
	struct v4l2_fwnode_endpoint vep = {
		.bus_type = V4L2_MBUS_PARALLEL,
	};
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(max9271->dev), 1, 0, 0);
	if (!ep) {
		dev_err(max9271->dev, "Unable to get sensor endpoint: %pOF\n",
			max9271->dev->of_node);
		return -ENOENT;
	}

	remote = fwnode_graph_get_remote_endpoint(ep);
	if (!remote) {
		dev_err(max9271->dev, "Unable to get remote endpoint: %pOF\n",
			max9271->dev->of_node);
		return -ENOENT;
	}

	ret = v4l2_fwnode_endpoint_parse(ep, &vep);
	fwnode_handle_put(ep);
	if (ret) {
		fwnode_handle_put(remote);
		dev_err(max9271->dev, "Unable to parse endpoint: %pOF\n",
			to_of_node(ep));
		return ret;
	}

	v4l2_async_notifier_init(&max9271->notifier);
	max9271->asd = v4l2_async_notifier_add_fwnode_subdev(&max9271->notifier,
					      remote, struct v4l2_async_subdev);
	fwnode_handle_put(remote);
	if (IS_ERR(max9271->asd))
		return PTR_ERR(max9271->asd);

	max9271->notifier.ops = &max9271_notifier_ops;
	max9271->notifier.flags = V4L2_ASYNC_NOTIFIER_DEFER_POST_REGISTER;
	ret = v4l2_async_subdev_notifier_register(&max9271->sd,
						  &max9271->notifier);
	if (ret < 0) {
		v4l2_async_notifier_cleanup(&max9271->notifier);
		return ret;
	}

	return 0;
}

static int max9271_init(struct max9271_device *max9271)
{
	int ret;
	u8 addr;

	max9271_wake_up(max9271);

	/* Re-program the chip address. */
	addr = max9271->client->addr;
	max9271->client->addr = MAX9271_DEFAULT_ADDR;
	ret = max9271_set_address(max9271, addr);
	if (ret < 0)
		return ret;
	max9271->client->addr = addr;

	/* Serial link disabled during conf as it needs a valid pixel clock. */
	ret = max9271_set_serial_link(max9271, false);
	if (ret)
		return ret;

	/*
	 *  Ensure that we have a good link configuration before attempting to
	 *  identify the device.
	 */
	ret = max9271_configure_i2c(max9271, MAX9271_I2CSLVSH_469NS_234NS |
					     MAX9271_I2CSLVTO_1024US |
					     MAX9271_I2CMSTBT_105KBPS);
	if (ret)
		return ret;

	/*
	 * Set reverse channel high threshold to increase noise immunity.
	 *
	 * This should be compensated by increasing the reverse channel
	 * amplitude on the remote deserializer side.
	 */
	return max9271_set_high_threshold(max9271, true);
}

static int max9271_probe(struct i2c_client *client)
{
	struct max9271_device *max9271;
	struct fwnode_handle *ep;
	int ret;

	max9271 = devm_kzalloc(&client->dev, sizeof(*max9271), GFP_KERNEL);
	if (!max9271)
		return -ENOMEM;
	max9271->dev = &client->dev;
	max9271->client = client;

	/* Initialize and register the subdevice. */
	v4l2_i2c_subdev_init(&max9271->sd, client, &max9271_subdev_ops);
	max9271->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	max9271->pads[MAX9271_SOURCE_PAD].flags = MEDIA_PAD_FL_SOURCE;
	max9271->pads[MAX9271_SINK_PAD].flags = MEDIA_PAD_FL_SINK;
	max9271->sd.entity.flags |= MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER;
	ret = media_entity_pads_init(&max9271->sd.entity, 2, max9271->pads);
	if (ret < 0)
		return ret;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(max9271->dev), 0, 0, 0);
	if (!ep) {
		dev_err(max9271->dev, "Unable to get endpoint 0: %pOF\n",
			max9271->dev->of_node);
		ret = -ENODEV;
		goto error_media_entity;
	}

	max9271->sd.fwnode = ep;
	ret = v4l2_async_register_subdev(&max9271->sd);
	if (ret)
		goto error_put_node;

	ret = max9271_parse_dt(max9271);
	if (ret)
		goto error_unregister_subdev;

	ret = max9271_init(max9271);
	if (ret)
		goto error_unregister_subdev;

	return 0;

error_unregister_subdev:
	v4l2_async_unregister_subdev(&max9271->sd);
error_put_node:
	fwnode_handle_put(max9271->sd.fwnode);
error_media_entity:
	media_entity_cleanup(&max9271->sd.entity);

	return ret;
}

static int max9271_remove(struct i2c_client *client)
{
	struct max9271_device *max9271 = i2c_to_max9271(client);

	v4l2_ctrl_handler_free(&max9271->ctrls);
	v4l2_async_notifier_cleanup(&max9271->notifier);
	v4l2_async_unregister_subdev(&max9271->sd);
	fwnode_handle_put(max9271->sd.fwnode);
	media_entity_cleanup(&max9271->sd.entity);

	return 0;
}

static const struct of_device_id max9271_of_ids[] = {
	{ .compatible = "imi,max9271", },
	{ }
};
MODULE_DEVICE_TABLE(of, max9271_of_ids);

static struct i2c_driver max9271_i2c_driver = {
	.driver	= {
		.name	= "max9271",
		.of_match_table = max9271_of_ids,
	},
	.probe_new	= max9271_probe,
	.remove		= max9271_remove,
};

module_i2c_driver(max9271_i2c_driver);

MODULE_DESCRIPTION("MAX9271 GMSL serializer subdevice driver");
MODULE_AUTHOR("Jacopo Mondi");
MODULE_LICENSE("GPL");
