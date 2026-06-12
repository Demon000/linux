// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Renesas Electronics Corporation
 */

#include <linux/counter.h>
#include <linux/errno.h>
#include <linux/mfd/rzg2l-gpt.h>
#include <linux/platform_device.h>

/**
 * struct rzg2l_gpt_count - GPT per-count private data
 *
 * @lock: Lock to protect access to count private data
 * @function: Count function
 * @direction: Count direction
 * @ceiling: Count ceiling value
 * @count: Count value
 * @enabled: Count enable value
 */
struct rzg2l_gpt_count {
	struct mutex lock;
	enum counter_function function;
	enum counter_count_direction direction;
	u32 ceiling;
	u32 count;
	bool enabled;
};

/**
 * struct rzg2l_gpt_counter - GPT private data
 *
 * @counts: Array of counts data
 * @gpt: Pointer to GPT core public data
 */
struct rzg2l_gpt_counter {
	struct rzg2l_gpt_count counts[RZG2L_MAX_HW_CHANNELS];
	struct rzg2l_gpt *gpt;
};

static const enum counter_function rzg2l_gpt_count_functions_list[] = {
	COUNTER_FUNCTION_PULSE_DIRECTION,
	COUNTER_FUNCTION_QUADRATURE_X1_A,
	COUNTER_FUNCTION_QUADRATURE_X1_B,
	COUNTER_FUNCTION_QUADRATURE_X2_A,
	COUNTER_FUNCTION_QUADRATURE_X2_B,
	COUNTER_FUNCTION_QUADRATURE_X4,
};

static const enum counter_synapse_action rzg2l_gpt_synapse_actions_list[] = {
	COUNTER_SYNAPSE_ACTION_NONE,
	COUNTER_SYNAPSE_ACTION_RISING_EDGE,
	COUNTER_SYNAPSE_ACTION_FALLING_EDGE,
	COUNTER_SYNAPSE_ACTION_BOTH_EDGES,
};

static enum counter_count_direction
rzg2l_gpt_get_count_direction(struct rzg2l_gpt_counter *const priv,
			      struct counter_count *count)
{
	u32 val = readl(priv->gpt->mmio + RZG2L_GTST(count->id));

	return (val & RZG2L_GTST_TUCF) ? COUNTER_COUNT_DIRECTION_FORWARD
				       : COUNTER_COUNT_DIRECTION_BACKWARD;
}

static void rzg2l_gpt_set_count_ceiling(struct rzg2l_gpt_counter *const priv,
					struct counter_count *count, u32 value)
{
	writel(value, priv->gpt->mmio + RZG2L_GTPR(count->id));
}

static u32 rzg2l_gpt_get_count_value(struct rzg2l_gpt_counter *const priv,
				     struct counter_count *count)
{
	return readl(priv->gpt->mmio + RZG2L_GTCNT(count->id));
}

static void rzg2l_gpt_set_count_enable(struct rzg2l_gpt_counter *const priv,
				       struct counter_count *count, bool enable)
{
	u32 value = readl(priv->gpt->mmio + RZG2L_GTCR(count->id));

	FIELD_MODIFY(RZG2L_GTCR_CST, &value, enable);

	writel(value, priv->gpt->mmio + RZG2L_GTCR(count->id));
}

static void rzg2l_gpt_set_count_value(struct rzg2l_gpt_counter *const priv,
				      struct counter_count *count, u32 value)
{
	struct rzg2l_gpt_count *count_priv = &priv->counts[count->id];

	/*
	 * Writing to the GTCNT register is disabled when CST = 1. Briefly
	 * toggle it if the counter is enabled to write to GTCNT.
	 */
	if (count_priv->enabled)
		rzg2l_gpt_set_count_enable(priv, count, false);

	writel(value, priv->gpt->mmio + RZG2L_GTCNT(count->id));

	if (count_priv->enabled)
		rzg2l_gpt_set_count_enable(priv, count, true);
}

static void rzg2l_gpt_setup_count_mode(struct rzg2l_gpt_counter *const priv,
				       struct counter_count *count)
{
	u32 value = readl(priv->gpt->mmio + RZG2L_GTCR(count->id));

