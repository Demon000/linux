// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016, Fuzhou Rockchip Electronics Co., Ltd.
 * Author: Lin Huang <hl@rock-chips.com>
 */

#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/devfreq.h>
#include <linux/devfreq-event.h>
#include <linux/interrupt.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/pm_qos.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/rwsem.h>
#include <linux/suspend.h>

#include <soc/rockchip/rockchip_grf.h>
#include <soc/rockchip/rockchip_sip.h>

#define DDR_CLK_FREQ_CHANGE_TIMEOUT	(17 * 5)

static const char * const px30_dts_timing[] = {
	"rockchip,ddr2_speed_bin",
	"rockchip,ddr3_speed_bin",
	"rockchip,ddr4_speed_bin",
	"rockchip,pd_idle",
	"rockchip,sr_idle",
	"rockchip,sr_mc_gate_idle",
	"rockchip,srpd_lite_idle",
	"rockchip,standby_idle",

	"rockchip,auto_pd_dis_freq",
	"rockchip,auto_sr_dis_freq",
	"rockchip,ddr2_dll_dis_freq",
	"rockchip,ddr3_dll_dis_freq",
	"rockchip,ddr4_dll_dis_freq",
	"rockchip,phy_dll_dis_freq",

	"rockchip,ddr2_odt_dis_freq",
	"rockchip,phy_ddr2_odt_dis_freq",
	"rockchip,ddr2_drv",
	"rockchip,ddr2_odt",
	"rockchip,phy_ddr2_ca_drv",
	"rockchip,phy_ddr2_ck_drv",
	"rockchip,phy_ddr2_dq_drv",
	"rockchip,phy_ddr2_odt",

	"rockchip,ddr3_odt_dis_freq",
	"rockchip,phy_ddr3_odt_dis_freq",
	"rockchip,ddr3_drv",
	"rockchip,ddr3_odt",
	"rockchip,phy_ddr3_ca_drv",
	"rockchip,phy_ddr3_ck_drv",
	"rockchip,phy_ddr3_dq_drv",
	"rockchip,phy_ddr3_odt",

	"rockchip,phy_lpddr2_odt_dis_freq",
	"rockchip,lpddr2_drv",
	"rockchip,phy_lpddr2_ca_drv",
	"rockchip,phy_lpddr2_ck_drv",
	"rockchip,phy_lpddr2_dq_drv",
	"rockchip,phy_lpddr2_odt",

	"rockchip,lpddr3_odt_dis_freq",
	"rockchip,phy_lpddr3_odt_dis_freq",
	"rockchip,lpddr3_drv",
	"rockchip,lpddr3_odt",
	"rockchip,phy_lpddr3_ca_drv",
	"rockchip,phy_lpddr3_ck_drv",
	"rockchip,phy_lpddr3_dq_drv",
	"rockchip,phy_lpddr3_odt",

	"rockchip,lpddr4_odt_dis_freq",
	"rockchip,phy_lpddr4_odt_dis_freq",
	"rockchip,lpddr4_drv",
	"rockchip,lpddr4_dq_odt",
	"rockchip,lpddr4_ca_odt",
	"rockchip,phy_lpddr4_ca_drv",
	"rockchip,phy_lpddr4_ck_cs_drv",
	"rockchip,phy_lpddr4_dq_drv",
	"rockchip,phy_lpddr4_odt",

	"rockchip,ddr4_odt_dis_freq",
	"rockchip,phy_ddr4_odt_dis_freq",
	"rockchip,ddr4_drv",
	"rockchip,ddr4_odt",
	"rockchip,phy_ddr4_ca_drv",
	"rockchip,phy_ddr4_ck_drv",
	"rockchip,phy_ddr4_dq_drv",
	"rockchip,phy_ddr4_odt",
};

static const char * const rk3328_dts_ca_timing[] = {
	"rockchip,ddr3a1_ddr4a9_de-skew",
	"rockchip,ddr3a0_ddr4a10_de-skew",
	"rockchip,ddr3a3_ddr4a6_de-skew",
	"rockchip,ddr3a2_ddr4a4_de-skew",
	"rockchip,ddr3a5_ddr4a8_de-skew",
	"rockchip,ddr3a4_ddr4a5_de-skew",
	"rockchip,ddr3a7_ddr4a11_de-skew",
	"rockchip,ddr3a6_ddr4a7_de-skew",
	"rockchip,ddr3a9_ddr4a0_de-skew",
	"rockchip,ddr3a8_ddr4a13_de-skew",
	"rockchip,ddr3a11_ddr4a3_de-skew",
	"rockchip,ddr3a10_ddr4cs0_de-skew",
	"rockchip,ddr3a13_ddr4a2_de-skew",
	"rockchip,ddr3a12_ddr4ba1_de-skew",
	"rockchip,ddr3a15_ddr4odt0_de-skew",
	"rockchip,ddr3a14_ddr4a1_de-skew",
	"rockchip,ddr3ba1_ddr4a15_de-skew",
	"rockchip,ddr3ba0_ddr4bg0_de-skew",
	"rockchip,ddr3ras_ddr4cke_de-skew",
	"rockchip,ddr3ba2_ddr4ba0_de-skew",
	"rockchip,ddr3we_ddr4bg1_de-skew",
	"rockchip,ddr3cas_ddr4a12_de-skew",
	"rockchip,ddr3ckn_ddr4ckn_de-skew",
	"rockchip,ddr3ckp_ddr4ckp_de-skew",
	"rockchip,ddr3cke_ddr4a16_de-skew",
	"rockchip,ddr3odt0_ddr4a14_de-skew",
	"rockchip,ddr3cs0_ddr4act_de-skew",
	"rockchip,ddr3reset_ddr4reset_de-skew",
	"rockchip,ddr3cs1_ddr4cs1_de-skew",
	"rockchip,ddr3odt1_ddr4odt1_de-skew",
};

