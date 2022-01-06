// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016, Fuzhou Rockchip Electronics Co., Ltd.
 * Author: Lin Huang <hl@rock-chips.com>
 */

#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/devfreq.h>
#include <linux/devfreq-event.h>
#include <linux/interrupt.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/rwsem.h>
#include <linux/suspend.h>

#include <soc/rockchip/px30_grf.h>
#include <soc/rockchip/rockchip_ddr.h>
#include <soc/rockchip/rockchip_sip.h>

#define ATF_MIN_VERSION		0x103

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

struct dram_timing {
	u32 ddr2_speed_bin;
	u32 ddr3_speed_bin;
	u32 ddr4_speed_bin;
	u32 pd_idle;
	u32 sr_idle;
	u32 sr_mc_gate_idle;
	u32 srpd_lite_idle;
	u32 standby_idle;

	u32 auto_pd_dis_freq;
	u32 auto_sr_dis_freq;
	u32 ddr2_dll_dis_freq;
	u32 ddr3_dll_dis_freq;
	u32 ddr4_dll_dis_freq;
	u32 phy_dll_dis_freq;

	u32 ddr2_odt_dis_freq;
	u32 phy_ddr2_odt_dis_freq;
	u32 ddr2_drv;
	u32 ddr2_odt;
	u32 phy_ddr2_ca_drv;
	u32 phy_ddr2_ck_drv;
	u32 phy_ddr2_dq_drv;
	u32 phy_ddr2_odt;

	u32 ddr3_odt_dis_freq;
	u32 phy_ddr3_odt_dis_freq;
	u32 ddr3_drv;
	u32 ddr3_odt;
	u32 phy_ddr3_ca_drv;
	u32 phy_ddr3_ck_drv;
	u32 phy_ddr3_dq_drv;
	u32 phy_ddr3_odt;

	u32 phy_lpddr2_odt_dis_freq;
	u32 lpddr2_drv;
	u32 phy_lpddr2_ca_drv;
	u32 phy_lpddr2_ck_drv;
	u32 phy_lpddr2_dq_drv;
	u32 phy_lpddr2_odt;

	u32 lpddr3_odt_dis_freq;
	u32 phy_lpddr3_odt_dis_freq;
	u32 lpddr3_drv;
	u32 lpddr3_odt;
	u32 phy_lpddr3_ca_drv;
	u32 phy_lpddr3_ck_drv;
	u32 phy_lpddr3_dq_drv;
	u32 phy_lpddr3_odt;

	u32 lpddr4_odt_dis_freq;
	u32 phy_lpddr4_odt_dis_freq;
	u32 lpddr4_drv;
	u32 lpddr4_dq_odt;
	u32 lpddr4_ca_odt;
	u32 phy_lpddr4_ca_drv;
	u32 phy_lpddr4_ck_cs_drv;
	u32 phy_lpddr4_dq_drv;
	u32 phy_lpddr4_odt;

	u32 ddr4_odt_dis_freq;
	u32 phy_ddr4_odt_dis_freq;
	u32 ddr4_drv;
	u32 ddr4_odt;
	u32 phy_ddr4_ca_drv;
	u32 phy_ddr4_ck_drv;
	u32 phy_ddr4_dq_drv;
	u32 phy_ddr4_odt;

	u32 ca_skew[15];
	u32 cs0_skew[44];
	u32 cs1_skew[44];
};

struct de_skew {
	u32 ca_de_skew[30];
	u32 cs0_de_skew[84];
	u32 cs1_de_skew[84];
};

struct px30_ddr {
	struct share_params params;
	struct dram_timing timing __aligned(PAGE_SIZE);
};

struct px30_dmcfreq {
	struct device *dev;
	struct devfreq *devfreq;
	struct devfreq_simple_ondemand_data ondemand_data;
	struct clk *dmc_clk;
	struct devfreq_event_dev *edev;
	struct mutex lock;
	struct regulator *vdd_center;
	unsigned long rate, target_rate;
	unsigned long volt, target_volt;
	struct px30_ddr __iomem *ddr;
};

