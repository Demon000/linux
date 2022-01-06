/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Author: Lin Huang <hl@rock-chips.com>
 */
#ifndef __SOC_ROCKCHIP_DDR_H
#define __SOC_ROCKCHIP_DDR_H

#include <linux/types.h>

struct share_params {
	u32 hz;
	u32 lcdc_type;
	u32 vop;
	u32 vop_dclk_mode;
	u32 sr_idle_en;
	u32 addr_mcu_el3;
	/*
	 * 1: need to wait flag1
	 * 0: never wait flag1
	 */
	u32 wait_flag1;
	/*
	 * 1: need to wait flag1
	 * 0: never wait flag1
	 */
	u32 wait_flag0;
	u32 complete_hwirq;
	 /* if need, add parameter after */
};

#endif
