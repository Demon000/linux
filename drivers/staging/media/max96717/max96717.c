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

	struct max96717_subdev_priv sd_privs[MAX96717_SUBDEVS_NUM];
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

#define max96717_update_bits_prep(priv, reg, mask, val) \
	max96717_update_bits(priv, reg, mask, ((val << __ffs(mask)) & mask))

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
			sd_priv->sd.name, ret);
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

static void max96717_v4l2_notifier_unregister(struct max96717_subdev_priv *sd_priv)
{
	v4l2_async_notifier_unregister(&sd_priv->notifier);
	v4l2_async_notifier_cleanup(&sd_priv->notifier);
}

static int max96717_s_stream(struct v4l2_subdev *sd, int enable)
{
#if 0
	struct max96717_priv *priv = sd_to_max96717(sd);
	int ret;

	ret = v4l2_subdev_call(priv->sensor, video, s_stream, enable);
	if (ret)
		dev_err(priv->dev, "Failed to start stream for camera device %d\n", ret);

	return ret;
#endif

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

	ret = v4l2_subdev_call(sd_priv->slave_sd, pad, set_fmt,
			       sd_priv->slave_sd_state, &sd_format);
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
	struct v4l2_subdev_mbus_code_enum sd_code = *code;
	int ret;

	if (code->pad != MAX96717_SOURCE_PAD)
		return -EINVAL;

	sd_code.pad = sd_priv->slave_sd_pad_id;

	ret = v4l2_subdev_call(sd_priv->slave_sd, pad, enum_mbus_code,
			       sd_priv->slave_sd_state, &sd_code);
	if (ret)
		return ret;

	code->code = sd_code.code;

	return 0;
}

static int max96717_enum_frame_size(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *sd_state,
				    struct v4l2_subdev_frame_size_enum *fse)
{
	struct max96717_subdev_priv *sd_priv = v4l2_get_subdevdata(sd);
	struct v4l2_subdev_frame_size_enum sd_fse = *fse;
	int ret;

