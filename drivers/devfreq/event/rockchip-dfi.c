// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016, Fuzhou Rockchip Electronics Co., Ltd
 * Author: Lin Huang <hl@rock-chips.com>
 */

#include <linux/clk.h>
#include <linux/devfreq-event.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/of.h>

#include <soc/rockchip/rockchip_grf.h>

#define MAX_DMC_NUM_CH		2

/* DDRMON_CTRL */
#define DDRMON_CTRL	0x04
#define CLR_DDRMON_CTRL	(0x1f0000 << 0)
#define LPDDR4_EN	(0x10001 << 4)
#define HARDWARE_EN	(0x10001 << 3)
#define LPDDR2_3_EN	(0x10001 << 2)
#define SOFTWARE_EN	(0x10001 << 1)
#define SOFTWARE_DIS	(0x10000 << 1)
#define TIME_CNT_EN	(0x10001 << 0)

#define DDRMON_CH0_COUNT_NUM		0x28
#define DDRMON_CH0_DFI_ACCESS_NUM	0x2c
#define DDRMON_CH1_COUNT_NUM		0x3c
#define DDRMON_CH1_DFI_ACCESS_NUM	0x40

struct dmc_usage {
	u32 access;
	u32 total;
};

/*
 * The dfi controller can monitor DDR load. It has an upper and lower threshold
 * for the operating points. Whenever the usage leaves these bounds an event is
 * generated to indicate the DDR frequency should be changed.
 */
struct rockchip_dfi {
	struct devfreq_event_dev *edev;
	struct devfreq_event_desc *desc;
	struct dmc_usage ch_usage[MAX_DMC_NUM_CH];
	void __iomem *regs;
	struct clk *clk;
	u32 dram_type;
	u32 ch_msk;
};

static void rockchip_dfi_start_hardware_counter(struct devfreq_event_dev *edev)
{
	struct rockchip_dfi *info = devfreq_event_get_drvdata(edev);
	void __iomem *dfi_regs = info->regs;

	/* clear DDRMON_CTRL setting */
	writel_relaxed(CLR_DDRMON_CTRL, dfi_regs + DDRMON_CTRL);

	/* set ddr type to dfi */
	if (info->dram_type == LPDDR3 || info->dram_type == LPDDR2)
		writel_relaxed(LPDDR2_3_EN, dfi_regs + DDRMON_CTRL);
	else if (info->dram_type == LPDDR4)
		writel_relaxed(LPDDR4_EN, dfi_regs + DDRMON_CTRL);

	/* enable count, use software mode */
	writel_relaxed(SOFTWARE_EN, dfi_regs + DDRMON_CTRL);
}

static void rockchip_dfi_stop_hardware_counter(struct devfreq_event_dev *edev)
{
	struct rockchip_dfi *info = devfreq_event_get_drvdata(edev);
	void __iomem *dfi_regs = info->regs;

	writel_relaxed(SOFTWARE_DIS, dfi_regs + DDRMON_CTRL);
}

static int rockchip_dfi_get_busier_ch(struct devfreq_event_dev *edev)
{
	struct rockchip_dfi *info = devfreq_event_get_drvdata(edev);
	u32 tmp, max = 0;
	u32 i, busier_ch = 0;
	void __iomem *dfi_regs = info->regs;

	rockchip_dfi_stop_hardware_counter(edev);

	/* Find out which channel is busier */
	for (i = 0; i < MAX_DMC_NUM_CH; i++) {
		if (!(info->ch_msk & BIT(i)))
			continue;
		info->ch_usage[i].access = readl_relaxed(dfi_regs +
				DDRMON_CH0_DFI_ACCESS_NUM + i * 20) * 4;
		info->ch_usage[i].total = readl_relaxed(dfi_regs +
				DDRMON_CH0_COUNT_NUM + i * 20);
		tmp = info->ch_usage[i].access;
		if (tmp > max) {
			busier_ch = i;
			max = tmp;
		}
	}
	rockchip_dfi_start_hardware_counter(edev);

	return busier_ch;
}

static int rockchip_dfi_disable(struct devfreq_event_dev *edev)
{
	struct rockchip_dfi *info = devfreq_event_get_drvdata(edev);

	rockchip_dfi_stop_hardware_counter(edev);
	if (info->clk)
		clk_disable_unprepare(info->clk);

	return 0;
}

static int rockchip_dfi_enable(struct devfreq_event_dev *edev)
{
	struct rockchip_dfi *info = devfreq_event_get_drvdata(edev);
	int ret;

	if (info->clk) {
		ret = clk_prepare_enable(info->clk);
		if (ret) {
			dev_err(&edev->dev, "failed to enable dfi clk: %d\n",
				ret);
			return ret;
		}
	}

	rockchip_dfi_start_hardware_counter(edev);
	return 0;
}

