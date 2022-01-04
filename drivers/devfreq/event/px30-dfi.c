// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: Cosmin Tanislav <demonsingur@gmail.com>
 */

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

#include <soc/rockchip/px30_grf.h>

#define PX30_DMC_NUM_CH	2

/* DDRMON_CTRL */
#define DDRMON_CTRL	0x04
#define CLR_DDRMON_CTRL	(0x1f0000 << 0)
#define DDR4_EN		(0x10001 << 5)
#define LPDDR4_EN	(0x10001 << 4)
#define HARDWARE_EN	(0x10001 << 3)
#define LPDDR2_3_EN	(0x10001 << 2)
#define SOFTWARE_EN	(0x10001 << 1)
#define SOFTWARE_DIS	(0x10000 << 1)
#define TIME_CNT_EN	(0x10001 << 0)

#define DDRMON_COUNT_NUM	0x28
#define DDRMON_ACCESS_NUM	0x2c

/*
 * The dfi controller can monitor DDR load. It has an upper and lower threshold
 * for the operating points. Whenever the usage leaves these bounds an event is
 * generated to indicate the DDR frequency should be changed.
 */
struct rockchip_dfi {
	struct devfreq_event_dev *edev;
	struct devfreq_event_desc *desc;
	void __iomem *regs;
	u32 ddr_type;
};

static void rockchip_dfi_start_hardware_counter(struct devfreq_event_dev *edev)
{
	struct rockchip_dfi *info = devfreq_event_get_drvdata(edev);
	void __iomem *dfi_regs = info->regs;
	u32 val;

	/* clear DDRMON_CTRL setting */
	writel_relaxed(CLR_DDRMON_CTRL, dfi_regs + DDRMON_CTRL);

	/* set ddr type to dfi */
	if (info->ddr_type == PX30_PMUGRF_DDRTYPE_LPDDR3 ||
		info->ddr_type == PX30_PMUGRF_DDRTYPE_LPDDR2)
		writel_relaxed(LPDDR2_3_EN, dfi_regs + DDRMON_CTRL);
	else if (info->ddr_type == PX30_PMUGRF_DDRTYPE_LPDDR4)
		writel_relaxed(LPDDR4_EN, dfi_regs + DDRMON_CTRL);
	else if (info->ddr_type == PX30_PMUGRF_DDRTYPE_DDR4)
		writel_relaxed(DDR4_EN, dfi_regs + DDRMON_CTRL);

	/* enable count, use software mode */
	writel_relaxed(SOFTWARE_EN, dfi_regs + DDRMON_CTRL);
}

static void rockchip_dfi_stop_hardware_counter(struct devfreq_event_dev *edev)
{
	struct rockchip_dfi *info = devfreq_event_get_drvdata(edev);
	void __iomem *dfi_regs = info->regs;

	writel_relaxed(SOFTWARE_DIS, dfi_regs + DDRMON_CTRL);
}

static int rockchip_dfi_disable(struct devfreq_event_dev *edev)
{
	rockchip_dfi_stop_hardware_counter(edev);

	return 0;
}

static int rockchip_dfi_enable(struct devfreq_event_dev *edev)
{
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
	void __iomem *dfi_regs = info->regs;

	rockchip_dfi_stop_hardware_counter(edev);

	edata->load_count = readl_relaxed(dfi_regs + DDRMON_ACCESS_NUM);

	if (info->ddr_type == PX30_PMUGRF_DDRTYPE_LPDDR4)
		edata->load_count *= 8;
	else
		edata->load_count *= 4;

	edata->total_count = readl_relaxed(dfi_regs + DDRMON_COUNT_NUM);

	rockchip_dfi_start_hardware_counter(edev);

	return 0;
}

static const struct devfreq_event_ops rockchip_dfi_ops = {
	.disable = rockchip_dfi_disable,
	.enable = rockchip_dfi_enable,
	.get_event = rockchip_dfi_get_event,
	.set_event = rockchip_dfi_set_event,
};

static const struct of_device_id rockchip_dfi_id_match[] = {
	{ .compatible = "rockchip,px30-dfi" },
	{ },
};
MODULE_DEVICE_TABLE(of, rockchip_dfi_id_match);

static int rockchip_dfi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rockchip_dfi *data;
	struct devfreq_event_desc *desc;
	struct device_node *np = pdev->dev.of_node, *node;
	struct regmap *regmap_pmugrf;

	data = devm_kzalloc(dev, sizeof(struct rockchip_dfi), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(data->regs))
		return PTR_ERR(data->regs);

	/* try to find the optional reference to the pmu syscon */
	node = of_parse_phandle(np, "rockchip,pmugrf", 0);
	if (node) {
		regmap_pmugrf = syscon_node_to_regmap(node);
		of_node_put(node);
		if (IS_ERR(regmap_pmugrf))
			return PTR_ERR(regmap_pmugrf);
	}

	/* get ddr type */
	regmap_read(regmap_pmugrf, PX30_PMUGRF_OS_REG2, &val);
	data->ddr_type = (val >> PX30_PMUGRF_DDRTYPE_SHIFT) &
			 PX30_PMUGRF_DDRTYPE_MASK;

	desc = devm_kzalloc(dev, sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	desc->ops = &rockchip_dfi_ops;
	desc->driver_data = data;
	desc->name = np->name;
	data->desc = desc;

	data->edev = devm_devfreq_event_add_edev(&pdev->dev, desc);
	if (IS_ERR(data->edev)) {
		dev_err(&pdev->dev,
			"failed to add devfreq-event device\n");
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
MODULE_AUTHOR("Cosmin Tanislav <demonsingur@gmail.com>");
MODULE_DESCRIPTION("PX30 DFI driver");
