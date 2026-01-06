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
 * - rz_mtu3_channel_io_map table is used to map the PWM channel to the
 *   corresponding HW channel as there are difference in number of IOs
 *   between HW channels.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/limits.h>
#include <linux/mfd/rz-mtu3.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/pwm.h>
#include <linux/time.h>

#define RZ_MTU3_MAX_PWM_CHANNELS	12
#define RZ_MTU3_MAX_HW_CHANNELS		7

/**
 * struct rz_mtu3_pwm_channel - MTU3 pwm channel data
 *
 * @mtu: MTU3 channel data
 */
struct rz_mtu3_pwm_channel {
	struct rz_mtu3_channel *mtu;
	u64 period_cycles;
	u8 enable_count;
	u8 user_count;
	u8 prescale;
};

/**
 * struct rz_mtu3_pwm_chip - MTU3 pwm private data
 *
 * @clk: MTU3 module clock
 * @lock: Lock to prevent concurrent access for usage count
 * @rate: MTU3 clock rate
 * @user_count: MTU3 usage count
 * @enable_count: MTU3 enable count
 * @prescale: MTU3 prescale
 * @channel_data: MTU3 pwm channel data
 */

struct rz_mtu3_pwm_chip {
	struct clk *clk;
	struct mutex lock;
	unsigned long rate;
	struct rz_mtu3_pwm_channel channel_data[RZ_MTU3_MAX_HW_CHANNELS];
};

struct mtu_tpsc {
	u8 tpsc;
	u8 tpsc2;
};

/*
 * The MTU channels are {0..4, 6, 7} and the number of IO on MTU1
 * and MTU2 channel is 1 compared to 2 on others.
 */
static const u8 rz_mtu3_pwm_channel_map[] = { 2, 1, 1, 2, 2, 2, 2 };

/*
 * Register field values extracted from rom RZ/G2L User Manual, Table 16.7,
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
	{ 0b100, 0b000 },
	{ 0b100, 0b000 },
};

static const struct mtu_tpsc rz_mtu3_pwm_tpsc_1024[] = {
	{ 0b000, 0b101 },
	{ 0b000, 0b100 },
	{ 0b111, 0b000 },
	{ 0b101, 0b000 },
	{ 0b101, 0b000 },
	{ 0b101, 0b000 },
	{ 0b101, 0b000 },
};

static inline struct rz_mtu3_pwm_chip *to_rz_mtu3_pwm_chip(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static int rz_mtu3_pwm_prescale_to_tpsc(unsigned int ch, u8 prescale,
					u8 *tpsc, u8 *tpsc2)
{
	if (prescale < ARRAY_SIZE(rz_mtu3_pwm_tpsc)) {
		*tpsc  = rz_mtu3_pwm_tpsc[prescale].tpsc;
		*tpsc2 = rz_mtu3_pwm_tpsc[prescale].tpsc2;
		return 0;
	} else if (prescale == 8) {
		if (ch >= ARRAY_SIZE(rz_mtu3_pwm_tpsc_256))
			return -EINVAL;

		*tpsc  = rz_mtu3_pwm_tpsc_256[ch].tpsc;
		*tpsc2 = rz_mtu3_pwm_tpsc_256[ch].tpsc2;
		return 0;
	} else if (prescale == 10) {
		if (ch >= ARRAY_SIZE(rz_mtu3_pwm_tpsc_1024))
			return -EINVAL;

		*tpsc  = rz_mtu3_pwm_tpsc_1024[ch].tpsc;
		*tpsc2 = rz_mtu3_pwm_tpsc_1024[ch].tpsc2;
		return 0;
	}

	return -EINVAL;
}

static u8 rz_mtu3_pwm_tpsc_to_prescale(unsigned int ch, u8 tpsc, u8 tpsc2,
				       u8 *prescale)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(rz_mtu3_pwm_tpsc); i++) {
		if (rz_mtu3_pwm_tpsc[i].tpsc  == tpsc &&
		    rz_mtu3_pwm_tpsc[i].tpsc2 == tpsc2) {
			*prescale = i;
			return 0;
		}
	}

	if (ch >= ARRAY_SIZE(rz_mtu3_pwm_tpsc_256))
		return -EINVAL;

	if (ch < ARRAY_SIZE(rz_mtu3_pwm_tpsc_256) &&
	    rz_mtu3_pwm_tpsc_256[ch].tpsc  == tpsc &&
	    rz_mtu3_pwm_tpsc_256[ch].tpsc2 == tpsc2) {
		*prescale = 8;
		return 0;
	}

	if (ch < ARRAY_SIZE(rz_mtu3_pwm_tpsc_1024) &&
	    rz_mtu3_pwm_tpsc_1024[ch].tpsc  == tpsc &&
	    rz_mtu3_pwm_tpsc_1024[ch].tpsc2 == tpsc2) {
		*prescale = 8;
		return 0;
	}

	return -EINVAL;
}

static u8 rz_mtu3_pwm_calculate_prescale(u64 period_cycles)
{
	u64 prescaled_period_cycles;
	u8 prescale;

	/*
	 * The hardware only supports a 16 bit counter. Calculate the number of
	 * bits remaining, and use that value to determine the prescale
	 * exponent.
	 * For example, if 1 bit remains, use /2 (2^1) for the prescale,
	 * resulting in a prescale exponent of 1.
	 */
	prescaled_period_cycles = period_cycles >> 16;
	if (!prescaled_period_cycles)
		return 0;

	prescale = fls64(prescaled_period_cycles);

	/*
	 * The hardware does not support /128 (2^7) or /512 (2^9) prescales,
	 * fall back to /256 (2^8) and /1024 (2^10) respectively for them.
	 */
	if (prescale <= 6)
		return prescale;
	else if (prescale <= 8)
		return 8;
	else if (prescale <= 10)
		return 10;

	return prescale;
}