	/*
	 * When GTUPSR or GTDNSR are set to any non-zero value, the counting
	 * operation uses hardware sources and the TPCS value is ignored.
	 *
	 * The counter cannot perform counting for 1 cycle of the clock
	 * specified by TPCS after the CST bit is set to 1 because the counting
	 * operation is synchronized with the TPCS clock.
	 *
	 * See the RZ/G3E User Manual Section 5.7.3.1.1 Counter Operations,
	 * (4) Event Count Operation in Up-counting Using Hardware Sources, and
	 * (5) Event Count Operation in Down-counting Using Hardware Sources.
	 *
	 * Set TPCS to 0 to use the fastest clock.
	 */
	value &= ~priv->gpt->info->gtcr_tpcs;

	FIELD_MODIFY(RZG2L_GTCR_MD, &value, RZG2L_GTCR_MD_SAW_WAVE_PWM_MODE);

	writel(value, priv->gpt->mmio + RZG2L_GTCR(count->id));
}

static void rzg2l_gpt_set_count_function(struct rzg2l_gpt_counter *const priv,
					 struct counter_count *count,
					 enum counter_function function)
{
	u32 up, down;

	switch (function) {
	case COUNTER_FUNCTION_PULSE_DIRECTION:
		up = RZG2L_GTUPDNSR_CARBH;
		down = RZG2L_GTUPDNSR_CARBL;
		break;
	case COUNTER_FUNCTION_QUADRATURE_X1_A:
		up = RZG2L_GTUPDNSR_CARBL;
		down = RZG2L_GTUPDNSR_CAFBL;
		break;
	case COUNTER_FUNCTION_QUADRATURE_X1_B:
		up = RZG2L_GTUPDNSR_CBRAH;
		down = RZG2L_GTUPDNSR_CBFAH;
		break;
	case COUNTER_FUNCTION_QUADRATURE_X2_A:
		up = RZG2L_GTUPDNSR_CARBL | RZG2L_GTUPDNSR_CAFBH;
		down = RZG2L_GTUPDNSR_CARBH | RZG2L_GTUPDNSR_CAFBL;
		break;
	case COUNTER_FUNCTION_QUADRATURE_X2_B:
		up = RZG2L_GTUPDNSR_CBRAH | RZG2L_GTUPDNSR_CBFAL;
		down = RZG2L_GTUPDNSR_CBRAL | RZG2L_GTUPDNSR_CBFAH;
		break;
	case COUNTER_FUNCTION_QUADRATURE_X4:
		up = RZG2L_GTUPDNSR_CARBL | RZG2L_GTUPDNSR_CAFBH |
		     RZG2L_GTUPDNSR_CBRAH | RZG2L_GTUPDNSR_CBFAL;
		down = RZG2L_GTUPDNSR_CARBH | RZG2L_GTUPDNSR_CAFBL |
		       RZG2L_GTUPDNSR_CBRAL | RZG2L_GTUPDNSR_CBFAH;
		break;
	default:
		return;
	}

	writel(up, priv->gpt->mmio + RZG2L_GTUPSR(count->id));
	writel(down, priv->gpt->mmio + RZG2L_GTDNSR(count->id));
}

static int rzg2l_gpt_count_ceiling_read(struct counter_device *counter,
					struct counter_count *count, u64 *ceiling)
{
	struct rzg2l_gpt_counter *const priv = counter_priv(counter);
	struct rzg2l_gpt_count *count_priv = &priv->counts[count->id];

	guard(mutex)(&count_priv->lock);

	*ceiling = count_priv->ceiling;

	return 0;
}

static int rzg2l_gpt_count_ceiling_write(struct counter_device *counter,
					 struct counter_count *count, u64 ceiling)
{
	struct rzg2l_gpt_counter *const priv = counter_priv(counter);
	struct rzg2l_gpt_count *count_priv = &priv->counts[count->id];

	if (ceiling > U32_MAX)
		return -EINVAL;

	guard(mutex)(&count_priv->lock);

	if (count_priv->enabled)
		count_priv->count = rzg2l_gpt_get_count_value(priv, count);

	if (ceiling < count_priv->count)
		return -EINVAL;

	if (count_priv->enabled)
		rzg2l_gpt_set_count_ceiling(priv, count, ceiling);

	count_priv->ceiling = ceiling;

	return 0;
}

static int rzg2l_gpt_count_floor_read(struct counter_device *counter,
				      struct counter_count *count, u64 *floor)
{
	/* Only a floor of 0 is supported. */
	*floor = 0;

	return 0;
}

static int rzg2l_gpt_count_direction_read(struct counter_device *counter,
					  struct counter_count *count,
					  enum counter_count_direction *direction)
{
	struct rzg2l_gpt_counter *const priv = counter_priv(counter);
	struct rzg2l_gpt_count *count_priv = &priv->counts[count->id];

