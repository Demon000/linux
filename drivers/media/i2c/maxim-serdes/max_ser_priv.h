// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim GMSL2 Serializer Driver
 *
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include <linux/i2c.h>
#include <linux/i2c-atr.h>
#include <linux/regmap.h>

#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include "max_ser.h"
#include "max_serdes_priv.h"

#ifndef MAX_SER_PRIV_H
#define MAX_SER_PRIV_H

struct max_ser_priv {
	struct device *dev;
	struct i2c_client *client;
	struct i2c_atr *atr;
	struct max_ser *ser;

	char name[V4L2_SUBDEV_NAME_SIZE];

	struct mutex lock;

	struct max_component *phys_comp;
	struct max_component *pipes_comp;
	struct max_component *links_comp;

	struct max_component phy_pipe_xbar_comp;
	struct max_component pipe_link_xbar_comp;

	struct max_component **phys_comps;
	struct fwnode_handle **phys_eps;
};

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

#endif // MAX_SER_PRIV_H