static struct rz_mtu3_pwm_channel *
rz_mtu3_get_channel(struct rz_mtu3_pwm_chip *rz_mtu3_pwm, u32 hwpwm, bool *primary)
{
	unsigned int base_pwm = 0;
	unsigned int ch;

	for (ch = 0; ch < RZ_MTU3_MAX_HW_CHANNELS; ch++) {
		if (primary)
			*primary = base_pwm == hwpwm;

		if (base_pwm + rz_mtu3_pwm_channel_map[ch] > hwpwm)
			break;

		base_pwm += rz_mtu3_pwm_channel_map[ch];
	}

	return &rz_mtu3_pwm->channel_data[ch];
}

static inline unsigned int
rz_mtu3_channel_number(struct rz_mtu3_pwm_chip *rz_mtu3_pwm,
		       struct rz_mtu3_pwm_channel *priv)
{
	return priv - rz_mtu3_pwm->channel_data;
}

static bool rz_mtu3_pwm_is_ch_enabled(struct rz_mtu3_pwm_chip *rz_mtu3_pwm,
				      u32 hwpwm)
{
	struct rz_mtu3_pwm_channel *priv;
	bool is_channel_en;
	bool is_primary;
	u8 val;

	priv = rz_mtu3_get_channel(rz_mtu3_pwm, hwpwm, &is_primary);

	is_channel_en = rz_mtu3_is_enabled(priv->mtu);
	if (!is_channel_en)
		return false;

	if (is_primary)
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

	priv = rz_mtu3_get_channel(rz_mtu3_pwm, pwm->hwpwm, NULL);

	mutex_lock(&rz_mtu3_pwm->lock);
	/*
	 * Each channel must be requested only once, so if the channel
	 * serves two PWMs and the other is already requested, skip over
	 * rz_mtu3_request_channel()
	 */
	if (!priv->user_count) {
		is_mtu3_channel_available = rz_mtu3_request_channel(priv->mtu);
		if (!is_mtu3_channel_available) {
			mutex_unlock(&rz_mtu3_pwm->lock);
			return -EBUSY;
		}
	}

	priv->user_count++;
	mutex_unlock(&rz_mtu3_pwm->lock);

	return 0;
}

static void rz_mtu3_pwm_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct rz_mtu3_pwm_chip *rz_mtu3_pwm = to_rz_mtu3_pwm_chip(chip);
	struct rz_mtu3_pwm_channel *priv;

	priv = rz_mtu3_get_channel(rz_mtu3_pwm, pwm->hwpwm, NULL);

	mutex_lock(&rz_mtu3_pwm->lock);
	priv->user_count--;
	if (!priv->user_count)
		rz_mtu3_release_channel(priv->mtu);

	mutex_unlock(&rz_mtu3_pwm->lock);
}

