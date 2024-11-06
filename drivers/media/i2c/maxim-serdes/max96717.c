// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim MAX96717 GMSL2 Serializer Driver
 *
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include <linux/gpio/driver.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinconf-generic.h>

#include "max_ser.h"
#include "max_serdes.h"

#define MAX96717_NAME				"max96717"
#define MAX96717_PINCTRL_NAME			MAX96717_NAME "-pinctrl"
#define MAX96717_GPIOCHIP_NAME			MAX96717_NAME "-gpiochip"
#define MAX96717_GPIO_NUM			11
#define MAX96717_PIPES_NUM			4
#define MAX96717_PHYS_NUM			2

#define MAX96717_REG0				0x0

#define MAX96717_REG2				0x2
#define MAX96717_REG2_VID_TX_EN_P(p)		BIT(4 + (p))

#define MAX96717_REG3				0x3
#define MAX96717_REG3_RCLKSEL			GENMASK(1, 0)

#define MAX96717_REG6				0x6
#define MAX96717_REG6_RCLKEN			BIT(5)

#define MAX96717_I2C_2(x)			(0x42 + (x) * 0x2)
#define MAX96717_I2C_2_SRC			GENMASK(7, 1)

#define MAX96717_I2C_3(x)			(0x43 + (x) * 0x2)
#define MAX96717_I2C_3_DST			GENMASK(7, 1)

#define MAX96717_TX3(p)				(0x53 + (p) * 0x4)
#define MAX96717_TX3_TX_STR_SEL			GENMASK(1, 0)

#define MAX96717_VIDEO_TX0(p)			(0x100 + (p) * 0x8)
#define MAX96717_VIDEO_TX0_AUTO_BPP		BIT(3)

#define MAX96717_VIDEO_TX1(p)			(0x101 + (p) * 0x8)
#define MAX96717_VIDEO_TX1_BPP			GENMASK(5, 0)

#define MAX96717_VIDEO_TX2(p)			(0x102 + (p) * 0x8)
#define MAX96717_VIDEO_TX2CLKDET		BIT(7)

#define MAX96717_GPIO_A(x)			(0x2be + (x) * 0x3)
#define MAX96717_GPIO_A_GPIO_OUT_DIS		BIT(0)
#define MAX96717_GPIO_A_GPIO_TX_EN		BIT(1)
#define MAX96717_GPIO_A_GPIO_RX_EN		BIT(2)
#define MAX96717_GPIO_A_GPIO_IN			BIT(3)
#define MAX96717_GPIO_A_GPIO_OUT		BIT(4)
#define MAX96717_GPIO_A_TX_COMP_EN		BIT(5)
#define MAX96717_GPIO_A_RES_CFG			BIT(7)

#define MAX96717_GPIO_B(x)			(0x2bf + (x) * 0x3)
#define MAX96717_GPIO_B_GPIO_TX_ID		GENMASK(4, 0)
#define MAX96717_GPIO_B_OUT_TYPE		BIT(5)
#define MAX96717_GPIO_B_PULL_UPDN_SEL		GENMASK(7, 6)
#define MAX96717_GPIO_B_PULL_UPDN_SEL_NONE	0b00
#define MAX96717_GPIO_B_PULL_UPDN_SEL_PU	0b01
#define MAX96717_GPIO_B_PULL_UPDN_SEL_PD	0b10

#define MAX96717_GPIO_C(x)			(0x2c0 + (x) * 0x3)
#define MAX96717_GPIO_C_GPIO_RX_ID		GENMASK(4, 0)

#define MAX96717_CMU2				0x302
#define MAX96717_CMU2_PFDDIV_RSHORT		GENMASK(6, 4)
#define MAX96717_CMU2_PFDDIV_RSHORT_1_1V	0b001

#define MAX96717_FRONTTOP_0			0x308
#define MAX96717_FRONTTOP_0_CLK_SEL_P(x)	BIT(x)
#define MAX96717_FRONTTOP_0_START_PORT(x)	BIT((x) + 4)

#define MAX96717_FRONTTOP_1(p)			(0x309 + (p) * 0x2)
#define MAX96717_FRONTTOP_2(p)			(0x30a + (p) * 0x2)

#define MAX96717_FRONTTOP_9			0x311
#define MAX96717_FRONTTOP_9_START_PORT(p, x)	BIT((p) + (x) * 4)

#define MAX96717_FRONTTOP_10			0x312
#define MAX96717_FRONTTOP_10_BPP8DBL(p)		BIT(p)

#define MAX96717_FRONTTOP_11			0x313
#define MAX96717_FRONTTOP_11_BPP10DBL(p)	BIT(p)
#define MAX96717_FRONTTOP_11_BPP12DBL(p)	BIT((p) + 4)

#define MAX96717_FRONTTOP_12(p, x)		(0x314 + (p) * 0x2 + (x))
#define MAX96717_MEM_DT_SEL			GENMASK(5, 0)
#define MAX96717_MEM_DT_EN			BIT(6)

#define MAX96717_FRONTTOP_20(p)			(0x31c + (p) * 0x1)
#define MAX96717_FRONTTOP_20_SOFT_BPP_EN	BIT(5)
#define MAX96717_FRONTTOP_20_SOFT_BPP		GENMASK(4, 0)

#define MAX96717_MIPI_RX0			0x330
#define MAX96717_MIPI_RX0_PHY_CONFIG		GENMASK(2, 0)
#define MAX96717_MIPI_RX0_NONCONTCLK_EN		BIT(6)

#define MAX96717_MIPI_RX1			0x331
#define MAX96717_MIPI_RX1_CTRL_NUM_LANES	GENMASK(5, 4)

#define MAX96717_MIPI_RX2			0x332
#define MAX96717_MIPI_RX2_PHY1_LANE_MAP		GENMASK(7, 4)

#define MAX96717_MIPI_RX3			0x333
#define MAX96717_MIPI_RX3_PHY2_LANE_MAP		GENMASK(3, 0)

#define MAX96717_MIPI_RX4			0x334
#define MAX96717_MIPI_RX4_PHY1_POL_MAP		GENMASK(6, 4)
#define MAX96717_MIPI_RX4_PHY1_POL_MAP_LANE(x)	BIT((x) + 4)

#define MAX96717_MIPI_RX5			0x335
#define MAX96717_MIPI_RX5_PHY2_POL_MAP		GENMASK(2, 0)
#define MAX96717_MIPI_RX5_PHY2_POL_MAP_CLK	BIT(2)
#define MAX96717_MIPI_RX5_PHY2_POL_MAP_LANE(x)	BIT(x)

