// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-mc.h>

#ifndef MAX_SERDES_H
#define MAX_SERDES_H

#define MAX_SERDES_STREAMS_NUM		4
#define MAX_SERDES_VC_ID_NUM		4

extern const struct regmap_config max_i2c_regmap;

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

	struct v4l2_device *v4l2_dev;
	struct i2c_client *client;
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

	unsigned int num_pads;

	unsigned int source_pads_start;
	unsigned int source_pads_end;

	unsigned int sink_pads_start;
	unsigned int sink_pads_end;
	bool sink_pads_first;
};

#define MAX_DT_FS			0x00
#define MAX_DT_FE			0x01
#define MAX_DT_EMB8			0x12
#define MAX_DT_YUV422_8B		0x1e
#define MAX_DT_YUV422_10B		0x1f
#define MAX_DT_RGB565			0x22
#define MAX_DT_RGB666			0x23
#define MAX_DT_RGB888			0x24
#define MAX_DT_RAW8			0x2a
#define MAX_DT_RAW10			0x2b
#define MAX_DT_RAW12			0x2c
#define MAX_DT_RAW14			0x2d
#define MAX_DT_RAW16			0x2e
#define MAX_DT_RAW20			0x2f

const struct max_format *max_format_by_index(unsigned int index);
const struct max_format *max_format_by_code(u32 code);
const struct max_format *max_format_by_dt(u8 dt);

#define MAX_SER_MAX96717_DEV_ID			0xbf
#define MAX_SER_MAX9265A_DEV_ID			0x91

int max_ser_reset(struct regmap *regmap);
int max_ser_wait(struct i2c_client *client, struct regmap *regmap, u8 addr);
int max_ser_wait_for_multiple(struct i2c_client *client, struct regmap *regmap,
			      u8 *addrs, unsigned int num_addrs);
int max_ser_change_address(struct i2c_client *client, struct regmap *regmap, u8 addr,
			   bool fix_tx_ids);

void max_set_priv_name(char *name, const char *label, struct i2c_client *client);
void max_set_sd_name(struct v4l2_subdev *sd, const char *name,
		     const char *type, unsigned int index);

int max_component_register_v4l2_sd(struct max_component *comp);
void max_component_unregister_v4l2_sd(struct max_component *comp);

int max_components_link(struct max_component *source, unsigned int source_offset,
			struct max_component *sink, unsigned int sink_offset,
			bool create);

#endif // MAX_SERDES_H
