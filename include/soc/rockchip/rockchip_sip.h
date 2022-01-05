/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2016, Fuzhou Rockchip Electronics Co., Ltd
 * Author: Lin Huang <hl@rock-chips.com>
 */
#ifndef __SOC_ROCKCHIP_SIP_H
#define __SOC_ROCKCHIP_SIP_H

#define ROCKCHIP_SIP_DRAM_FREQ			0x82000008
#define ROCKCHIP_SIP_SHARE_MEM			0x82000009
#define ROCKCHIP_SIP_CONFIG_DRAM_INIT		0x00
#define ROCKCHIP_SIP_CONFIG_DRAM_SET_RATE	0x01
#define ROCKCHIP_SIP_CONFIG_DRAM_ROUND_RATE	0x02
#define ROCKCHIP_SIP_CONFIG_DRAM_SET_AT_SR	0x03
#define ROCKCHIP_SIP_CONFIG_DRAM_GET_BW		0x04
#define ROCKCHIP_SIP_CONFIG_DRAM_GET_RATE	0x05
#define ROCKCHIP_SIP_CONFIG_DRAM_CLR_IRQ	0x06
#define ROCKCHIP_SIP_CONFIG_DRAM_SET_PARAM	0x07
#define ROCKCHIP_SIP_CONFIG_DRAM_SET_ODT_PD	0x08

#define SHARE_PAGE_TYPE_DDR	2

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