#define MAX96717_EXTA(x)			(0x3dc + (x))

#define MAX96717_EXT11				0x383
#define MAX96717_EXT11_TUN_MODE			BIT(7)

#define MAX96717_EXT21				0x38d
#define MAX96717_EXT22				0x38e
#define MAX96717_EXT23				0x38f
#define MAX96717_EXT24				0x390

#define MAX96717_REF_VTG0			0x3f1
#define MAX96717_REF_VTG0_PCLK_EN		BIT(0)
#define MAX96717_REF_VTG0_PCLK_GPIO		GENMASK(5, 1)
#define MAX96717_REF_VTG0_RCLKEN_Y		BIT(7)

#define MAX96717_PIO_SLEW_0			0x56f
#define MAX96717_PIO_SLEW_0_PIO00_SLEW		GENMASK(1, 0)
#define MAX96717_PIO_SLEW_0_PIO01_SLEW		GENMASK(3, 2)
#define MAX96717_PIO_SLEW_0_PIO02_SLEW		GENMASK(5, 4)

#define MAX96717_PIO_SLEW_1			0x570
#define MAX96717_PIO_SLEW_1_PIO05_SLEW		GENMASK(3, 2)
#define MAX96717_PIO_SLEW_1_PIO06_SLEW		GENMASK(5, 4)

#define MAX96717_PIO_SLEW_2			0x571
#define MAX96717_PIO_SLEW_2_PIO010_SLEW		GENMASK(5, 4)
#define MAX96717_PIO_SLEW_2_PIO011_SLEW		GENMASK(7, 6)

#define field_get(mask, val) (((val) & (mask)) >> __ffs(mask))
#define field_prep(mask, val) (((val) << __ffs(mask)) & (mask))

struct max96717_priv {
	struct max_ser ser;
	struct pinctrl_desc pctldesc;
	struct gpio_chip gc;
	const struct max96717_chip_info *info;

	struct device *dev;
	struct i2c_client *client;
	struct regmap *regmap;
	struct pinctrl_dev *pctldev;
};

struct max96717_chip_info {
	bool supports_tunnel_mode;
	bool supports_noncontinuous_clock;
	bool supports_pkt_cnt;
	unsigned int num_pipes;
	unsigned int num_dts_per_pipe;
	unsigned int pipe_hw_ids[MAX96717_PIPES_NUM];
	unsigned int num_phys;
	unsigned int phy_hw_ids[MAX96717_PHYS_NUM];
};

#define ser_to_priv(ser) \
	container_of(ser, struct max96717_priv, ser)

static int max96717_read(struct max96717_priv *priv, int reg)
{
	int ret, val;

	ret = regmap_read(priv->regmap, reg, &val);
	dev_dbg(priv->dev, "read %d 0x%x = 0x%02x\n", ret, reg, val);
	if (ret) {
		dev_err(priv->dev, "read 0x%04x failed\n", reg);
		return ret;
	}

	return val;
}

static int max96717_write(struct max96717_priv *priv, unsigned int reg, u8 val)
{
	int ret;

	ret = regmap_write(priv->regmap, reg, val);
	dev_dbg(priv->dev, "write %d 0x%x = 0x%02x\n", ret, reg, val);
	if (ret)
		dev_err(priv->dev, "write 0x%04x failed\n", reg);

	return ret;
}

static int max96717_update_bits(struct max96717_priv *priv, unsigned int reg,
				u8 mask, u8 val)
{
	int ret;

	ret = regmap_update_bits(priv->regmap, reg, mask, val);
	dev_dbg(priv->dev, "update %d 0x%x 0x%02x = 0x%02x\n", ret, reg, mask, val);
	if (ret)
		dev_err(priv->dev, "update 0x%04x failed\n", reg);

	return ret;
}

static int max96717_wait_for_device(struct max96717_priv *priv)
{
	unsigned int i;
	int ret;

	for (i = 0; i < 10; i++) {
		ret = max96717_read(priv, MAX96717_REG0);
		if (ret >= 0)
			return 0;

		msleep(100);

		dev_err(priv->dev, "Retry %u waiting for serializer: %d\n", i, ret);
	}

	return ret;
}

#define MAX96717_PIN(n) \
	PINCTRL_PIN(n, "mfp" __stringify(n))

static const struct pinctrl_pin_desc max96717_pins[] = {
	MAX96717_PIN(0),
	MAX96717_PIN(1),
	MAX96717_PIN(2),
	MAX96717_PIN(3),
	MAX96717_PIN(4),
	MAX96717_PIN(5),
	MAX96717_PIN(6),
	MAX96717_PIN(7),
	MAX96717_PIN(8),
	MAX96717_PIN(9),
	MAX96717_PIN(10),
};

#define MAX96717_GROUP_PINS(name, ...) \
	static const unsigned int name ## _pins[] = { __VA_ARGS__ }

MAX96717_GROUP_PINS(mfp0, 0);
MAX96717_GROUP_PINS(mfp1, 1);
MAX96717_GROUP_PINS(mfp2, 2);
MAX96717_GROUP_PINS(mfp3, 3);
MAX96717_GROUP_PINS(mfp4, 4);
MAX96717_GROUP_PINS(mfp5, 5);
MAX96717_GROUP_PINS(mfp6, 6);
MAX96717_GROUP_PINS(mfp7, 7);
MAX96717_GROUP_PINS(mfp8, 8);
MAX96717_GROUP_PINS(mfp9, 9);
MAX96717_GROUP_PINS(mfp10, 10);

