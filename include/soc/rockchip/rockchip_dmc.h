/*
 * Copyright (c) 2017, Fuzhou Rockchip Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */
#ifndef __SOC_ROCKCHIP_DMC_H
#define __SOC_ROCKCHIP_DMC_H

#include <linux/devfreq.h>

#ifdef CONFIG_ARM_ROCKCHIP_DMC_DEVFREQ
void rockchip_dmcfreq_lock(void);
void rockchip_dmcfreq_unlock(void);
#else
static inline void rockchip_dmcfreq_lock(void)
{
}

static inline void rockchip_dmcfreq_unlock(void)
{
}

#endif

#endif
