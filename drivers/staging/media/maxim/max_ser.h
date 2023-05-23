// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include <linux/i2c.h>
#include <linux/regmap.h>

#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include "max_serdes.h"

#ifndef MAX_SER_H
#define MAX_SER_H

#define MAX_SER_SOURCE_PAD	0
#define MAX_SER_SINK_PAD	1
#define MAX_SER_PAD_NUM		2

#define MAX_SER_SUBDEVS_NUM	2

#define MAX_SER_DT_EMB8				0x12
#define MAX_SER_DT_YUV422_8B			0x1e
#define MAX_SER_DT_YUV422_10B			0x1f
#define MAX_SER_DT_RGB565			0x22
#define MAX_SER_DT_RGB666			0x23
#define MAX_SER_DT_RGB888			0x24
#define MAX_SER_DT_RAW8				0x2a
#define MAX_SER_DT_RAW10			0x2b
#define MAX_SER_DT_RAW12			0x2c
#define MAX_SER_DT_RAW14			0x2d
#define MAX_SER_DT_RAW16			0x2e
#define MAX_SER_DT_RAW20			0x2f

extern const struct regmap_config max_ser_i2c_regmap;

struct max_ser_asd {
	struct v4l2_async_subdev base;
	struct max_ser_subdev_priv *sd_priv;
};

struct max_ser_format {
	u32 code;
	u32 dt;
};

struct max_ser_subdev_priv {
	struct v4l2_subdev sd;
	unsigned int index;
	struct fwnode_handle *fwnode;

	struct max_ser_priv *priv;

	struct v4l2_subdev *slave_sd;
	struct fwnode_handle *slave_fwnode;
	struct v4l2_subdev_state *slave_sd_state;
	unsigned int slave_sd_pad_id;

	struct v4l2_async_notifier notifier;
	struct media_pad pads[MAX_SER_PAD_NUM];

	struct v4l2_fwnode_bus_mipi_csi2 mipi;
	unsigned int stream_id;
};

struct max_ser_ops {
	int (*mipi_enable)(struct max_ser_priv *priv, bool enable);
	int (*set_dt)(struct max_ser_priv *priv, u32 code);
	int (*init)(struct max_ser_priv *priv);
	int (*set_tunnel_mode)(struct max_ser_priv *priv);
	int (*init_ch)(struct max_ser_priv *priv, struct max_ser_subdev_priv *sd_priv);
	int (*post_init)(struct max_ser_priv *priv);
};

struct max_ser_priv {
	const struct max_ser_ops *ops;

	struct device *dev;
	struct i2c_client *client;

	unsigned int lane_config;
	bool tunnel_mode;

	struct max_ser_subdev_priv sd_privs[MAX_SER_SUBDEVS_NUM];
};

int max_ser_probe(struct max_ser_priv *priv);

int max_ser_remove(struct max_ser_priv *priv);

const struct max_ser_format *max_ser_format_by_code(u32 code);

int max_ser_reset(struct regmap *regmap);

int max_ser_wait(struct i2c_client *client, struct regmap *regmap, u8 addr);
int max_ser_wait_for_multiple(struct i2c_client *client, struct regmap *regmap,
			      u8 *addrs, unsigned int num_addrs);

int max_ser_change_address(struct regmap *regmap, u8 addr);

int max_ser_init_i2c_xlate(struct regmap *regmap, unsigned int i,
			   struct max_i2c_xlate *i2c_xlate);

#endif // MAX_SER_H
