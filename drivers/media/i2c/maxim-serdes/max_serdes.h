// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include <linux/regmap.h>

#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-mc.h>
#include <media/mipi-csi2.h>

#ifndef MAX_SERDES_H
#define MAX_SERDES_H

#define MAX_SERDES_PHYS_MAX		4
#define MAX_SERDES_STREAMS_NUM		4
#define MAX_SERDES_VC_ID_NUM		4

extern const struct regmap_config max_i2c_regmap;

struct max_serdes_phy_configs {
	unsigned int lanes[MAX_SERDES_PHYS_MAX];
	unsigned int clock_lane[MAX_SERDES_PHYS_MAX];
};

struct max_serdes_phys_configs {
	const struct max_serdes_phy_configs *configs;
	unsigned int num_configs;
};

struct max_i2c_xlate {
	u8 src;
	u8 dst;
};

struct max_format {
	const char *name;
	u32 code;
	u8 dt;
	u8 bpp;
	bool dbl;
};

#define sd_max_component(sd) container_of(sd, struct max_component, sd)

struct max_component {
	struct v4l2_subdev sd;
	struct v4l2_subdev *remote_sd;

	const struct v4l2_subdev_ops *sd_ops;
	const struct v4l2_subdev_internal_ops *internal_ops;
	const struct media_entity_operations *mc_ops;

	void *priv;
	struct device *dev;
	struct i2c_client *client;
	struct v4l2_device *v4l2_dev;
	struct fwnode_handle *fwnode;

	struct v4l2_async_notifier notifier;
	struct max_component **notifier_comps;
	struct fwnode_handle **notifier_eps;
	unsigned int num_notifier_eps;

	unsigned int num_source_pads;
	unsigned int num_sink_pads;

	const char *prefix;
	const char *name;
	unsigned int index;

	struct media_pad *pads;
	u64 *pads_enabled_streams;

	unsigned int num_pads;

	unsigned int source_pads_start;
	unsigned int source_pads_end;

	unsigned int sink_pads_start;
	unsigned int sink_pads_end;
	bool sink_pads_first;

	u64 enabled_source_streams;
	enum v4l2_subdev_routing_restriction routing_disallow;
};

const struct max_format *max_format_by_index(unsigned int index);
const struct max_format *max_format_by_code(u32 code);
const struct max_format *max_format_by_dt(u8 dt);

void max_set_priv_name(char *name, const char *label, struct i2c_client *client);
void max_set_name_i2c_client(char *name, size_t size, struct i2c_client *client);

int max_component_register_v4l2_sd(struct max_component *comp);
void max_component_unregister_v4l2_sd(struct max_component *comp);

int max_components_link(struct max_component *source, unsigned int source_offset,
			struct max_component *sink, unsigned int sink_offset);

int max_component_init_routing(struct max_component *comp,
			       struct v4l2_subdev_krouting *routing);
int max_component_validate_routing(struct max_component *comp,
				   struct v4l2_subdev_krouting *routing);
int max_component_set_routing(struct max_component *comp,
			      struct v4l2_subdev_state *state,
			      enum v4l2_subdev_format_whence which,
			      struct v4l2_subdev_krouting *routing);
int max_component_set_validate_routing(struct v4l2_subdev *sd,
					      struct v4l2_subdev_state *state,
					      enum v4l2_subdev_format_whence which,
					      struct v4l2_subdev_krouting *routing);
int max_component_init_cfg(struct v4l2_subdev *sd, struct v4l2_subdev_state *state);

int max_component_set_remote_streams_enable(struct max_component *comp,
					    struct v4l2_subdev_state *state,
					    u32 pad, u64 streams_mask,
					    bool enable);
u64 max_component_set_enabled_streams(struct max_component *comp,
				      u32 pad, u64 streams_mask, bool enable);
int max_component_streams_enable(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask);
int max_component_streams_disable(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask);

static inline int max_component_is_pad_sink(struct max_component *comp, unsigned int pad)
{
	return comp->sink_pads_start <= pad && pad < comp->sink_pads_end;
}

static inline int max_component_is_pad_source(struct max_component *comp, unsigned int pad)
{
	return comp->source_pads_start <= pad && pad < comp->source_pads_end;
}

#endif // MAX_SERDES_H
