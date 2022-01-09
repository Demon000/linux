// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2016 Rockchip Electronics Co. Ltd.
 * Author: Lin Huang <hl@rock-chips.com>
 */

#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <soc/rockchip/rockchip_sip.h>

#include "clk.h"

#define SIP_RET_SET_RATE_TIMEOUT	-6
#define ATF_NUM_PAGES			2

struct rockchip_ddrclk {
	struct clk_hw	hw;
	void __iomem	*reg_base;
	struct share_params __iomem *share_memory;
	int		mux_offset;
	int		mux_shift;
	int		mux_width;
	int		div_shift;
	int		div_width;
	int		ddr_flag;
	spinlock_t	*lock;
};

#define to_rockchip_ddrclk_hw(hw) container_of(hw, struct rockchip_ddrclk, hw)

#define SCREEN_NULL		0
#define SCREEN_LVDS		2
#define SCREEN_TVOUT		5
#define SCREEN_HDMI		6
#define SCREEN_MIPI		7
#define SCREEN_EDP		9
#define SCREEN_DP		13

static int rk_drm_get_lcdc_type(void)
{
	struct drm_device *drm;
	u32 lcdc_type = 0;

	drm = drm_device_get_by_name("rockchip");
	if (drm) {
		struct drm_connector *conn;

		list_for_each_entry(conn, &drm->mode_config.connector_list,
				    head) {
			if (conn->encoder) {
				lcdc_type = conn->connector_type;
				break;
			}
		}
	}

	switch (lcdc_type) {
	case DRM_MODE_CONNECTOR_LVDS:
		lcdc_type = SCREEN_LVDS;
		break;
	case DRM_MODE_CONNECTOR_DisplayPort:
		lcdc_type = SCREEN_DP;
		break;
	case DRM_MODE_CONNECTOR_HDMIA:
	case DRM_MODE_CONNECTOR_HDMIB:
		lcdc_type = SCREEN_HDMI;
		break;
	case DRM_MODE_CONNECTOR_TV:
		lcdc_type = SCREEN_TVOUT;
		break;
	case DRM_MODE_CONNECTOR_eDP:
		lcdc_type = SCREEN_EDP;
		break;
	case DRM_MODE_CONNECTOR_DSI:
		lcdc_type = SCREEN_MIPI;
		break;
	default:
		lcdc_type = SCREEN_NULL;
		break;
	}

	return lcdc_type;
}

static int rockchip_ddrclk_sip_set_rate(struct clk_hw *hw, unsigned long drate,
					unsigned long prate)
{
	struct rockchip_ddrclk *ddrclk = to_rockchip_ddrclk_hw(hw);
	unsigned long flags;
	struct arm_smccc_res res;

	spin_lock_irqsave(ddrclk->lock, flags);
	arm_smccc_smc(ROCKCHIP_SIP_DRAM_FREQ, drate, 0,
		      ROCKCHIP_SIP_CONFIG_DRAM_SET_RATE,
		      0, 0, 0, 0, &res);
	spin_unlock_irqrestore(ddrclk->lock, flags);

	return res.a0;
}

static unsigned long
rockchip_ddrclk_sip_recalc_rate(struct clk_hw *hw,
				unsigned long parent_rate)
{
	struct arm_smccc_res res;

	arm_smccc_smc(ROCKCHIP_SIP_DRAM_FREQ, 0, 0,
		      ROCKCHIP_SIP_CONFIG_DRAM_GET_RATE,
		      0, 0, 0, 0, &res);

	return res.a0;
}

static long rockchip_ddrclk_sip_round_rate(struct clk_hw *hw,
					   unsigned long rate,
					   unsigned long *prate)
{
	struct arm_smccc_res res;

	arm_smccc_smc(ROCKCHIP_SIP_DRAM_FREQ, rate, 0,
		      ROCKCHIP_SIP_CONFIG_DRAM_ROUND_RATE,
		      0, 0, 0, 0, &res);

	return res.a0;
}

static u8 rockchip_ddrclk_get_parent(struct clk_hw *hw)
{
	struct rockchip_ddrclk *ddrclk = to_rockchip_ddrclk_hw(hw);
	u32 val;

	val = readl(ddrclk->reg_base +
			ddrclk->mux_offset) >> ddrclk->mux_shift;
	val &= GENMASK(ddrclk->mux_width - 1, 0);

	return val;
}

static const struct clk_ops rockchip_ddrclk_sip_ops = {
	.recalc_rate = rockchip_ddrclk_sip_recalc_rate,
	.set_rate = rockchip_ddrclk_sip_set_rate,
	.round_rate = rockchip_ddrclk_sip_round_rate,
	.get_parent = rockchip_ddrclk_get_parent,
};