static const char * const rk3328_dts_cs0_timing[] = {
	"rockchip,cs0_dm0_rx_de-skew",
	"rockchip,cs0_dm0_tx_de-skew",
	"rockchip,cs0_dq0_rx_de-skew",
	"rockchip,cs0_dq0_tx_de-skew",
	"rockchip,cs0_dq1_rx_de-skew",
	"rockchip,cs0_dq1_tx_de-skew",
	"rockchip,cs0_dq2_rx_de-skew",
	"rockchip,cs0_dq2_tx_de-skew",
	"rockchip,cs0_dq3_rx_de-skew",
	"rockchip,cs0_dq3_tx_de-skew",
	"rockchip,cs0_dq4_rx_de-skew",
	"rockchip,cs0_dq4_tx_de-skew",
	"rockchip,cs0_dq5_rx_de-skew",
	"rockchip,cs0_dq5_tx_de-skew",
	"rockchip,cs0_dq6_rx_de-skew",
	"rockchip,cs0_dq6_tx_de-skew",
	"rockchip,cs0_dq7_rx_de-skew",
	"rockchip,cs0_dq7_tx_de-skew",
	"rockchip,cs0_dqs0_rx_de-skew",
	"rockchip,cs0_dqs0p_tx_de-skew",
	"rockchip,cs0_dqs0n_tx_de-skew",

	"rockchip,cs0_dm1_rx_de-skew",
	"rockchip,cs0_dm1_tx_de-skew",
	"rockchip,cs0_dq8_rx_de-skew",
	"rockchip,cs0_dq8_tx_de-skew",
	"rockchip,cs0_dq9_rx_de-skew",
	"rockchip,cs0_dq9_tx_de-skew",
	"rockchip,cs0_dq10_rx_de-skew",
	"rockchip,cs0_dq10_tx_de-skew",
	"rockchip,cs0_dq11_rx_de-skew",
	"rockchip,cs0_dq11_tx_de-skew",
	"rockchip,cs0_dq12_rx_de-skew",
	"rockchip,cs0_dq12_tx_de-skew",
	"rockchip,cs0_dq13_rx_de-skew",
	"rockchip,cs0_dq13_tx_de-skew",
	"rockchip,cs0_dq14_rx_de-skew",
	"rockchip,cs0_dq14_tx_de-skew",
	"rockchip,cs0_dq15_rx_de-skew",
	"rockchip,cs0_dq15_tx_de-skew",
	"rockchip,cs0_dqs1_rx_de-skew",
	"rockchip,cs0_dqs1p_tx_de-skew",
	"rockchip,cs0_dqs1n_tx_de-skew",

	"rockchip,cs0_dm2_rx_de-skew",
	"rockchip,cs0_dm2_tx_de-skew",
	"rockchip,cs0_dq16_rx_de-skew",
	"rockchip,cs0_dq16_tx_de-skew",
	"rockchip,cs0_dq17_rx_de-skew",
	"rockchip,cs0_dq17_tx_de-skew",
	"rockchip,cs0_dq18_rx_de-skew",
	"rockchip,cs0_dq18_tx_de-skew",
	"rockchip,cs0_dq19_rx_de-skew",
	"rockchip,cs0_dq19_tx_de-skew",
	"rockchip,cs0_dq20_rx_de-skew",
	"rockchip,cs0_dq20_tx_de-skew",
	"rockchip,cs0_dq21_rx_de-skew",
	"rockchip,cs0_dq21_tx_de-skew",
	"rockchip,cs0_dq22_rx_de-skew",
	"rockchip,cs0_dq22_tx_de-skew",
	"rockchip,cs0_dq23_rx_de-skew",
	"rockchip,cs0_dq23_tx_de-skew",
	"rockchip,cs0_dqs2_rx_de-skew",
	"rockchip,cs0_dqs2p_tx_de-skew",
	"rockchip,cs0_dqs2n_tx_de-skew",

	"rockchip,cs0_dm3_rx_de-skew",
	"rockchip,cs0_dm3_tx_de-skew",
	"rockchip,cs0_dq24_rx_de-skew",
	"rockchip,cs0_dq24_tx_de-skew",
	"rockchip,cs0_dq25_rx_de-skew",
	"rockchip,cs0_dq25_tx_de-skew",
	"rockchip,cs0_dq26_rx_de-skew",
	"rockchip,cs0_dq26_tx_de-skew",
	"rockchip,cs0_dq27_rx_de-skew",
	"rockchip,cs0_dq27_tx_de-skew",
	"rockchip,cs0_dq28_rx_de-skew",
	"rockchip,cs0_dq28_tx_de-skew",
	"rockchip,cs0_dq29_rx_de-skew",
	"rockchip,cs0_dq29_tx_de-skew",
	"rockchip,cs0_dq30_rx_de-skew",
	"rockchip,cs0_dq30_tx_de-skew",
	"rockchip,cs0_dq31_rx_de-skew",
	"rockchip,cs0_dq31_tx_de-skew",
	"rockchip,cs0_dqs3_rx_de-skew",
	"rockchip,cs0_dqs3p_tx_de-skew",
	"rockchip,cs0_dqs3n_tx_de-skew",
};

