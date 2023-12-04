// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim GMSL2 Serializer Driver
 *
 * Copyright (C) 2023 Analog Devices Inc.
 */

#include "max_ser.h"
#include "max_serdes.h"

static int max_ser_i2c_atr_attach_client(struct i2c_atr *atr, u32 chan_id,
					 const struct i2c_client *client, u16 alias)
{
	struct max_ser_priv *priv = i2c_atr_get_driver_data(atr);
	struct max_ser *ser = priv->ser;
	struct max_i2c_xlate *xlate;

	if (ser->num_i2c_xlates == ser->ops->num_i2c_xlates) {
		dev_err(priv->dev,
			"Reached maximum number of I2C translations\n");
		return -EINVAL;
	}

	xlate = &ser->i2c_xlates[ser->num_i2c_xlates++];
	xlate->src = alias;
	xlate->dst = client->addr;

	return ser->ops->init_i2c_xlate(ser);
}

static void max_ser_i2c_atr_detach_client(struct i2c_atr *atr, u32 chan_id,
					  const struct i2c_client *client)
{
	struct max_ser_priv *priv = i2c_atr_get_driver_data(atr);
	struct max_ser *ser = priv->ser;
	struct max_i2c_xlate *xlate;
	unsigned int i;

	/* Find index of matching I2C translation. */
	for (i = 0; i < ser->num_i2c_xlates; i++) {
		xlate = &ser->i2c_xlates[i];

		if (xlate->dst == client->addr)
			break;
	}

	WARN_ON(i == ser->num_i2c_xlates);

	/* Starting from index + 1, copy index translation into index - 1. */
	for (i++; i < ser->num_i2c_xlates; i++) {
		ser->i2c_xlates[i - 1].src = ser->i2c_xlates[i].src;
		ser->i2c_xlates[i - 1].dst = ser->i2c_xlates[i].dst;
	}

	/* Zero out last index translation. */
	ser->i2c_xlates[ser->num_i2c_xlates].src = 0;
	ser->i2c_xlates[ser->num_i2c_xlates].dst = 0;

	/* Decrease number of translations. */
	ser->num_i2c_xlates--;

	ser->ops->init_i2c_xlate(ser);
}

static const struct i2c_atr_ops max_ser_i2c_atr_ops = {
	.attach_client = max_ser_i2c_atr_attach_client,
	.detach_client = max_ser_i2c_atr_detach_client,
};

int max_ser_i2c_atr_init(struct max_ser_priv *priv)
{
	if (!i2c_check_functionality(priv->client->adapter,
				     I2C_FUNC_SMBUS_WRITE_BYTE_DATA))
		return -ENODEV;

	priv->atr = i2c_atr_new(priv->client->adapter, priv->dev,
				&max_ser_i2c_atr_ops, 1);
	if (IS_ERR(priv->atr))
		return PTR_ERR(priv->atr);

	i2c_atr_set_driver_data(priv->atr, priv);

	return i2c_atr_add_adapter(priv->atr, 0, NULL, NULL);
}

void max_ser_i2c_atr_deinit(struct max_ser_priv *priv)
{
	/* Deleting adapters that haven't been added does no harm. */
	i2c_atr_del_adapter(priv->atr, 0);

	i2c_atr_delete(priv->atr);
}
