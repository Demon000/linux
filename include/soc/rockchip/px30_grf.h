/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Rockchip General Register Files definitions
 *
 * Copyright (c) 2018, Collabora Ltd.
 * Author: Enric Balletbo i Serra <enric.balletbo@collabora.com>
 */

#ifndef __SOC_PX30_GRF_H
#define __SOC_PX30_GRF_H

/* PMU GRF Registers */
#define PX30_PMUGRF_OS_REG2		0x208
#define PX30_PMUGRF_DDRTYPE_SHIFT	13
#define PX30_PMUGRF_DDRTYPE_MASK	7
#define PX30_PMUGRF_DDRTYPE_DDR4	0
#define PX30_PMUGRF_DDRTYPE_DDR3	3
#define PX30_PMUGRF_DDRTYPE_LPDDR2	5
#define PX30_PMUGRF_DDRTYPE_LPDDR3	6
#define PX30_PMUGRF_DDRTYPE_LPDDR4	7

#endif
