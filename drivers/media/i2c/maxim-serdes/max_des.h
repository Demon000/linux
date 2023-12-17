// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Analog Devices Inc.
 */

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

#endif // MAX_DES_H