	if (fse->pad != MAX96717_SOURCE_PAD)
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

static int max96717_post_register(struct v4l2_subdev *sd)
{
	return 0;
}

static int max96717_get_fwnode_pad(struct media_entity *entity,
				   struct fwnode_endpoint *endpoint)
{
	return endpoint->port > MAX96717_SUBDEVS_NUM ? MAX96717_SOURCE_PAD
						     : MAX96717_SINK_PAD;
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
};

static const struct v4l2_subdev_core_ops max96717_core_ops = {
	.post_register	= max96717_post_register,
};

static const struct v4l2_subdev_ops max96717_subdev_ops = {
	.core		= &max96717_core_ops,
	.video		= &max96717_video_ops,
	.pad		= &max96717_pad_ops,
};

static const struct media_entity_operations max96717_entity_ops = {
	.get_fwnode_pad = max96717_get_fwnode_pad,
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
	max96717_update_bits_prep(priv, 0x302, GENMASK(6, 4), 0b001);

	for_each_subdev(priv, sd_priv)
		max96717_init_phy(sd_priv);

	/* Enable GPIO 0. */
	/* TODO: Implement pinctrl. */
	max96717_write(priv, 0x2be, 0x90);
	max96717_write(priv, 0x2bf, 0x60);

	/* Enable RCLK output at fastest slew rate on GPIO 4. */
	max96717_update_bits(priv, 0x6, BIT(5), BIT(5));
	max96717_update_bits(priv, 0x3, GENMASK(1, 0), 0b00);
	max96717_update_bits(priv, 0x570, GENMASK(5, 4),
						 FIELD_PREP(GENMASK(5, 4), 0b00));

	max96717_update_bits(priv, 0x3f1, BIT(7), BIT(7));
	max96717_update_bits(priv, 0x3f1, BIT(0), BIT(0));
	max96717_update_bits(priv, 0x3f1, GENMASK(5, 1),
						 FIELD_PREP(GENMASK(5, 1), 0x4));
}

static const char *max96717_subdev_names[] = {
	":0", ":1"
};

static int max96717_v4l2_register_sd(struct max96717_subdev_priv *sd_priv)
{
	struct max96717_priv *priv = sd_priv->priv;
	unsigned int index = sd_priv->index;
	int ret;

	if (index >= ARRAY_SIZE(max96717_subdev_names))
		return -EINVAL;

	ret = max96717_v4l2_notifier_register(sd_priv);
	if (ret)
		return ret;

	v4l2_i2c_subdev_init(&sd_priv->sd, priv->client, &max96717_subdev_ops);
	v4l2_i2c_subdev_set_name(&sd_priv->sd, priv->client, NULL, max96717_subdev_names[index]);
	sd_priv->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	sd_priv->sd.entity.ops = &max96717_entity_ops;
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
	max96717_v4l2_notifier_unregister(sd_priv);

	return ret;
}

static void max96717_v4l2_unregister_sd(struct max96717_subdev_priv *sd_priv)
{
	max96717_v4l2_notifier_unregister(sd_priv);
	v4l2_async_unregister_subdev(&sd_priv->sd);
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
					  struct fwnode_handle *fwnode,
					  unsigned int port)
{
	struct max96717_priv *priv = sd_priv->priv;
	struct fwnode_handle *ep;

	ep = fwnode_graph_get_endpoint_by_id(fwnode, port, 0, 0);
	if (!ep) {
		dev_err(priv->dev, "Not connected to subdevice\n");
		return -EINVAL;
	}

	sd_priv->fwnode = ep;

	return 0;
}

static int max96717_parse_sink_dt_endpoint(struct max96717_subdev_priv *sd_priv,
					   struct fwnode_handle *fwnode,
					   unsigned int port)
{
	struct max96717_priv *priv = sd_priv->priv;
	struct v4l2_fwnode_endpoint v4l2_ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	struct fwnode_handle *ep, *remote_ep;
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(fwnode, port, 0, 0);
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
	struct fwnode_handle *fwnode = dev_fwnode(priv->dev);
	struct max96717_subdev_priv *sd_priv;
	unsigned int i, j;
	int ret;

	for (i = 0; i < MAX96717_SUBDEVS_NUM; i++) {
		sd_priv = &priv->sd_privs[i];
		sd_priv->priv = priv;
		sd_priv->index = i;

		ret = max96717_parse_sink_dt_endpoint(sd_priv, fwnode, i);
		if (ret)
			continue;

		ret = max96717_parse_src_dt_endpoint(sd_priv, fwnode,
						     i + MAX96717_SUBDEVS_NUM);
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

static int max96717_dump_regs(struct max96717_priv *priv)
{
	static const struct {
		u32 offset;
		u32 mask;
		const char * const name;
	} registers[] = {
		{ 0x0, GENMASK(7, 1), "DEV_ADDR" },
		{ 0x112, BIT(7), "PCLKDET" },
	};
	unsigned int i;
	u32 cfg;

	dev_info(priv->dev, "--- REGISTERS ---\n");

	for (i = 0; i < ARRAY_SIZE(registers); i++) {
		cfg = max96717_read(priv, registers[i].offset);
		if (cfg < 0)
			return -EINVAL;

		dev_info(priv->dev, "0x%04x: 0x%02x\n", registers[i].offset, cfg);
		cfg = (cfg & registers[i].mask) >> __ffs(registers[i].mask);
		dev_info(priv->dev, "%14s: 0x%02x\n", registers[i].name, cfg);
	}

	return 0;
}

static int max96717_dump_regs_show(struct seq_file *m, void *private)
{
	struct max96717_priv *priv = m->private;

	return max96717_dump_regs(priv);
}
DEFINE_SHOW_ATTRIBUTE(max96717_dump_regs);

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
