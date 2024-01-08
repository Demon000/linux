// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include <linux/i2c-atr.h>

#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-device.h>

#include "max_des.h"
#include "max_serdes_priv.h"

#ifndef MAX_DES_PRIV_H
#define MAX_DES_PRIV_H

struct max_des_priv {
	struct device *dev;
	struct i2c_client *client;
	struct i2c_atr *atr;
	struct max_des *des;

	char name[V4L2_SUBDEV_NAME_SIZE];

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

#endif // MAX_DES_PRIV_H