	guard(mutex)(&count_priv->lock);

	if (count_priv->enabled)
		count_priv->direction = rzg2l_gpt_get_count_direction(priv, count);

	*direction = count_priv->direction;

	return 0;
}

static int rzg2l_gpt_count_enable_read(struct counter_device *counter,
				   struct counter_count *count, u8 *enable)
{
	struct rzg2l_gpt_counter *const priv = counter_priv(counter);
	struct rzg2l_gpt_count *count_priv = &priv->counts[count->id];

	guard(mutex)(&count_priv->lock);

	*enable = count_priv->enabled;

	return 0;
}

static int rzg2l_gpt_count_enable_write(struct counter_device *counter,
				    struct counter_count *count, u8 enable)
{
	struct rzg2l_gpt_counter *const priv = counter_priv(counter);
	struct rzg2l_gpt_count *count_priv = &priv->counts[count->id];
	int ret;

	guard(mutex)(&count_priv->lock);
	if (count_priv->enabled == enable)
		return 0;

	if (enable) {
		ret = rzg2l_gpt_request_channel(priv->gpt, count->id);
		if (ret)
			return ret;

		rzg2l_gpt_setup_count_mode(priv, count);
		rzg2l_gpt_set_count_function(priv, count, count_priv->function);
		rzg2l_gpt_set_count_ceiling(priv, count, count_priv->ceiling);
		rzg2l_gpt_set_count_value(priv, count, count_priv->count);
		rzg2l_gpt_set_count_enable(priv, count, 1);
	} else {
		rzg2l_gpt_set_count_enable(priv, count, 0);

		count_priv->count = rzg2l_gpt_get_count_value(priv, count);
		count_priv->direction = rzg2l_gpt_get_count_direction(priv, count);

		rzg2l_gpt_release_channel(priv->gpt, count->id);
	}

	count_priv->enabled = enable;

	return 0;
}

static int rzg2l_gpt_count_read(struct counter_device *counter,
				struct counter_count *count, u64 *val)
{
	struct rzg2l_gpt_counter *const priv = counter_priv(counter);
	struct rzg2l_gpt_count *count_priv = &priv->counts[count->id];

	guard(mutex)(&count_priv->lock);

	if (count_priv->enabled)
		count_priv->count = rzg2l_gpt_get_count_value(priv, count);

	*val = count_priv->count;

	return 0;
}

static int rzg2l_gpt_count_write(struct counter_device *counter,
				 struct counter_count *count, u64 val)
{
	struct rzg2l_gpt_counter *const priv = counter_priv(counter);
	struct rzg2l_gpt_count *count_priv = &priv->counts[count->id];

	guard(mutex)(&count_priv->lock);

	if (val > count_priv->ceiling)
		return -EINVAL;

	if (count_priv->enabled)
		rzg2l_gpt_set_count_value(priv, count, val);

	count_priv->count = val;

	return 0;
}

static int rzg2l_gpt_count_function_read(struct counter_device *counter,
					 struct counter_count *count,
					 enum counter_function *function)
{
	struct rzg2l_gpt_counter *const priv = counter_priv(counter);
	struct rzg2l_gpt_count *count_priv = &priv->counts[count->id];

	guard(mutex)(&count_priv->lock);

	*function = count_priv->function;

	return 0;
}

static int rzg2l_gpt_count_function_write(struct counter_device *counter,
					  struct counter_count *count,
					  enum counter_function function)
{
	struct rzg2l_gpt_counter *const priv = counter_priv(counter);
	struct rzg2l_gpt_count *count_priv = &priv->counts[count->id];

	guard(mutex)(&count_priv->lock);

	if (count_priv->enabled)
		rzg2l_gpt_set_count_function(priv, count, function);

	count_priv->function = function;

	return 0;
}

static int rzg2l_gpt_action_read(struct counter_device *counter,
				 struct counter_count *count,
				 struct counter_synapse *synapse,
				 enum counter_synapse_action *action)
{
	struct rzg2l_gpt_counter *const priv = counter_priv(counter);
	struct rzg2l_gpt_count *count_priv = &priv->counts[count->id];
	bool signal_a = synapse->signal->id == count->synapses[0].signal->id;
	enum counter_count_direction direction;

	*action = COUNTER_SYNAPSE_ACTION_NONE;

	guard(mutex)(&count_priv->lock);