static const char * const rk3328_dts_cs1_timing[] = {
	"rockchip,cs1_dm0_rx_de-skew",
	"rockchip,cs1_dm0_tx_de-skew",
	"rockchip,cs1_dq0_rx_de-skew",
	"rockchip,cs1_dq0_tx_de-skew",
	"rockchip,cs1_dq1_rx_de-skew",
	"rockchip,cs1_dq1_tx_de-skew",
	"rockchip,cs1_dq2_rx_de-skew",
	"rockchip,cs1_dq2_tx_de-skew",
	"rockchip,cs1_dq3_rx_de-skew",
	"rockchip,cs1_dq3_tx_de-skew",
	"rockchip,cs1_dq4_rx_de-skew",
	"rockchip,cs1_dq4_tx_de-skew",
	"rockchip,cs1_dq5_rx_de-skew",
	"rockchip,cs1_dq5_tx_de-skew",
	"rockchip,cs1_dq6_rx_de-skew",
	"rockchip,cs1_dq6_tx_de-skew",
	"rockchip,cs1_dq7_rx_de-skew",
	"rockchip,cs1_dq7_tx_de-skew",
	"rockchip,cs1_dqs0_rx_de-skew",
	"rockchip,cs1_dqs0p_tx_de-skew",
	"rockchip,cs1_dqs0n_tx_de-skew",

	"rockchip,cs1_dm1_rx_de-skew",
	"rockchip,cs1_dm1_tx_de-skew",
	"rockchip,cs1_dq8_rx_de-skew",
	"rockchip,cs1_dq8_tx_de-skew",
	"rockchip,cs1_dq9_rx_de-skew",
	"rockchip,cs1_dq9_tx_de-skew",
	"rockchip,cs1_dq10_rx_de-skew",
	"rockchip,cs1_dq10_tx_de-skew",
	"rockchip,cs1_dq11_rx_de-skew",
	"rockchip,cs1_dq11_tx_de-skew",
	"rockchip,cs1_dq12_rx_de-skew",
	"rockchip,cs1_dq12_tx_de-skew",
	"rockchip,cs1_dq13_rx_de-skew",
	"rockchip,cs1_dq13_tx_de-skew",
	"rockchip,cs1_dq14_rx_de-skew",
	"rockchip,cs1_dq14_tx_de-skew",
	"rockchip,cs1_dq15_rx_de-skew",
	"rockchip,cs1_dq15_tx_de-skew",
	"rockchip,cs1_dqs1_rx_de-skew",
	"rockchip,cs1_dqs1p_tx_de-skew",
	"rockchip,cs1_dqs1n_tx_de-skew",

	"rockchip,cs1_dm2_rx_de-skew",
	"rockchip,cs1_dm2_tx_de-skew",
	"rockchip,cs1_dq16_rx_de-skew",
	"rockchip,cs1_dq16_tx_de-skew",
	"rockchip,cs1_dq17_rx_de-skew",
	"rockchip,cs1_dq17_tx_de-skew",
	"rockchip,cs1_dq18_rx_de-skew",
	"rockchip,cs1_dq18_tx_de-skew",
	"rockchip,cs1_dq19_rx_de-skew",
	"rockchip,cs1_dq19_tx_de-skew",
	"rockchip,cs1_dq20_rx_de-skew",
	"rockchip,cs1_dq20_tx_de-skew",
	"rockchip,cs1_dq21_rx_de-skew",
	"rockchip,cs1_dq21_tx_de-skew",
	"rockchip,cs1_dq22_rx_de-skew",
	"rockchip,cs1_dq22_tx_de-skew",
	"rockchip,cs1_dq23_rx_de-skew",
	"rockchip,cs1_dq23_tx_de-skew",
	"rockchip,cs1_dqs2_rx_de-skew",
	"rockchip,cs1_dqs2p_tx_de-skew",
	"rockchip,cs1_dqs2n_tx_de-skew",

	"rockchip,cs1_dm3_rx_de-skew",
	"rockchip,cs1_dm3_tx_de-skew",
	"rockchip,cs1_dq24_rx_de-skew",
	"rockchip,cs1_dq24_tx_de-skew",
	"rockchip,cs1_dq25_rx_de-skew",
	"rockchip,cs1_dq25_tx_de-skew",
	"rockchip,cs1_dq26_rx_de-skew",
	"rockchip,cs1_dq26_tx_de-skew",
	"rockchip,cs1_dq27_rx_de-skew",
	"rockchip,cs1_dq27_tx_de-skew",
	"rockchip,cs1_dq28_rx_de-skew",
	"rockchip,cs1_dq28_tx_de-skew",
	"rockchip,cs1_dq29_rx_de-skew",
	"rockchip,cs1_dq29_tx_de-skew",
	"rockchip,cs1_dq30_rx_de-skew",
	"rockchip,cs1_dq30_tx_de-skew",
	"rockchip,cs1_dq31_rx_de-skew",
	"rockchip,cs1_dq31_tx_de-skew",
	"rockchip,cs1_dqs3_rx_de-skew",
	"rockchip,cs1_dqs3p_tx_de-skew",
	"rockchip,cs1_dqs3n_tx_de-skew",
};

struct px30_dram_timing {
	unsigned int ddr2_speed_bin;
	unsigned int ddr3_speed_bin;
	unsigned int ddr4_speed_bin;
	unsigned int pd_idle;
	unsigned int sr_idle;
	unsigned int sr_mc_gate_idle;
	unsigned int srpd_lite_idle;
	unsigned int standby_idle;

	unsigned int auto_pd_dis_freq;
	unsigned int auto_sr_dis_freq;
	unsigned int ddr2_dll_dis_freq;
	unsigned int ddr3_dll_dis_freq;
	unsigned int ddr4_dll_dis_freq;
	unsigned int phy_dll_dis_freq;

	unsigned int ddr2_odt_dis_freq;
	unsigned int phy_ddr2_odt_dis_freq;
	unsigned int ddr2_drv;
	unsigned int ddr2_odt;
	unsigned int phy_ddr2_ca_drv;
	unsigned int phy_ddr2_ck_drv;
	unsigned int phy_ddr2_dq_drv;
	unsigned int phy_ddr2_odt;

	unsigned int ddr3_odt_dis_freq;
	unsigned int phy_ddr3_odt_dis_freq;
	unsigned int ddr3_drv;
	unsigned int ddr3_odt;
	unsigned int phy_ddr3_ca_drv;
	unsigned int phy_ddr3_ck_drv;
	unsigned int phy_ddr3_dq_drv;
	unsigned int phy_ddr3_odt;

