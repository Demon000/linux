// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/stringify.h>

#include <media/mipi-csi2.h>

#include <uapi/linux/media-bus-format.h>

#include "max_serdes.h"

#define MAX_MIPI_FMT(_dt, _bpp)	\
{				\
	.dt = (_dt),		\
	.bpp = (_bpp),		\
}

static const struct max_mipi_format max_mipi_formats[] = {
	MAX_MIPI_FMT(MIPI_CSI2_DT_EMBEDDED_8B, 8),
	MAX_MIPI_FMT(MIPI_CSI2_DT_YUV422_8B, 16),
	MAX_MIPI_FMT(MIPI_CSI2_DT_YUV422_10B, 20),
	MAX_MIPI_FMT(MIPI_CSI2_DT_RGB565, 16),
	MAX_MIPI_FMT(MIPI_CSI2_DT_RGB666, 18),
	MAX_MIPI_FMT(MIPI_CSI2_DT_RGB888, 24),
	MAX_MIPI_FMT(MIPI_CSI2_DT_RAW8, 8),
	MAX_MIPI_FMT(MIPI_CSI2_DT_RAW10, 10),
	MAX_MIPI_FMT(MIPI_CSI2_DT_RAW12, 12),
	MAX_MIPI_FMT(MIPI_CSI2_DT_RAW14, 14),
	MAX_MIPI_FMT(MIPI_CSI2_DT_RAW16, 16),
};

const struct max_mipi_format *max_mipi_format_by_dt(u8 dt)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(max_mipi_formats); i++)
		if (max_mipi_formats[i].dt == dt)
			return &max_mipi_formats[i];

	return NULL;
}
EXPORT_SYMBOL_GPL(max_mipi_format_by_dt);

int max_get_fd_stream_entry(struct v4l2_subdev *sd,
			    unsigned int pad, unsigned int stream,
			    struct v4l2_mbus_frame_desc_entry *entry)
{
	struct v4l2_mbus_frame_desc fd;
	unsigned int i;
	int ret;

	ret = v4l2_subdev_call(sd, pad, get_frame_desc, pad, &fd);
	if (ret)
		return ret;

	if (fd.type != V4L2_MBUS_FRAME_DESC_TYPE_CSI2)
		return -EOPNOTSUPP;

	for (i = 0; i < fd.num_entries; i++) {
		if (fd.entry[i].stream == stream) {
			*entry = fd.entry[i];
			return 0;
		}
	}

	return -ENOENT;
}
EXPORT_SYMBOL(max_get_fd_stream_entry);

int max_get_bpps(struct max_source *sources, u32 source_sink_pad_offset,
		 const struct v4l2_subdev_krouting *routing,
		 u32 pad, u64 streams_mask, u32 *bpps)
{
	struct v4l2_subdev_route *route;
	int ret;

	for_each_active_route(routing, route) {
		struct v4l2_mbus_frame_desc_entry entry;
		const struct max_mipi_format *format;
		struct max_source *source;

		if (route->sink_pad == pad) {
			if (!(BIT_ULL(route->sink_stream) & streams_mask))
				continue;
		} else if (route->source_pad == pad) {
			if (!(BIT_ULL(route->source_stream) & streams_mask))
				continue;
		} else {
			continue;
		}

		source = &sources[route->sink_pad + source_sink_pad_offset];

		ret = max_get_fd_stream_entry(source->sd, source->pad,
					      route->sink_stream, &entry);
		if (ret)
			return ret;

		format = max_mipi_format_by_dt(entry.bus.csi2.dt);
		if (!format)
			continue;

		*bpps |= BIT(format->bpp);
	}

	return 0;
}
EXPORT_SYMBOL(max_get_bpps);

MODULE_DESCRIPTION("Maxim GMSL2 Serializer/Deserializer Driver");
MODULE_AUTHOR("Cosmin Tanislav <cosmin.tanislav@analog.com>");
MODULE_LICENSE("GPL");
