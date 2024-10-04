// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include <linux/i2c.h>
#include <linux/i2c-atr.h>
#include <linux/regmap.h>

#include <media/v4l2-mediabus.h>

#include "max_serdes.h"

#ifndef MAX_SER_H
#define MAX_SER_H

#define MAX_SER_MAX96717_DEV_ID			0xbf
#define MAX_SER_MAX9265A_DEV_ID			0x91

extern const struct regmap_config max_ser_i2c_regmap;

struct max_ser_phy {
	unsigned int index;
	struct v4l2_mbus_config_mipi_csi2 mipi;
	bool enabled;
	bool active;
};

struct max_ser_pipe {
	unsigned int index;
	unsigned int phy_id;
	unsigned int stream_id;
	unsigned int *dts;
	unsigned int num_dts;
	unsigned int vcs;
	unsigned int soft_bpp;
	unsigned int bpp;
	bool dbl8;
	bool dbl10;
	bool dbl12;
	bool enabled;
};

struct max_ser;

struct max_ser_ops {
	unsigned int num_pipes;
	unsigned int num_dts_per_pipe;
	unsigned int num_phys;
	unsigned int num_i2c_xlates;
	bool supports_noncontinuous_clock;

	struct max_phys_configs phys_configs;

	int (*reg_read)(struct max_ser *ser, unsigned int reg, unsigned int *val);
	int (*reg_write)(struct max_ser *ser, unsigned int reg, unsigned int val);
	int (*log_status)(struct max_ser *ser, const char *name);
	int (*log_pipe_status)(struct max_ser *ser, struct max_ser_pipe *pipe,
			       const char *name);
	int (*log_phy_status)(struct max_ser *ser, struct max_ser_phy *phy,
			      const char *name);
	int (*init)(struct max_ser *ser);
	int (*init_i2c_xlate)(struct max_ser *ser);
	int (*init_phy)(struct max_ser *ser, struct max_ser_phy *phy);
	int (*set_phy_active)(struct max_ser *ser, struct max_ser_phy *phy,
			      bool enable);
	int (*init_pipe)(struct max_ser *ser, struct max_ser_pipe *pipe);
	int (*set_pipe_enable)(struct max_ser *ser, struct max_ser_pipe *pipe,
			       bool enable);
	int (*set_pipe_dt)(struct max_ser *ser, struct max_ser_pipe *pipe,
			   unsigned int i, unsigned int dt);
	int (*set_pipe_dt_en)(struct max_ser *ser, struct max_ser_pipe *pipe,
			      unsigned int i, bool enable);
	int (*set_pipe_vcs)(struct max_ser *ser, struct max_ser_pipe *pipe,
			    unsigned int vcs);
	int (*set_pipe_stream_id)(struct max_ser *ser, struct max_ser_pipe *pipe,
				  unsigned int stream_id);
	int (*set_pipe_phy)(struct max_ser *ser, struct max_ser_pipe *pipe,
			    struct max_ser_phy *phy);
	int (*post_init)(struct max_ser *ser);
};

struct max_ser_priv;

struct max_ser {
	struct max_ser_priv *priv;

	const struct max_ser_ops *ops;

	struct max_i2c_xlate *i2c_xlates;
	unsigned int num_i2c_xlates;

	struct max_ser_phy *phys;
	struct max_ser_pipe *pipes;

	unsigned int phys_config;
	unsigned int active;
};

int max_ser_probe(struct i2c_client *client, struct max_ser *ser);

int max_ser_remove(struct max_ser *ser);

int max_ser_reset(struct regmap *regmap);

int max_ser_wait(struct i2c_client *client, struct regmap *regmap, u8 addr);
int max_ser_wait_for_multiple(struct i2c_client *client, struct regmap *regmap,
			      u8 *addrs, unsigned int num_addrs);

int max_ser_change_address(struct i2c_client *client, struct regmap *regmap, u8 addr,
			   bool fix_tx_ids);

#endif // MAX_SER_H