	if (count_priv->enabled)
		count_priv->direction = rzg2l_gpt_get_count_direction(priv, count);

	direction = count_priv->direction;

	switch (count_priv->function) {
	case COUNTER_FUNCTION_PULSE_DIRECTION:
		if (signal_a)
			*action = COUNTER_SYNAPSE_ACTION_RISING_EDGE;
		return 0;
	case COUNTER_FUNCTION_QUADRATURE_X1_A:
		if (signal_a) {
			if (direction == COUNTER_COUNT_DIRECTION_FORWARD)
				*action = COUNTER_SYNAPSE_ACTION_RISING_EDGE;
			else
				*action = COUNTER_SYNAPSE_ACTION_FALLING_EDGE;
		}
		return 0;
	case COUNTER_FUNCTION_QUADRATURE_X1_B:
		if (!signal_a) {
			if (direction == COUNTER_COUNT_DIRECTION_FORWARD)
				*action = COUNTER_SYNAPSE_ACTION_RISING_EDGE;
			else
				*action = COUNTER_SYNAPSE_ACTION_FALLING_EDGE;
		}
		return 0;
	case COUNTER_FUNCTION_QUADRATURE_X2_A:
		if (signal_a)
			*action = COUNTER_SYNAPSE_ACTION_BOTH_EDGES;
		return 0;
	case COUNTER_FUNCTION_QUADRATURE_X2_B:
		if (!signal_a)
			*action = COUNTER_SYNAPSE_ACTION_BOTH_EDGES;
		return 0;
	case COUNTER_FUNCTION_QUADRATURE_X4:
		*action = COUNTER_SYNAPSE_ACTION_BOTH_EDGES;
		return 0;
	default:
		return -EINVAL;
	}
}

#define RZG2L_GPT_QUAD_SIGNAL(_id, _name) {	\
	.id = (_id),				\
	.name = (_name),			\
}

static struct counter_signal rzg2l_gpt_signals[] = {
	RZG2L_GPT_QUAD_SIGNAL(0, "Channel 1 Quadrature A"),
	RZG2L_GPT_QUAD_SIGNAL(1, "Channel 1 Quadrature B"),
	RZG2L_GPT_QUAD_SIGNAL(2, "Channel 2 Quadrature A"),
	RZG2L_GPT_QUAD_SIGNAL(3, "Channel 2 Quadrature B"),
	RZG2L_GPT_QUAD_SIGNAL(4, "Channel 3 Quadrature A"),
	RZG2L_GPT_QUAD_SIGNAL(5, "Channel 3 Quadrature B"),
	RZG2L_GPT_QUAD_SIGNAL(6, "Channel 4 Quadrature A"),
	RZG2L_GPT_QUAD_SIGNAL(7, "Channel 4 Quadrature B"),
	RZG2L_GPT_QUAD_SIGNAL(8, "Channel 5 Quadrature A"),
	RZG2L_GPT_QUAD_SIGNAL(9, "Channel 5 Quadrature B"),
	RZG2L_GPT_QUAD_SIGNAL(10, "Channel 6 Quadrature A"),
	RZG2L_GPT_QUAD_SIGNAL(11, "Channel 6 Quadrature B"),
	RZG2L_GPT_QUAD_SIGNAL(12, "Channel 7 Quadrature A"),
	RZG2L_GPT_QUAD_SIGNAL(13, "Channel 7 Quadrature B"),
	RZG2L_GPT_QUAD_SIGNAL(14, "Channel 8 Quadrature A"),
	RZG2L_GPT_QUAD_SIGNAL(15, "Channel 8 Quadrature B"),
};

#define RZG2L_GPT_COUNT_SYNAPSES(_id) {						\
	{									\
		.actions_list = rzg2l_gpt_synapse_actions_list,			\
		.num_actions = ARRAY_SIZE(rzg2l_gpt_synapse_actions_list),	\
		.signal = rzg2l_gpt_signals + 2 * (_id)				\
	},									\
	{									\
		.actions_list = rzg2l_gpt_synapse_actions_list,			\
		.num_actions = ARRAY_SIZE(rzg2l_gpt_synapse_actions_list),	\
		.signal = rzg2l_gpt_signals + 2 * (_id) + 1			\
	},									\
}