static int px30_dmcfreq_target(struct device *dev, unsigned long *freq,
			       u32 flags)
{
	struct px30_dmcfreq *dmcfreq = dev_get_drvdata(dev);
	struct dev_pm_opp *opp;
	unsigned long old_clk_rate = dmcfreq->rate;
	unsigned long target_volt, target_rate;
	int err;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (IS_ERR(opp))
		return PTR_ERR(opp);

	target_volt = dev_pm_opp_get_voltage(opp);
	dev_pm_opp_put(opp);

	target_rate = clk_round_rate(dmcfreq->dmc_clk, *freq);
	if ((long)target_rate <= 0)
		target_rate = *freq;

	if (dmcfreq->rate == target_rate && dmcfreq->volt == target_volt)
		return 0;

	mutex_lock(&dmcfreq->lock);

	if (dmcfreq->rate == target_rate) {
		err = regulator_set_voltage(dmcfreq->vdd_center, target_volt,
					    INT_MAX);
		if (err) {
			dev_err(dev, "Cannot set voltage %lu uV\n",
				target_volt);
			return err;
		}

		dmcfreq->volt = target_volt;
		return 0;
	} else if (!dmcfreq->volt) {
		dmcfreq->volt = regulator_get_voltage(dmcfreq->vdd_center);
	}

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

	err = clk_set_rate(dmcfreq->dmc_clk, target_rate);
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
	} else if (old_clk_rate > target_rate)
		err = regulator_set_voltage(dmcfreq->vdd_center, target_volt,
					    INT_MAX);
	if (err)
		dev_err(dev, "Cannot set voltage %lu uV\n", target_volt);

	dmcfreq->rate = target_rate;
	dmcfreq->volt = target_volt;

out:
	mutex_unlock(&dmcfreq->lock);
	return err;
}