#define MAX96717_GROUP(name) \
	PINCTRL_PINGROUP(__stringify(name), name ## _pins, ARRAY_SIZE(name ## _pins))

static const struct pingroup max96717_ctrl_groups[] = {
	MAX96717_GROUP(mfp0),
	MAX96717_GROUP(mfp1),
	MAX96717_GROUP(mfp2),
	MAX96717_GROUP(mfp3),
	MAX96717_GROUP(mfp4),
	MAX96717_GROUP(mfp5),
	MAX96717_GROUP(mfp6),
	MAX96717_GROUP(mfp7),
	MAX96717_GROUP(mfp8),
	MAX96717_GROUP(mfp9),
	MAX96717_GROUP(mfp10),
};

#define MAX96717_FUNC_GROUPS(name, ...) \
	static const char * const name ## _groups[] = { __VA_ARGS__ }

MAX96717_FUNC_GROUPS(gpio, "mfp0", "mfp1", "mfp2", "mfp3", "mfp4", "mfp5",
			   "mfp6", "mfp7", "mfp8", "mfp9", "mfp10");
MAX96717_FUNC_GROUPS(pclk, "mfp0", "mfp1", "mfp2", "mfp3", "mfp4", "mfp7", "mfp8");
MAX96717_FUNC_GROUPS(rclkout, "mfp4", "mfp2");

enum max96717_func {
	max96717_func_gpio,
	max96717_func_pclk,
	max96717_func_rclkout,
};

#define MAX96717_FUNC(name)						\
	[max96717_func_ ## name] =					\
		PINCTRL_PINFUNCTION(__stringify(name), name ## _groups,	\
				    ARRAY_SIZE(name ## _groups))

static const struct pinfunction max96717_functions[] = {
	MAX96717_FUNC(gpio),
	MAX96717_FUNC(pclk),
	MAX96717_FUNC(rclkout),
};

#define MAX96717_PINCTRL_X(x) (PIN_CONFIG_END + x)
#define MAX96717_PINCTRL_PULL_STRENGTH_WEAK	MAX96717_PINCTRL_X(1)
#define MAX96717_PINCTRL_JITTER_COMPENSATION_EN	MAX96717_PINCTRL_X(2)
#define MAX96717_PINCTRL_GMSL_TX_EN		MAX96717_PINCTRL_X(3)
#define MAX96717_PINCTRL_GMSL_RX_EN		MAX96717_PINCTRL_X(4)
#define MAX96717_PINCTRL_GMSL_TX_ID		MAX96717_PINCTRL_X(5)
#define MAX96717_PINCTRL_GMSL_RX_ID		MAX96717_PINCTRL_X(6)
#define MAX96717_PINCTRL_RCLKOUT_CLK		MAX96717_PINCTRL_X(7)
#define MAX96717_PINCTRL_INPUT_VALUE		MAX96717_PINCTRL_X(8)

static const struct pinconf_generic_params max96717_cfg_params[] = {
	{ "maxim,pull-strength-weak", MAX96717_PINCTRL_PULL_STRENGTH_WEAK, 0 },
	{ "maxim,jitter-compensation", MAX96717_PINCTRL_JITTER_COMPENSATION_EN, 0 },
	{ "maxim,gmsl-tx", MAX96717_PINCTRL_GMSL_TX_EN, 0 },
	{ "maxim,gmsl-rx", MAX96717_PINCTRL_GMSL_RX_EN, 0 },
	{ "maxim,gmsl-tx-id", MAX96717_PINCTRL_GMSL_TX_ID, 0 },
	{ "maxim,gmsl-rx-id", MAX96717_PINCTRL_GMSL_RX_ID, 0 },
	{ "maxim,rclkout-clock", MAX96717_PINCTRL_RCLKOUT_CLK, 0 },
};

static int max96717_ctrl_get_groups_count(struct pinctrl_dev *pctldev)
{
	return ARRAY_SIZE(max96717_ctrl_groups);
}

static const char *max96717_ctrl_get_group_name(struct pinctrl_dev *pctldev,
						unsigned selector)
{
	return max96717_ctrl_groups[selector].name;
}

static int max96717_ctrl_get_group_pins(struct pinctrl_dev *pctldev, unsigned selector,
					const unsigned **pins, unsigned *num_pins)
{
	*pins = (unsigned *) max96717_ctrl_groups[selector].pins;
	*num_pins = max96717_ctrl_groups[selector].npins;

	return 0;
}

static int max96717_get_pin_config_reg(unsigned int offset, u32 param,
				       unsigned int *reg, unsigned int *mask,
				       unsigned int *val)
{
	*reg = MAX96717_GPIO_A(offset);

	switch (param) {
	case PIN_CONFIG_OUTPUT_ENABLE:
		*mask = MAX96717_GPIO_A_GPIO_OUT_DIS;
		*val = 0b0;
		return 0;
	case PIN_CONFIG_INPUT_ENABLE:
		*mask = MAX96717_GPIO_A_GPIO_OUT_DIS;
		*val = 0b1;
		return 0;
	case MAX96717_PINCTRL_GMSL_TX_EN:
		*mask = MAX96717_GPIO_A_GPIO_TX_EN;
		*val = 0b1;
		return 0;
	case MAX96717_PINCTRL_GMSL_RX_EN:
		*mask = MAX96717_GPIO_A_GPIO_RX_EN;
		*val = 0b1;
		return 0;
	case MAX96717_PINCTRL_INPUT_VALUE:
		*mask = MAX96717_GPIO_A_GPIO_IN;
		*val = 0b1;
		return 0;
	case PIN_CONFIG_OUTPUT:
		*mask = MAX96717_GPIO_A_GPIO_OUT;
		*val = 0b1;
		return 0;
	case MAX96717_PINCTRL_JITTER_COMPENSATION_EN:
		*mask = MAX96717_GPIO_A_TX_COMP_EN;
		*val = 0b1;
		return 0;
	case MAX96717_PINCTRL_PULL_STRENGTH_WEAK:
		*mask = MAX96717_GPIO_A_RES_CFG;
		*val = 0b0;
		return 0;
	}

	*reg = MAX96717_GPIO_B(offset);

	switch(param) {
	case MAX96717_PINCTRL_GMSL_TX_ID:
		*mask = MAX96717_GPIO_B_GPIO_TX_ID;
		return 0;
	case PIN_CONFIG_DRIVE_OPEN_DRAIN:
		*mask = MAX96717_GPIO_B_OUT_TYPE;
		*val = 0b0;
		return 0;
	case PIN_CONFIG_DRIVE_PUSH_PULL:
		*mask = MAX96717_GPIO_B_OUT_TYPE;
		*val = 0b1;
		return 0;
	case PIN_CONFIG_BIAS_DISABLE:
		*mask = MAX96717_GPIO_B_PULL_UPDN_SEL;
		*val = MAX96717_GPIO_B_PULL_UPDN_SEL_NONE;
		return 0;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		*mask = MAX96717_GPIO_B_PULL_UPDN_SEL;
		*val = MAX96717_GPIO_B_PULL_UPDN_SEL_PD;
		return 0;
	case PIN_CONFIG_BIAS_PULL_UP:
		*mask = MAX96717_GPIO_B_PULL_UPDN_SEL;
		*val = MAX96717_GPIO_B_PULL_UPDN_SEL_PU;
		return 0;
	}

	switch(param) {
	case PIN_CONFIG_SLEW_RATE:
		if (offset < 3) {
			*reg = MAX96717_PIO_SLEW_0;
			if (offset == 0)
				*mask = MAX96717_PIO_SLEW_0_PIO00_SLEW;
			else if (offset == 1)
				*mask = MAX96717_PIO_SLEW_0_PIO01_SLEW;
			else
				*mask = MAX96717_PIO_SLEW_0_PIO02_SLEW;
		} else if (offset < 5) {
			*reg = MAX96717_PIO_SLEW_1;
			if (offset == 3)
				*mask = MAX96717_PIO_SLEW_1_PIO05_SLEW;
			else
				*mask = MAX96717_PIO_SLEW_1_PIO06_SLEW;
		} else if (offset < 7) {
			return -EINVAL;
		} else if (offset < 9) {
			*reg  = MAX96717_PIO_SLEW_2;
			if (offset == 7)
				*mask = MAX96717_PIO_SLEW_2_PIO010_SLEW;
			else
				*mask = MAX96717_PIO_SLEW_2_PIO011_SLEW;
		} else {
			return -EINVAL;
		}
		return 0;
	case MAX96717_PINCTRL_GMSL_RX_ID:
		*reg = MAX96717_GPIO_C(offset);
		*mask = MAX96717_GPIO_C_GPIO_RX_ID;
		return 0;
	case MAX96717_PINCTRL_RCLKOUT_CLK:
		*reg = MAX96717_REG3;
		*mask = MAX96717_REG3_RCLKSEL;
		return 0;
	default:
		return -ENOTSUPP;
	}
}

static int max96717_conf_pin_config_get(struct pinctrl_dev *pctldev,
					unsigned int offset,
					unsigned long *config)
{
	struct max96717_priv *priv = pinctrl_dev_get_drvdata(pctldev);
	u32 param = pinconf_to_config_param(*config);
	unsigned int reg, mask, val;
	int ret;

	ret = max96717_get_pin_config_reg(offset, param, &reg, &mask, &val);
	if (ret)
		return ret;

	switch (param) {
	case PIN_CONFIG_DRIVE_OPEN_DRAIN:
	case PIN_CONFIG_DRIVE_PUSH_PULL:
	case PIN_CONFIG_BIAS_DISABLE:
	case PIN_CONFIG_BIAS_PULL_DOWN:
	case PIN_CONFIG_BIAS_PULL_UP:
		ret = max96717_read(priv, reg);
		if (ret < 0)
			return ret;

		val = field_get(mask, ret) == val;
		if (!val)
			return -EINVAL;

		break;
	case MAX96717_PINCTRL_JITTER_COMPENSATION_EN:
	case MAX96717_PINCTRL_PULL_STRENGTH_WEAK:
	case MAX96717_PINCTRL_GMSL_TX_EN:
	case MAX96717_PINCTRL_GMSL_RX_EN:
	case MAX96717_PINCTRL_INPUT_VALUE:
	case PIN_CONFIG_OUTPUT_ENABLE:
	case PIN_CONFIG_INPUT_ENABLE:
	case PIN_CONFIG_OUTPUT:
		ret = max96717_read(priv, reg);
		if (ret < 0)
			return ret;

		val = field_get(mask, ret) == val;
		break;
	case MAX96717_PINCTRL_GMSL_TX_ID:
	case MAX96717_PINCTRL_GMSL_RX_ID:
	case PIN_CONFIG_SLEW_RATE:
	case MAX96717_PINCTRL_RCLKOUT_CLK:
		ret = max96717_read(priv, reg);
		if (ret < 0)
			return ret;

		val = field_get(mask, val);
		break;
	default:
		return -ENOTSUPP;
	}

	*config = pinconf_to_config_packed(param, val);

	return 0;
}

static int max96717_conf_pin_config_set_one(struct max96717_priv *priv,
					    unsigned int offset,
					    unsigned long config)
{
	u32 param = pinconf_to_config_param(config);
	u32 arg = pinconf_to_config_argument(config);
	unsigned int reg, mask, val;
	int ret;

	ret = max96717_get_pin_config_reg(offset, param, &reg, &mask, &val);
	if (ret)
		return ret;

	switch (param) {
	case PIN_CONFIG_DRIVE_OPEN_DRAIN:
	case PIN_CONFIG_DRIVE_PUSH_PULL:
	case PIN_CONFIG_BIAS_DISABLE:
	case PIN_CONFIG_BIAS_PULL_DOWN:
	case PIN_CONFIG_BIAS_PULL_UP:
		val = field_prep(mask, val);

		ret = max96717_update_bits(priv, reg, mask, val);
		break;
	case MAX96717_PINCTRL_JITTER_COMPENSATION_EN:
	case MAX96717_PINCTRL_PULL_STRENGTH_WEAK:
	case MAX96717_PINCTRL_GMSL_TX_EN:
	case MAX96717_PINCTRL_GMSL_RX_EN:
	case PIN_CONFIG_OUTPUT_ENABLE:
	case PIN_CONFIG_INPUT_ENABLE:
	case PIN_CONFIG_OUTPUT:
		val = field_prep(mask, arg ? val : ~val);

		ret = max96717_update_bits(priv, reg, mask, val);
		break;
	case MAX96717_PINCTRL_GMSL_TX_ID:
	case MAX96717_PINCTRL_GMSL_RX_ID:
	case PIN_CONFIG_SLEW_RATE:
	case MAX96717_PINCTRL_RCLKOUT_CLK:
		val = field_prep(mask, arg);

		ret = max96717_update_bits(priv, reg, mask, val);
		break;
	default:
		return -ENOTSUPP;
	}

	if (ret)
		return ret;

	switch (param) {
	case PIN_CONFIG_OUTPUT:
		config = pinconf_to_config_packed(PIN_CONFIG_OUTPUT_ENABLE, 1);
		return max96717_conf_pin_config_set_one(priv, offset, config);
	case PIN_CONFIG_OUTPUT_ENABLE:
		config = pinconf_to_config_packed(MAX96717_PINCTRL_GMSL_RX_EN, 0);
		return max96717_conf_pin_config_set_one(priv, offset, config);
	default:
		break;
	}

	return 0;
}

static int max96717_conf_pin_config_set(struct pinctrl_dev *pctldev,
					unsigned int offset,
					unsigned long *configs,
					unsigned int num_configs)
{
	struct max96717_priv *priv = pinctrl_dev_get_drvdata(pctldev);
	int ret;

	while (num_configs--) {
		unsigned long config = *configs;

		ret = max96717_conf_pin_config_set_one(priv, offset, config);
		if (ret)
			return ret;

		configs++;
	}

	return 0;
}

static int max96717_mux_get_functions_count(struct pinctrl_dev *pctldev)
{
	return ARRAY_SIZE(max96717_functions);
}

static const char *max96717_mux_get_function_name(struct pinctrl_dev *pctldev,
						  unsigned selector)
{
	return max96717_functions[selector].name;
}

static int max96717_mux_get_groups(struct pinctrl_dev *pctldev,
				   unsigned selector,
				   const char * const **groups,
				   unsigned * const num_groups)
{
	*groups = max96717_functions[selector].groups;
	*num_groups = max96717_functions[selector].ngroups;

	return 0;
}

static int max96717_mux_set_pclk(struct max96717_priv *priv, unsigned int group)
{
	int ret;

	ret = max96717_update_bits(priv, MAX96717_REF_VTG0,
				   MAX96717_REF_VTG0_PCLK_EN,
				   MAX96717_REF_VTG0_PCLK_EN);
	if (ret)
		return ret;

	ret = max96717_update_bits(priv, MAX96717_REF_VTG0,
				   MAX96717_REF_VTG0_PCLK_GPIO,
				   FIELD_PREP(MAX96717_REF_VTG0_PCLK_GPIO, group));
	if (ret)
		return ret;

	return 0;
}

static int max96717_mux_set_rclkout(struct max96717_priv *priv, unsigned int group)
{
	int ret;

	/* Enable RCLK. */
	ret = max96717_update_bits(priv, MAX96717_REG6,
				   MAX96717_REG6_RCLKEN,
				   MAX96717_REG6_RCLKEN);
	if (ret)
		return ret;

	/* Enable RCLK output on PCLK. */
	ret = max96717_update_bits(priv, MAX96717_REF_VTG0,
				   MAX96717_REF_VTG0_RCLKEN_Y,
				   MAX96717_REF_VTG0_RCLKEN_Y);
	if (ret)
		return ret;

	return 0;
}

static int max96717_mux_set(struct pinctrl_dev *pctldev, unsigned selector,
			    unsigned group)
{
	struct max96717_priv *priv = pinctrl_dev_get_drvdata(pctldev);
	int ret;

	switch (selector) {
	case max96717_func_pclk:
		return max96717_mux_set_pclk(priv, group);
	case max96717_func_rclkout:
		ret = max96717_mux_set_pclk(priv, group);
		if (ret)
			return ret;

		return max96717_mux_set_rclkout(priv, group);
	}

	return 0;
}
static int max96717_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	unsigned long config = pinconf_to_config_packed(PIN_CONFIG_OUTPUT_ENABLE, 0);
	struct max96717_priv *priv = gpiochip_get_data(gc);
	int ret;

	ret = max96717_conf_pin_config_get(priv->pctldev, offset, &config);
	if (ret)
		return ret;

	return pinconf_to_config_argument(config) ? GPIO_LINE_DIRECTION_OUT
						  : GPIO_LINE_DIRECTION_IN;
}

static int max96717_gpio_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	unsigned long config = pinconf_to_config_packed(PIN_CONFIG_INPUT_ENABLE, 1);
	struct max96717_priv *priv = gpiochip_get_data(gc);

	return max96717_conf_pin_config_set_one(priv, offset, config);
}

static int max96717_gpio_direction_output(struct gpio_chip *gc, unsigned int offset,
					  int value)
{
	unsigned long config = pinconf_to_config_packed(PIN_CONFIG_OUTPUT, value);
	struct max96717_priv *priv = gpiochip_get_data(gc);

	return max96717_conf_pin_config_set_one(priv, offset, config);
}

static int max96717_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	unsigned long config = pinconf_to_config_packed(MAX96717_PINCTRL_INPUT_VALUE, 0);
	struct max96717_priv *priv = gpiochip_get_data(gc);
	int ret;

	ret = max96717_conf_pin_config_get(priv->pctldev, offset, &config);
	if (ret)
		return ret;

	return pinconf_to_config_argument(config);
}

static void max96717_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	unsigned long config = pinconf_to_config_packed(PIN_CONFIG_OUTPUT, value);
	struct max96717_priv *priv = gpiochip_get_data(gc);
	int ret;

	ret = max96717_conf_pin_config_set_one(priv, offset, config);
	if (ret)
		dev_err(priv->dev, "Failed to set GPIO %u output value, err: %d\n",
			offset, ret);
}