	unsigned int phy_lpddr2_odt_dis_freq;
	unsigned int lpddr2_drv;
	unsigned int phy_lpddr2_ca_drv;
	unsigned int phy_lpddr2_ck_drv;
	unsigned int phy_lpddr2_dq_drv;
	unsigned int phy_lpddr2_odt;

	unsigned int lpddr3_odt_dis_freq;
	unsigned int phy_lpddr3_odt_dis_freq;
	unsigned int lpddr3_drv;
	unsigned int lpddr3_odt;
	unsigned int phy_lpddr3_ca_drv;
	unsigned int phy_lpddr3_ck_drv;
	unsigned int phy_lpddr3_dq_drv;
	unsigned int phy_lpddr3_odt;

	unsigned int lpddr4_odt_dis_freq;
	unsigned int phy_lpddr4_odt_dis_freq;
	unsigned int lpddr4_drv;
	unsigned int lpddr4_dq_odt;
	unsigned int lpddr4_ca_odt;
	unsigned int phy_lpddr4_ca_drv;
	unsigned int phy_lpddr4_ck_cs_drv;
	unsigned int phy_lpddr4_dq_drv;
	unsigned int phy_lpddr4_odt;

	unsigned int ddr4_odt_dis_freq;
	unsigned int phy_ddr4_odt_dis_freq;
	unsigned int ddr4_drv;
	unsigned int ddr4_odt;
	unsigned int phy_ddr4_ca_drv;
	unsigned int phy_ddr4_ck_drv;
	unsigned int phy_ddr4_dq_drv;
	unsigned int phy_ddr4_odt;

	unsigned int ca_skew[15];
	unsigned int cs0_skew[44];
	unsigned int cs1_skew[44];

	unsigned int available;
};

struct de_skew {
	unsigned int ca_de_skew[30];
	unsigned int cs0_de_skew[84];
	unsigned int cs1_de_skew[84];
};

struct rk3399_dram_timing {
	unsigned int ddr3_speed_bin;
	unsigned int pd_idle;
	unsigned int sr_idle;
	unsigned int sr_mc_gate_idle;
	unsigned int srpd_lite_idle;
	unsigned int standby_idle;
	unsigned int auto_pd_dis_freq;
	unsigned int dram_dll_dis_freq;
	unsigned int phy_dll_dis_freq;
	unsigned int ddr3_odt_dis_freq;
	unsigned int ddr3_drv;
	unsigned int ddr3_odt;
	unsigned int phy_ddr3_ca_drv;
	unsigned int phy_ddr3_dq_drv;
	unsigned int phy_ddr3_odt;
	unsigned int lpddr3_odt_dis_freq;
	unsigned int lpddr3_drv;
	unsigned int lpddr3_odt;
	unsigned int phy_lpddr3_ca_drv;
	unsigned int phy_lpddr3_dq_drv;
	unsigned int phy_lpddr3_odt;
	unsigned int lpddr4_odt_dis_freq;
	unsigned int lpddr4_drv;
	unsigned int lpddr4_dq_odt;
	unsigned int lpddr4_ca_odt;
	unsigned int phy_lpddr4_ca_drv;
	unsigned int phy_lpddr4_ck_cs_drv;
	unsigned int phy_lpddr4_dq_drv;
	unsigned int phy_lpddr4_odt;
};

struct rockchip_dmcfreq {
	struct devfreq *devfreq;
	struct devfreq_simple_ondemand_data ondemand_data;
	struct clk *dmc_clk;
	struct devfreq_event_dev *edev;
	struct regulator *vdd_center;
	struct regmap *regmap_pmu;
	unsigned long rate, target_rate;
	unsigned long volt, target_volt;
	unsigned int odt_dis_freq;
	int odt_pd_arg0, odt_pd_arg1;
};

static struct pm_qos_request pm_qos;

static DECLARE_RWSEM(rockchip_dmcfreq_sem);

void rockchip_dmcfreq_lock(void)
{
	down_read(&rockchip_dmcfreq_sem);
}
EXPORT_SYMBOL(rockchip_dmcfreq_lock);

void rockchip_dmcfreq_unlock(void)
{
	up_read(&rockchip_dmcfreq_sem);
}
EXPORT_SYMBOL(rockchip_dmcfreq_unlock);

