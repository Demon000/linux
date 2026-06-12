// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Renesas Electronics Corporation
 */

#include <linux/cleanup.h>
#include <linux/container_of.h>
#include <linux/clk.h>
#include <linux/mfd/core.h>
#include <linux/mfd/rzg2l-gpt.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

struct rzg2l_gpt_channel {
	struct mutex lock; /* Protect busy flag */
	bool is_busy;
};

struct rzg2l_gpt_priv {
	struct rzg2l_gpt gpt;
	struct rzg2l_gpt_channel channels[RZG2L_MAX_HW_CHANNELS];
};

static struct rzg2l_gpt_priv *gpt_to_priv(struct rzg2l_gpt *ddata)
{
	return container_of(ddata, struct rzg2l_gpt_priv, gpt);
}

int rzg2l_gpt_request_channel(struct rzg2l_gpt *ddata, unsigned int index)
{
	struct rzg2l_gpt_priv *priv = gpt_to_priv(ddata);
	struct rzg2l_gpt_channel *ch;

	if (index >= ddata->num_channels)
		return -EINVAL;

	ch = &priv->channels[index];

	guard(mutex)(&ch->lock);
	if (ch->is_busy)
		return -EBUSY;

	ch->is_busy = true;

	return 0;
}
EXPORT_SYMBOL_GPL(rzg2l_gpt_request_channel);

void rzg2l_gpt_release_channel(struct rzg2l_gpt *ddata, unsigned int index)
{
	struct rzg2l_gpt_priv *priv = gpt_to_priv(ddata);
	struct rzg2l_gpt_channel *ch;

	if (index >= ddata->num_channels)
		return;

	ch = &priv->channels[index];

	guard(mutex)(&ch->lock);
	WARN_ON(!ch->is_busy);

	ch->is_busy = false;
}
EXPORT_SYMBOL_GPL(rzg2l_gpt_release_channel);

static const struct mfd_cell rzg2l_gpt_devs[] = {
	MFD_CELL_NAME("pwm-rzg2l-gpt"),
};

static int rzg2l_gpt_probe(struct platform_device *pdev)
{
	u32 num_channels = RZG2L_MAX_HW_CHANNELS;
	struct device *dev = &pdev->dev;
	struct rzg2l_gpt_priv *priv;
	struct rzg2l_gpt *ddata;
	struct reset_control *rstc;
	struct clk *clk;
	unsigned int i;

	if (!device_property_read_u32(dev, "renesas,num-channels", &num_channels)) {
		if (num_channels > RZG2L_MAX_HW_CHANNELS)
			return -EINVAL;
	}

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	ddata = &priv->gpt;
	ddata->info = device_get_match_data(dev);
	ddata->num_channels = num_channels;

	ddata->mmio = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ddata->mmio))
		return PTR_ERR(ddata->mmio);

	for (i = 0; i < RZG2L_MAX_HW_CHANNELS; i++)
		mutex_init(&priv->channels[i].lock);

	rstc = devm_reset_control_get_exclusive_deasserted(dev, NULL);
	if (IS_ERR(rstc))
		return dev_err_probe(dev, PTR_ERR(rstc), "Cannot deassert reset control\n");

	rstc = devm_reset_control_get_optional_exclusive_deasserted(dev, "rst_s");
	if (IS_ERR(rstc))
		return dev_err_probe(dev, PTR_ERR(rstc), "Cannot deassert rst_s reset\n");

	clk = devm_clk_get_optional_enabled(dev, "bus");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "Cannot get bus clock\n");

	ddata->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(ddata->clk))
		return dev_err_probe(dev, PTR_ERR(ddata->clk), "Cannot get clock\n");

	platform_set_drvdata(pdev, ddata);

	return devm_mfd_add_devices(dev, PLATFORM_DEVID_AUTO, rzg2l_gpt_devs,
				    ARRAY_SIZE(rzg2l_gpt_devs), NULL, 0, NULL);
}

static const u8 rzg3e_gpt_prescales[] = {
	[0] = 0x0,
	[1] = 0x1,
	[2] = 0x2,
	[3] = 0x3,
	[4] = 0x4,
	[5] = 0x5,
	[6] = 0x6,
	[7] = RZG2L_INVALID_TPCS,
	[8] = 0x8,
	[9] = RZG2L_INVALID_TPCS,
	[10] = 0xA,
};

static const struct rzg2l_gpt_info rzg3e_data = {
	.prescales = rzg3e_gpt_prescales,
	.num_prescales = ARRAY_SIZE(rzg3e_gpt_prescales),
	.gtcr_tpcs = RZG3E_GTCR_TPCS,
};

static const u8 rzg2l_gpt_prescales[] = {
	[0] = 0x0,
	[1] = RZG2L_INVALID_TPCS,
	[2] = 0x1,
	[3] = RZG2L_INVALID_TPCS,
	[4] = 0x2,
	[5] = RZG2L_INVALID_TPCS,
	[6] = 0x3,
	[7] = RZG2L_INVALID_TPCS,
	[8] = 0x4,
	[9] = RZG2L_INVALID_TPCS,
	[10] = 0x5,
};

static const struct rzg2l_gpt_info rzg2l_data = {
	.prescales = rzg2l_gpt_prescales,
	.num_prescales = ARRAY_SIZE(rzg2l_gpt_prescales),
	.gtcr_tpcs = RZG2L_GTCR_TPCS,
};

static const struct of_device_id rzg2l_gpt_of_match[] = {
	{ .compatible = "renesas,r9a09g047-gpt", .data = &rzg3e_data },
	{ .compatible = "renesas,rzg2l-gpt", .data = &rzg2l_data, },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rzg2l_gpt_of_match);

static struct platform_driver rzg2l_gpt_driver = {
	.probe = rzg2l_gpt_probe,
	.driver	= {
		.name = "rz-gpt",
		.of_match_table = rzg2l_gpt_of_match,
	},
};
module_platform_driver(rzg2l_gpt_driver);

MODULE_AUTHOR("Cosmin Tanislav <cosmin-gabriel.tanislav.xa@renesas.com>");
MODULE_DESCRIPTION("Renesas RZ/G2L General PWM Timer (GPT) Core Driver");
MODULE_LICENSE("GPL");