static unsigned int max96717_pipe_id(struct max96717_priv *priv,
				     struct max_ser_pipe *pipe)
{
	return priv->info->pipe_hw_ids[pipe->index];
}

static unsigned int max96717_phy_id(struct max96717_priv *priv,
				    struct max_ser_phy *phy)
{
	return priv->info->phy_hw_ids[phy->index];
}

static int max96717_set_pipe_enable(struct max_ser *ser,
				    struct max_ser_pipe *pipe, bool enable)
{
	struct max96717_priv *priv = ser_to_priv(ser);
	unsigned int index = max96717_pipe_id(priv, pipe);
	unsigned int mask = MAX96717_REG2_VID_TX_EN_P(index);

	return max96717_update_bits(priv, MAX96717_REG2, mask, enable ? mask : 0);
}

static int max96717_reg_read(struct max_ser *ser, unsigned int reg,
			     unsigned int *val)
{
	struct max96717_priv *priv = ser_to_priv(ser);

	return regmap_read(priv->regmap, reg, val);
}

static int max96717_reg_write(struct max_ser *ser, unsigned int reg,
			      unsigned int val)
{
	struct max96717_priv *priv = ser_to_priv(ser);

	return regmap_write(priv->regmap, reg, val);
}

static int max96717_set_pipe_dt_en(struct max_ser *ser, struct max_ser_pipe *pipe,
				   unsigned int i, bool enable)
{
	struct max96717_priv *priv = ser_to_priv(ser);
	unsigned int index = max96717_pipe_id(priv, pipe);
	unsigned int reg;

	if (i < 2)
		reg = MAX96717_FRONTTOP_12(index, i);
	else
		/*
		 * DT 7 and 8 are only supported on MAX96717, no need for pipe
		 * index to be taken into account.
		 */
		reg = MAX96717_EXTA(i - 2);

	return max96717_update_bits(priv, reg, MAX96717_MEM_DT_EN,
				    enable ? MAX96717_MEM_DT_EN : 0);
}