static int rockchip_dmcfreq_target(struct device *dev, unsigned long *freq,
				   u32 flags)
{
	struct rockchip_dmcfreq *dmcfreq = dev_get_drvdata(dev);
	struct dev_pm_opp *opp;
	struct cpufreq_policy *policy;
	unsigned long old_clk_rate = dmcfreq->rate;
	unsigned long opp_rate, target_volt, target_rate;
	struct arm_smccc_res res;
	bool odt_enable = false;
	unsigned int cpu_cur;
	int err;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (IS_ERR(opp))
		return PTR_ERR(opp);

	opp_rate = dev_pm_opp_get_freq(opp);
	target_volt = dev_pm_opp_get_voltage(opp);
	dev_pm_opp_put(opp);

	target_rate = clk_round_rate(dmcfreq->dmc_clk, opp_rate);
	if ((long)target_rate <= 0)
		target_rate = opp_rate;

	if (dmcfreq->rate == target_rate && dmcfreq->volt == target_volt)
		return 0;

	if (dmcfreq->volt != target_volt) {
		err = regulator_set_voltage(dmcfreq->vdd_center, target_volt,
					    INT_MAX);
		if (err) {
			dev_err(dev, "Cannot set voltage %lu uV\n",
				target_volt);
			return err;
		}

		dmcfreq->volt = target_volt;
		return 0;
	}

	if (dmcfreq->regmap_pmu) {
		if (target_rate >= dmcfreq->odt_dis_freq)
			odt_enable = true;

		/*
		 * This makes a SMC call to the TF-A to set the DDR PD
		 * (power-down) timings and to enable or disable the
		 * ODT (on-die termination) resistors.
		 */
		arm_smccc_smc(ROCKCHIP_SIP_DRAM_FREQ, dmcfreq->odt_pd_arg0,
			      dmcfreq->odt_pd_arg1,
			      ROCKCHIP_SIP_CONFIG_DRAM_SET_ODT_PD,
			      odt_enable, 0, 0, 0, &res);
	}

	/*
	 * We need to prevent cpu hotplug from happening while a dmc freq rate
	 * change is happening.
	 *
	 * Do this before taking the policy rwsem to avoid deadlocks between the
	 * mutex that is locked/unlocked in cpu_hotplug_disable/enable. And it
	 * can also avoid deadlocks between the mutex that is locked/unlocked
	 * in get/put_online_cpus (such as store_scaling_max_freq()).
	 */
	cpus_read_lock();

	/*
	 * Go to specified cpufreq and block other cpufreq changes since
	 * set_rate needs to complete during vblank.
	 */
	cpu_cur = raw_smp_processor_id();
	policy = cpufreq_cpu_get(cpu_cur);
	if (!policy) {
		dev_err(dev, "cpu%d policy NULL\n", cpu_cur);
		goto cpufreq;
	}
	down_write(&policy->rwsem);

	/*
	 * If frequency scaling from low to high, adjust voltage first.
	 * If frequency scaling from high to low, adjust frequency first.
	 */
	if (old_clk_rate < target_rate) {
		err = regulator_set_voltage(dmcfreq->vdd_center, target_volt,
					    INT_MAX);
		if (err) {
			dev_err(dev, "Cannot set voltage %lu uV\n",
				target_volt);
			goto out;
		}
	}

	/*
	 * Writer in rwsem may block readers even during its waiting in queue,
	 * and this may lead to a deadlock when the code path takes read sem
	 * twice (e.g. one in vop_lock() and another in rockchip_pmu_lock()).
	 * As a (suboptimal) workaround, let writer to spin until it gets the
	 * lock.
	 */
	while (!down_write_trylock(&rockchip_dmcfreq_sem))
		cond_resched();
	err = clk_set_rate(dmcfreq->dmc_clk, target_rate);
	up_write(&rockchip_dmcfreq_sem);
	if (err) {
		dev_err(dev, "Cannot set frequency %lu (%d)\n", target_rate,
			err);
		regulator_set_voltage(dmcfreq->vdd_center, dmcfreq->volt,
				      INT_MAX);
		goto out;
	}

	/*
	 * Check the dpll rate,
	 * There only two result we will get,
	 * 1. Ddr frequency scaling fail, we still get the old rate.
	 * 2. Ddr frequency scaling sucessful, we get the rate we set.
	 */
	dmcfreq->rate = clk_get_rate(dmcfreq->dmc_clk);

	/* If get the incorrect rate, set voltage to old value. */
	if (dmcfreq->rate != target_rate) {
		dev_err(dev, "Got wrong frequency, Request %lu, Current %lu\n",
			target_rate, dmcfreq->rate);
		regulator_set_voltage(dmcfreq->vdd_center, dmcfreq->volt,
				      INT_MAX);
		goto out;
	} else if (old_clk_rate > target_rate) {
		err = regulator_set_voltage(dmcfreq->vdd_center, target_volt,
					    INT_MAX);
		if (err) {
			dev_err(dev, "Cannot set vol %lu uV\n", target_volt);
			goto out;
		}
	}

	dmcfreq->rate = opp_rate;
	dmcfreq->volt = target_volt;

out:
	up_write(&policy->rwsem);
	cpufreq_cpu_put(policy);
cpufreq:
	cpus_read_unlock();

	return err;
}

static int rockchip_dmcfreq_get_dev_status(struct device *dev,
					   struct devfreq_dev_status *stat)
{
	struct rockchip_dmcfreq *dmcfreq = dev_get_drvdata(dev);
	struct devfreq_event_data edata;
	int ret = 0;

	ret = devfreq_event_get_event(dmcfreq->edev, &edata);
	if (ret < 0)
		return ret;

	stat->current_frequency = dmcfreq->rate;
	stat->busy_time = edata.load_count;
	stat->total_time = edata.total_count;

	return ret;
}

static int rockchip_dmcfreq_get_cur_freq(struct device *dev,
					 unsigned long *freq)
{
	struct rockchip_dmcfreq *dmcfreq = dev_get_drvdata(dev);

	*freq = dmcfreq->rate;

	return 0;
}

static struct devfreq_dev_profile rockchip_devfreq_dmc_profile = {
	.polling_ms	= 50,
	.target		= rockchip_dmcfreq_target,
	.get_dev_status	= rockchip_dmcfreq_get_dev_status,
	.get_cur_freq	= rockchip_dmcfreq_get_cur_freq,
};

static __maybe_unused int rockchip_dmcfreq_suspend(struct device *dev)
{
	struct rockchip_dmcfreq *dmcfreq = dev_get_drvdata(dev);
	int ret = 0;

	ret = devfreq_event_disable_edev(dmcfreq->edev);
	if (ret < 0) {
		dev_err(dev, "failed to disable the devfreq-event devices\n");
		return ret;
	}

	ret = devfreq_suspend_device(dmcfreq->devfreq);
	if (ret < 0) {
		dev_err(dev, "failed to suspend the devfreq devices\n");
		return ret;
	}

	return 0;
}

static __maybe_unused int rockchip_dmcfreq_resume(struct device *dev)
{
	struct rockchip_dmcfreq *dmcfreq = dev_get_drvdata(dev);
	int ret = 0;

	ret = devfreq_event_enable_edev(dmcfreq->edev);
	if (ret < 0) {
		dev_err(dev, "failed to enable the devfreq-event devices\n");
		return ret;
	}

	ret = devfreq_resume_device(dmcfreq->devfreq);
	if (ret < 0) {
		dev_err(dev, "failed to resume the devfreq devices\n");
		return ret;
	}
	return ret;
}

