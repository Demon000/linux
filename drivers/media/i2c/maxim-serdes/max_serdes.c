// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include <linux/regmap.h>

#include "max_serdes.h"

const struct regmap_config max_i2c_regmap = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0x1f00,
};
EXPORT_SYMBOL_GPL(max_i2c_regmap);

#define MAX_FMT(_code, _dt, _bpp, _dbl) 	\
{						\
	.name = __stringify(_code),		\
	.code = MEDIA_BUS_FMT_ ## _code,	\
	.dt = (_dt),				\
	.bpp = (_bpp),				\
	.dbl = (_dbl),				\
}

static const struct max_format max_formats[] = {
	MAX_FMT(FIXED, MAX_DT_EMB8, 8, 1),
	MAX_FMT(YUYV8_1X16, MAX_DT_YUV422_8B, 16, 0),
	MAX_FMT(YUYV10_1X20, MAX_DT_YUV422_10B, 20, 0),
	MAX_FMT(RGB565_1X16, MAX_DT_RGB565, 16, 0),
	MAX_FMT(RGB666_1X18, MAX_DT_RGB666, 18, 0),
	MAX_FMT(RGB888_1X24, MAX_DT_RGB888, 24, 0),
	MAX_FMT(SBGGR8_1X8, MAX_DT_RAW8, 8, 1),
	MAX_FMT(SGBRG8_1X8, MAX_DT_RAW8, 8, 1),
	MAX_FMT(SGRBG8_1X8, MAX_DT_RAW8, 8, 1),
	MAX_FMT(SRGGB8_1X8, MAX_DT_RAW8, 8, 1),
	MAX_FMT(SBGGR10_1X10, MAX_DT_RAW10, 10, 1),
	MAX_FMT(SGBRG10_1X10, MAX_DT_RAW10, 10, 1),
	MAX_FMT(SGRBG10_1X10, MAX_DT_RAW10, 10, 1),
	MAX_FMT(SRGGB10_1X10, MAX_DT_RAW10, 10, 1),
	MAX_FMT(SBGGR12_1X12, MAX_DT_RAW12, 12, 1),
	MAX_FMT(SGBRG12_1X12, MAX_DT_RAW12, 12, 1),
	MAX_FMT(SGRBG12_1X12, MAX_DT_RAW12, 12, 1),
	MAX_FMT(SRGGB12_1X12, MAX_DT_RAW12, 12, 1),
	MAX_FMT(SBGGR14_1X14, MAX_DT_RAW14, 14, 0),
	MAX_FMT(SGBRG14_1X14, MAX_DT_RAW14, 14, 0),
	MAX_FMT(SGRBG14_1X14, MAX_DT_RAW14, 14, 0),
	MAX_FMT(SRGGB14_1X14, MAX_DT_RAW14, 14, 0),
	MAX_FMT(SBGGR16_1X16, MAX_DT_RAW16, 16, 0),
	MAX_FMT(SGBRG16_1X16, MAX_DT_RAW16, 16, 0),
	MAX_FMT(SGRBG16_1X16, MAX_DT_RAW16, 16, 0),
	MAX_FMT(SRGGB16_1X16, MAX_DT_RAW16, 16, 0),
};

const struct max_format *max_format_by_index(unsigned int index)
{
	if (index >= ARRAY_SIZE(max_formats))
		return NULL;

	return &max_formats[index];
}
EXPORT_SYMBOL_GPL(max_format_by_index);

const struct max_format *max_format_by_code(u32 code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(max_formats); i++)
		if (max_formats[i].code == code)
			return &max_formats[i];

	return NULL;
}
EXPORT_SYMBOL_GPL(max_format_by_code);

const struct max_format *max_format_by_dt(u8 dt)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(max_formats); i++)
		if (max_formats[i].dt == dt)
			return &max_formats[i];

	return NULL;
}
EXPORT_SYMBOL_GPL(max_format_by_dt);

void max_set_sd_name(struct v4l2_subdev *sd, const char *name,
		     const char *type, unsigned int index)
{
	snprintf(sd->name, sizeof(sd->name), "%s %s%u", name, type, index);
}
EXPORT_SYMBOL_GPL(max_set_sd_name);

