# SPDX-License-Identifier: GPL-2.0
max_serdes_all-objs := max_serdes.o max_ser_common.o

max_ser_all-objs := max_ser.o max_ser_atr.o
max_ser_all-objs += max_ser_phy.o max_ser_pipe.o max_ser_link.o
max_ser_all-objs += max_ser_phy_pipe_xbar.o max_ser_pipe_link_xbar.o

max_des_all-objs := max_des.o max_des_atr.o
max_des_all-objs += max_des_link.o max_des_pipe.o max_des_phy.o
max_des_all-objs += max_des_link_pipe_xbar.o max_des_pipe_phy_xbar.o

obj-$(CONFIG_VIDEO_MAXIM_SERDES) += max_serdes_all.o
obj-$(CONFIG_VIDEO_MAXIM_SER) += max_ser_all.o
obj-$(CONFIG_VIDEO_MAXIM_DES) += max_des_all.o
obj-$(CONFIG_VIDEO_MAXIM_AGGREGATOR) += max_aggregator.o
obj-$(CONFIG_VIDEO_MAX96717) += max96717.o
obj-$(CONFIG_VIDEO_MAX96724) += max96724.o
obj-$(CONFIG_VIDEO_MAX9296A) += max9296a.o