static int rz_mtu3_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct rz_mtu3_pwm_chip *rz_mtu3_pwm = to_rz_mtu3_pwm_chip(chip);
	struct rz_mtu3_pwm_channel *priv;
	bool is_primary;
	u8 val;
	int rc;

	rc = pm_runtime_resume_and_get(pwmchip_parent(chip));
	if (rc)
		return rc;

	priv = rz_mtu3_get_channel(rz_mtu3_pwm, pwm->hwpwm, &is_primary);

	val = RZ_MTU3_TIOR_OC_IOB_L_COMP_MATCH | RZ_MTU3_TIOR_OC_IOA_H_COMP_MATCH;

	rz_mtu3_8bit_ch_write(priv->mtu, RZ_MTU3_TMDR1, RZ_MTU3_TMDR1_MD_PWMMODE1);
	if (is_primary)
		rz_mtu3_8bit_ch_write(priv->mtu, RZ_MTU3_TIORH, val);
	else
		rz_mtu3_8bit_ch_write(priv->mtu, RZ_MTU3_TIORL, val);

	mutex_lock(&rz_mtu3_pwm->lock);
	if (!priv->enable_count)
		rz_mtu3_enable(priv->mtu);

	priv->enable_count++;
	mutex_unlock(&rz_mtu3_pwm->lock);

	return 0;
}

static void rz_mtu3_pwm_disable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct rz_mtu3_pwm_chip *rz_mtu3_pwm = to_rz_mtu3_pwm_chip(chip);
	struct rz_mtu3_pwm_channel *priv;
	bool is_primary;

	priv = rz_mtu3_get_channel(rz_mtu3_pwm, pwm->hwpwm, &is_primary);

	/* Disable output pins of MTU3 channel */
	if (is_primary)
		rz_mtu3_8bit_ch_write(priv->mtu, RZ_MTU3_TIORH, RZ_MTU3_TIOR_OC_RETAIN);
	else
		rz_mtu3_8bit_ch_write(priv->mtu, RZ_MTU3_TIORL, RZ_MTU3_TIOR_OC_RETAIN);

	mutex_lock(&rz_mtu3_pwm->lock);
	priv->enable_count--;
	if (!priv->enable_count)
		rz_mtu3_disable(priv->mtu);

	mutex_unlock(&rz_mtu3_pwm->lock);

	pm_runtime_put_sync(pwmchip_parent(chip));
}

static u64 rz_mtu3_pwm_calculate_ns(struct rz_mtu3_pwm_chip *rz_mtu3_pwm,
				    u16 value, u8 prescale)
{
	return mul_u64_u32_div((u64)value << prescale, NSEC_PER_SEC,
			       rz_mtu3_pwm->rate);
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
		bool is_primary;
		u16 dc, pv;
		u32 ch;

		priv = rz_mtu3_get_channel(rz_mtu3_pwm, pwm->hwpwm, &is_primary);
		ch = rz_mtu3_channel_number(rz_mtu3_pwm, priv);
		pv = rz_mtu3_16bit_ch_read(priv->mtu, RZ_MTU3_TGRA);

		if (is_primary)
			dc = rz_mtu3_16bit_ch_read(priv->mtu, RZ_MTU3_TGRB);
		else
			dc = rz_mtu3_16bit_ch_read(priv->mtu, RZ_MTU3_TGRD);

		val = rz_mtu3_8bit_ch_read(priv->mtu, RZ_MTU3_TCR);
		tpsc = FIELD_GET(RZ_MTU3_TCR_TPSC, val);

		val = rz_mtu3_8bit_ch_read(priv->mtu, RZ_MTU3_TCR2);
		tpsc2 = FIELD_GET(RZ_MTU3_TCR2_TPSC2, val);

		rc = rz_mtu3_pwm_tpsc_to_prescale(ch, tpsc, tpsc2, &prescale);
		if (rc)
			return rc;

		state->period = rz_mtu3_pwm_calculate_ns(rz_mtu3_pwm, pv, prescale);
		state->duty_cycle = rz_mtu3_pwm_calculate_ns(rz_mtu3_pwm, dc, prescale);

		pr_err("%s: ch: %u\n", __func__, ch);
		pr_err("%s: rate: %lu\n", __func__, rz_mtu3_pwm->rate);
		pr_err("%s: state->period: %llu\n", __func__, state->period);
		pr_err("%s: state->duty_cycle: %llu\n", __func__, state->duty_cycle);
		pr_err("%s: priv->period_cycles: %llu\n", __func__, priv->period_cycles);
		pr_err("%s: priv->prescale: %u\n", __func__, priv->prescale);
		pr_err("%s: prescale: %u\n", __func__, prescale);
		pr_err("%s: pv: %u\n", __func__, pv);
		pr_err("%s: dc: %u\n", __func__, dc);

		if (state->duty_cycle > state->period)
			state->duty_cycle = state->period;
	}

	state->polarity = PWM_POLARITY_NORMAL;
	pm_runtime_put(pwmchip_parent(chip));

	return 0;
}

