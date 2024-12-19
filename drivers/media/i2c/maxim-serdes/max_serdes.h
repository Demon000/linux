// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Analog Devices Inc.
 */

#ifndef MAX_SERDES_H
#define MAX_SERDES_H

#include <linux/types.h>

#include <media/v4l2-subdev.h>

#define MAX_SERDES_PHYS_MAX		4
#define MAX_SERDES_STREAMS_NUM 		4
#define MAX_SERDES_VC_ID_NUM		4

struct max_phys_config {
	unsigned int lanes[MAX_SERDES_PHYS_MAX];
	unsigned int clock_lane[MAX_SERDES_PHYS_MAX];
};

struct max_phys_configs {
	const struct max_phys_config *configs;
	unsigned int num_configs;
};

struct max_i2c_xlate {
	u8 src;
	u8 dst;
};

struct max_mipi_format {
	u8 dt;
	u8 bpp;
};

struct max_source {
	struct v4l2_subdev *sd;
	u16 pad;
	struct fwnode_handle *ep_fwnode;

	unsigned int index;
};

struct max_asc {
	struct v4l2_async_connection base;
	struct max_source *source;
};

static inline struct max_asc *asc_to_max(struct v4l2_async_connection *asc)
{
	return container_of(asc, struct max_asc, base);
}

const struct max_mipi_format *max_mipi_format_by_dt(u8 dt);

int max_get_fd_stream_entry(struct v4l2_subdev *sd,
			    unsigned int pad, unsigned int stream,
			    struct v4l2_mbus_frame_desc_entry *entry);

#endif // MAX_SERDES_H