static int max96717_set_pipe_dt(struct max_ser *ser, struct max_ser_pipe *pipe,
				unsigned int i, unsigned int dt)
{
	struct max96717_priv *priv = ser_to_priv(ser);
	unsigned int index = max96717_pipe_id(priv, pipe);
	unsigned int reg;

	if (i < 2)
		reg = MAX96717_FRONTTOP_12(index,  i);
	else
		reg = MAX96717_EXTA(i - 2);

	return max96717_update_bits(priv, reg, MAX96717_MEM_DT_SEL,
				    FIELD_PREP(MAX96717_MEM_DT_SEL, dt));
}

static int max96717_set_pipe_vcs(struct max_ser *ser,
				 struct max_ser_pipe *pipe,
				 unsigned int vcs)
{
	struct max96717_priv *priv = ser_to_priv(ser);
	unsigned int index = max96717_pipe_id(priv, pipe);
	int ret;

	ret = max96717_write(priv, MAX96717_FRONTTOP_1(index),
			     (vcs >> 0) & 0xff);
	if (ret)
		return ret;

	return max96717_write(priv, MAX96717_FRONTTOP_2(index),
			      (vcs >> 8) & 0xff);
}

static int max96717_log_status(struct max_ser *ser, const char *name)
{
	struct max96717_priv *priv = ser_to_priv(ser);
	int ret;

	if (!priv->info->supports_tunnel_mode)
		return 0;

	ret = max96717_read(priv, MAX96717_EXT23);
	if (ret < 0)
		return ret;

	pr_info("%s: \t\ttun_pkt_cnt: %u\n", name, ret);

	return 0;
}

