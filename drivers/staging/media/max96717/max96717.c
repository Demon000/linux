// SPDX-License-Identifier: GPL-2.0+

#include <linux/debugfs.h>
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
#define MAX96717_PAD_NUM	2

#define MAX96717_SUBDEVS_NUM	2

#define MAX96717_DT_EMB8			0x12
#define MAX96717_DT_YUV422_8B			0x1e
#define MAX96717_DT_YUV422_10B			0x1f
#define MAX96717_DT_RGB565			0x22
#define MAX96717_DT_RGB666			0x23
#define MAX96717_DT_RGB888			0x24
#define MAX96717_DT_RAW8			0x2a
#define MAX96717_DT_RAW10			0x2b
#define MAX96717_DT_RAW12			0x2c
#define MAX96717_DT_RAW14			0x2d
#define MAX96717_DT_RAW16			0x2e
#define MAX96717_DT_RAW20			0x2f

#define v4l2_subdev_state v4l2_subdev_pad_config
#define v4l2_subdev_alloc_state v4l2_subdev_alloc_pad_config
#define v4l2_subdev_free_state v4l2_subdev_free_pad_config
#undef v4l2_async_notifier_add_fwnode_subdev
#define v4l2_async_notifier_add_fwnode_subdev(__notifier, __fwnode, __type) \
((__type *)__v4l2_async_notifier_add_fwnode_subdev(__notifier, __fwnode,    \
                           sizeof(__type)))

struct max96717_asd {
	struct v4l2_async_subdev base;
	struct max96717_subdev_priv *sd_priv;
};

struct max96717_format {
	u32 code;
	u32 dt;
	u8 bpp;
};

struct max96717_subdev_priv {
	struct v4l2_subdev sd;
	unsigned int index;
	struct fwnode_handle *fwnode;

	struct max96717_priv *priv;

	struct v4l2_subdev *slave_sd;
	struct fwnode_handle *slave_fwnode;
	struct v4l2_subdev_state *slave_sd_state;
	unsigned int slave_sd_pad_id;

	struct v4l2_async_notifier notifier;
	struct media_pad pads[MAX96717_PAD_NUM];

	struct v4l2_fwnode_bus_mipi_csi2 mipi;
};

struct max96717_priv {
	struct device *dev;
	struct dentry *debugfs_root;
	struct i2c_client *client;
	struct regmap *regmap;

	unsigned int lane_config;
	bool pixel_mode;

	struct max96717_subdev_priv sd_privs[MAX96717_SUBDEVS_NUM];

	unsigned			cached_reg_addr;
	char				read_buf[20];
	unsigned int			read_buf_len;
};

#define MAX96717_FMT(_code, _dt, _bpp)	\
{					\
	.code = (_code),		\
	.dt = (_dt),			\
	.bpp = (_bpp),			\
}