static SIMPLE_DEV_PM_OPS(rockchip_dmcfreq_pm, rockchip_dmcfreq_suspend,
			 rockchip_dmcfreq_resume);

struct dmcfreq_wait_ctrl_t {
	wait_queue_head_t wait_wq;
	int irq;
	int wait_flag;
};

static struct dmcfreq_wait_ctrl_t wait_ctrl;

static irqreturn_t wait_complete_irq(int irqno, void *dev_id)
{
	struct dmcfreq_wait_ctrl_t *ctrl = dev_id;

	ctrl->wait_flag = 1;
	wake_up(&ctrl->wait_wq);

	return IRQ_HANDLED;
}

int rockchip_dmcfreq_wait_complete(void)
{
	wait_ctrl.wait_flag = 0;

	enable_irq(wait_ctrl.irq);

	/*
	 * CPUs only enter WFI when idle to make sure that
	 * FIQn can quick response.
	 */
	cpu_latency_qos_update_request(&pm_qos, 0);

	wait_event_timeout(wait_ctrl.wait_wq, wait_ctrl.wait_flag,
			   msecs_to_jiffies(DDR_CLK_FREQ_CHANGE_TIMEOUT));

	cpu_latency_qos_update_request(&pm_qos, PM_QOS_DEFAULT_VALUE);
	disable_irq(wait_ctrl.irq);

	return 0;
}

static int of_get_rk3399_timings(struct rk3399_dram_timing *timing,
				 struct device_node *np)
{
	int ret = 0;

	ret = of_property_read_u32(np, "rockchip,ddr3_speed_bin",
				   &timing->ddr3_speed_bin);
	ret |= of_property_read_u32(np, "rockchip,pd_idle",
				    &timing->pd_idle);
	ret |= of_property_read_u32(np, "rockchip,sr_idle",
				    &timing->sr_idle);
	ret |= of_property_read_u32(np, "rockchip,sr_mc_gate_idle",
				    &timing->sr_mc_gate_idle);
	ret |= of_property_read_u32(np, "rockchip,srpd_lite_idle",
				    &timing->srpd_lite_idle);
	ret |= of_property_read_u32(np, "rockchip,standby_idle",
				    &timing->standby_idle);
	ret |= of_property_read_u32(np, "rockchip,auto_pd_dis_freq",
				    &timing->auto_pd_dis_freq);
	ret |= of_property_read_u32(np, "rockchip,dram_dll_dis_freq",
				    &timing->dram_dll_dis_freq);
	ret |= of_property_read_u32(np, "rockchip,phy_dll_dis_freq",
				    &timing->phy_dll_dis_freq);
	ret |= of_property_read_u32(np, "rockchip,ddr3_odt_dis_freq",
				    &timing->ddr3_odt_dis_freq);
	ret |= of_property_read_u32(np, "rockchip,ddr3_drv",
				    &timing->ddr3_drv);
	ret |= of_property_read_u32(np, "rockchip,ddr3_odt",
				    &timing->ddr3_odt);
	ret |= of_property_read_u32(np, "rockchip,phy_ddr3_ca_drv",
				    &timing->phy_ddr3_ca_drv);
	ret |= of_property_read_u32(np, "rockchip,phy_ddr3_dq_drv",
				    &timing->phy_ddr3_dq_drv);
	ret |= of_property_read_u32(np, "rockchip,phy_ddr3_odt",
				    &timing->phy_ddr3_odt);
	ret |= of_property_read_u32(np, "rockchip,lpddr3_odt_dis_freq",
				    &timing->lpddr3_odt_dis_freq);
	ret |= of_property_read_u32(np, "rockchip,lpddr3_drv",
				    &timing->lpddr3_drv);
	ret |= of_property_read_u32(np, "rockchip,lpddr3_odt",
				    &timing->lpddr3_odt);
	ret |= of_property_read_u32(np, "rockchip,phy_lpddr3_ca_drv",
				    &timing->phy_lpddr3_ca_drv);
	ret |= of_property_read_u32(np, "rockchip,phy_lpddr3_dq_drv",
				    &timing->phy_lpddr3_dq_drv);
	ret |= of_property_read_u32(np, "rockchip,phy_lpddr3_odt",
				    &timing->phy_lpddr3_odt);
	ret |= of_property_read_u32(np, "rockchip,lpddr4_odt_dis_freq",
				    &timing->lpddr4_odt_dis_freq);
	ret |= of_property_read_u32(np, "rockchip,lpddr4_drv",
				    &timing->lpddr4_drv);
	ret |= of_property_read_u32(np, "rockchip,lpddr4_dq_odt",
				    &timing->lpddr4_dq_odt);
	ret |= of_property_read_u32(np, "rockchip,lpddr4_ca_odt",
				    &timing->lpddr4_ca_odt);
	ret |= of_property_read_u32(np, "rockchip,phy_lpddr4_ca_drv",
				    &timing->phy_lpddr4_ca_drv);
	ret |= of_property_read_u32(np, "rockchip,phy_lpddr4_ck_cs_drv",
				    &timing->phy_lpddr4_ck_cs_drv);
	ret |= of_property_read_u32(np, "rockchip,phy_lpddr4_dq_drv",
				    &timing->phy_lpddr4_dq_drv);
	ret |= of_property_read_u32(np, "rockchip,phy_lpddr4_odt",
				    &timing->phy_lpddr4_odt);

	return ret;
}

static int rk3399_dmc_init(struct platform_device *pdev,
			   struct rockchip_dmcfreq *data)
{
	struct device_node *np = pdev->dev.of_node, *node;
	struct device *dev = &pdev->dev;
	struct rk3399_dram_timing *timings;
	struct arm_smccc_res res;
	int index, size;
	u32 ddr_type;
	u32 *timing;
	u32 val;

