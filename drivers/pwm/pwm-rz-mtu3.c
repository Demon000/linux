// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas RZ/G2L MTU3a PWM Timer driver
 *
 * Copyright (C) 2023 Renesas Electronics Corporation
 *
 * Hardware manual for this IP can be found here
 * https://www.renesas.com/eu/en/document/mah/rzg2l-group-rzg2lc-group-users-manual-hardware-0?language=en
 *
 * Limitations:
 * - When PWM is disabled, the output is driven to Hi-Z.
 * - While the hardware supports both polarities, the driver (for now)
 *   only handles normal polarity.
 * - HW uses one counter and two match components to configure duty_cycle
 *   and period.
 * - Multi-Function Timer Pulse Unit (a.k.a MTU) has 7 HW channels for PWM
 *   operations. (The channels are MTU{0..4, 6, 7}.)
 * - MTU{1, 2} channels have a single IO, whereas all other HW channels have
 *   2 IOs.
 * - Each IO is modelled as an independent PWM channel.
 * - Sibling IOs must use the same period as they share a common counter.
 *   The counter can be reset on one of the following conditions: TGRA or TGRB
 *   or TGRC or TGRD compare match, or when the counter is cleared in another
 *   channel when synchronous clearing is enabled.
 *   The driver always uses TGRA compare match to reset the counter.
 *   The driver adjusts the period and duty cycle of the sibling IO when
 *   appropriate.
 * - rz_mtu3_pwm_io_map table is used to map the PWM channel to the
 *   corresponding HW channel as there are difference in number of IOs
 *   between HW channels.
 */

#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/clk.h>
#include <linux/limits.h>
#include <linux/mfd/rz-mtu3.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/pwm.h>
#include <linux/time.h>

#define RZ_MTU3_MAX_HW_CHANNELS		7

/**
 * struct rz_mtu3_pwm_channel - MTU3 pwm channel data
 *
 * @mtu: MTU3 channel data
 * @period_cycles: MTU3 period cycles
 * @user_count: MTU3 usage count
 * @enable_count: MTU3 enable count
 * @prescale: MTU3 prescale
 */
struct rz_mtu3_pwm_channel {
	struct rz_mtu3_channel *mtu;
	u64 period_cycles;
	u32 user_count;
	u32 enable_count;
	u8 prescale;
};

/**
 * struct rz_mtu3_pwm_chip - MTU3 pwm private data
 *
 * @lock: Lock to prevent concurrent access for usage count
 * @rate: MTU3 clock rate
 * @channel_data: MTU3 pwm channel data
 */

struct rz_mtu3_pwm_chip {
	struct mutex lock;
	unsigned long rate;
	struct rz_mtu3_pwm_channel channel_data[RZ_MTU3_MAX_HW_CHANNELS];
};

struct mtu_tpsc {
	u8 tpsc;
	u8 tpsc2;
};

#define RZ_MTU3_PWM_IO(ch, secondary) \
	(((ch) << 1) | (secondary))

/*
 * The MTU channels are {0..4, 6, 7} and the number of IO on MTU1
 * and MTU2 channel is 1 compared to 2 on others.
 */
static const u8 rz_mtu3_pwm_io_map[] = {
	RZ_MTU3_PWM_IO(0, 0), /* MTU0 IOA */
	RZ_MTU3_PWM_IO(0, 1), /* MTU0 IOB */
	RZ_MTU3_PWM_IO(1, 0), /* MTU1 IOA */
	RZ_MTU3_PWM_IO(2, 0), /* MTU2 IOA */
	RZ_MTU3_PWM_IO(3, 0), /* MTU3 IOA */
	RZ_MTU3_PWM_IO(3, 1), /* MTU3 IOB */
	RZ_MTU3_PWM_IO(4, 0), /* MTU4 IOA */
	RZ_MTU3_PWM_IO(4, 1), /* MTU4 IOB */
	RZ_MTU3_PWM_IO(5, 0), /* MTU6 IOA */
	RZ_MTU3_PWM_IO(5, 1), /* MTU6 IOB */
	RZ_MTU3_PWM_IO(6, 0), /* MTU7 IOA */
	RZ_MTU3_PWM_IO(6, 1), /* MTU7 IOB */
};