static const struct max96717_format max96717_formats[] = {
	MAX96717_FMT(MEDIA_BUS_FMT_YUYV8_1X16, MAX96717_DT_YUV422_8B, 8),
	MAX96717_FMT(MEDIA_BUS_FMT_YUYV10_1X20, MAX96717_DT_YUV422_10B, 10),
	MAX96717_FMT(MEDIA_BUS_FMT_RGB565_1X16, MAX96717_DT_RGB565, 16),
	MAX96717_FMT(MEDIA_BUS_FMT_RGB666_1X18, MAX96717_DT_RGB666, 18),
	MAX96717_FMT(MEDIA_BUS_FMT_RGB888_1X24, MAX96717_DT_RGB888, 24),
	MAX96717_FMT(MEDIA_BUS_FMT_SBGGR8_1X8, MAX96717_DT_RAW8, 8),
	MAX96717_FMT(MEDIA_BUS_FMT_SGBRG8_1X8, MAX96717_DT_RAW8, 8),
	MAX96717_FMT(MEDIA_BUS_FMT_SGRBG8_1X8, MAX96717_DT_RAW8, 8),
	MAX96717_FMT(MEDIA_BUS_FMT_SRGGB8_1X8, MAX96717_DT_RAW8, 8),
	MAX96717_FMT(MEDIA_BUS_FMT_SBGGR10_1X10, MAX96717_DT_RAW10, 10),
	MAX96717_FMT(MEDIA_BUS_FMT_SGBRG10_1X10, MAX96717_DT_RAW10, 10),
	MAX96717_FMT(MEDIA_BUS_FMT_SGRBG10_1X10, MAX96717_DT_RAW10, 10),
	MAX96717_FMT(MEDIA_BUS_FMT_SRGGB10_1X10, MAX96717_DT_RAW10, 10),
	MAX96717_FMT(MEDIA_BUS_FMT_SBGGR12_1X12, MAX96717_DT_RAW12, 12),
	MAX96717_FMT(MEDIA_BUS_FMT_SGBRG12_1X12, MAX96717_DT_RAW12, 12),
	MAX96717_FMT(MEDIA_BUS_FMT_SGRBG12_1X12, MAX96717_DT_RAW12, 12),
	MAX96717_FMT(MEDIA_BUS_FMT_SRGGB12_1X12, MAX96717_DT_RAW12, 12),
	MAX96717_FMT(MEDIA_BUS_FMT_SBGGR14_1X14, MAX96717_DT_RAW14, 14),
	MAX96717_FMT(MEDIA_BUS_FMT_SGBRG14_1X14, MAX96717_DT_RAW14, 14),
	MAX96717_FMT(MEDIA_BUS_FMT_SGRBG14_1X14, MAX96717_DT_RAW14, 14),
	MAX96717_FMT(MEDIA_BUS_FMT_SRGGB14_1X14, MAX96717_DT_RAW14, 14),
	MAX96717_FMT(MEDIA_BUS_FMT_SBGGR16_1X16, MAX96717_DT_RAW16, 16),
	MAX96717_FMT(MEDIA_BUS_FMT_SGBRG16_1X16, MAX96717_DT_RAW16, 16),
	MAX96717_FMT(MEDIA_BUS_FMT_SGRBG16_1X16, MAX96717_DT_RAW16, 16),
	MAX96717_FMT(MEDIA_BUS_FMT_SRGGB16_1X16, MAX96717_DT_RAW16, 16),
};

static struct max96717_subdev_priv *next_subdev(struct max96717_priv *priv,
						struct max96717_subdev_priv *sd_priv)
{
	if (!sd_priv)
		sd_priv = &priv->sd_privs[0];
	else
		sd_priv++;

	for (; sd_priv < &priv->sd_privs[MAX96717_SUBDEVS_NUM]; sd_priv++) {
		if (sd_priv->fwnode)
			return sd_priv;
	}

	return NULL;
}

#define for_each_subdev(priv, sd_priv) \
	for ((sd_priv) = NULL; ((sd_priv) = next_subdev((priv), (sd_priv))); )

static inline struct max96717_asd *to_max96717_asd(struct v4l2_async_subdev *asd)
{
	return container_of(asd, struct max96717_asd, base);
}

static inline struct max96717_subdev_priv *sd_to_max96717(struct v4l2_subdev *sd)
{
	return container_of(sd, struct max96717_subdev_priv, sd);
}

static int max96717_read(struct max96717_priv *priv, int reg)
{
	int ret, val;

	ret = regmap_read(priv->regmap, reg, &val);
	if (ret) {
		dev_err(priv->dev, "read 0x%04x failed\n", reg);
		return ret;
	}

	return val;
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

static void max96717_reset(struct max96717_priv *priv)
{
	max96717_update_bits(priv, 0x10, 0x80, 0x80);
	msleep(80);
}

static int max96717_wait_for_device(struct max96717_priv *priv)
{
	unsigned int i;
	int ret;

	for (i = 0; i < 100; i++) {
		ret = max96717_read(priv, 0x0);
		if (ret >= 0)
			return 0;

		msleep(10);

		dev_err(priv->dev, "Retry %u waiting for serializer: %d\n", i, ret);
	}

	return ret;
}

static const struct max96717_format *max96717_format_by_code(u32 code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(max96717_formats); i++)
		if (max96717_formats[i].code == code)
			return &max96717_formats[i];

	return NULL;
}

static const bool max96717_format_valid(struct max96717_priv *priv, u32 code)
{
	if (!priv->pixel_mode)
		return true;

	return max96717_format_by_code(code);
}