void max_set_priv_name(char *name, const char *label, struct i2c_client *client)
{
	size_t size = V4L2_SUBDEV_NAME_SIZE;

	if (label)
		strscpy(name, label, size);
	else
		snprintf(name, size, "%s %d-%04x", client->dev.driver->name,
			 i2c_adapter_id(client->adapter), client->addr);
}
EXPORT_SYMBOL_GPL(max_set_priv_name);

int max_ser_reset(struct regmap *regmap)
{
	int ret;

	ret = regmap_update_bits(regmap, 0x10, 0x80, 0x80);
	if (ret)
		return ret;

	msleep(50);

	return 0;
}
EXPORT_SYMBOL_GPL(max_ser_reset);

int max_ser_wait_for_multiple(struct i2c_client *client, struct regmap *regmap,
			      u8 *addrs, unsigned int num_addrs)
{
	unsigned int i, j, val;
	int ret;

	for (i = 0; i < 10; i++) {
		for (j = 0; j < num_addrs; j++) {
			client->addr = addrs[j];

			ret = regmap_read(regmap, 0x0, &val);
			if (ret >= 0)
				return 0;
		}

		msleep(100);

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

static int max_ser_get_dev_id(struct regmap *regmap, unsigned int *dev_id)
{
	return regmap_read(regmap, 0xd, dev_id);
}

static int max_ser_fix_tx_ids(struct regmap *regmap, u8 addr)
{
	unsigned int addr_regs[] = { 0x7b, 0x83, 0x8b, 0x93, 0xa3, 0xab };
	unsigned int dev_id;
	unsigned int i;
	int ret;

	ret = max_ser_get_dev_id(regmap, &dev_id);
	if (ret)
		return ret;

	switch (dev_id) {
	case MAX_SER_MAX9265A_DEV_ID:
		for (i = 0; i < ARRAY_SIZE(addr_regs); i++) {
			ret = regmap_write(regmap, addr_regs[i], addr);
			if (ret)
				return ret;
		}

		break;
	default:
		return 0;
	}

	return 0;
}

int max_ser_change_address(struct i2c_client *client, struct regmap *regmap, u8 addr,
			   bool fix_tx_ids)
{
	int ret;

	ret = regmap_write(regmap, 0x0, addr << 1);
	if (ret)
		return ret;

	client->addr = addr;

	if (fix_tx_ids) {
		ret = max_ser_fix_tx_ids(regmap, addr);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(max_ser_change_address);

struct max_component_asc {
	struct v4l2_async_connection asc;
	struct fwnode_handle *ep;
	unsigned int index;
};

static int max_comp_notifier_bound(struct v4l2_async_notifier *notifier,
				   struct v4l2_subdev *sd,
				   struct v4l2_async_connection *base_asc)
{
	struct max_component_asc *asc = container_of(base_asc, struct max_component_asc, asc);
	struct max_component *comp = container_of(notifier, struct max_component, notifier);
	struct max_component *dest_comp;
	struct media_pad *sink;

	if (comp->notifier_comps && comp->notifier_comps[asc->index])
		dest_comp = comp->notifier_comps[asc->index];
	else if (comp->num_notifier_eps == 1)
		dest_comp = comp;
	else
		return -EINVAL;

	if (dest_comp->num_sink_pads != 1)
		return -EINVAL;

	sink = &dest_comp->sd.entity.pads[dest_comp->sink_pads_start];
	dest_comp->remote_sd = sd;

	return v4l2_create_fwnode_links_to_pad(sd, sink, MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
}

const struct v4l2_async_notifier_operations max_comp_notifier_ops = {
	.bound = max_comp_notifier_bound,
};

static void max_component_unregister_notifier(struct max_component *comp)
{
	if (!comp->num_notifier_eps)
		return;

	v4l2_async_nf_unregister(&comp->notifier);
	v4l2_async_nf_cleanup(&comp->notifier);
}

static int max_component_register_notifier(struct max_component *comp)
{
	struct max_component_asc *asc;
	unsigned int i;
	int ret;

	if (!comp->num_notifier_eps)
		return 0;

	v4l2_async_subdev_nf_init(&comp->notifier, &comp->sd);

	for (i = 0; i < comp->num_notifier_eps; i++) {
		if (!comp->notifier_eps[i])
			continue;

		asc = v4l2_async_nf_add_fwnode_remote(&comp->notifier, comp->notifier_eps[i],
						      struct max_component_asc);
		if (IS_ERR(asc)) {
			ret = PTR_ERR(asc);
			goto error;
		}

		asc->index = i;
	}

	comp->notifier.ops = &max_comp_notifier_ops;;

	ret = v4l2_async_nf_register(&comp->notifier);
	if (ret)
		goto error;

	return 0;

error:
	v4l2_async_nf_cleanup(&comp->notifier);

	return ret;
}

int max_component_register_v4l2_sd(struct max_component *comp)
{
	struct v4l2_subdev *sd = &comp->sd;
	unsigned int i;
	int ret;

	comp->num_pads = comp->num_source_pads + comp->num_sink_pads;

	comp->pads = kcalloc(comp->num_pads, sizeof(*comp->pads), GFP_KERNEL);
	if (!comp->pads)
		return -ENOMEM;

	v4l2_i2c_subdev_init(sd, comp->client, comp->sd_ops);

	max_set_sd_name(sd, comp->prefix, comp->name, comp->index);

	sd->entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sd->internal_ops = comp->internal_ops;
	sd->fwnode = comp->fwnode;

	if (comp->sink_pads_first) {
		comp->sink_pads_start = 0;
		comp->source_pads_start = comp->num_sink_pads;
	} else {
		comp->source_pads_start = 0;
		comp->sink_pads_start = comp->num_source_pads;
	}

	comp->sink_pads_end = comp->sink_pads_start + comp->num_sink_pads;
	comp->source_pads_end = comp->source_pads_start + comp->num_source_pads;

	for (i = comp->source_pads_start; i < comp->source_pads_end; i++)
		comp->pads[i].flags = MEDIA_PAD_FL_SOURCE;
	for (i = comp->sink_pads_start; i < comp->sink_pads_end; i++)
		comp->pads[i].flags = MEDIA_PAD_FL_SINK;

	v4l2_set_subdevdata(sd, comp);

	ret = media_entity_pads_init(&sd->entity, comp->num_pads, comp->pads);
	if (ret)
		goto error;

	ret = v4l2_subdev_init_finalize(sd);
	if (ret)
		goto error;

	ret = max_component_register_notifier(comp);
	if (ret)
		return ret;

	if (comp->v4l2_dev)
		ret = v4l2_device_register_subdev(comp->v4l2_dev, sd);
	else
		ret = v4l2_async_register_subdev(sd);

	if (ret)
		goto error;

	return 0;

error:
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	kfree(comp->pads);

	return ret;
}
EXPORT_SYMBOL_GPL(max_component_register_v4l2_sd);

void max_component_unregister_v4l2_sd(struct max_component *comp)
{
	struct v4l2_subdev *sd = &comp->sd;

	max_component_unregister_notifier(comp);

	if (comp->v4l2_dev)
		v4l2_device_unregister_subdev(sd);
	else
		v4l2_async_unregister_subdev(sd);

	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	kfree(comp->pads);
}
EXPORT_SYMBOL_GPL(max_component_unregister_v4l2_sd);

int max_components_link(struct max_component *source, unsigned int source_offset,
			struct max_component *sink, unsigned int sink_offset,
			bool create)
{
	unsigned int num_links = min(source->num_source_pads - source_offset,
				     sink->num_sink_pads - sink_offset);
	u32 flags = MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE;
	unsigned int i;
	int ret;

	if (!create)
		return num_links;

	for (i = 0; i < num_links; i++) {
		unsigned int source_pad = source->source_pads_start + source_offset + i;
		unsigned int sink_pad = sink->sink_pads_start + sink_offset + i;

		ret = media_create_pad_link(&source->sd.entity, source_pad,
					    &sink->sd.entity, sink_pad, flags);
		if (ret)
			return ret;
	}

	return num_links;
}
EXPORT_SYMBOL_GPL(max_components_link);

MODULE_LICENSE("GPL");