#define RZ_MTU3_MAX_PWM_CHANNELS ARRAY_SIZE(rz_mtu3_pwm_io_map)

/*
 * Register field values extracted from RZ/G2L User Manual, Table 16.7,
 * Table 16.8, Table 16.9, and Table 16.10.
 * /1 to /64 are shared across all channels, while /256 and /1024 are different
 * between channels 0, 1, 2 and 3-7.
 */
static const struct mtu_tpsc rz_mtu3_pwm_tpsc[] = {
	{ 0b000, 0b000 }, /* /1 */
	{ 0b000, 0b001 }, /* /2 */
	{ 0b001, 0b000 }, /* /4 */
	{ 0b000, 0b010 }, /* /8 */
	{ 0b010, 0b000 }, /* /16 */
	{ 0b000, 0b011 }, /* /32 */
	{ 0b011, 0b000 }, /* /64 */
};

static const struct mtu_tpsc rz_mtu3_pwm_tpsc_256[] = {
	{ 0b000, 0b100 },
	{ 0b110, 0b000 },
	{ 0b000, 0b100 },
	{ 0b100, 0b000 },
	{ 0b100, 0b000 },
	{ 0b000, 0b000 }, /* MTU5, unused */
	{ 0b100, 0b000 },
	{ 0b100, 0b000 },
};

static const struct mtu_tpsc rz_mtu3_pwm_tpsc_1024[] = {
	{ 0b000, 0b101 },
	{ 0b000, 0b100 },
	{ 0b111, 0b000 },
	{ 0b101, 0b000 },
	{ 0b101, 0b000 },
	{ 0b000, 0b000 }, /* MTU5, unused */
	{ 0b101, 0b000 },
	{ 0b101, 0b000 },
};

static inline struct rz_mtu3_pwm_chip *to_rz_mtu3_pwm_chip(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static void rz_mtu3_pwm_read_tgr_registers(struct rz_mtu3_pwm_channel *priv,
					   u16 reg_pv_offset, u16 *pv_val,
					   u16 reg_dc_offset, u16 *dc_val)
{
	*pv_val = rz_mtu3_16bit_ch_read(priv->mtu, reg_pv_offset);
	*dc_val = rz_mtu3_16bit_ch_read(priv->mtu, reg_dc_offset);
}

static void rz_mtu3_pwm_write_tgr_registers(struct rz_mtu3_pwm_channel *priv,
					    u16 reg_pv_offset, u16 pv_val,
					    u16 reg_dc_offset, u16 dc_val)
{
	rz_mtu3_16bit_ch_write(priv->mtu, reg_pv_offset, pv_val);
	rz_mtu3_16bit_ch_write(priv->mtu, reg_dc_offset, dc_val);
}

static int rz_mtu3_pwm_prescale_to_tpsc(struct rz_mtu3_pwm_channel *priv,
					u8 prescale, u8 *tpsc, u8 *tpsc2)
{
	u32 ch = priv->mtu->channel_number;

	if (prescale < ARRAY_SIZE(rz_mtu3_pwm_tpsc)) {
		*tpsc  = rz_mtu3_pwm_tpsc[prescale].tpsc;
		*tpsc2 = rz_mtu3_pwm_tpsc[prescale].tpsc2;
	} else if (prescale == 8 &&
		   ch < ARRAY_SIZE(rz_mtu3_pwm_tpsc_256)) {
		*tpsc  = rz_mtu3_pwm_tpsc_256[ch].tpsc;
		*tpsc2 = rz_mtu3_pwm_tpsc_256[ch].tpsc2;
	} else if (prescale == 10 &&
		   ch < ARRAY_SIZE(rz_mtu3_pwm_tpsc_1024)) {
		*tpsc  = rz_mtu3_pwm_tpsc_1024[ch].tpsc;
		*tpsc2 = rz_mtu3_pwm_tpsc_1024[ch].tpsc2;
	} else {
		return -EINVAL;
	}

	return 0;
}

static int rz_mtu3_pwm_tpsc_to_prescale(struct rz_mtu3_pwm_channel *priv,
					u8 tpsc, u8 tpsc2, u8 *prescale)
{
	u32 ch = priv->mtu->channel_number;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(rz_mtu3_pwm_tpsc); i++) {
		if (rz_mtu3_pwm_tpsc[i].tpsc  == tpsc &&
		    rz_mtu3_pwm_tpsc[i].tpsc2 == tpsc2) {
			*prescale = i;
			return 0;
		}
	}

	if (ch < ARRAY_SIZE(rz_mtu3_pwm_tpsc_256) &&
	    rz_mtu3_pwm_tpsc_256[ch].tpsc  == tpsc &&
	    rz_mtu3_pwm_tpsc_256[ch].tpsc2 == tpsc2) {
		*prescale = 8;
	} else if (ch < ARRAY_SIZE(rz_mtu3_pwm_tpsc_1024) &&
		   rz_mtu3_pwm_tpsc_1024[ch].tpsc  == tpsc &&
		   rz_mtu3_pwm_tpsc_1024[ch].tpsc2 == tpsc2) {
		*prescale = 10;
	} else {
		return -EINVAL;
	}

	return 0;
}

