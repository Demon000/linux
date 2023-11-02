// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include <linux/i2c-atr.h>

#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-device.h>

#include "max_serdes.h"

#ifndef MAX_DES_H
#define MAX_DES_H

/* TODO: remove */
#define MAX_DES_REMAPS_NUM		16

#define MAX_DES_DT_VC(dt, vc) (((vc) & 0x3) << 6 | ((dt) & 0x3f))

struct max_des_dt_vc_remap {
	u8 from_dt;
	u8 from_vc;
	u8 to_dt;
	u8 to_vc;
	u8 phy;
};

struct max_des_link_data {
	unsigned int index;
	struct max_i2c_xlate ser_xlate;
	bool ser_xlate_enabled;
	bool tunnel_mode;
	bool enabled;
};

struct max_des_link {
	struct max_component comp;
	struct max_des_priv *priv;
	struct max_des_link_data data;
};

struct max_des_pipe_data {
	unsigned int index;
	unsigned int phy_id;
	unsigned int stream_id;
	unsigned int link_id;
	struct max_des_dt_vc_remap remaps[MAX_DES_REMAPS_NUM];
	unsigned int num_remaps;
	bool dbl8;
	bool dbl10;
	bool dbl12;
	bool dbl8mode;
	bool dbl10mode;
};

struct max_des_pipe {
	struct max_component comp;
	struct max_des_priv *priv;
	struct max_des_pipe_data data;
};

struct max_des_pipe_phy_xbar {
	struct max_component comp;
	struct max_des_priv *priv;
};

struct max_des_link_pipe_xbar {
	struct max_component comp;
	struct max_des_priv *priv;
};

struct max_des_phy_data {
	unsigned int index;
	u64 link_frequency;
	struct v4l2_mbus_config_mipi_csi2 mipi;
	bool alt_mem_map8;
	bool alt2_mem_map8;
	bool alt_mem_map10;
	bool alt_mem_map12;
	bool enabled;
};

struct max_des_phy {
	struct max_component comp;
	struct max_des_priv *priv;
	struct max_des_phy_data data;
};

struct max_des_ops {
	unsigned int num_phys;
	unsigned int num_pipes;
	unsigned int num_links;
	bool fix_tx_ids;
	bool supports_pipe_link_remap;
	bool supports_pipe_stream_autoselect;
	bool supports_tunnel_mode;

	int (*log_status)(struct max_des_priv *priv, const char *name);
	int (*log_pipe_status)(struct max_des_priv *priv, struct max_des_pipe_data *data,
			       const char *name);
	int (*log_phy_status)(struct max_des_priv *priv, struct max_des_phy_data *data,
			      const char *name);

	int (*mipi_enable)(struct max_des_priv *priv, bool enable);
	int (*init)(struct max_des_priv *priv);
	int (*init_phy)(struct max_des_priv *priv, struct max_des_phy_data *data);
	int (*init_pipe)(struct max_des_priv *priv, struct max_des_pipe_data *data);
	int (*update_pipe_remaps)(struct max_des_priv *priv, struct max_des_pipe_data *data);
	int (*select_links)(struct max_des_priv *priv, unsigned int mask);
	int (*post_init)(struct max_des_priv *priv);
};

struct max_des_priv {
	const struct max_des_ops *ops;

	struct device *dev;
	struct i2c_client *client;
	struct regmap *regmap;

	struct i2c_atr *atr;

	struct v4l2_async_notifier notifier;
	struct mutex lock;
	bool active;

	bool pipe_stream_autoselect;
	char name[V4L2_SUBDEV_NAME_SIZE];

	unsigned int num_enabled_phys;
	unsigned int num_bound_phys;
	unsigned int num_streams_per_link;
	struct max_des_pipe_phy_xbar pipe_phy_xbar;
	struct max_des_link_pipe_xbar link_pipe_xbar;
	struct max_des_phy *phys;
	struct max_des_pipe *pipes;
	struct max_des_link *links;
	struct fwnode_handle **links_eps;
	struct max_component **links_comps;
};

int max_des_i2c_atr_init(struct max_des_priv *priv);
void max_des_i2c_atr_deinit(struct max_des_priv *priv);

int max_des_probe(struct max_des_priv *priv);
int max_des_remove(struct max_des_priv *priv);

int max_des_register_v4l2(struct max_des_priv *priv, struct v4l2_device *v4l2_dev);
void max_des_unregister_v4l2(struct max_des_priv *priv);

int max_des_register_link_notifiers(struct max_des_priv *priv,
				    struct v4l2_subdev *sd);
void max_des_unregister_link_notifiers(struct max_des_priv *priv);

static inline struct max_des_phy *max_des_phy_by_id(struct max_des_priv *priv,
						    unsigned int index)
{
	return &priv->phys[index];
}

int max_des_link_register_v4l2_sd(struct max_des_link *link,
				  struct v4l2_device *v4l2_dev);
void max_des_link_unregister_v4l2_sd(struct max_des_link *link);

int max_des_link_pipe_xbar_register_v4l2_sd(struct max_des_link_pipe_xbar *xbar,
					    struct v4l2_device *v4l2_dev);
void max_des_link_pipe_xbar_unregister_v4l2_sd(struct max_des_link_pipe_xbar *xbar);

int max_des_pipe_register_v4l2_sd(struct max_des_pipe *pipe,
				  struct v4l2_device *v4l2_dev);
void max_des_pipe_unregister_v4l2_sd(struct max_des_pipe *pipe);

int max_des_pipe_phy_xbar_register_v4l2_sd(struct max_des_pipe_phy_xbar *xbar,
					   struct v4l2_device *v4l2_dev);
void max_des_pipe_phy_xbar_unregister_v4l2_sd(struct max_des_pipe_phy_xbar *xbar);

int max_des_phy_register_v4l2_sd(struct max_des_phy *phy, bool attach_notifier);
void max_des_phy_unregister_v4l2_sd(struct max_des_phy *phy);

int max_des_phy_parse_dt(struct max_des_priv *priv, struct max_des_phy_data *data,
			 struct fwnode_handle *fwnode);
int max_des_pipe_parse_dt(struct max_des_priv *priv, struct max_des_pipe_data *data,
			  struct fwnode_handle *fwnode);
int max_des_link_parse_dt(struct max_des_priv *priv, struct max_des_link_data *data,
			  struct fwnode_handle *fwnode);

#endif // MAX_DES_H
