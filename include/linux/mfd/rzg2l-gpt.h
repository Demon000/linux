/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Renesas Electronics Corporation
 */

#ifndef __MFD_RZG2L_GPT_H__
#define __MFD_RZG2L_GPT_H__

#include <linux/clk.h>
#include <linux/io.h>

#define RZG2L_GTCR_TPCS			GENMASK(26, 24)
#define RZG3E_GTCR_TPCS			GENMASK(26, 23)

#define RZG2L_MAX_HW_CHANNELS		8

#define RZG2L_INVALID_TPCS		0xff

/**
 * struct rzg2l_gpt_info - GPT SoC-specific info
 *
 * @prescales: Maps a prescale to its GTCR.TPCS value. Unsupported prescales
 *	       must be RZG2L_INVALID_TPCS. Last entry must be valid.
 * @num_prescales: Number of prescales.
 * @gtcr_tpcs: GTCR.TPCS mask.
 */
struct rzg2l_gpt_info {
	/*
	 * Maps a prescale to its GTCR.TPCS value. Unsupported prescales must be
	 * RZG2L_INVALID_TPCS. Last entry must be valid.
	 */
	const u8 *prescales;
	unsigned int num_prescales;
	u32 gtcr_tpcs;
};

/**
 * struct rzg2l_gpt - GPT core data
 *
 * @mmio: MMIO register base.
 * @clk: Module clock.
 */
struct rzg2l_gpt {
	void __iomem *mmio;
	struct clk *clk;
	unsigned int num_channels;
	const struct rzg2l_gpt_info *info;
};

int rzg2l_gpt_request_channel(struct rzg2l_gpt *gpt, unsigned int index);
void rzg2l_gpt_release_channel(struct rzg2l_gpt *gpt, unsigned int index);

#endif /* __MFD_RZG2L_GPT_H__ */
