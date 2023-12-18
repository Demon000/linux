// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include <media/v4l2-mediabus.h>

#include "max_serdes.h"

#ifndef MAX_SER_H
#define MAX_SER_H

struct max_ser_phy {
	unsigned int index;
	struct v4l2_mbus_config_mipi_csi2 mipi;
	bool enabled;
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

struct max_ser_link {
	unsigned int index;
};

struct max_ser;

struct max_ser_ops {
	unsigned int num_pipes;
	unsigned int num_dts_per_pipe;
	unsigned int num_phys;
	unsigned int num_i2c_xlates;
	bool supports_tunnel_mode;
	bool supports_noncontinuous_clock;

	struct max_serdes_phys_configs phys_configs;

	int (*log_pipe_status)(struct max_ser *ser, struct max_ser_pipe *pipe,
			       const char *name);
	int (*log_phy_status)(struct max_ser *ser, struct max_ser_phy *phy,
			      const char *name);
	int (*set_pipe_enable)(struct max_ser *ser, struct max_ser_pipe *pipe, bool enable);
	int (*set_pipe_phy)(struct max_ser *ser, struct max_ser_pipe *pipe,
			    struct max_ser_phy *phy);
	int (*set_pipe_stream_id)(struct max_ser *ser, struct max_ser_pipe *pipe,
				  unsigned int stream_id);
	int (*set_pipe_dt)(struct max_ser *ser, struct max_ser_pipe *pipe,
			   unsigned int i, u32 dt);
	int (*set_pipe_dt_enable)(struct max_ser *ser, struct max_ser_pipe *pipe,
				  unsigned int i, bool enable);
	int (*set_pipe_vcs)(struct max_ser *ser, struct max_ser_pipe *pipe, u32 vcs);
	int (*set_phy_enable)(struct max_ser *ser, struct max_ser_phy *phy, bool enable);

	int (*init)(struct max_ser *ser);
	int (*init_i2c_xlate)(struct max_ser *ser);
	int (*init_phy)(struct max_ser *ser, struct max_ser_phy *phy);
	int (*init_pipe)(struct max_ser *ser, struct max_ser_pipe *pipe);
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
	struct max_ser_link *links;

	bool tunnel_mode;
};

int max_ser_probe(struct i2c_client *client, struct max_ser *ser);
int max_ser_remove(struct max_ser *ser);

#endif // MAX_SER_H
