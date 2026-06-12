/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Renesas Electronics Corporation
 */

#ifndef __MFD_RZG2L_GPT_H__
#define __MFD_RZG2L_GPT_H__

#include <linux/clk.h>
#include <linux/io.h>

#define RZG2L_GET_CH(hwpwm)		((hwpwm) / 2)
#define RZG2L_GET_CH_OFFS(ch)		(0x100 * (ch))

#define RZG2L_GTUPSR(ch)		(0x1c + RZG2L_GET_CH_OFFS(ch))
#define RZG2L_GTDNSR(ch)		(0x20 + RZG2L_GET_CH_OFFS(ch))
#define RZG2L_GTCR(ch)			(0x2c + RZG2L_GET_CH_OFFS(ch))
#define RZG2L_GTUDDTYC(ch)		(0x30 + RZG2L_GET_CH_OFFS(ch))
#define RZG2L_GTIOR(ch)			(0x34 + RZG2L_GET_CH_OFFS(ch))
#define RZG2L_GTINTAD(ch)		(0x38 + RZG2L_GET_CH_OFFS(ch))
#define RZG2L_GTST(ch)			(0x3c + RZG2L_GET_CH_OFFS(ch))
#define RZG2L_GTBER(ch)			(0x40 + RZG2L_GET_CH_OFFS(ch))
#define RZG2L_GTCNT(ch)			(0x48 + RZG2L_GET_CH_OFFS(ch))
#define RZG2L_GTCCR(ch, sub_ch)		(0x4c + RZG2L_GET_CH_OFFS(ch) + 4 * (sub_ch))
#define RZG2L_GTPR(ch)			(0x64 + RZG2L_GET_CH_OFFS(ch))

#define RZG2L_GTUPDNSR_CARBL		BIT(8)
#define RZG2L_GTUPDNSR_CARBH		BIT(9)
#define RZG2L_GTUPDNSR_CAFBL		BIT(10)
#define RZG2L_GTUPDNSR_CAFBH		BIT(11)
#define RZG2L_GTUPDNSR_CBRAL		BIT(12)
#define RZG2L_GTUPDNSR_CBRAH		BIT(13)
#define RZG2L_GTUPDNSR_CBFAL		BIT(14)
#define RZG2L_GTUPDNSR_CBFAH		BIT(15)

#define RZG2L_GTCR_CST			BIT(0)
#define RZG2L_GTCR_MD			GENMASK(18, 16)
#define RZG2L_GTCR_TPCS			GENMASK(26, 24)
#define RZG3E_GTCR_TPCS			GENMASK(26, 23)

#define RZG2L_GTCR_MD_SAW_WAVE_PWM_MODE	FIELD_PREP(RZG2L_GTCR_MD, 0)

#define RZG2L_GTUDDTYC_UP		BIT(0)
#define RZG2L_GTUDDTYC_UDF		BIT(1)
#define RZG2L_GTUDDTYC_UP_COUNTING	(RZG2L_GTUDDTYC_UP | RZG2L_GTUDDTYC_UDF)

#define RZG2L_GTIOR_GTIOA		GENMASK(4, 0)
#define RZG2L_GTIOR_OADF		GENMASK(10, 9)
#define RZG2L_GTIOR_GTIOB		GENMASK(20, 16)
#define RZG2L_GTIOR_OBDF		GENMASK(26, 25)
#define RZG2L_GTIOR_GTIOx(sub_ch)	((sub_ch) ? RZG2L_GTIOR_GTIOB : RZG2L_GTIOR_GTIOA)
#define RZG2L_GTIOR_OAE			BIT(8)
#define RZG2L_GTIOR_OBE			BIT(24)
#define RZG2L_GTIOR_OxE(sub_ch)		((sub_ch) ? RZG2L_GTIOR_OBE : RZG2L_GTIOR_OAE)

#define RZG2L_GTIOR_OADF_HIGH_IMP_ON_OUT_DISABLE	BIT(9)
#define RZG2L_GTIOR_OBDF_HIGH_IMP_ON_OUT_DISABLE	BIT(25)
#define RZG2L_GTIOR_PIN_DISABLE_SETTING \
	(RZG2L_GTIOR_OADF_HIGH_IMP_ON_OUT_DISABLE | RZG2L_GTIOR_OBDF_HIGH_IMP_ON_OUT_DISABLE)

#define RZG2L_INIT_OUT_HI_OUT_HI_END_TOGGLE	0x1b
#define RZG2L_GTIOR_GTIOA_OUT_HI_END_TOGGLE_CMP_MATCH \
	(RZG2L_INIT_OUT_HI_OUT_HI_END_TOGGLE | RZG2L_GTIOR_OAE)
#define RZG2L_GTIOR_GTIOB_OUT_HI_END_TOGGLE_CMP_MATCH \
	(FIELD_PREP(RZG2L_GTIOR_GTIOB, RZG2L_INIT_OUT_HI_OUT_HI_END_TOGGLE) | RZG2L_GTIOR_OBE)

#define RZG2L_GTIOR_GTIOx_OUT_HI_END_TOGGLE_CMP_MATCH(sub_ch) \
	((sub_ch) ? RZG2L_GTIOR_GTIOB_OUT_HI_END_TOGGLE_CMP_MATCH : \
	 RZG2L_GTIOR_GTIOA_OUT_HI_END_TOGGLE_CMP_MATCH)

#define RZG2L_GTINTAD_GRP_MASK		GENMASK(25, 24)

#define RZG2L_GTST_TUCF			BIT(15)

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