static int max96717_notify_bound(struct v4l2_async_notifier *notifier,
				 struct v4l2_subdev *subdev,
				 struct v4l2_async_subdev *asd)
{
	struct max96717_subdev_priv *sd_priv = sd_to_max96717(notifier->sd);
	struct max96717_priv *priv = sd_priv->priv;
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
				    MAX96717_SINK_PAD,
				    MEDIA_LNK_FL_ENABLED |
				    MEDIA_LNK_FL_IMMUTABLE);
	if (ret) {
		dev_err(priv->dev,
			"Unable to link %s:%u -> %s:%u\n",
			sd_priv->slave_sd->name,
			sd_priv->slave_sd_pad_id,
			sd_priv->sd.name,
			MAX96717_SINK_PAD);
		return ret;
	}

	dev_err(priv->dev, "Bound %s:%u on %s:%u\n",
		sd_priv->slave_sd->name,
		sd_priv->slave_sd_pad_id,
		sd_priv->sd.name,
		MAX96717_SINK_PAD);

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

static void max96717_notify_unbind(struct v4l2_async_notifier *notifier,
				   struct v4l2_subdev *subdev,
				   struct v4l2_async_subdev *asd)
{
	struct max96717_subdev_priv *sd_priv = sd_to_max96717(notifier->sd);

	sd_priv->slave_sd = NULL;
	v4l2_subdev_free_state(sd_priv->slave_sd_state);
	sd_priv->slave_sd_state = NULL;
}

static const struct v4l2_async_notifier_operations max96717_notify_ops = {
	.bound = max96717_notify_bound,
	.unbind = max96717_notify_unbind,
};

static int max96717_v4l2_notifier_register(struct max96717_subdev_priv *sd_priv)
{
	struct max96717_priv *priv = sd_priv->priv;
	struct max96717_asd *mas;
	int ret;

	v4l2_async_notifier_init(&sd_priv->notifier);

	mas = (struct max96717_asd *)
	      v4l2_async_notifier_add_fwnode_subdev(&sd_priv->notifier,
						    sd_priv->slave_fwnode, struct max96717_asd);
	if (IS_ERR(mas)) {
		ret = PTR_ERR(mas);
		dev_err(priv->dev,
			"Failed to add subdev notifier for subdev %s: %d\n",
			sd_priv->sd.name, ret);
		goto error_cleanup_notifier;
	}

	mas->sd_priv = sd_priv;

	sd_priv->notifier.ops = &max96717_notify_ops;
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

static int max96717_mipi_enable(struct max96717_priv *priv, bool enable)
{
	return max96717_update_bits(priv, 0x2, BIT(6), enable ? BIT(6) : 0);
}

static int max96717_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct max96717_subdev_priv *sd_priv = sd_to_max96717(sd);
	struct max96717_priv *priv = sd_priv->priv;
	int ret;

	ret = max96717_mipi_enable(priv, enable);
	if (ret)
		return ret;

	ret = v4l2_subdev_call(sd_priv->slave_sd, video, s_stream, enable);
	if (ret)
		dev_err(priv->dev, "Failed to start stream for %s: %d\n",
			sd_priv->slave_sd->name, ret);

	return 0;
}

static int max96717_get_selection(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_selection *sel)
{
	struct max96717_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_selection sd_sel = *sel;
	int ret;

	if (sel->pad != MAX96717_SOURCE_PAD)
		return -EINVAL;

	sd_sel.pad = sd_priv->slave_sd_pad_id;

	ret = v4l2_subdev_call(sd_priv->slave_sd, pad, get_selection,
			       sd_priv->slave_sd_state, &sd_sel);
	if (ret)
		return ret;

	sel->r = sd_sel.r;

	return 0;
}

static int max96717_fix_fmt_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *format)
{
	struct v4l2_subdev_mbus_code_enum code = {
		.pad = MAX96717_SOURCE_PAD,
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};
	int ret;

	ret = v4l2_subdev_call(sd, pad, enum_mbus_code, sd_state, &code);
	if (ret)
		return ret;

	format->format.code = code.code;

	return 0;
}