static struct counter_synapse rzg2l_gpt_count_synapses[][2] = {
	RZG2L_GPT_COUNT_SYNAPSES(0),
	RZG2L_GPT_COUNT_SYNAPSES(1),
	RZG2L_GPT_COUNT_SYNAPSES(2),
	RZG2L_GPT_COUNT_SYNAPSES(3),
	RZG2L_GPT_COUNT_SYNAPSES(4),
	RZG2L_GPT_COUNT_SYNAPSES(5),
	RZG2L_GPT_COUNT_SYNAPSES(6),
	RZG2L_GPT_COUNT_SYNAPSES(7)
};

static struct counter_comp rzg2l_gpt_count_ext[] = {
	COUNTER_COMP_CEILING(rzg2l_gpt_count_ceiling_read,
			     rzg2l_gpt_count_ceiling_write),
	COUNTER_COMP_FLOOR(rzg2l_gpt_count_floor_read, NULL),
	COUNTER_COMP_DIRECTION(rzg2l_gpt_count_direction_read),
	COUNTER_COMP_ENABLE(rzg2l_gpt_count_enable_read,
			    rzg2l_gpt_count_enable_write),
};

#define RZG2L_GPT_COUNT(_id, _cntname) {					\
	.id = (_id),								\
	.name = (_cntname),							\
	.functions_list = rzg2l_gpt_count_functions_list,			\
	.num_functions = ARRAY_SIZE(rzg2l_gpt_count_functions_list),		\
	.synapses = rzg2l_gpt_count_synapses[(_id)],				\
	.num_synapses =	2,							\
	.ext = rzg2l_gpt_count_ext,						\
	.num_ext = ARRAY_SIZE(rzg2l_gpt_count_ext)				\
}

static struct counter_count rzg2l_gpt_counts[] = {
	RZG2L_GPT_COUNT(0, "Channel 1 Count"),
	RZG2L_GPT_COUNT(1, "Channel 2 Count"),
	RZG2L_GPT_COUNT(2, "Channel 3 Count"),
	RZG2L_GPT_COUNT(3, "Channel 4 Count"),
	RZG2L_GPT_COUNT(4, "Channel 5 Count"),
	RZG2L_GPT_COUNT(5, "Channel 6 Count"),
	RZG2L_GPT_COUNT(6, "Channel 7 Count"),
	RZG2L_GPT_COUNT(7, "Channel 8 Count")
};

static const struct counter_ops rzg2l_gpt_counter_ops = {
	.count_read = rzg2l_gpt_count_read,
	.count_write = rzg2l_gpt_count_write,
	.function_read = rzg2l_gpt_count_function_read,
	.function_write = rzg2l_gpt_count_function_write,
	.action_read = rzg2l_gpt_action_read,
};

static int rzg2l_gpt_counter_probe(struct platform_device *pdev)
{
	struct rzg2l_gpt *ddata = dev_get_drvdata(pdev->dev.parent);
	struct device *dev = &pdev->dev;
	struct counter_device *counter;
	struct rzg2l_gpt_counter *priv;
	unsigned int i;

	if (ddata->num_channels > ARRAY_SIZE(rzg2l_gpt_counts) ||
	    ddata->num_channels * 2 > ARRAY_SIZE(rzg2l_gpt_signals))
		return -E2BIG;

	counter = devm_counter_alloc(dev, sizeof(*priv));
	if (!counter)
		return -ENOMEM;

	priv = counter_priv(counter);
	priv->gpt = ddata;

	for (i = 0; i < ddata->num_channels; i++) {
		mutex_init(&priv->counts[i].lock);
		priv->counts[i].function = COUNTER_FUNCTION_QUADRATURE_X4;
		priv->counts[i].ceiling = U32_MAX;
	}

	counter->name = dev_name(dev);
	counter->parent = dev;
	counter->ops = &rzg2l_gpt_counter_ops;
	counter->counts = rzg2l_gpt_counts;
	counter->num_counts = ddata->num_channels;
	counter->signals = rzg2l_gpt_signals;
	counter->num_signals = ddata->num_channels * 2;

	return devm_counter_add(dev, counter);
}

static struct platform_driver rzg2l_gpt_counter_driver = {
	.probe = rzg2l_gpt_counter_probe,
	.driver = {
		.name = "rzg2l-gpt-cnt",
	},
};
module_platform_driver(rzg2l_gpt_counter_driver);

MODULE_AUTHOR("Cosmin Tanislav <cosmin-gabriel.tanislav.xa@renesas.com>");
MODULE_DESCRIPTION("Renesas RZ/G2L General PWM Timer (GPT) Counter Driver");
MODULE_ALIAS("platform:rzg2l-gpt-cnt");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("COUNTER");