static int max96717_log_pipe_status(struct max_ser *ser,
				    struct max_ser_pipe *pipe,
				    const char *name)
{
	struct max96717_priv *priv = ser_to_priv(ser);
	unsigned int index = max96717_pipe_id(priv, pipe);
	int ret;

	ret = max96717_read(priv, MAX96717_VIDEO_TX2(index));
	if (ret < 0)
		return ret;

	pr_info("%s: \tpclkdet: %u\n", name, !!(ret & MAX96717_VIDEO_TX2CLKDET));

	return 0;
}

static int max96717_log_phy_status(struct max_ser *ser,
				   struct max_ser_phy *phy,
				   const char *name)
{
	struct max96717_priv *priv = ser_to_priv(ser);
	int ret;

	if (!priv->info->supports_pkt_cnt)
		return 0;

	ret = max96717_read(priv, MAX96717_EXT21);
	if (ret < 0)
		return ret;

	pr_info("%s: \tphy_pkt_cnt: %u\n", name, ret);

	ret = max96717_read(priv, MAX96717_EXT22);
	if (ret < 0)
		return ret;

	pr_info("%s: \tcsi_pkt_cnt: %u\n", name, ret);

	ret = max96717_read(priv, MAX96717_EXT24);
	if (ret < 0)
		return ret;

	pr_info("%s: \tphy_clk_cnt: %u\n", name, ret);

	return 0;
}

static int max96717_init_phy(struct max_ser *ser,
			     struct max_ser_phy *phy)
{
	struct max96717_priv *priv = ser_to_priv(ser);
	unsigned int num_data_lanes = phy->mipi.num_data_lanes;
	unsigned int used_data_lanes = 0;
	unsigned int val;
	unsigned int i;
	int ret;

	/* Configure a lane count. */
	ret = max96717_update_bits(priv, MAX96717_MIPI_RX1,
				   MAX96717_MIPI_RX1_CTRL_NUM_LANES,
				   FIELD_PREP(MAX96717_MIPI_RX1_CTRL_NUM_LANES,
					      num_data_lanes - 1));
	if (ret)
		return ret;

	/* Configure lane mapping. */
	val = 0;
	for (i = 0; i < 4; i++) {
		unsigned int map;

		if (i < num_data_lanes)
			map = phy->mipi.data_lanes[i] - 1;
		else
			map = ffz(used_data_lanes);

		val |= map << (i * 2);
		used_data_lanes |= BIT(map);
	}

	ret = max96717_update_bits(priv, MAX96717_MIPI_RX3,
				   MAX96717_MIPI_RX3_PHY2_LANE_MAP,
				   FIELD_PREP(MAX96717_MIPI_RX3_PHY2_LANE_MAP, val));
	if (ret)
		return ret;

	ret = max96717_update_bits(priv, MAX96717_MIPI_RX2,
				   MAX96717_MIPI_RX2_PHY1_LANE_MAP,
				   FIELD_PREP(MAX96717_MIPI_RX2_PHY1_LANE_MAP, val >> 4));
	if (ret)
		return ret;

	/* Configure lane polarity. */
	/* Lower two lanes. */
	val = 0;
	for (i = 0; i < 3 && i < num_data_lanes + 1; i++)
		if (phy->mipi.lane_polarities[i])
			val |= i == 0 ? MAX96717_MIPI_RX5_PHY2_POL_MAP_CLK
				      : MAX96717_MIPI_RX5_PHY2_POL_MAP_LANE(i - 1);

	ret = max96717_update_bits(priv, MAX96717_MIPI_RX5,
				   MAX96717_MIPI_RX5_PHY2_POL_MAP, val);
	if (ret)
		return ret;

	/* Upper two lanes. */
	val = 0;
	for (i = 3; i < num_data_lanes + 1; i++)
		if (phy->mipi.lane_polarities[i])
			val |= MAX96717_MIPI_RX4_PHY1_POL_MAP_LANE(i - 3);

	ret = max96717_update_bits(priv, MAX96717_MIPI_RX4,
				   MAX96717_MIPI_RX4_PHY1_POL_MAP, val);
	if (ret)
		return ret;

	if (priv->info->supports_noncontinuous_clock) {
		val = !!(phy->mipi.flags & V4L2_MBUS_CSI2_NONCONTINUOUS_CLOCK);

		ret = max96717_update_bits(priv, MAX96717_MIPI_RX0,
					   MAX96717_MIPI_RX0_NONCONTCLK_EN,
					   FIELD_PREP(MAX96717_MIPI_RX0_NONCONTCLK_EN, val));
		if (ret)
			return ret;
	}

	return 0;
}

static int max96717_set_phy_active(struct max_ser *ser, struct max_ser_phy *phy,
				   bool enable)
{
	struct max96717_priv *priv = ser_to_priv(ser);
	unsigned int index = max96717_phy_id(priv, phy);
	unsigned int mask = MAX96717_FRONTTOP_0_START_PORT(index);

	return max96717_update_bits(priv, MAX96717_FRONTTOP_0,
				    mask, enable ? mask : 0);
}