static u8 rz_mtu3_pwm_calculate_prescale(u64 period_cycles)
{
	u64 prescaled_period_cycles;
	u8 prescale;

	/*
	 * Calculate the number of bits exceeding the 16-bit counter, and use
	 * that value to determine the prescale exponent.
	 * For example, if the value is 17 bits wide, use /2 (2^1) for the
	 * prescale, resulting in a prescale exponent of 1.
	 */
	prescaled_period_cycles = period_cycles >> 16;
	prescale = fls64(prescaled_period_cycles);

	/*
	 * The hardware does not support /128 (2^7) or /512 (2^9) prescales,
	 * fall back to /256 (2^8) and /1024 (2^10) respectively for them.
	 */
	if (prescale <= 6)
		return prescale;
	else if (prescale <= 8)
		return 8;

	return 10;
}

static unsigned int rz_mtu3_hwpwm_io(u32 hwpwm)
{
	return rz_mtu3_pwm_io_map[hwpwm] & 1;
}

static bool rz_mtu3_hwpwm_is_primary(u32 hwpwm)
{
	return !rz_mtu3_hwpwm_io(hwpwm);
}

static struct rz_mtu3_pwm_channel *
rz_mtu3_get_channel(struct rz_mtu3_pwm_chip *rz_mtu3_pwm, u32 hwpwm)
{
	unsigned int ch = rz_mtu3_pwm_io_map[hwpwm] >> 1;

	return &rz_mtu3_pwm->channel_data[ch];
}

static bool rz_mtu3_pwm_is_ch_enabled(struct rz_mtu3_pwm_chip *rz_mtu3_pwm,
				      u32 hwpwm)
{
	struct rz_mtu3_pwm_channel *priv;
	bool is_channel_en;
	u8 val;

	priv = rz_mtu3_get_channel(rz_mtu3_pwm, hwpwm);

	is_channel_en = rz_mtu3_is_enabled(priv->mtu);
	if (!is_channel_en)
		return false;

	if (rz_mtu3_hwpwm_is_primary(hwpwm))
		val = rz_mtu3_8bit_ch_read(priv->mtu, RZ_MTU3_TIORH);
	else
		val = rz_mtu3_8bit_ch_read(priv->mtu, RZ_MTU3_TIORL);

	return val & RZ_MTU3_TIOR_IOA;
}

static int rz_mtu3_pwm_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct rz_mtu3_pwm_chip *rz_mtu3_pwm = to_rz_mtu3_pwm_chip(chip);
	struct rz_mtu3_pwm_channel *priv;
	bool is_mtu3_channel_available;

	priv = rz_mtu3_get_channel(rz_mtu3_pwm, pwm->hwpwm);

	guard(mutex)(&rz_mtu3_pwm->lock);

	/*
	 * Each channel must be requested only once, so if the channel
	 * serves two PWMs and the other is already requested, skip over
	 * rz_mtu3_request_channel()
	 */
	if (!priv->user_count) {
		is_mtu3_channel_available = rz_mtu3_request_channel(priv->mtu);
		if (!is_mtu3_channel_available)
			return -EBUSY;
	}

	priv->user_count++;

	return 0;
}

