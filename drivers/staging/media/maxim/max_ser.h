// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include <linux/i2c.h>
#include <linux/regmap.h>

extern const struct regmap_config max_ser_i2c_regmap;

int max_ser_reset(struct regmap *regmap);

int max_ser_wait(struct i2c_client *client, struct regmap *regmap, u8 addr);
int max_ser_wait_for_multiple(struct i2c_client *client, struct regmap *regmap,
			      u8 *addrs, unsigned int num_addrs);

int max_ser_change_address(struct regmap *regmap, u8 addr);