static int max96717_set_pipe_stream_id(struct max_ser *ser,
				       struct max_ser_pipe *pipe,
				       unsigned int stream_id)
{
	struct max96717_priv *priv = ser_to_priv(ser);
	unsigned int index = max96717_pipe_id(priv, pipe);

	return max96717_update_bits(priv, MAX96717_TX3(index),
				    MAX96717_TX3_TX_STR_SEL,
				    FIELD_PREP(MAX96717_TX3_TX_STR_SEL,
					       stream_id));
}

static int max96717_set_pipe_phy(struct max_ser *ser,
				 struct max_ser_pipe *pipe,
				 struct max_ser_phy *phy)
{
	struct max96717_priv *priv = ser_to_priv(ser);
	unsigned int index = max96717_pipe_id(priv, pipe);
	unsigned int phy_id = max96717_phy_id(priv, phy);
	unsigned int mask, val;
	int ret;

	val = phy_id == 1 ? MAX96717_FRONTTOP_0_CLK_SEL_P(index) : 0;
	ret = max96717_update_bits(priv, MAX96717_FRONTTOP_0,
				   MAX96717_FRONTTOP_0_CLK_SEL_P(index), val);
	if (ret)
		return ret;

	mask = MAX96717_FRONTTOP_9_START_PORT(index, 0) |
	       MAX96717_FRONTTOP_9_START_PORT(index, 1);
	val = MAX96717_FRONTTOP_9_START_PORT(index, phy_id);

	return max96717_update_bits(priv, MAX96717_FRONTTOP_9, mask, val);
}

static int max96717_init_pipe(struct max_ser *ser,
			      struct max_ser_pipe *pipe)
{
	struct max96717_priv *priv = ser_to_priv(ser);
	unsigned int index = max96717_pipe_id(priv, pipe);
	unsigned int mask;
	int ret;

	mask = MAX96717_FRONTTOP_10_BPP8DBL(index);
	ret = max96717_update_bits(priv, MAX96717_FRONTTOP_10,
				   mask, pipe->dbl8 ? mask : 0);
	if (ret)
		return ret;

	mask = MAX96717_FRONTTOP_11_BPP10DBL(index);
	ret = max96717_update_bits(priv, MAX96717_FRONTTOP_11,
				   mask, pipe->dbl10 ? mask : 0);
	if (ret)
		return ret;

	mask = MAX96717_FRONTTOP_11_BPP12DBL(index);
	ret = max96717_update_bits(priv, MAX96717_FRONTTOP_11,
				   mask, pipe->dbl12 ? mask : 0);
	if (ret)
		return ret;

	ret = max96717_update_bits(priv, MAX96717_FRONTTOP_20(index),
				   MAX96717_FRONTTOP_20_SOFT_BPP_EN,
				   FIELD_PREP(MAX96717_FRONTTOP_20_SOFT_BPP_EN,
					      !!pipe->soft_bpp));
	if (ret)
		return ret;

	ret = max96717_update_bits(priv, MAX96717_FRONTTOP_20(index),
				   MAX96717_FRONTTOP_20_SOFT_BPP,
				   FIELD_PREP(MAX96717_FRONTTOP_20_SOFT_BPP,
					      pipe->soft_bpp));
	if (ret)
		return ret;

	ret = max96717_update_bits(priv, MAX96717_VIDEO_TX0(index),
				   MAX96717_VIDEO_TX0_AUTO_BPP,
				   FIELD_PREP(MAX96717_VIDEO_TX0_AUTO_BPP,
					      !pipe->bpp));
	if (ret)
		return ret;

	ret = max96717_update_bits(priv, MAX96717_VIDEO_TX1(index),
				   MAX96717_VIDEO_TX1_BPP,
				   FIELD_PREP(MAX96717_VIDEO_TX1_BPP,
					      pipe->bpp));
	if (ret)
		return ret;

	return 0;
}

static int max96717_init_i2c_xlate(struct max_ser *ser)
{
	struct max96717_priv *priv = ser_to_priv(ser);
	unsigned int i;
	int ret;

	for (i = 0; i < ser->ops->num_i2c_xlates; i++) {
		u8 src = 0, dst = 0;

		if (i < ser->num_i2c_xlates) {
			src = ser->i2c_xlates[i].src;
			dst = ser->i2c_xlates[i].dst;
		}

		ret = max96717_update_bits(priv, MAX96717_I2C_2(i),
					   MAX96717_I2C_2_SRC,
					   FIELD_PREP(MAX96717_I2C_2_SRC, src));
		if (ret)
			return ret;

		ret = max96717_update_bits(priv, MAX96717_I2C_3(i),
					   MAX96717_I2C_3_DST,
					   FIELD_PREP(MAX96717_I2C_3_DST, dst));
		if (ret)
			return ret;
	}

	return 0;
}

static const unsigned int max96717_phys_configs_reg_val[] = {
	0b000,
	0b000,
};

static const struct max_phys_config max96717_phys_configs[] = {
	{ { 4 } },
	{ { 2 } },
};

static int max96717_init(struct max_ser *ser)
{
	struct max96717_priv *priv = ser_to_priv(ser);
	int ret;

	/*
	 * Set CMU2 PFDDIV to 1.1V for correct functionality of the device,
	 * as mentioned in the datasheet, under section MANDATORY REGISTER PROGRAMMING.
	 */
	ret = max96717_update_bits(priv, MAX96717_CMU2,
				   MAX96717_CMU2_PFDDIV_RSHORT,
				   FIELD_PREP(MAX96717_CMU2_PFDDIV_RSHORT,
					      MAX96717_CMU2_PFDDIV_RSHORT_1_1V));
	if (ret)
		return ret;

	if (priv->info->supports_tunnel_mode) {
		ret = max96717_update_bits(priv, MAX96717_EXT11,
					   MAX96717_EXT11_TUN_MODE,
					   FIELD_PREP(MAX96717_EXT11_TUN_MODE, 0));
		if (ret)
			return ret;
	}

	/* Set PHY mode. */
	if (ser->phys_config >= ARRAY_SIZE(max96717_phys_configs_reg_val))
		return -EINVAL;

	ret = max96717_update_bits(priv, MAX96717_MIPI_RX0,
				   MAX96717_MIPI_RX0_PHY_CONFIG,
				   FIELD_PREP(MAX96717_MIPI_RX0_PHY_CONFIG,
					      max96717_phys_configs_reg_val[ser->phys_config]));
	if (ret)
		return ret;

	return 0;
}