static int max96717_check_fmt_code(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_format *format)
{
	struct max96717_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct max96717_priv *priv = sd_priv->priv;
	int ret;

	if (max96717_format_valid(priv, format->format.code))
		return 0;

	ret = max96717_fix_fmt_code(sd, sd_state, format);
	if (ret)
		return ret;

	ret = v4l2_subdev_call(sd_priv->slave_sd, pad, set_fmt,
			       sd_priv->slave_sd_state, format);
	if (ret)
		return ret;

	if (!max96717_format_valid(priv, format->format.code))
		return -EINVAL;

	return 0;
}

static int max96717_get_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *sd_state,
			    struct v4l2_subdev_format *format)
{
	struct max96717_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_format sd_format = *format;
	int ret;

	if (format->pad != MAX96717_SOURCE_PAD)
		return -EINVAL;

	sd_format.pad = sd_priv->slave_sd_pad_id;

	ret = v4l2_subdev_call(sd_priv->slave_sd, pad, get_fmt,
			       sd_priv->slave_sd_state, &sd_format);
	if (ret)
		return ret;

	ret = max96717_check_fmt_code(sd, sd_state, &sd_format);
	if (ret)
		return ret;

	format->format = sd_format.format;

	return 0;
}

static int max96717_set_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *sd_state,
			    struct v4l2_subdev_format *format)
{
	struct max96717_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_format sd_format = *format;
	int ret;

	if (format->pad != MAX96717_SOURCE_PAD)
		return -EINVAL;

	sd_format.pad = sd_priv->slave_sd_pad_id;

	ret = max96717_check_fmt_code(sd, sd_state, &sd_format);
	if (ret)
		return ret;

	format->format = sd_format.format;

	return 0;
}

static int max96717_enum_mbus_code(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_mbus_code_enum *code)
{
	struct max96717_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct max96717_priv *priv = sd_priv->priv;
	struct v4l2_subdev_mbus_code_enum sd_code = *code;
	int ret;

	if (code->pad != MAX96717_SOURCE_PAD)
		return -EINVAL;

	sd_code.pad = sd_priv->slave_sd_pad_id;

	while (true) {
		ret = v4l2_subdev_call(sd_priv->slave_sd, pad, enum_mbus_code,
				       sd_priv->slave_sd_state, &sd_code);
		if (ret)
			return ret;

		if (max96717_format_valid(priv, sd_code.code))
			break;

		sd_code.index++;
	}

	code->code = sd_code.code;

	return 0;
}

static int max96717_enum_frame_size(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *sd_state,
				    struct v4l2_subdev_frame_size_enum *fse)
{
	struct max96717_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct max96717_priv *priv = sd_priv->priv;
	struct v4l2_subdev_frame_size_enum sd_fse = *fse;
	int ret;

	if (fse->pad != MAX96717_SOURCE_PAD)
		return -EINVAL;

	sd_fse.pad = sd_priv->slave_sd_pad_id;