static int rockchip_ddrclk_sip_set_rate_v2(struct clk_hw *hw,
					   unsigned long drate,
					   unsigned long prate)
{
	struct rockchip_ddrclk *ddrclk = to_rockchip_ddrclk_hw(hw);
	struct arm_smccc_res res;

	ddrclk->share_memory->hz = drate;
	ddrclk->share_memory->lcdc_type = rk_drm_get_lcdc_type();

	arm_smccc_smc(ROCKCHIP_SIP_DRAM_FREQ, SHARE_PAGE_TYPE_DDR, 0,
		      ROCKCHIP_SIP_CONFIG_DRAM_SET_RATE, 0, 0, 0, 0, &res);

	if ((int)res.a1 == SIP_RET_SET_RATE_TIMEOUT)
		pr_err("%s: timeout setting rate\n", __func__);

	return 0;
}

static unsigned long
rockchip_ddrclk_sip_recalc_rate_v2(struct clk_hw *hw,
				   unsigned long parent_rate)
{
	struct arm_smccc_res res;

	arm_smccc_smc(ROCKCHIP_SIP_DRAM_FREQ, SHARE_PAGE_TYPE_DDR, 0,
		      ROCKCHIP_SIP_CONFIG_DRAM_GET_RATE, 0, 0, 0, 0, &res);
	if (!res.a0)
		return res.a1;

	return 0;
}

static long rockchip_ddrclk_sip_round_rate_v2(struct clk_hw *hw,
					      unsigned long rate,
					      unsigned long *prate)
{
	struct rockchip_ddrclk *ddrclk = to_rockchip_ddrclk_hw(hw);
	struct arm_smccc_res res;

	ddrclk->share_memory->hz = rate;

	arm_smccc_smc(ROCKCHIP_SIP_DRAM_FREQ, SHARE_PAGE_TYPE_DDR, 0,
		      ROCKCHIP_SIP_CONFIG_DRAM_ROUND_RATE, 0, 0, 0, 0, &res);
	if (!res.a0)
		return res.a1;

	return 0;
}

static const struct clk_ops rockchip_ddrclk_sip_ops_v2 = {
	.recalc_rate = rockchip_ddrclk_sip_recalc_rate_v2,
	.set_rate = rockchip_ddrclk_sip_set_rate_v2,
	.round_rate = rockchip_ddrclk_sip_round_rate_v2,
	.get_parent = rockchip_ddrclk_get_parent,
};

struct clk *rockchip_clk_register_ddrclk(const char *name, int flags,
					 const char *const *parent_names,
					 u8 num_parents, int mux_offset,
					 int mux_shift, int mux_width,
					 int div_shift, int div_width,
					 int ddr_flag, void __iomem *reg_base,
					 spinlock_t *lock)
{
	struct arm_smccc_res res;
	struct rockchip_ddrclk *ddrclk;
	struct clk_init_data init;
	struct clk *clk;

	ddrclk = kzalloc(sizeof(*ddrclk), GFP_KERNEL);
	if (!ddrclk)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.parent_names = parent_names;
	init.num_parents = num_parents;

	init.flags = flags;
	init.flags |= CLK_SET_RATE_NO_REPARENT;

	switch (ddr_flag) {
	case ROCKCHIP_DDRCLK_SIP:
		init.ops = &rockchip_ddrclk_sip_ops;
		break;
	case ROCKCHIP_DDRCLK_SIP_V2:
		arm_smccc_smc(ROCKCHIP_SIP_SHARE_MEM, ATF_NUM_PAGES,
			      SHARE_PAGE_TYPE_DDR, 0, 0, 0, 0, 0, &res);
		if (res.a0) {
			pr_err("%s: failed to get ATF memory: %lu\n", __func__,
			       res.a0);
			kfree(ddrclk);
			return ERR_PTR(-EINVAL);
		}

		ddrclk->share_memory = ioremap(res.a1, ATF_NUM_PAGES * PAGE_SIZE);
		if (!ddrclk->share_memory) {
			pr_err("%s: failed to remap ATF memory\n", __func__);
			kfree(ddrclk);
			return ERR_PTR(-EINVAL);
		}

		init.ops = &rockchip_ddrclk_sip_ops_v2;
		break;
	default:
		pr_err("%s: unsupported ddrclk type %d\n", __func__, ddr_flag);
		kfree(ddrclk);
		return ERR_PTR(-EINVAL);
	}

	ddrclk->reg_base = reg_base;
	ddrclk->lock = lock;
	ddrclk->hw.init = &init;
	ddrclk->mux_offset = mux_offset;
	ddrclk->mux_shift = mux_shift;
	ddrclk->mux_width = mux_width;
	ddrclk->div_shift = div_shift;
	ddrclk->div_width = div_width;
	ddrclk->ddr_flag = ddr_flag;

	clk = clk_register(NULL, &ddrclk->hw);
	if (IS_ERR(clk))
		kfree(ddrclk);

	return clk;
}
EXPORT_SYMBOL_GPL(rockchip_clk_register_ddrclk);