static u16 rz_mtu3_pwm_calculate_pv_or_dc(u64 period_or_duty_cycle, u8 prescale)
{
	return period_or_duty_cycle >> prescale;
}

static int rz_mtu3_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
			      const struct pwm_state *state)
{
	struct rz_mtu3_pwm_chip *rz_mtu3_pwm = to_rz_mtu3_pwm_chip(chip);
	struct rz_mtu3_pwm_channel *priv;
	u64 period_cycles;
	u64 duty_cycles;
	bool is_primary;
	u8 prescale;
	u16 pv, dc;
	int rc;
	u32 ch;

	priv = rz_mtu3_get_channel(rz_mtu3_pwm, pwm->hwpwm, &is_primary);
	ch = rz_mtu3_channel_number(rz_mtu3_pwm, priv);

	period_cycles = mul_u64_u32_div(state->period, rz_mtu3_pwm->rate,
					NSEC_PER_SEC);
	prescale = rz_mtu3_pwm_calculate_prescale(period_cycles);

	/*
	 * The counter is shared by all IOs of a HW channel, and we cannot clear
	 * it from multiple sources, as the TCR register for each HW channel can
	 * only select one clearing source between TGRA, TGRB, TGRC, and TGRD.
	 * Enforce that all IOs use the same period cycle.
	 */
	if (priv->enable_count + !pwm->state.enabled > 1 &&
	    priv->period_cycles != period_cycles)
		return -EBUSY;

	pv = rz_mtu3_pwm_calculate_pv_or_dc(period_cycles, prescale);

	duty_cycles = mul_u64_u32_div(state->duty_cycle, rz_mtu3_pwm->rate,
				      NSEC_PER_SEC);
	dc = rz_mtu3_pwm_calculate_pv_or_dc(duty_cycles, prescale);

	rc = pm_runtime_resume_and_get(pwmchip_parent(chip));
	if (rc)
		return rc;

	pr_err("%s: ch: %u\n", __func__, ch);
	pr_err("%s: rate: %lu\n", __func__, rz_mtu3_pwm->rate);
	pr_err("%s: state->period: %llu\n", __func__, state->period);
	pr_err("%s: state->duty_cycle: %llu\n", __func__, state->duty_cycle);
	pr_err("%s: priv->period_cycles: %llu\n", __func__, priv->period_cycles);
	pr_err("%s: period_cycles: %llu\n", __func__, period_cycles);
	pr_err("%s: duty_cycles: %llu\n", __func__, duty_cycles);
	pr_err("%s: priv->prescale: %u\n", __func__, priv->prescale);
	pr_err("%s: prescale: %u\n", __func__, prescale);
	pr_err("%s: pv: %u\n", __func__, pv);
	pr_err("%s: dc: %u\n", __func__, dc);

	/* Counter must be stopped while updating TCR register */
	if (priv->prescale != prescale) {
		u8 tpsc, tpsc2;

		if (priv->enable_count)
			rz_mtu3_disable(priv->mtu);

		rc = rz_mtu3_pwm_prescale_to_tpsc(ch, prescale, &tpsc, &tpsc2);
		if (rc)
			return rc;

		rz_mtu3_8bit_ch_write(priv->mtu, RZ_MTU3_TCR,
				      RZ_MTU3_TCR_CCLR_TGRA |
				      RZ_MTU3_TCR_CKEG_RISING |
				      FIELD_PREP(RZ_MTU3_TCR_TPSC, tpsc));
		rz_mtu3_8bit_ch_write(priv->mtu, RZ_MTU3_TCR2,
				      FIELD_PREP(RZ_MTU3_TCR2_TPSC2, tpsc2));
	}

	/* TGRA is used to reset the counter for both IOs. */
	if (priv->period_cycles != period_cycles)
		rz_mtu3_16bit_ch_write(priv->mtu, RZ_MTU3_TGRA, pv);

	if (is_primary) {
		rz_mtu3_16bit_ch_write(priv->mtu, RZ_MTU3_TGRB, dc);
	} else {
		rz_mtu3_16bit_ch_write(priv->mtu, RZ_MTU3_TGRC, pv);
		rz_mtu3_16bit_ch_write(priv->mtu, RZ_MTU3_TGRD, dc);
	}

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

	if (!state->enabled) {
		if (enabled)
			rz_mtu3_pwm_disable(chip, pwm);

		return 0;
	}

	mutex_lock(&rz_mtu3_pwm->lock);
	ret = rz_mtu3_pwm_config(chip, pwm, state);
	mutex_unlock(&rz_mtu3_pwm->lock);
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

static int rz_mtu3_pwm_pm_runtime_suspend(struct device *dev)
{
	struct pwm_chip *chip = dev_get_drvdata(dev);
	struct rz_mtu3_pwm_chip *rz_mtu3_pwm = to_rz_mtu3_pwm_chip(chip);

	clk_disable_unprepare(rz_mtu3_pwm->clk);

	return 0;
}

static int rz_mtu3_pwm_pm_runtime_resume(struct device *dev)
{
	struct pwm_chip *chip = dev_get_drvdata(dev);
	struct rz_mtu3_pwm_chip *rz_mtu3_pwm = to_rz_mtu3_pwm_chip(chip);

	return clk_prepare_enable(rz_mtu3_pwm->clk);
}

static DEFINE_RUNTIME_DEV_PM_OPS(rz_mtu3_pwm_pm_ops,
				 rz_mtu3_pwm_pm_runtime_suspend,
				 rz_mtu3_pwm_pm_runtime_resume, NULL);

static int rz_mtu3_pwm_probe(struct platform_device *pdev)
{
	struct rz_mtu3 *parent_ddata = dev_get_drvdata(pdev->dev.parent);
	struct rz_mtu3_pwm_chip *rz_mtu3_pwm;
	struct pwm_chip *chip;
	unsigned int i, j = 0;
	int ret;

	chip = devm_pwmchip_alloc(&pdev->dev, RZ_MTU3_MAX_PWM_CHANNELS,
				  sizeof(*rz_mtu3_pwm));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	rz_mtu3_pwm = to_rz_mtu3_pwm_chip(chip);

	rz_mtu3_pwm->clk = parent_ddata->clk;

	for (i = 0; i < RZ_MTU_NUM_CHANNELS; i++) {
		if (i == RZ_MTU3_CHAN_5 || i == RZ_MTU3_CHAN_8)
			continue;

		rz_mtu3_pwm->channel_data[j].mtu = &parent_ddata->channels[i];
		rz_mtu3_pwm->channel_data[j].prescale = U8_MAX;
		j++;
	}

	mutex_init(&rz_mtu3_pwm->lock);
	platform_set_drvdata(pdev, chip);

	ret = devm_clk_rate_exclusive_get(&pdev->dev, rz_mtu3_pwm->clk);
	if (ret)
		return ret;

	rz_mtu3_pwm->rate = clk_get_rate(rz_mtu3_pwm->clk);
	if (!rz_mtu3_pwm->rate)
		return -EINVAL;

	/*
	 * Refuse clk rates > 1 GHz to prevent overflow later for computing
	 * period and duty cycle.
	 */
	if (rz_mtu3_pwm->rate > NSEC_PER_SEC)
		return -EINVAL;

	ret = devm_pm_runtime_enable(&pdev->dev);
	if (ret)
		return ret;

	chip->ops = &rz_mtu3_pwm_ops;

	return devm_pwmchip_add(&pdev->dev, chip);
}

static struct platform_driver rz_mtu3_pwm_driver = {
	.driver = {
		.name = "pwm-rz-mtu3",
		.pm = pm_ptr(&rz_mtu3_pwm_pm_ops),
	},
	.probe = rz_mtu3_pwm_probe,
};
module_platform_driver(rz_mtu3_pwm_driver);

MODULE_AUTHOR("Biju Das <biju.das.jz@bp.renesas.com>");
MODULE_ALIAS("platform:pwm-rz-mtu3");
MODULE_DESCRIPTION("Renesas RZ/G2L MTU3a PWM Timer Driver");
MODULE_LICENSE("GPL");