	while (true) {
		ret = v4l2_subdev_call(sd_priv->slave_sd, pad, enum_frame_size,
				       sd_priv->slave_sd_state, &sd_fse);
		if (ret)
			return ret;

		if (max96717_format_valid(priv, sd_fse.code))
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

static int max96717_enum_frame_interval(struct v4l2_subdev *sd,
					struct v4l2_subdev_state *sd_state,
					struct v4l2_subdev_frame_interval_enum *fie)
{
	struct max96717_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct max96717_priv *priv = sd_priv->priv;
	struct v4l2_subdev_frame_interval_enum sd_fie = *fie;
	int ret;

	if (fie->pad != MAX96717_SOURCE_PAD)
		return -EINVAL;

	sd_fie.pad = sd_priv->slave_sd_pad_id;

	while (true) {
		ret = v4l2_subdev_call(sd_priv->slave_sd, pad, enum_frame_interval,
				       sd_priv->slave_sd_state, &sd_fie);
		if (ret)
			return ret;

		if (max96717_format_valid(priv, sd_fie.code))
			break;

		sd_fie.index++;
	}

	fie->code = sd_fie.code;
	fie->width = sd_fie.width;
	fie->height = sd_fie.height;
	fie->interval = sd_fie.interval;

	return 0;
}

static int max96717_post_register(struct v4l2_subdev *sd)
{
	return 0;
}

static const struct v4l2_subdev_video_ops max96717_video_ops = {
	.s_stream	= max96717_s_stream,
};

static const struct v4l2_subdev_pad_ops max96717_pad_ops = {
	.get_selection = max96717_get_selection,
	.get_fmt = max96717_get_fmt,
	.set_fmt = max96717_set_fmt,
	.enum_mbus_code = max96717_enum_mbus_code,
	.enum_frame_size = max96717_enum_frame_size,
	.enum_frame_interval = max96717_enum_frame_interval,
};

static const struct v4l2_subdev_core_ops max96717_core_ops = {
	.post_register	= max96717_post_register,
};

static const struct v4l2_subdev_ops max96717_subdev_ops = {
	.core		= &max96717_core_ops,
	.video		= &max96717_video_ops,
	.pad		= &max96717_pad_ops,
};

static void max96717_init_phy(struct max96717_subdev_priv *sd_priv)
{
	unsigned int num_data_lanes = sd_priv->mipi.num_data_lanes;
	struct max96717_priv *priv = sd_priv->priv;
	unsigned int index = sd_priv->index;
	unsigned int val, shift, mask;
	unsigned int i;

	if (num_data_lanes == 4)
		val = 0x3;
	else
		val = 0x1;

	shift = index * 4;
	mask = 0x3;

	/* Configure a lane count. */
	/* TODO: Add support for 1-lane configurations. */
	max96717_update_bits(priv, 0x331, mask << shift, val << shift);

	/* Configure lane mapping. */
	/* TODO: Add support for lane swapping. */
	max96717_update_bits(priv, 0x332, 0xf0, 0xe0);
	max96717_update_bits(priv, 0x333, 0x0f, 0x04);

	/* Configure lane polarity. */
	/* Lower two lanes. */
	val = 0;
	for (i = 0; i < 3 && i < num_data_lanes + 1; i++)
		if (sd_priv->mipi.lane_polarities[i])
			val |= BIT(i == 0 ? 2 : i - 1);
	max96717_update_bits(priv, 0x335, 0x7, val);

	/* Upper two lanes. */
	val = 0;
	shift = 4;
	for (i = 3; i < num_data_lanes + 1; i++)
		if (sd_priv->mipi.lane_polarities[i])
			val |= BIT(i - 3);
	max96717_update_bits(priv, 0x334, 0x7 << shift, val << shift);

	/* Set stream ID to 0. */
	/* TODO: Make this generic by better parsing of chip ports. */
	max96717_write(priv, 0x5b, 0x00);
}

static void max96717_init(struct max96717_priv *priv)
{
	struct max96717_subdev_priv *sd_priv;

	/*
	 * Set CMU2 PFDDIV to 1.1V for correct functionality of the device,
	 * as mentioned in the datasheet, under section MANDATORY REGISTER PROGRAMMING.
	 */
	max96717_update_bits(priv, 0x302, 0x70, 0x10);

	max96717_mipi_enable(priv, false);

	if (priv->pixel_mode) {
		/* Disable Auto BPP mode. */
		max96717_update_bits(priv, 0x110, BIT(3), 0x00);

		/* Select pixel mode. */
		max96717_update_bits(priv, 0x383, BIT(7), 0x00);

		/* Enable double 12bit mode. */
		max96717_update_bits(priv, 0x313, BIT(6), BIT(6));

		/* Software override BPP. */
		max96717_update_bits(priv, 0x31e, GENMASK(4, 0),
				     FIELD_PREP(GENMASK(4, 0), 24));

		/* Enable software override BPP. */
		max96717_update_bits(priv, 0x31e, BIT(5), BIT(5));
	} else {
		/* Select tunnel mode. */
		max96717_update_bits(priv, 0x383, BIT(7), BIT(7));
	}

	for_each_subdev(priv, sd_priv)
		max96717_init_phy(sd_priv);

	/* Enable RCLK output at fastest slew rate on GPIO 4. */
	max96717_update_bits(priv, 0x6, BIT(5), BIT(5));
	max96717_update_bits(priv, 0x3, GENMASK(1, 0), 0b00);
	max96717_update_bits(priv, 0x570, GENMASK(5, 4),
			     FIELD_PREP(GENMASK(5, 4), 0b00));

	max96717_update_bits(priv, 0x3f1, BIT(7), BIT(7));
	max96717_update_bits(priv, 0x3f1, BIT(0), BIT(0));
	max96717_update_bits(priv, 0x3f1, GENMASK(5, 1),
			     FIELD_PREP(GENMASK(5, 1), 0x4));

	msleep(1000);

	/* Enable GPIO 0. */
	/* TODO: Implement pinctrl. */
	max96717_write(priv, 0x2be, 0x80);
	msleep(1000);
	max96717_write(priv, 0x2be, 0x90);
	max96717_write(priv, 0x2bf, 0x60);
}

static int max96717_v4l2_register_sd(struct max96717_subdev_priv *sd_priv)
{
	struct max96717_priv *priv = sd_priv->priv;
	unsigned int index = sd_priv->index;
	char postfix[3];
	int ret;

	ret = max96717_v4l2_notifier_register(sd_priv);
	if (ret)
		return ret;

	snprintf(postfix, sizeof(postfix), ":%d", index);

	v4l2_i2c_subdev_init(&sd_priv->sd, priv->client, &max96717_subdev_ops);
	v4l2_i2c_subdev_set_name(&sd_priv->sd, priv->client, NULL, postfix);
	sd_priv->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	sd_priv->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sd_priv->sd.fwnode = sd_priv->fwnode;

	sd_priv->pads[MAX96717_SOURCE_PAD].flags = MEDIA_PAD_FL_SOURCE;
	sd_priv->pads[MAX96717_SINK_PAD].flags = MEDIA_PAD_FL_SINK;

	ret = media_entity_pads_init(&sd_priv->sd.entity, MAX96717_PAD_NUM, sd_priv->pads);
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

static void max96717_v4l2_unregister_sd(struct max96717_subdev_priv *sd_priv)
{
	v4l2_async_notifier_unregister(&sd_priv->notifier);
	v4l2_async_notifier_cleanup(&sd_priv->notifier);
	v4l2_async_unregister_subdev(&sd_priv->sd);
	media_entity_cleanup(&sd_priv->sd.entity);
	fwnode_handle_put(sd_priv->sd.fwnode);
}

static int max96717_v4l2_register(struct max96717_priv *priv)
{
	struct max96717_subdev_priv *sd_priv;
	int ret;

	for_each_subdev(priv, sd_priv) {
		ret = max96717_v4l2_register_sd(sd_priv);
		if (ret)
			return ret;
	}

	return 0;
}

static void max96717_v4l2_unregister(struct max96717_priv *priv)
{
	struct max96717_subdev_priv *sd_priv;

	for_each_subdev(priv, sd_priv)
		max96717_v4l2_unregister_sd(sd_priv);
}

static int max96717_parse_src_dt_endpoint(struct max96717_subdev_priv *sd_priv,
					  struct fwnode_handle *fwnode)
{
	struct max96717_priv *priv = sd_priv->priv;
	struct fwnode_handle *ep;

	ep = fwnode_graph_get_endpoint_by_id(fwnode, MAX96717_SOURCE_PAD, 0, 0);
	if (!ep) {
		dev_err(priv->dev, "Not connected to subdevice\n");
		return -EINVAL;
	}

	return 0;
}

static int max96717_parse_sink_dt_endpoint(struct max96717_subdev_priv *sd_priv,
					   struct fwnode_handle *fwnode)
{
	struct max96717_priv *priv = sd_priv->priv;
	struct v4l2_fwnode_endpoint v4l2_ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	struct fwnode_handle *ep, *remote_ep;
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(fwnode, MAX96717_SINK_PAD, 0, 0);
	if (!ep) {
		dev_err(priv->dev, "Not connected to subdevice\n");
		return -EINVAL;
	}

	remote_ep = fwnode_graph_get_remote_endpoint(ep);
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

	sd_priv->mipi = v4l2_ep.bus.mipi_csi2;
	sd_priv->slave_fwnode = remote_ep;

	return 0;
}

static const unsigned int max96717_lane_configs[][MAX96717_SUBDEVS_NUM] = {
	{ 0, 4 },
	{ 2, 0 },
	{ 0, 2 },
	{ 2, 2 },
};

static int max96717_parse_dt(struct max96717_priv *priv)
{
	struct max96717_subdev_priv *sd_priv;
	struct fwnode_handle *fwnode;
	unsigned int i, j;
	u32 index;
	int ret;

	priv->pixel_mode = device_property_read_bool(priv->dev, "max,pixel-mode");

	device_for_each_child_node(priv->dev, fwnode) {
		struct device_node *of_node = to_of_node(fwnode);

		if (!of_node_name_eq(of_node, "channel"))
			continue;

		ret = fwnode_property_read_u32(fwnode, "reg", &index);
		if (ret) {
			dev_err(priv->dev, "Failed to read reg: %d\n", ret);
			continue;
		}

		if (index >= MAX96717_SUBDEVS_NUM) {
			dev_err(priv->dev, "Invalid channel number %u\n", index);
			return -EINVAL;
		}

		sd_priv = &priv->sd_privs[index];
		sd_priv->fwnode = fwnode;
		sd_priv->priv = priv;
		sd_priv->index = index;

		ret = max96717_parse_sink_dt_endpoint(sd_priv, fwnode);
		if (ret)
			continue;

		ret = max96717_parse_src_dt_endpoint(sd_priv, fwnode);
		if (ret)
			continue;
	}

	for (i = 0; i < ARRAY_SIZE(max96717_lane_configs); i++) {
		bool matching = true;

		for (j = 0; j < MAX96717_SUBDEVS_NUM; j++) {
			sd_priv = &priv->sd_privs[j];

			if (sd_priv->fwnode && sd_priv->mipi.num_data_lanes !=
			    max96717_lane_configs[i][j]) {
				matching = false;
				break;
			}
		}

		if (matching)
			break;
	}

	if (i == ARRAY_SIZE(max96717_lane_configs)) {
		dev_err(priv->dev, "Invalid lane configuration\n");
		return -EINVAL;
	}

	priv->lane_config = i;

	return 0;
}

static const struct regmap_config max96717_i2c_regmap = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0x1f00,
};

static int max96717_dump_regs(struct max96717_priv *priv, struct seq_file *m)
{
	static const struct {
		unsigned int start;
		unsigned int end;
	} registers[] = {
		{0x0, 0x6},
		{0x8, 0x8},
		{0xc, 0xe},
		{0x10, 0x13},
		{0x18, 0x22},
		{0x24, 0x26},
		{0x28, 0x2d},
		{0x30, 0x31},
		{0x40, 0x45},
		{0x48, 0x48},
		{0x4c, 0x4d},
		{0x4f, 0x4f},
		{0x58, 0x58},
		{0x5b, 0x5b},
		{0x78, 0x78},
		{0x7b, 0x7c},
		{0x80, 0x80},
		{0x83, 0x87},
		{0x90, 0x90},
		{0x93, 0x97},
		{0xa0, 0xa0},
		{0xa3, 0xa8},
		{0xab, 0xaf},
		{0x110, 0x112},
		{0x170, 0x178},
		{0x236, 0x278},
		{0x2be, 0x2de},
		{0x302, 0x302},
		{0x308, 0x308},
		{0x30d, 0x30e},
		{0x311, 0x313},
		{0x318, 0x319},
		{0x31e, 0x31e},
		{0x320, 0x320},
		{0x323, 0x323},
		{0x325, 0x325},
		{0x330, 0x335},
		{0x337, 0x338},
		{0x33b, 0x33e},
		{0x343, 0x347},
		{0x36c, 0x36f},
		{0x377, 0x381},
		{0x383, 0x383},
		{0x38d, 0x390},
		{0x3c8, 0x3cb},
		{0x3d1, 0x3d1},
		{0x3dc, 0x3dd},
		{0x3e0, 0x3f9},
		{0x500, 0x502},
		{0x508, 0x509},
		{0x50c, 0x534},
		{0x53e, 0x53e},
		{0x548, 0x54b},
		{0x550, 0x557},
		{0x55f, 0x55f},
		{0x56e, 0x571},
		{0x584, 0x588},
		{0x1300, 0x1300},
		{0x1380, 0x1380},
		{0x1404, 0x1407},
		{0x1417, 0x1417},
		{0x141c, 0x141d},
		{0x141f, 0x141f},
		{0x1432, 0x1432},
		{0x143a, 0x143b},
		{0x1464, 0x1464},
		{0x1470, 0x1476},
		{0x14a8, 0x14aa},
		{0x14ce, 0x14ce},
		{0x1a00, 0x1a00},
		{0x1a03, 0x1a03},
		{0x1a07, 0x1a0a},
		{0x1c50, 0x1c67},
		{0x1d00, 0x1d03},
		{0x1d08, 0x1d0c},
		{0x1d12, 0x1d14},
		{0x1d20, 0x1d20},
		{0x1d28, 0x1d28},
		{0x1d31, 0x1d35},
		{0x1d37, 0x1d37},
		{0x1d3a, 0x1d3d},
	};
	unsigned int i, j;
	int val;

	for (i = 0; i < ARRAY_SIZE(registers); i++) {
		for (j = registers[i].start; j <= registers[i].end; j++) {
			val = max96717_read(priv, j);
			if (val < 0)
				return -EINVAL;

			seq_printf(m, "0x%04x: 0x%02x\n", j, val);
		}
	}

	return 0;
}

static int max96717_dump_regs_show(struct seq_file *m, void *private)
{
	struct max96717_priv *priv = m->private;

	return max96717_dump_regs(priv, m);
}
DEFINE_SHOW_ATTRIBUTE(max96717_dump_regs);

static ssize_t max96717_debugfs_read_reg(struct file *file, char __user *userbuf,
				      size_t count, loff_t *ppos)
{
	struct max96717_priv *priv = file->private_data;
	int ret;

	if (*ppos > 0)
		return simple_read_from_buffer(userbuf, count, ppos,
					       priv->read_buf,
					       priv->read_buf_len);

	ret = max96717_read(priv, priv->cached_reg_addr);
	if (ret < 0) {
		dev_err(priv->dev, "%s: read failed\n", __func__);
		return ret;
	}

	priv->read_buf_len = snprintf(priv->read_buf,
				      sizeof(priv->read_buf),
				      "0x%02X\n", ret);

	return simple_read_from_buffer(userbuf, count, ppos,
				       priv->read_buf,
				       priv->read_buf_len);
}

static ssize_t max96717_debugfs_write_reg(struct file *file,
				       const char __user *userbuf,
				       size_t count, loff_t *ppos)
{
	struct max96717_priv *priv = file->private_data;
	unsigned reg, val;
	char buf[80];
	int ret;

	count = min_t(size_t, count, (sizeof(buf)-1));
	if (copy_from_user(buf, userbuf, count))
		return -EFAULT;

	buf[count] = 0;

	ret = sscanf(buf, "%i %i", &reg, &val);

	if (ret != 1 && ret != 2)
		return -EINVAL;

	priv->cached_reg_addr = reg;

	if (ret == 1)
		return count;

	ret = max96717_write(priv, reg, val);
	if (ret) {
		dev_err(priv->dev, "%s: write failed\n", __func__);
		return ret;
	}

	return count;
}

static const struct file_operations max96717_reg_fops = {
	.open = simple_open,
	.read = max96717_debugfs_read_reg,
	.write = max96717_debugfs_write_reg,
};

static int max96717_probe(struct i2c_client *client)
{
	struct max96717_priv *priv;
	int ret;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &client->dev;
	priv->client = client;

	priv->regmap = devm_regmap_init_i2c(client, &max96717_i2c_regmap);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	priv->debugfs_root = debugfs_create_dir(dev_name(priv->dev), NULL);
	debugfs_create_file("dump_regs", 0600, priv->debugfs_root, priv,
			    &max96717_dump_regs_fops);
	debugfs_create_file("reg", 0600, priv->debugfs_root, priv,
			    &max96717_reg_fops);

	max96717_reset(priv);

	ret = max96717_wait_for_device(priv);
	if (ret)
		return ret;

	ret = max96717_parse_dt(priv);
	if (ret)
		return ret;

	max96717_init(priv);

	return max96717_v4l2_register(priv);
}

static int max96717_remove(struct i2c_client *client)
{
	struct max96717_priv *priv = i2c_get_clientdata(client);

	max96717_v4l2_unregister(priv);

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
