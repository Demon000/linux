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

#define MAX_DES_DT_VC(dt, vc) (((vc) & 0x3) << 6 | ((dt) & 0x3f))

struct max_des_dt_vc_remap {
	u8 from_dt;
	u8 from_vc;
	u8 to_dt;
	u8 to_vc;
	u8 phy;
};

struct max_des_link {
	unsigned int index;
	struct max_i2c_xlate ser_xlate;
	bool ser_xlate_enabled;
	bool tunnel_mode;
	bool enabled;
};

struct max_des_pipe {
	unsigned int index;
	unsigned int phy_id;
	unsigned int stream_id;
	unsigned int link_id;
	struct max_des_dt_vc_remap *remaps;
	unsigned int num_remaps;
	bool dbl8;
	bool dbl10;
	bool dbl12;
	bool dbl8mode;
	bool dbl10mode;
	bool enabled;
};

struct max_des_phy {
	unsigned int index;
	u64 link_frequency;
	struct v4l2_mbus_config_mipi_csi2 mipi;
	bool alt_mem_map8;
	bool alt2_mem_map8;
	bool alt_mem_map10;
	bool alt_mem_map12;
	bool enabled;
};

struct max_des;

struct max_des_ops {
	unsigned int num_phys;
	unsigned int num_pipes;
	unsigned int num_links;
	unsigned int num_remaps_per_pipe;
	bool fix_tx_ids;
	bool supports_pipe_link_remap;
	bool supports_pipe_stream_autoselect;
	bool supports_tunnel_mode;

	struct max_serdes_phys_configs phys_configs;

	int (*log_status)(struct max_des *des, const char *name);
	int (*log_pipe_status)(struct max_des *des, struct max_des_pipe *pipe,
			       const char *name);
	int (*log_phy_status)(struct max_des *des, struct max_des_phy *phy,
			      const char *name);

	int (*set_enable)(struct max_des *des, bool enable);
	int (*init)(struct max_des *des);
	int (*init_phy)(struct max_des *des, struct max_des_phy *phy);
	int (*set_phy_enable)(struct max_des *des, struct max_des_phy *phy,
			      bool enable);
	int (*init_pipe)(struct max_des *des, struct max_des_pipe *pipe);
	int (*set_pipe_link)(struct max_des *des, struct max_des_pipe *pipe,
			     struct max_des_link *link);
	int (*set_pipe_stream_id)(struct max_des *des, struct max_des_pipe *pipe,
				  unsigned int stream_id);
	int (*set_pipe_phy)(struct max_des *des, struct max_des_pipe *pipe,
			    struct max_des_phy *phy);
	int (*set_pipe_enable)(struct max_des *des, struct max_des_pipe *pipe,
			       bool enable);
	int (*set_pipe_remaps)(struct max_des *des, struct max_des_pipe *pipe,
			       struct max_des_dt_vc_remap *remaps,
			       unsigned int num_remaps);
	int (*select_links)(struct max_des *des, unsigned int mask);
	int (*post_init)(struct max_des *des);
};

struct max_des_priv;

struct max_des {
	struct max_des_priv *priv;

	const struct max_des_ops *ops;

	char name[V4L2_SUBDEV_NAME_SIZE];

	unsigned int phys_config;
	bool pipe_stream_autoselect;
	unsigned int num_streams_per_link;

	unsigned int num_enabled_phys;
	struct max_des_phy **enabled_phys;

	struct max_des_phy *phys;
	struct max_des_pipe *pipes;
	struct max_des_link *links;
};

struct max_des_priv {
	struct device *dev;
	struct i2c_client *client;
	struct i2c_atr *atr;
	struct max_des *des;

	struct v4l2_async_notifier notifier;
	struct mutex lock;
	bool active;

	unsigned int num_bound_phys;

	struct max_component *phys_comp;
	struct max_component *pipes_comp;
	struct max_component *links_comp;

	struct max_component pipe_phy_xbar_comp;
	struct max_component link_pipe_xbar_comp;

	struct max_component **links_comps;
	struct fwnode_handle **links_eps;
};

int max_des_i2c_atr_init(struct max_des_priv *priv);
void max_des_i2c_atr_deinit(struct max_des_priv *priv);

int max_des_probe(struct i2c_client *client, struct max_des *des);
int max_des_remove(struct max_des *des);

int max_des_register_v4l2(struct max_des_priv *priv, struct v4l2_device *v4l2_dev);
void max_des_unregister_v4l2(struct max_des_priv *priv);

int max_des_register_link_notifiers(struct max_des_priv *priv,
				    struct v4l2_subdev *sd);
void max_des_unregister_link_notifiers(struct max_des_priv *priv);

int max_des_link_register_v4l2_sd(struct max_des_priv *priv,
				  struct max_des_link *link,
				  struct max_component *comp,
				  struct v4l2_device *v4l2_dev);
void max_des_link_unregister_v4l2_sd(struct max_des_priv *priv,
				     struct max_component *comp);

int max_des_link_pipe_xbar_register_v4l2_sd(struct max_des_priv *priv,
					    struct max_component *comp,
					    struct v4l2_device *v4l2_dev);
void max_des_link_pipe_xbar_unregister_v4l2_sd(struct max_des_priv *priv,
					       struct max_component *comp);

int max_des_pipe_register_v4l2_sd(struct max_des_priv *priv,
				  struct max_des_pipe *pipe,
				  struct max_component *comp,
				  struct v4l2_device *v4l2_dev);
void max_des_pipe_unregister_v4l2_sd(struct max_des_priv *priv,
				     struct max_component *comp);

int max_des_pipe_phy_xbar_register_v4l2_sd(struct max_des_priv *priv,
					   struct max_component *comp,
					   struct v4l2_device *v4l2_dev);
void max_des_pipe_phy_xbar_unregister_v4l2_sd(struct max_des_priv *priv,
					      struct max_component *comp);

int max_des_phy_register_v4l2_sd(struct max_des_priv *priv,
				 struct max_des_phy *phy,
				 struct max_component *comp,
				 bool attach_notifier);
void max_des_phy_unregister_v4l2_sd(struct max_des_priv *priv,
				    struct max_component *comp);

int max_des_phy_parse_dt(struct max_des_priv *priv, struct max_des_phy *phy,
			 struct fwnode_handle *fwnode);
int max_des_pipe_parse_dt(struct max_des_priv *priv, struct max_des_pipe *pipe,
			  struct fwnode_handle *fwnode);
int max_des_link_parse_dt(struct max_des_priv *priv, struct max_des_link *link,
			  struct fwnode_handle *fwnode);

#endif // MAX_DES_H