static void rz_mtu3_pwm_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct rz_mtu3_pwm_chip *rz_mtu3_pwm = to_rz_mtu3_pwm_chip(chip);
	struct rz_mtu3_pwm_channel *priv;

	priv = rz_mtu3_get_channel(rz_mtu3_pwm, pwm->hwpwm);

	guard(mutex)(&rz_mtu3_pwm->lock);

	priv->user_count--;
	if (!priv->user_count)
		rz_mtu3_release_channel(priv->mtu);
}

static void rz_mtu3_pwm_set_toer_bit(struct rz_mtu3_pwm_chip *rz_mtu3_pwm,
				     u32 hwpwm, bool set)
{
	struct rz_mtu3_pwm_channel *priv;
	u8 bitpos;
	u16 reg;

	priv = rz_mtu3_get_channel(rz_mtu3_pwm, hwpwm);

	/*
	 * HW channels 4 and 7 require an additional register write to enable
	 * PWM output.
	 */
	if (priv->mtu->channel_number == RZ_MTU3_CHAN_4)
		reg = RZ_MTU3_TOERA;
	else if (priv->mtu->channel_number == RZ_MTU3_CHAN_7)
		reg = RZ_MTU3_TOERB;
	else
		return;

	if (rz_mtu3_hwpwm_is_primary(hwpwm))
		bitpos = 1;
	else
		bitpos = 4;

	rz_mtu3_shared_reg_update_bit(priv->mtu, reg, bitpos, set);
}

static u8 rz_mtu3_pwm_tior(u16 pv, u16 dc)
{
	/*
	 * At 0% duty, the line is toggled high by the period match, and then
	 * toggled low by the duty match one cycle later, causing a spike.
	 * Output constant low for 0% duty.
	 */
	if (dc == 0)
		return RZ_MTU3_TIOR_CONST_LOW;

	/*
	 * At 100% duty, the period and duty compare matches occur at the same
	 * time, in which case the output does not change.
	 * See RZ/T2H User Manual Figure 18.29.
	 */
	if (dc >= pv)
		return RZ_MTU3_TIOR_CONST_HIGH;

	return RZ_MTU3_TIOR_OC_IOB_L_COMP_MATCH | RZ_MTU3_TIOR_OC_IOA_H_COMP_MATCH;
}

static void rz_mtu3_pwm_set_tior(struct rz_mtu3_pwm_channel *priv, u32 hwpwm,
				 u16 pv, u16 dc)
{
	u8 val = rz_mtu3_pwm_tior(pv, dc);

	if (rz_mtu3_hwpwm_is_primary(hwpwm))
		rz_mtu3_8bit_ch_write(priv->mtu, RZ_MTU3_TIORH, val);
	else
		rz_mtu3_8bit_ch_write(priv->mtu, RZ_MTU3_TIORL, val);
}

static int rz_mtu3_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct rz_mtu3_pwm_chip *rz_mtu3_pwm = to_rz_mtu3_pwm_chip(chip);
	struct rz_mtu3_pwm_channel *priv;
	int rc;

	rc = pm_runtime_resume_and_get(pwmchip_parent(chip));
	if (rc)
		return rc;

	priv = rz_mtu3_get_channel(rz_mtu3_pwm, pwm->hwpwm);

	rz_mtu3_8bit_ch_write(priv->mtu, RZ_MTU3_TMDR1, RZ_MTU3_TMDR1_MD_PWMMODE1);

	if (!priv->enable_count)
		rz_mtu3_enable(priv->mtu);

	priv->enable_count++;

	return 0;
}

