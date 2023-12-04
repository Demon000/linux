// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include <linux/i2c.h>
#include <linux/i2c-atr.h>
#include <linux/regmap.h>

#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

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

	char name[V4L2_SUBDEV_NAME_SIZE];

	struct max_i2c_xlate *i2c_xlates;
	unsigned int num_i2c_xlates;

	struct max_ser_phy *phys;
	struct max_ser_pipe *pipes;
	struct max_ser_link *links;

	bool tunnel_mode;
};

struct max_ser_priv {
	struct device *dev;
	struct i2c_client *client;
	struct i2c_atr *atr;
	struct max_ser *ser;

	struct mutex lock;

	struct max_component *phys_comp;
	struct max_component *pipes_comp;
	struct max_component *links_comp;

	struct max_component phy_pipe_xbar_comp;
	struct max_component pipe_link_xbar_comp;

	struct max_component **phys_comps;
	struct fwnode_handle **phys_eps;
};

int max_ser_probe(struct i2c_client *client, struct max_ser *ser);

int max_ser_remove(struct max_ser *ser);

int max_ser_register_v4l2(struct max_ser_priv *priv, struct v4l2_device *v4l2_dev);
void max_ser_unregister_v4l2(struct max_ser_priv *priv);

int max_ser_i2c_atr_init(struct max_ser_priv *priv);
void max_ser_i2c_atr_deinit(struct max_ser_priv *priv);

int max_ser_phy_register_v4l2_sd(struct max_ser_priv *priv,
				 struct max_ser_phy *phy,
				 struct max_component *comp,
				 struct v4l2_device *v4l2_dev);

void max_ser_phy_unregister_v4l2_sd(struct max_ser_priv *priv,
				    struct max_component *comp);

int max_ser_phy_pipe_xbar_register_v4l2_sd(struct max_ser_priv *priv,
					   struct max_component *comp,
					   struct v4l2_device *v4l2_dev);
void max_ser_phy_pipe_xbar_unregister_v4l2_sd(struct max_ser_priv *priv,
					      struct max_component *comp);

int max_ser_pipe_register_v4l2_sd(struct max_ser_priv *priv,
				  struct max_ser_pipe *pipe,
				  struct max_component *comp,
				  struct v4l2_device *v4l2_dev);
void max_ser_pipe_unregister_v4l2_sd(struct max_ser_priv *priv,
				     struct max_component *comp);

int max_ser_pipe_link_xbar_register_v4l2_sd(struct max_ser_priv *priv,
					    struct max_component *comp,
					    struct v4l2_device *v4l2_dev);
void max_ser_pipe_link_xbar_unregister_v4l2_sd(struct max_ser_priv *priv,
					       struct max_component *comp);

int max_ser_link_register_v4l2_sd(struct max_ser_priv *priv,
				  struct max_ser_link *link,
				  struct max_component *comp);
void max_ser_link_unregister_v4l2_sd(struct max_ser_priv *priv,
				     struct max_component *comp);

int max_ser_pipe_parse_dt(struct max_ser_priv *priv, struct max_ser_pipe *pipe,
			  struct fwnode_handle *fwnode);
int max_ser_phy_parse_dt(struct max_ser_priv *priv, struct max_ser_phy *phy,
			 struct fwnode_handle *fwnode);

#endif // MAX_SER_H