	timings = devm_kzalloc(dev, sizeof(*timings), GFP_KERNEL);
	if (!timings)
		return -ENOMEM;

	/*
	 * Get dram timing and pass it to arm trust firmware,
	 * the dram drvier in arm trust firmware will get these
	 * timing and to do dram initial.
	 */
	if (!of_get_rk3399_timings(timings, np)) {
		timing = &timings->ddr3_speed_bin;
		size = sizeof(struct rk3399_dram_timing) / 4;
		for (index = 0; index < size; index++) {
			arm_smccc_smc(ROCKCHIP_SIP_DRAM_FREQ, *timing++, index,
				      ROCKCHIP_SIP_CONFIG_DRAM_SET_PARAM,
				      0, 0, 0, 0, &res);
			if (res.a0) {
				dev_err(dev, "Failed to set dram param: %ld\n",
					res.a0);
				return -EINVAL;
			}
		}
	}

	arm_smccc_smc(ROCKCHIP_SIP_DRAM_FREQ, 0, 0,
		      ROCKCHIP_SIP_CONFIG_DRAM_INIT,
		      0, 0, 0, 0, &res);

	node = of_parse_phandle(np, "rockchip,pmu", 0);
	if (!node)
		return 0;

	data->regmap_pmu = syscon_node_to_regmap(node);
	of_node_put(node);
	if (IS_ERR(data->regmap_pmu)) {
		return PTR_ERR(data->regmap_pmu);
	}

	regmap_read(data->regmap_pmu, PMUGRF_OS_REG2, &val);
	ddr_type = READ_DRAMTYPE_INFO(val);

	switch (ddr_type) {
	case DDR3:
		data->odt_dis_freq = timings->ddr3_odt_dis_freq;
		break;
	case LPDDR3:
		data->odt_dis_freq = timings->lpddr3_odt_dis_freq;
		break;
	case LPDDR4:
		data->odt_dis_freq = timings->lpddr4_odt_dis_freq;
		break;
	default:
		return -EINVAL;
	}

	/*
	 * In TF-A there is a platform SIP call to set the PD (power-down)
	 * timings and to enable or disable the ODT (on-die termination).
	 * This call needs three arguments as follows:
	 *
	 * arg0:
	 *     bit[0-7]   : sr_idle
	 *     bit[8-15]  : sr_mc_gate_idle
	 *     bit[16-31] : standby idle
	 * arg1:
	 *     bit[0-11]  : pd_idle
	 *     bit[16-27] : srpd_lite_idle
	 * arg2:
	 *     bit[0]     : odt enable
	 */
	data->odt_pd_arg0 = (timings->sr_idle & 0xff) |
			    ((timings->sr_mc_gate_idle & 0xff) << 8) |
			    ((timings->standby_idle & 0xffff) << 16);
	data->odt_pd_arg1 = (timings->pd_idle & 0xfff) |
			    ((timings->srpd_lite_idle & 0xfff) << 16);

	return 0;
}

static void de_skew_set_to_reg(struct px30_dram_timing *timing,
			       struct de_skew *de_skew)
{
	u32 n;
	u32 offset;
	u32 shift;

	memset_io(timing->ca_skew, 0, sizeof(timing->ca_skew));
	memset_io(timing->cs0_skew, 0, sizeof(timing->cs0_skew));
	memset_io(timing->cs1_skew, 0, sizeof(timing->cs1_skew));

	/* CA de-skew */
	for (n = 0; n < ARRAY_SIZE(de_skew->ca_de_skew); n++) {
		offset = n / 2;
		shift = n % 2;
		/* 0 => 4; 1 => 0 */
		shift = (shift == 0) ? 4 : 0;
		timing->ca_skew[offset] &= ~(0xf << shift);
		timing->ca_skew[offset] |= (de_skew->ca_de_skew[n] << shift);
	}

	/* CS0 data de-skew */
	for (n = 0; n < ARRAY_SIZE(de_skew->cs0_de_skew); n++) {
		offset = ((n / 21) * 11) + ((n % 21) / 2);
		shift = ((n % 21) % 2);
		if ((n % 21) == 20)
			shift = 0;
		else
			/* 0 => 4; 1 => 0 */
			shift = (shift == 0) ? 4 : 0;
		timing->cs0_skew[offset] &= ~(0xf << shift);
		timing->cs0_skew[offset] |= (de_skew->cs0_de_skew[n] << shift);
	}

	/* CS1 data de-skew */
	for (n = 0; n < ARRAY_SIZE(de_skew->cs1_de_skew); n++) {
		offset = ((n / 21) * 11) + ((n % 21) / 2);
		shift = ((n % 21) % 2);
		if ((n % 21) == 20)
			shift = 0;
		else
			/* 0 => 4; 1 => 0 */
			shift = (shift == 0) ? 4 : 0;
		timing->cs1_skew[offset] &= ~(0xf << shift);
		timing->cs1_skew[offset] |= (de_skew->cs1_de_skew[n] << shift);
	}
}

static int of_get_px30_timings(struct px30_dram_timing *timing,
			       struct device_node *np)
{
	struct de_skew *de_skew;
	int ret = 0;
	u32 i;

	de_skew = kmalloc(sizeof(*de_skew), GFP_KERNEL);
	if (!de_skew)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(px30_dts_timing); i++)
		ret |= of_property_read_u32(np, px30_dts_timing[i],
					((u32 *)timing) + i);

	for (i = 0; i < ARRAY_SIZE(rk3328_dts_ca_timing); i++)
		ret |= of_property_read_u32(np, rk3328_dts_ca_timing[i],
					de_skew->ca_de_skew + i);

	for (i = 0; i < ARRAY_SIZE(rk3328_dts_cs0_timing); i++)
		ret |= of_property_read_u32(np, rk3328_dts_cs0_timing[i],
					de_skew->cs0_de_skew + i);

	for (i = 0; i < ARRAY_SIZE(rk3328_dts_cs1_timing); i++)
		ret |= of_property_read_u32(np, rk3328_dts_cs1_timing[i],
					de_skew->cs1_de_skew + i);

	if (!ret)
		de_skew_set_to_reg(timing, de_skew);

	timing->available = 1;

	kfree(de_skew);

	return ret;
}