static void rz_mtu3_pwm_disable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct rz_mtu3_pwm_chip *rz_mtu3_pwm = to_rz_mtu3_pwm_chip(chip);
	struct rz_mtu3_pwm_channel *priv;

	priv = rz_mtu3_get_channel(rz_mtu3_pwm, pwm->hwpwm);

	/* Disable output pins of MTU3 channel */
	if (rz_mtu3_hwpwm_is_primary(pwm->hwpwm))
		rz_mtu3_8bit_ch_write(priv->mtu, RZ_MTU3_TIORH, RZ_MTU3_TIOR_OC_RETAIN);
	else
		rz_mtu3_8bit_ch_write(priv->mtu, RZ_MTU3_TIORL, RZ_MTU3_TIOR_OC_RETAIN);

	rz_mtu3_pwm_set_toer_bit(rz_mtu3_pwm, pwm->hwpwm, false);

	priv->enable_count--;
	if (!priv->enable_count)
		rz_mtu3_disable(priv->mtu);

	pm_runtime_put_sync(pwmchip_parent(chip));
}

static int rz_mtu3_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
				 struct pwm_state *state)
{
	struct rz_mtu3_pwm_chip *rz_mtu3_pwm = to_rz_mtu3_pwm_chip(chip);
	int rc;

	rc = pm_runtime_resume_and_get(pwmchip_parent(chip));
	if (rc)
		return rc;

	state->enabled = rz_mtu3_pwm_is_ch_enabled(rz_mtu3_pwm, pwm->hwpwm);
	if (state->enabled) {
		struct rz_mtu3_pwm_channel *priv;
		u8 prescale, tpsc, tpsc2, val;
		u16 dc, pv;
		u64 tmp;

		priv = rz_mtu3_get_channel(rz_mtu3_pwm, pwm->hwpwm);
		if (rz_mtu3_hwpwm_is_primary(pwm->hwpwm)) {
			rz_mtu3_pwm_read_tgr_registers(priv, RZ_MTU3_TGRA, &pv,
						       RZ_MTU3_TGRB, &dc);
			val = rz_mtu3_8bit_ch_read(priv->mtu, RZ_MTU3_TIORH);
		} else {
			rz_mtu3_pwm_read_tgr_registers(priv, RZ_MTU3_TGRC, &pv,
						       RZ_MTU3_TGRD, &dc);
			val = rz_mtu3_8bit_ch_read(priv->mtu, RZ_MTU3_TIORL);
		}

		if (val == RZ_MTU3_TIOR_CONST_HIGH)
			dc = pv;

		val = rz_mtu3_8bit_ch_read(priv->mtu, RZ_MTU3_TCR);
		tpsc = FIELD_GET(RZ_MTU3_TCR_TPCS, val);

		val = rz_mtu3_8bit_ch_read(priv->mtu, RZ_MTU3_TCR2);
		tpsc2 = FIELD_GET(RZ_MTU3_TCR2_TPSC2, val);

		rc = rz_mtu3_pwm_tpsc_to_prescale(priv, tpsc, tpsc2, &prescale);
		if (rc) {
			pm_runtime_put(pwmchip_parent(chip));
			return rc;
		}

		/* With prescale <= 10 and pv <= 0xffff this doesn't overflow. */
		tmp = NSEC_PER_SEC * (u64)pv << prescale;
		state->period = DIV_ROUND_UP_ULL(tmp, rz_mtu3_pwm->rate);
		tmp = NSEC_PER_SEC * (u64)dc << prescale;
		state->duty_cycle = DIV_ROUND_UP_ULL(tmp, rz_mtu3_pwm->rate);

		if (state->duty_cycle > state->period)
			state->duty_cycle = state->period;
	}

	state->polarity = PWM_POLARITY_NORMAL;
	pm_runtime_put(pwmchip_parent(chip));

	return 0;
}

static u16 rz_mtu3_pwm_calculate_pv_or_dc(u64 period_or_duty_cycle, u8 prescale)
{
	return min(period_or_duty_cycle >> prescale, (u64)U16_MAX);
}

