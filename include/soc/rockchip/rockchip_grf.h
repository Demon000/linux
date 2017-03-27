/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Rockchip General Register Files definitions
 *
 * Copyright (c) 2018, Collabora Ltd.
 * Author: Enric Balletbo i Serra <enric.balletbo@collabora.com>
 */

#ifndef __SOC_ROCKCHIP_GRF_H
#define __SOC_ROCKCHIP_GRF_H

#define PMUGRF_OS_REG2	0x308
#define READ_DRAMTYPE_INFO(n)		(((n) >> 13) & 0x7)
#define READ_CH_INFO(n)			(((n) >> 28) & 0x3)

enum {
	DDR3 = 3,
	LPDDR3 = 6,
	LPDDR4 = 7,
	UNUSED = 0xFF
};

#endif
