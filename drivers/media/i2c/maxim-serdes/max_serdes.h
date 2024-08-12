// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include <linux/types.h>

#ifndef MAX_SERDES_H
#define MAX_SERDES_H

#define MAX_SERDES_PHYS_MAX		4
#define MAX_SERDES_STREAMS_NUM 		4
#define MAX_SERDES_VC_ID_NUM		4

struct max_phy_configs {
	unsigned int lanes[MAX_SERDES_PHYS_MAX];
	unsigned int clock_lane[MAX_SERDES_PHYS_MAX];
};

struct max_phys_configs {
	const struct max_phy_configs *configs;
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

const struct max_format *max_format_by_index(unsigned int index);
const struct max_format *max_format_by_code(u32 code);
const struct max_format *max_format_by_dt(u8 dt);
u8 max_format_dt_by_code(u32 code);

#endif // MAX_SERDES_H