static int rockchip_dfi_set_event(struct devfreq_event_dev *edev)
{
	return 0;
}

static int rockchip_dfi_get_event(struct devfreq_event_dev *edev,
				  struct devfreq_event_data *edata)
{
	struct rockchip_dfi *info = devfreq_event_get_drvdata(edev);
	int busier_ch;
	unsigned long flags;

	local_irq_save(flags);
	busier_ch = rockchip_dfi_get_busier_ch(edev);
	local_irq_restore(flags);

	edata->load_count = info->ch_usage[busier_ch].access;
	edata->total_count = info->ch_usage[busier_ch].total;

	return 0;
}

static const struct devfreq_event_ops rockchip_dfi_ops = {
	.disable = rockchip_dfi_disable,
	.enable = rockchip_dfi_enable,
	.get_event = rockchip_dfi_get_event,
	.set_event = rockchip_dfi_set_event,
};

static __init int px30_dfi_init(struct platform_device *pdev)
{
	struct rockchip_dfi *data = platform_get_drvdata(pdev);
	struct devfreq_event_desc *desc = data->desc;
	struct device_node *np = pdev->dev.of_node, *node;
	struct regmap *regmap_pmugrf;
	u32 val;

	data->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(data->regs))
		return PTR_ERR(data->regs);

	node = of_parse_phandle(np, "rockchip,pmugrf", 0);
	if (node) {
		regmap_pmugrf = syscon_node_to_regmap(node);
		if (IS_ERR(regmap_pmugrf))
			return PTR_ERR(regmap_pmugrf);
	}

	regmap_read(regmap_pmugrf, PX30_PMUGRF_OS_REG2, &val);
	data->dram_type = READ_DRAMTYPE_INFO(val);
	data->ch_msk = 1;
	data->clk = NULL;

	desc->ops = &rockchip_dfi_ops;

	return 0;
}

static __init int rockchip_dfi_init(struct platform_device *pdev)
{
	struct rockchip_dfi *data = platform_get_drvdata(pdev);
	struct devfreq_event_desc *desc = data->desc;
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node, *node;
	struct regmap *regmap_pmu;
	u32 val;

	data->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(data->regs))
		return PTR_ERR(data->regs);

	data->clk = devm_clk_get(dev, "pclk_ddr_mon");
	if (IS_ERR(data->clk)) {
		dev_err(dev, "Cannot get the clk dmc_clk\n");
		return PTR_ERR(data->clk);
	}

	/* try to find the optional reference to the pmu syscon */
	node = of_parse_phandle(np, "rockchip,pmu", 0);
	if (node) {
		regmap_pmu = syscon_node_to_regmap(node);
		of_node_put(node);
		if (IS_ERR(regmap_pmu))
			return PTR_ERR(regmap_pmu);
	}

	regmap_read(regmap_pmu, PMUGRF_OS_REG2, &val);
	data->dram_type = READ_DRAMTYPE_INFO(val);
	data->ch_msk = READ_CH_INFO(val);

	desc->ops = &rockchip_dfi_ops;

	return 0;
}

static const struct of_device_id rockchip_dfi_id_match[] = {
	{ .compatible = "rockchip,px30-dfi", .data = px30_dfi_init },
	{ .compatible = "rockchip,rk3399-dfi", .data = rockchip_dfi_init },
	{ },
};

static int rockchip_dfi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rockchip_dfi *data;
	struct devfreq_event_desc *desc;
	struct device_node *np = pdev->dev.of_node;
	int (*init)(struct platform_device *pdev, struct rockchip_dfi *data,
		    struct devfreq_event_desc *desc);

	data = devm_kzalloc(dev, sizeof(struct rockchip_dfi), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	platform_set_drvdata(pdev, data);

	desc = devm_kzalloc(dev, sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	desc->driver_data = data;
	desc->name = np->name;
	data->desc = desc;

	init = device_get_match_data(dev);
	if (!init)
		return -EINVAL;

	init(pdev, data, desc);

	data->edev = devm_devfreq_event_add_edev(dev, desc);
	if (IS_ERR(data->edev)) {
		dev_err(dev, "failed to add devfreq-event device\n");
		return PTR_ERR(data->edev);
	}

	return 0;
}

static struct platform_driver rockchip_dfi_driver = {
	.probe	= rockchip_dfi_probe,
	.driver = {
		.name	= "rockchip-dfi",
		.of_match_table = rockchip_dfi_id_match,
	},
};
module_platform_driver(rockchip_dfi_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Lin Huang <hl@rock-chips.com>");
MODULE_DESCRIPTION("Rockchip DFI driver");