static int rz_mtu3_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
			      const struct pwm_state *state)
{
	struct rz_mtu3_pwm_chip *rz_mtu3_pwm = to_rz_mtu3_pwm_chip(chip);
	struct rz_mtu3_pwm_channel *priv;
	u64 period_cycles;
	u64 duty_cycles;
	u8 tpsc, tpsc2;
	u8 prescale;
	u16 pv, dc;
	u16 dc_reg;
	int rc;

	priv = rz_mtu3_get_channel(rz_mtu3_pwm, pwm->hwpwm);

	period_cycles = mul_u64_u32_div(state->period, rz_mtu3_pwm->rate,
					NSEC_PER_SEC);

	/*
	 * The counter is shared by all IOs of a HW channel, and we cannot clear
	 * it from multiple sources, as the TCR register for each HW channel can
	 * only select one clearing source between TGRA, TGRB, TGRC, and TGRD.
	 * Enforce that all IOs use the same period cycle.
	 *
	 * enable_count includes this IO only if it is already enabled.
	 * pwm->state.enabled indicates whether this PWM is already enabled.
	 * Compare them to determine whether the other IO is enabled.
	 */
	if (priv->enable_count > pwm->state.enabled) {
		if (priv->period_cycles > period_cycles)
			return -EBUSY;

		period_cycles = priv->period_cycles;
	}

	prescale = rz_mtu3_pwm_calculate_prescale(period_cycles);
	pv = rz_mtu3_pwm_calculate_pv_or_dc(period_cycles, prescale);

	duty_cycles = mul_u64_u32_div(state->duty_cycle, rz_mtu3_pwm->rate,
				      NSEC_PER_SEC);
	if (duty_cycles > period_cycles)
		duty_cycles = period_cycles;
	dc = rz_mtu3_pwm_calculate_pv_or_dc(duty_cycles, prescale);

	rc = rz_mtu3_pwm_prescale_to_tpsc(priv, prescale, &tpsc, &tpsc2);
	if (rc)
		return rc;

	/*
	 * If the PWM channel is disabled, make sure to turn on the clock
	 * before writing the register.
	 */
	if (!pwm->state.enabled) {
		rc = pm_runtime_resume_and_get(pwmchip_parent(chip));
		if (rc)
			return rc;
	}

	/* Counter must be stopped while updating TCR register */
	if (priv->prescale != prescale && priv->enable_count)
		rz_mtu3_disable(priv->mtu);

	rz_mtu3_8bit_ch_write(priv->mtu, RZ_MTU3_TCR,
			      RZ_MTU3_TCR_CCLR_TGRA |
			      RZ_MTU3_TCR_CKEG_RISING |
			      FIELD_PREP(RZ_MTU3_TCR_TPCS, tpsc));
	rz_mtu3_8bit_ch_write(priv->mtu, RZ_MTU3_TCR2,
			      FIELD_PREP(RZ_MTU3_TCR2_TPSC2, tpsc2));

	/*
	 * At 100% duty, the period and duty compare matches occur at the same
	 * time, in which case they are ignored. Limit duty to be one less than
	 * period to avoid it.
	 */
	dc_reg = dc;
	if (dc_reg >= pv && pv != 0)
		dc_reg = pv - 1;

	if (rz_mtu3_hwpwm_is_primary(pwm->hwpwm)) {
		rz_mtu3_pwm_write_tgr_registers(priv, RZ_MTU3_TGRA, pv,
						RZ_MTU3_TGRB, dc_reg);
	} else {
		/* TGRA is used to reset the counter for both IOs. */
		rz_mtu3_16bit_ch_write(priv->mtu, RZ_MTU3_TGRA, pv);
		rz_mtu3_pwm_write_tgr_registers(priv, RZ_MTU3_TGRC, pv,
						RZ_MTU3_TGRD, dc_reg);
	}

	/*
	 * The counter keeps its value after a disable. Clear it before
	 * enabling, otherwise counting will start mid-cycle and it will
	 * generate a malformed first cycle.
	 */
	if (!priv->enable_count)
		rz_mtu3_16bit_ch_write(priv->mtu, RZ_MTU3_TCNT, 0);

	/*
	 * TOERA/TOERB must be set before TIOR, see RZ/T2H User Manual Section
	 * 18.3.23.
	 */
	rz_mtu3_pwm_set_toer_bit(rz_mtu3_pwm, pwm->hwpwm, true);
	rz_mtu3_pwm_set_tior(priv, pwm->hwpwm, pv, dc);

	if (priv->prescale != prescale) {
		/*
		 * Prescalar is shared by multiple channels, we cache the
		 * prescalar value from first enabled channel and use the same
		 * value for both channels.
		 */
		priv->prescale = prescale;

		if (priv->enable_count)
			rz_mtu3_enable(priv->mtu);
	}

	priv->period_cycles = period_cycles;

	/* If the PWM is not enabled, turn the clock off again to save power. */
	if (!pwm->state.enabled)
		pm_runtime_put(pwmchip_parent(chip));

	return 0;
}