static int px30_dmc_init(struct platform_device *pdev,
			 struct rockchip_dmcfreq *data)
{
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct px30_dram_timing *timings;
	struct share_params *params;
	struct arm_smccc_res res;
	void __iomem *mem;
	int irq;
	int ret;

	mem = rockchip_ddr_clk_get_atf_mem(data->dmc_clk);
	if (!mem) {
		dev_err(dev, "Failed to get ATF memory\n");
		return -EINVAL;
	}

	params = mem;
	timings = mem + PAGE_SIZE;
	ret = of_get_px30_timings(timings, np);
	if (ret) {
		dev_err(dev, "Failed to get timings\n");
		return ret;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "Failed to get irq: %d\n", irq);
		return irq;
	}

	ret = devm_request_irq(&pdev->dev, irq, wait_complete_irq,
			       0, dev_name(&pdev->dev), &wait_ctrl);
	if (ret) {
		dev_err(&pdev->dev, "Cannot request irq: %d\n", ret);
		return ret;
	}
	disable_irq(irq);

	init_waitqueue_head(&wait_ctrl.wait_wq);
	wait_ctrl.irq = irq;

	params->complete_hwirq = irqd_to_hwirq(irq_get_irq_data(irq));

	arm_smccc_smc(ROCKCHIP_SIP_DRAM_FREQ, SHARE_PAGE_TYPE_DDR, 0,
		      ROCKCHIP_SIP_CONFIG_DRAM_INIT,
		      0, 0, 0, 0, &res);

	return 0;
}

static void rockchip_dmcfreq_disable_edev(void *data)
{
	struct devfreq_event_dev *edev = data;

	devfreq_event_disable_edev(edev);
}

static int rockchip_dmcfreq_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct rockchip_dmcfreq *data;
	unsigned long opp_rate;
	struct dev_pm_opp *opp;
	int (*init)(struct platform_device *pdev,
		    struct rockchip_dmcfreq *data);
	int ret;

	data = devm_kzalloc(dev, sizeof(struct rockchip_dmcfreq), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->vdd_center = devm_regulator_get_optional(dev, "center");
	if (IS_ERR(data->vdd_center))
		return dev_err_probe(dev, PTR_ERR(data->vdd_center),
				     "Cannot get the regulator \"center\"\n");

	data->dmc_clk = devm_clk_get(dev, "dmc_clk");
	if (IS_ERR(data->dmc_clk))
		return dev_err_probe(dev, PTR_ERR(data->dmc_clk),
				     "Cannot get the clk dmc_clk\n");

	data->edev = devfreq_event_get_edev_by_phandle(dev, "devfreq-events", 0);
	if (IS_ERR(data->edev))
		return -EPROBE_DEFER;

	ret = devfreq_event_enable_edev(data->edev);
	if (ret < 0) {
		dev_err(dev, "failed to enable devfreq-event devices\n");
		return ret;
	}

	ret = devm_add_action_or_reset(dev, rockchip_dmcfreq_disable_edev,
				       data->edev);
	if (ret)
		return ret;

	init = device_get_match_data(dev);
	if (!init)
		return -EINVAL;

	init(pdev, data);

	/*
	 * We add a devfreq driver to our parent since it has a device tree node
	 * with operating points.
	 */
	ret = devm_pm_opp_of_add_table(dev);
	if (ret) {
		dev_err(dev, "Invalid operating-points in device tree.\n");
		return -EINVAL;
	}

	of_property_read_u32(np, "upthreshold",
			     &data->ondemand_data.upthreshold);
	of_property_read_u32(np, "downdifferential",
			     &data->ondemand_data.downdifferential);

	data->rate = clk_get_rate(data->dmc_clk);
	data->volt = regulator_get_voltage(data->vdd_center);

	opp_rate = data->rate;
	opp = devfreq_recommended_opp(dev, &opp_rate, 0);
	if (IS_ERR(opp)) {
		dev_err(dev, "Failed to find opp for %lu Hz\n", opp_rate);
		return PTR_ERR(opp);
	}
	dev_pm_opp_put(opp);

	rockchip_devfreq_dmc_profile.initial_freq = opp_rate;

	cpu_latency_qos_add_request(&pm_qos, PM_QOS_DEFAULT_VALUE);

	data->devfreq = devm_devfreq_add_device(dev,
					   &rockchip_devfreq_dmc_profile,
					   DEVFREQ_GOV_SIMPLE_ONDEMAND,
					   &data->ondemand_data);
	if (IS_ERR(data->devfreq))
		return PTR_ERR(data->devfreq);

	ret = devm_devfreq_register_opp_notifier(dev, data->devfreq);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, data);

	return 0;
}

static const struct of_device_id rockchip_dmcfreq_of_match[] = {
	{ .compatible = "rockchip,rk3399-dmc", .data = rk3399_dmc_init },
	{ .compatible = "rockchip,px30-dmc", .data = px30_dmc_init },
	{ },
};
MODULE_DEVICE_TABLE(of, rockchip_dmcfreq_of_match);

static struct platform_driver rockchip_dmcfreq_driver = {
	.probe	= rockchip_dmcfreq_probe,
	.driver = {
		.name	= "rockchip-dmc",
		.pm	= &rockchip_dmcfreq_pm,
		.of_match_table = rockchip_dmcfreq_of_match,
	},
};
module_platform_driver(rockchip_dmcfreq_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Lin Huang <hl@rock-chips.com>");
MODULE_DESCRIPTION("rockchip dmcfreq driver with devfreq framework");