static int px30_dmcfreq_get_dev_status(struct device *dev,
				       struct devfreq_dev_status *stat)
{
	struct px30_dmcfreq *dmcfreq = dev_get_drvdata(dev);
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

static int px30_dmcfreq_get_cur_freq(struct device *dev, unsigned long *freq)
{
	struct px30_dmcfreq *dmcfreq = dev_get_drvdata(dev);

	*freq = dmcfreq->rate;

	return 0;
}

static struct devfreq_dev_profile px30_devfreq_dmc_profile = {
	.polling_ms	= 50,
	.target		= px30_dmcfreq_target,
	.get_dev_status	= px30_dmcfreq_get_dev_status,
	.get_cur_freq	= px30_dmcfreq_get_cur_freq,
};

static __maybe_unused int px30_dmcfreq_suspend(struct device *dev)
{
	struct px30_dmcfreq *dmcfreq = dev_get_drvdata(dev);
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

static __maybe_unused int px30_dmcfreq_resume(struct device *dev)
{
	struct px30_dmcfreq *dmcfreq = dev_get_drvdata(dev);
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

static void de_skew_set_to_reg(struct dram_timing *timing,
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

static SIMPLE_DEV_PM_OPS(px30_dmcfreq_pm, px30_dmcfreq_suspend,
			 px30_dmcfreq_resume);

static int of_get_ddr_timings(struct dram_timing *timing,
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

	kfree(de_skew);

	return ret;
}

static int px30_dmcfreq_probe(struct platform_device *pdev)
{
	struct arm_smccc_res res;
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;
	struct px30_dmcfreq *data;
	struct dev_pm_opp *opp;
	int ret;

	arm_smccc_smc(ROCKCHIP_SIP_DRAM_FREQ, 0, 0,
		      ROCKCHIP_SIP_CONFIG_DRAM_GET_VERSION,
		      0, 0, 0, 0, &res);
	dev_notice(dev, "ATF version 0x%lx!\n", res.a1);
	if (res.a0 || res.a1 < ATF_MIN_VERSION) {
		dev_err(dev, "ATF version invalid!\n");
		return -ENXIO;
	}

	data = devm_kzalloc(dev, sizeof(struct px30_dmcfreq), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	mutex_init(&data->lock);

	data->vdd_center = devm_regulator_get(dev, "center");
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

	data->ddr = rockchip_ddr_clk_get_atf_mem(data->dmc_clk);
	if (!data->ddr) {
		dev_err(dev, "Failed to get ATF memory\n");
		return -EINVAL;
	}

	ret = of_get_ddr_timings(&data->ddr->timing, np);
	if (ret) {
		dev_err(dev, "Failed to get timings\n");
		return ret;
	}

	arm_smccc_smc(ROCKCHIP_SIP_DRAM_FREQ, SHARE_PAGE_TYPE_DDR, 0,
		      ROCKCHIP_SIP_CONFIG_DRAM_INIT,
		      0, 0, 0, 0, &res);

	/*
	 * We add a devfreq driver to our parent since it has a device tree node
	 * with operating points.
	 */
	if (dev_pm_opp_of_add_table(dev)) {
		dev_err(dev, "Invalid operating-points in device tree.\n");
		ret = -EINVAL;
		goto err_edev;
	}

	of_property_read_u32(np, "upthreshold",
			     &data->ondemand_data.upthreshold);
	of_property_read_u32(np, "downdifferential",
			     &data->ondemand_data.downdifferential);

	data->rate = clk_get_rate(data->dmc_clk);

	opp = devfreq_recommended_opp(dev, &data->rate, 0);
	if (IS_ERR(opp)) {
		ret = PTR_ERR(opp);
		goto err_free_opp;
	}

	data->rate = dev_pm_opp_get_freq(opp);
	data->volt = dev_pm_opp_get_voltage(opp);
	dev_pm_opp_put(opp);

	px30_devfreq_dmc_profile.initial_freq = data->rate;

	data->devfreq = devm_devfreq_add_device(dev,
					   &px30_devfreq_dmc_profile,
					   DEVFREQ_GOV_SIMPLE_ONDEMAND,
					   &data->ondemand_data);
	if (IS_ERR(data->devfreq)) {
		ret = PTR_ERR(data->devfreq);
		goto err_free_opp;
	}

	devm_devfreq_register_opp_notifier(dev, data->devfreq);

	data->dev = dev;
	platform_set_drvdata(pdev, data);

	return 0;

err_free_opp:
	dev_pm_opp_of_remove_table(&pdev->dev);
err_edev:
	devfreq_event_disable_edev(data->edev);

	return ret;
}

static int px30_dmcfreq_remove(struct platform_device *pdev)
{
	struct px30_dmcfreq *dmcfreq = dev_get_drvdata(&pdev->dev);

	/*
	 * Before remove the opp table we need to unregister the opp notifier.
	 */
	devm_devfreq_unregister_opp_notifier(dmcfreq->dev, dmcfreq->devfreq);
	dev_pm_opp_of_remove_table(dmcfreq->dev);

	return 0;
}

static const struct of_device_id px30dmc_devfreq_of_match[] = {
	{ .compatible = "rockchip,px30-dmc" },
	{ },
};
MODULE_DEVICE_TABLE(of, px30dmc_devfreq_of_match);

static struct platform_driver px30_dmcfreq_driver = {
	.probe	= px30_dmcfreq_probe,
	.remove = px30_dmcfreq_remove,
	.driver = {
		.name	= "px30-dmc-freq",
		.pm	= &px30_dmcfreq_pm,
		.of_match_table = px30dmc_devfreq_of_match,
	},
};
module_platform_driver(px30_dmcfreq_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Lin Huang <hl@rock-chips.com>");
MODULE_DESCRIPTION("PX30 dmcfreq driver with devfreq framework");
