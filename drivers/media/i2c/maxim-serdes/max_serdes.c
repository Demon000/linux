// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include "max_serdes_priv.h"

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
	.dt = MIPI_CSI2_DT_ ## _dt,		\
	.bpp = (_bpp),				\
	.dbl = (_dbl),				\
}

static const struct max_format max_formats[] = {
	MAX_FMT(FIXED, EMBEDDED_8B, 8, 1),
	MAX_FMT(YUYV8_1X16, YUV422_8B, 16, 0),
	MAX_FMT(YUYV10_1X20, YUV422_10B, 20, 0),
	MAX_FMT(RGB565_1X16, RGB565, 16, 0),
	MAX_FMT(RGB666_1X18, RGB666, 18, 0),
	MAX_FMT(RGB888_1X24, RGB888, 24, 0),
	MAX_FMT(SBGGR8_1X8, RAW8, 8, 1),
	MAX_FMT(SGBRG8_1X8, RAW8, 8, 1),
	MAX_FMT(SGRBG8_1X8, RAW8, 8, 1),
	MAX_FMT(SRGGB8_1X8, RAW8, 8, 1),
	MAX_FMT(SBGGR10_1X10, RAW10, 10, 1),
	MAX_FMT(SGBRG10_1X10, RAW10, 10, 1),
	MAX_FMT(SGRBG10_1X10, RAW10, 10, 1),
	MAX_FMT(SRGGB10_1X10, RAW10, 10, 1),
	MAX_FMT(SBGGR12_1X12, RAW12, 12, 1),
	MAX_FMT(SGBRG12_1X12, RAW12, 12, 1),
	MAX_FMT(SGRBG12_1X12, RAW12, 12, 1),
	MAX_FMT(SRGGB12_1X12, RAW12, 12, 1),
	MAX_FMT(SBGGR14_1X14, RAW14, 14, 0),
	MAX_FMT(SGBRG14_1X14, RAW14, 14, 0),
	MAX_FMT(SGRBG14_1X14, RAW14, 14, 0),
	MAX_FMT(SRGGB14_1X14, RAW14, 14, 0),
	MAX_FMT(SBGGR16_1X16, RAW16, 16, 0),
	MAX_FMT(SGBRG16_1X16, RAW16, 16, 0),
	MAX_FMT(SGRBG16_1X16, RAW16, 16, 0),
	MAX_FMT(SRGGB16_1X16, RAW16, 16, 0),
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

u8 max_format_dt_by_code(u32 code)
{
	const struct max_format *fmt;

	fmt = max_format_by_code(code);
	if (!fmt)
		return 0;

	return fmt->dt;
}
EXPORT_SYMBOL_GPL(max_format_dt_by_code);

static void max_comp_set_name(struct max_component *comp)
{
	struct v4l2_subdev *sd = &comp->sd;
	char index_str[3] = { 0 };

	if (!comp->unique)
		snprintf(index_str, sizeof(index_str), "%u", comp->index);

	if (comp->prefix)
		snprintf(sd->name, sizeof(sd->name), "%s %s%s",
			 comp->prefix, comp->name, index_str);
	else
		snprintf(sd->name, sizeof(sd->name), "%s %d-%04x %s%s",
			 comp->client->dev.driver->name,
			 i2c_adapter_id(comp->client->adapter),
			 comp->client->addr,
			 comp->name, index_str);
}

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

static int max_component_init_1_to_1_routing(struct max_component *comp,
					     struct v4l2_subdev_krouting *routing)
{
	unsigned int i;

	routing->num_routes = min(comp->num_source_pads, comp->num_sink_pads);

	routing->routes = kcalloc(routing->num_routes, sizeof(*routing->routes),
				  GFP_KERNEL);
	if (!routing->routes)
		return -ENOMEM;

	for (i = 0; i < routing->num_routes; i++) {
		struct v4l2_subdev_route *route = &routing->routes[i];

		route->sink_pad = comp->sink_pads_start + i;
		route->sink_stream = 0;
		route->source_pad = comp->source_pads_start + i;
		route->source_stream = 0;
		route->flags = V4L2_SUBDEV_ROUTE_FL_ACTIVE;
	}

	return 0;
}

int max_component_init_routing(struct max_component *comp,
			       struct v4l2_subdev_krouting *routing)
{
	if (comp->routing_disallow ==
		 (V4L2_SUBDEV_ROUTING_ONLY_1_TO_1 |
		  V4L2_SUBDEV_ROUTING_NO_STREAM_MIX))
		return max_component_init_1_to_1_routing(comp, routing);
	else
		return -EINVAL;

	return 0;
}
EXPORT_SYMBOL_GPL(max_component_init_routing);

int max_component_init_cfg(struct v4l2_subdev *sd, struct v4l2_subdev_state *state)
{
	struct max_component *comp = sd_max_component(sd);
	struct v4l2_subdev_krouting routing;
	int ret;

	if (comp->init_routing)
		ret = comp->init_routing(comp, &routing);
	else
		ret = max_component_init_routing(comp, &routing);

	if (!ret) {
		ret = v4l2_subdev_set_routing(&comp->sd, state, &routing);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(max_component_init_cfg);

struct v4l2_subdev *max_component_get_remote_sd(struct max_component *comp,
						u32 pad, u32 *remote_pad)
{
	struct v4l2_subdev *sd = &comp->sd;
	struct media_pad *remote_media_pad;
	struct media_pad *local_media_pad;

	if (pad > sd->entity.num_pads)
		return NULL;

	local_media_pad = &sd->entity.pads[pad];

	remote_media_pad = media_pad_remote_pad_unique(local_media_pad);
	if (IS_ERR(remote_media_pad))
		return NULL;

	if (!is_media_entity_v4l2_subdev(remote_media_pad->entity))
		return NULL;

	*remote_pad = remote_media_pad->index;

	return media_entity_to_v4l2_subdev(remote_media_pad->entity);
}
EXPORT_SYMBOL_GPL(max_component_get_remote_sd);

int max_component_validate_routing(struct max_component *comp,
				   enum v4l2_subdev_format_whence which,
				   struct v4l2_subdev_krouting *routing)
{
	if (which == V4L2_SUBDEV_FORMAT_ACTIVE && comp->enabled_source_streams)
		return -EBUSY;

	if (routing->num_routes > V4L2_FRAME_DESC_ENTRY_MAX)
		return -EINVAL;

	return v4l2_subdev_routing_validate(&comp->sd, routing, comp->routing_disallow);
}
EXPORT_SYMBOL_GPL(max_component_validate_routing);

int max_component_set_routing(struct max_component *comp,
			      struct v4l2_subdev_state *state,
			      enum v4l2_subdev_format_whence which,
			      struct v4l2_subdev_krouting *routing)
{
	return v4l2_subdev_set_routing(&comp->sd, state, routing);
}
EXPORT_SYMBOL_GPL(max_component_set_routing);

int max_component_set_validate_routing(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *state,
				       enum v4l2_subdev_format_whence which,
				       struct v4l2_subdev_krouting *routing)
{
	struct max_component *comp = sd_max_component(sd);
	int ret;

	ret = max_component_validate_routing(comp, which, routing);
	if (ret)
		return ret;

	return max_component_set_routing(comp, state, which, routing);
}
EXPORT_SYMBOL_GPL(max_component_set_validate_routing);

u64 max_component_set_enabled_streams(struct max_component *comp,
				      u32 pad, u64 streams_mask, bool enable)
{
	u32 index;

	if (!max_component_is_pad_source(comp, pad))
		return 0;

	index = pad - comp->source_pads_start;

	if (enable)
		comp->pads_enabled_streams[index] |= streams_mask;
	else
		comp->pads_enabled_streams[index] &= ~streams_mask;

	return comp->pads_enabled_streams[index];
}
EXPORT_SYMBOL_GPL(max_component_set_enabled_streams);

int max_component_set_remote_streams_enable(struct max_component *comp,
					    struct v4l2_subdev_state *state,
					    u32 pad, u64 streams_mask,
					    bool enable)
{
	unsigned int i;
	int ret;

	if (!max_component_is_pad_source(comp, pad))
		return 0;

	for (i = comp->sink_pads_start; i < comp->sink_pads_end; i++) {
		u64 source_streams = streams_mask;
		struct v4l2_subdev *remote_sd;
		u64 sink_streams;
		u32 remote_pad;

		sink_streams = v4l2_subdev_state_xlate_streams(state, pad, i,
							       &source_streams);
		if (!sink_streams)
			continue;

		remote_sd = max_component_get_remote_sd(comp, i, &remote_pad);
		if (!remote_sd)
			return -ENODEV;

		if (enable)
			ret = v4l2_subdev_enable_streams(remote_sd, remote_pad,
							 sink_streams);
		else
			ret = v4l2_subdev_disable_streams(remote_sd, remote_pad,
							  sink_streams);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(max_component_set_remote_streams_enable);

int max_component_streams_enable(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct max_component *comp = sd_max_component(sd);

	max_component_set_enabled_streams(comp, pad, streams_mask, true);

	return max_component_set_remote_streams_enable(comp, state, pad,
						       streams_mask, true);
}
EXPORT_SYMBOL_GPL(max_component_streams_enable);

int max_component_streams_disable(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct max_component *comp = sd_max_component(sd);

	max_component_set_enabled_streams(comp, pad, streams_mask, false);

	return max_component_set_remote_streams_enable(comp, state, pad,
						       streams_mask, false);
}
EXPORT_SYMBOL_GPL(max_component_streams_disable);

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

int max_component_get_sink_pad_frame_desc(struct max_component *comp,
					  unsigned int pad,
					  struct v4l2_mbus_frame_desc *fd)
{
	struct v4l2_subdev *remote_sd;
	u32 remote_pad;

	/* Find the remote subdev and pad for this sink pad. */
	remote_sd = max_component_get_remote_sd(comp, pad, &remote_pad);
	if (!remote_sd)
		return -ENODEV;

	/* Get the remote subdev's frame descriptors. */
	return v4l2_subdev_call(remote_sd, pad, get_frame_desc, remote_pad, fd);
}
EXPORT_SYMBOL_GPL(max_component_get_sink_pad_frame_desc);

int max_component_get_stream_frame_entry(struct v4l2_mbus_frame_desc *fd,
					 unsigned int stream,
					 struct v4l2_mbus_frame_desc_entry *entry)
{
	unsigned int i;

	for (i = 0; i < fd->num_entries; i++)
		if (fd->entry[i].stream == stream)
			break;

	if (i == fd->num_entries)
		return -EINVAL;

	*entry = fd->entry[i];

	return 0;
}
EXPORT_SYMBOL_GPL(max_component_get_stream_frame_entry);

int max_component_routing_get_frame_desc(struct v4l2_subdev *sd,
					 struct v4l2_subdev_krouting *routing,
					 unsigned int pad,
					 struct v4l2_mbus_frame_desc *fd)
{
	struct max_component *comp = sd_max_component(sd);
	struct v4l2_subdev_route *route;
	u64 sink_pads_mask = 0;
	unsigned int i, j;
	int ret;

	if (!max_component_is_pad_source(comp, pad))
		return -EINVAL;

	fd->type = V4L2_MBUS_FRAME_DESC_TYPE_CSI2;
	fd->num_entries = 0;

	/*
	 * Find all sink pads connected to this source pad to reduce the number
	 * of calls to get_frame_desc.
	 */
	for_each_active_route(routing, route) {
		if (route->source_pad != pad)
			continue;

		sink_pads_mask |= BIT_ULL(route->sink_pad);
	}

	for (i = comp->sink_pads_start; i < comp->sink_pads_end; i++) {
		struct v4l2_mbus_frame_desc remote_fd;

		if (!(sink_pads_mask & BIT_ULL(i)))
			continue;

		/* Get the frame descriptor of the remote side of this sink pad. */
		ret = max_component_get_sink_pad_frame_desc(comp, i, &remote_fd);
		if (ret)
			continue;

		for (j = 0; j < remote_fd.num_entries; j++) {
			u32 sink_stream = remote_fd.entry[j].stream;
			u32 source_stream;
			u32 source_pad;

			/*
			 * Translate back from this sink pad to the source pad to find
			 * the source stream.
			 */
			ret = v4l2_subdev_routing_find_opposite_end(routing,
								    i, sink_stream,
								    &source_pad,
								    &source_stream);
			if (ret)
				continue;

			if (source_pad != pad)
				continue;

			if (fd->num_entries == V4L2_FRAME_DESC_ENTRY_MAX)
				return -E2BIG;

			fd->entry[fd->num_entries] = remote_fd.entry[j];
			fd->entry[fd->num_entries].stream = source_stream;

			fd->num_entries++;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(max_component_routing_get_frame_desc);

int max_component_get_frame_desc(struct v4l2_subdev *sd, unsigned int pad,
				 struct v4l2_mbus_frame_desc *fd)
{
	struct v4l2_subdev_state *state;
	int ret;

	state = v4l2_subdev_lock_and_get_active_state(sd);

	ret = max_component_routing_get_frame_desc(sd, &state->routing, pad, fd);

	v4l2_subdev_unlock_state(state);

	return ret;
}
EXPORT_SYMBOL_GPL(max_component_get_frame_desc);

int max_component_register_v4l2_sd(struct max_component *comp)
{
	struct v4l2_subdev *sd = &comp->sd;
	struct v4l2_subdev_state *state;
	unsigned int i;
	int ret;

	comp->num_pads = comp->num_source_pads + comp->num_sink_pads;

	comp->pads = kcalloc(comp->num_pads, sizeof(*comp->pads), GFP_KERNEL);
	if (!comp->pads)
		return -ENOMEM;

	comp->pads_enabled_streams = kcalloc(comp->num_source_pads,
					     sizeof(comp->pads_enabled_streams),
					     GFP_KERNEL);
	if (!comp->pads_enabled_streams)
		return -ENOMEM;

	v4l2_subdev_init(sd, comp->sd_ops);

	max_comp_set_name(comp);

	if (!comp->dev)
		comp->dev = &comp->client->dev;
	sd->owner = comp->dev->driver->owner;
	sd->dev = comp->dev;
	sd->entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	sd->entity.ops = comp->mc_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_STREAMS;
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

	state = v4l2_subdev_lock_and_get_active_state(sd);
	/*
	 * Call set_routing just to initialize an empty routing configuration.
	 * This can't be done in init_cfg since that gets called to initialize
	 * try states too, which we don't want to.
	 * Downside is an extra memory copy.
	 */
	ret = v4l2_subdev_call(sd, pad, set_routing, state,
			       V4L2_SUBDEV_FORMAT_ACTIVE, &state->routing);
	v4l2_subdev_unlock_state(state);
	if (ret)
		return ret;

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
			struct max_component *sink, unsigned int sink_offset)
{
	unsigned int num_links = min(source->num_source_pads - source_offset,
				     sink->num_sink_pads - sink_offset);
	u32 flags = MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE;
	unsigned int i, source_pad, sink_pad;
	int ret;

	for (i = 0; i < num_links; i++) {
		source_pad = source->source_pads_start + source_offset + i;
		sink_pad = sink->sink_pads_start + sink_offset + i;

		ret = media_create_pad_link(&source->sd.entity, source_pad,
					    &sink->sd.entity, sink_pad, flags);
		if (ret)
			return ret;
	}

	return num_links;
}
EXPORT_SYMBOL_GPL(max_components_link);

struct v4l2_subdev_stream_config *
max_find_stream_config(struct v4l2_subdev_stream_configs *configs,
		       u32 pad, u32 stream)
{
	struct v4l2_subdev_stream_config *config;
	unsigned int i;

	if (!configs)
		return NULL;

	for (i = 0; i < configs->num_configs; i++) {
		config = &configs->configs[i];

		if (config->pad == pad && config->stream == stream)
			break;
	}

	if (i == configs->num_configs)
		return NULL;

	return config;
}
EXPORT_SYMBOL_GPL(max_find_stream_config);

MODULE_LICENSE("GPL");
