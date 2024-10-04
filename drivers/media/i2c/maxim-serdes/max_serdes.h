// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include <linux/types.h>

#include <media/v4l2-subdev.h>

#ifndef MAX_SERDES_H
#define MAX_SERDES_H

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

int max_get_fd_stream_entry(struct v4l2_subdev *sd,
			    unsigned int pad, unsigned int stream,
			    struct v4l2_mbus_frame_desc_entry *entry);

#endif // MAX_SERDES_H
