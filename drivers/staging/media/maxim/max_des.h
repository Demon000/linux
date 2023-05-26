// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include <linux/i2c-mux.h>

#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include "max_serdes.h"

#ifndef MAX_DES_H
#define MAX_DES_H

#define MAX_DES_REMAPS_NUM		16
#define MAX_DES_I2C_XLATES_NUM		2

#define MAX_DES_SOURCE_PAD		0
#define MAX_DES_SINK_PAD		1
#define MAX_DES_PAD_NUM			2

extern const struct regmap_config max_des_i2c_regmap;

struct max_des_asd {
	struct v4l2_async_subdev base;
	struct max_des_subdev_priv *sd_priv;
};

#define MAX_DES_DT_VC(dt, vc) (((vc) & 0x3) << 6 | ((dt) & 0x3f))

struct max_des_dt_vc_remap {
	u8 from_dt;
	u8 from_vc;
	u8 to_dt;
	u8 to_vc;
	u8 phy;
};

struct max_des_subdev_priv {
	struct v4l2_subdev sd;
	unsigned int index;
	struct fwnode_handle *fwnode;

	struct max_des_priv *priv;

	struct v4l2_subdev *slave_sd;
	struct fwnode_handle *slave_fwnode;
	struct v4l2_subdev_state *slave_sd_state;
	unsigned int slave_sd_pad_id;

	struct v4l2_async_notifier notifier;
	struct media_pad pads[MAX_DES_PAD_NUM];

	bool active;
	unsigned int pipe_id;
	struct max_des_dt_vc_remap remaps[MAX_DES_REMAPS_NUM];
	unsigned int num_remaps;
};

struct max_des_link {
	unsigned int index;
	bool enabled;
	struct max_i2c_xlate ser_xlate;
	bool ser_xlate_enabled;
	struct max_i2c_xlate i2c_xlates[MAX_DES_I2C_XLATES_NUM];
	unsigned int num_i2c_xlates;
};

struct max_des_pipe {
	unsigned int index;
	unsigned int phy_id;
	unsigned int stream_id;
	unsigned int link_id;
	struct max_des_dt_vc_remap remaps[MAX_DES_REMAPS_NUM];
	unsigned int num_remaps;
	bool enabled;
};

struct max_des_phy {
	unsigned int index;
	struct v4l2_fwnode_bus_mipi_csi2 mipi;
	bool enabled;
};

struct max_des_ops {
	unsigned int num_phys;
	unsigned int num_pipes;
	unsigned int num_links;

	int (*mux_select)(struct max_des_priv *priv, unsigned int link);
	int (*mipi_enable)(struct max_des_priv *priv, bool enable);
	int (*init)(struct max_des_priv *priv);
	int (*init_phy)(struct max_des_priv *priv, struct max_des_phy *phy);
	int (*init_pipe)(struct max_des_priv *priv, struct max_des_pipe *pipe);
	int (*disable_links)(struct max_des_priv *priv);
	int (*enable_link)(struct max_des_priv *priv, struct max_des_link *link);
	int (*post_init)(struct max_des_priv *priv);
};

struct max_des_priv {
	const struct max_des_ops *ops;

	struct device *dev;
	struct i2c_client *client;

	struct i2c_mux_core *mux;
	int mux_channel;

	unsigned int num_subdevs;
	unsigned int lane_config;
	struct mutex lock;
	bool active;

	struct max_des_phy *phys;
	struct max_des_pipe *pipes;
	struct max_des_link *links;
	struct max_des_subdev_priv *sd_privs;
};

int max_des_probe(struct max_des_priv *priv);

int max_des_remove(struct max_des_priv *priv);

#endif // MAX_DES_H
