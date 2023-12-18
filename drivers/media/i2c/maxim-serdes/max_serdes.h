// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include <linux/regmap.h>

#include <media/mipi-csi2.h>
#include <media/v4l2-mediabus.h>

#ifndef MAX_SERDES_H
#define MAX_SERDES_H

#define MAX_SERDES_PHYS_MAX		4
#define MAX_SERDES_STREAMS_NUM		4
#define MAX_SERDES_VC_ID_NUM		4

extern const struct regmap_config max_i2c_regmap;

struct max_serdes_phy_configs {
	unsigned int lanes[MAX_SERDES_PHYS_MAX];
	unsigned int clock_lane[MAX_SERDES_PHYS_MAX];
};

struct max_serdes_phys_configs {
	const struct max_serdes_phy_configs *configs;
	unsigned int num_configs;
};

struct max_i2c_xlate {
	u8 src;
	u8 dst;
};

struct max_format {
	const char *name;
	u32 code;
	u8 dt;
	u8 bpp;
	bool dbl;
};

#endif // MAX_SERDES_H