static int max96717_post_init(struct max_ser *ser)
{
	msleep(100);

	return 0;
}

static struct pinctrl_ops max96717_ctrl_ops = {
	.get_groups_count = max96717_ctrl_get_groups_count,
	.get_group_name = max96717_ctrl_get_group_name,
	.get_group_pins = max96717_ctrl_get_group_pins,
	.dt_node_to_map = pinconf_generic_dt_node_to_map_pin,
	.dt_free_map = pinconf_generic_dt_free_map,
};

static const struct pinconf_ops max96717_conf_ops = {
	.pin_config_get = max96717_conf_pin_config_get,
	.pin_config_set = max96717_conf_pin_config_set,
	.is_generic = true,
};

static const struct pinmux_ops max96717_mux_ops = {
	.get_functions_count = max96717_mux_get_functions_count,
	.get_function_name = max96717_mux_get_function_name,
	.get_function_groups = max96717_mux_get_groups,
	.set_mux = max96717_mux_set,
};

static const struct max_ser_ops max96717_ops = {
	.num_i2c_xlates = 2,
	.phys_configs = {
		.num_configs = ARRAY_SIZE(max96717_phys_configs),
		.configs = max96717_phys_configs,
	},
	.reg_read = max96717_reg_read,
	.reg_write = max96717_reg_write,
	.log_status = max96717_log_status,
	.log_pipe_status = max96717_log_pipe_status,
	.log_phy_status = max96717_log_phy_status,
	.init = max96717_init,
	.init_i2c_xlate = max96717_init_i2c_xlate,
	.init_phy = max96717_init_phy,
	.set_phy_active = max96717_set_phy_active,
	.init_pipe = max96717_init_pipe,
	.set_pipe_enable = max96717_set_pipe_enable,
	.set_pipe_dt = max96717_set_pipe_dt,
	.set_pipe_dt_en = max96717_set_pipe_dt_en,
	.set_pipe_vcs = max96717_set_pipe_vcs,
	.set_pipe_stream_id = max96717_set_pipe_stream_id,
	.set_pipe_phy = max96717_set_pipe_phy,
	.post_init = max96717_post_init,
};

static int max96717_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct max96717_priv *priv;
	struct max_ser_ops *ops;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	ops = devm_kzalloc(dev, sizeof(*ops), GFP_KERNEL);
	if (!ops)
		return -ENOMEM;

	priv->info = device_get_match_data(dev);
	if (!priv->info) {
		dev_err(dev, "Failed to get match data\n");
		return -ENODEV;
	}

	priv->dev = dev;
	priv->client = client;
	i2c_set_clientdata(client, priv);

	priv->regmap = devm_regmap_init_i2c(client, &max_ser_i2c_regmap);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	*ops = max96717_ops;

	ops->supports_noncontinuous_clock = priv->info->supports_noncontinuous_clock;
	ops->num_pipes = priv->info->num_pipes;
	ops->num_dts_per_pipe = priv->info->num_dts_per_pipe;
	ops->num_phys = priv->info->num_phys;
	priv->ser.ops = ops;

	ret = max96717_wait_for_device(priv);
	if (ret)
		return ret;

	priv->pctldesc = (struct pinctrl_desc) {
		.owner = THIS_MODULE,
		.name = MAX96717_PINCTRL_NAME,
		.pins = max96717_pins,
		.npins = ARRAY_SIZE(max96717_pins),
		.pctlops = &max96717_ctrl_ops,
		.confops = &max96717_conf_ops,
		.pmxops = &max96717_mux_ops,
		.custom_params = max96717_cfg_params,
		.num_custom_params = ARRAY_SIZE(max96717_cfg_params),
	};

	ret = devm_pinctrl_register_and_init(dev, &priv->pctldesc, priv, &priv->pctldev);
	if (ret)
		return ret;

	ret = pinctrl_enable(priv->pctldev);
	if (ret)
		return ret;

	priv->gc = (struct gpio_chip) {
		.owner = THIS_MODULE,
		.label = MAX96717_GPIOCHIP_NAME,
		.base = -1,
		.ngpio = MAX96717_GPIO_NUM,
		.parent = dev,
		.can_sleep = true,
		.request = gpiochip_generic_request,
		.free = gpiochip_generic_free,
		.set_config = gpiochip_generic_config,
		.get_direction = max96717_gpio_get_direction,
		.direction_input = max96717_gpio_direction_input,
		.direction_output = max96717_gpio_direction_output,
		.get = max96717_gpio_get,
		.set = max96717_gpio_set,
	};

	ret = devm_gpiochip_add_data(dev, &priv->gc, priv);
	if (ret)
		return ret;

	return max_ser_probe(client, &priv->ser);
}

static void max96717_remove(struct i2c_client *client)
{
	struct max96717_priv *priv = i2c_get_clientdata(client);

	max_ser_remove(&priv->ser);
}

static const struct max96717_chip_info max96717_info = {
	.supports_pkt_cnt = true,
	.supports_tunnel_mode = true,
	.supports_noncontinuous_clock = true,
	.num_pipes = 1,
	.num_dts_per_pipe = 4,
	.pipe_hw_ids = { 2 },
	.num_phys = 1,
	.phy_hw_ids = { 1 },
};

static const struct max96717_chip_info max9295a_info = {
	.num_pipes = 4,
	.num_dts_per_pipe = 2,
	.pipe_hw_ids = { 0, 1, 2, 3 },
	.num_phys = 1,
	.phy_hw_ids = { 1 },
};

static const struct of_device_id max96717_of_ids[] = {
	{ .compatible = "maxim,max96717", .data = &max96717_info },
	{ .compatible = "maxim,max9295a", .data = &max9295a_info },
	{ }
};
MODULE_DEVICE_TABLE(of, max96717_of_ids);

static struct i2c_driver max96717_i2c_driver = {
	.driver	= {
		.name = MAX96717_NAME,
		.of_match_table = max96717_of_ids,
	},
	.probe = max96717_probe,
	.remove = max96717_remove,
};

module_i2c_driver(max96717_i2c_driver);

MODULE_DESCRIPTION("MAX96717 GMSL2 Serializer Driver");
MODULE_AUTHOR("Cosmin Tanislav <cosmin.tanislav@analog.com>");
MODULE_LICENSE("GPL");