static int rz_mtu3_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			     const struct pwm_state *state)
{
	struct rz_mtu3_pwm_chip *rz_mtu3_pwm = to_rz_mtu3_pwm_chip(chip);
	bool enabled = pwm->state.enabled;
	int ret;

	if (state->polarity != PWM_POLARITY_NORMAL)
		return -EINVAL;

	guard(mutex)(&rz_mtu3_pwm->lock);

	if (!state->enabled) {
		if (enabled)
			rz_mtu3_pwm_disable(chip, pwm);

		return 0;
	}

	ret = rz_mtu3_pwm_config(chip, pwm, state);
	if (ret)
		return ret;

	if (!enabled)
		ret = rz_mtu3_pwm_enable(chip, pwm);

	return ret;
}

static const struct pwm_ops rz_mtu3_pwm_ops = {
	.request = rz_mtu3_pwm_request,
	.free = rz_mtu3_pwm_free,
	.get_state = rz_mtu3_pwm_get_state,
	.apply = rz_mtu3_pwm_apply,
};

static int rz_mtu3_pwm_probe(struct platform_device *pdev)
{
	struct rz_mtu3 *parent_ddata = dev_get_drvdata(pdev->dev.parent);
	struct rz_mtu3_pwm_chip *rz_mtu3_pwm;
	struct device *dev = &pdev->dev;
	struct pwm_chip *chip;
	unsigned int i, j = 0;
	int ret;

	/*
	 * This MFD sub-device does not have an associated device tree node.
	 * Reuse the parent device tree node to allow the PWM core to retrieve
	 * the PWM chip based on it.
	 */
	device_set_of_node_from_dev(dev, pdev->dev.parent);

	chip = devm_pwmchip_alloc(dev, RZ_MTU3_MAX_PWM_CHANNELS,
				  sizeof(*rz_mtu3_pwm));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	rz_mtu3_pwm = to_rz_mtu3_pwm_chip(chip);

	for (i = 0; i < RZ_MTU_NUM_CHANNELS; i++) {
		if (i == RZ_MTU3_CHAN_5 || i == RZ_MTU3_CHAN_8)
			continue;

		rz_mtu3_pwm->channel_data[j].mtu = &parent_ddata->channels[i];
		j++;
	}

	mutex_init(&rz_mtu3_pwm->lock);
	platform_set_drvdata(pdev, chip);

	ret = devm_clk_rate_exclusive_get(dev, parent_ddata->clk);
	if (ret)
		return ret;

	rz_mtu3_pwm->rate = clk_get_rate(parent_ddata->clk);
	/*
	 * Refuse clk rates > 1 GHz to prevent overflow later for computing
	 * period and duty cycle.
	 */
	if (!rz_mtu3_pwm->rate || rz_mtu3_pwm->rate > NSEC_PER_SEC)
		return -EINVAL;

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	chip->ops = &rz_mtu3_pwm_ops;
	ret = devm_pwmchip_add(dev, chip);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add PWM chip\n");

	return 0;
}

static struct platform_driver rz_mtu3_pwm_driver = {
	.driver = {
		.name = "pwm-rz-mtu3",
	},
	.probe = rz_mtu3_pwm_probe,
};
module_platform_driver(rz_mtu3_pwm_driver);

MODULE_AUTHOR("Biju Das <biju.das.jz@bp.renesas.com>");
MODULE_ALIAS("platform:pwm-rz-mtu3");
MODULE_DESCRIPTION("Renesas RZ/G2L MTU3a PWM Timer Driver");
MODULE_LICENSE("GPL");
