#include <linux/regmap.h>

#include "max_des.h"
#include "max_serdes.h"

static int max_des_init_link_ser_xlate(struct max_des_priv *priv,
				       struct max_des_link_data *data,
				       u8 power_up_addr, u8 new_addr)
{
	u8 addrs[] = { power_up_addr, new_addr };
	struct i2c_client *client;
	struct regmap *regmap;
	int ret;

	client = i2c_new_dummy_device(priv->client->adapter, power_up_addr);
	if (IS_ERR(client)) {
		ret = PTR_ERR(client);
		dev_err(priv->dev,
			"Failed to create I2C client: %d\n", ret);
		return ret;
	}

	regmap = regmap_init_i2c(client, &max_i2c_regmap);
	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		dev_err(priv->dev,
			"Failed to create I2C regmap: %d\n", ret);
		goto err_unregister_client;
	}

	ret = priv->ops->select_links(priv, BIT(data->index));
	if (ret)
		goto err_regmap_exit;

	ret = max_ser_wait_for_multiple(client, regmap, addrs, ARRAY_SIZE(addrs));
	if (ret) {
		dev_err(priv->dev,
			"Failed waiting for serializer with new or old address: %d\n", ret);
		goto err_regmap_exit;
	}

	ret = max_ser_reset(regmap);
	if (ret) {
		dev_err(priv->dev, "Failed to reset serializer: %d\n", ret);
		goto err_regmap_exit;
	}

	ret = max_ser_wait(client, regmap, power_up_addr);
	if (ret) {
		dev_err(priv->dev,
			"Failed waiting for serializer with new address: %d\n", ret);
		goto err_regmap_exit;
	}

	ret = max_ser_change_address(client, regmap, new_addr, priv->ops->fix_tx_ids);
	if (ret) {
		dev_err(priv->dev, "Failed to change serializer address: %d\n", ret);
		goto err_regmap_exit;
	}

err_regmap_exit:
	regmap_exit(regmap);

err_unregister_client:
	i2c_unregister_device(client);

	return ret;
}

static int max_des_atr_attach_client(struct i2c_atr *atr, u32 chan_id,
				     const struct i2c_client *client, u16 alias)
{
	struct max_des_priv *priv = i2c_atr_get_driver_data(atr);
	struct max_des_link *link = &priv->links[chan_id];
	struct max_des_link_data *data = &link->data;

	if (data->ser_xlate_enabled) {
		dev_err(priv->dev, "Serializer for link %u already bound\n", data->index);
		return -EINVAL;
	}

	data->ser_xlate.src = alias;
	data->ser_xlate.dst = client->addr;
	data->ser_xlate_enabled = true;

	return max_des_init_link_ser_xlate(priv, data, client->addr, alias);
}

static void max_des_atr_detach_client(struct i2c_atr *atr, u32 chan_id,
				      const struct i2c_client *client)
{
	/* Don't do anything. */
}

static const struct i2c_atr_ops max_des_i2c_atr_ops = {
	.attach_client = max_des_atr_attach_client,
	.detach_client = max_des_atr_detach_client,
};

void max_des_i2c_atr_deinit(struct max_des_priv *priv)
{
	unsigned int i;

	for (i = 0; i < priv->ops->num_links; i++) {
		struct max_des_link *link = &priv->links[i];
		struct max_des_link_data *data = &link->data;

		/* Deleting adapters that haven't been added does no harm. */
		i2c_atr_del_adapter(priv->atr, data->index);
	}

	i2c_atr_delete(priv->atr);
}

int max_des_i2c_atr_init(struct max_des_priv *priv)
{
	unsigned int i;
	int ret;

	if (!i2c_check_functionality(priv->client->adapter,
				     I2C_FUNC_SMBUS_WRITE_BYTE_DATA))
		return -ENODEV;

	priv->atr = i2c_atr_new(priv->client->adapter, priv->dev,
				&max_des_i2c_atr_ops, priv->ops->num_links);
	if (IS_ERR(priv->atr))
		return PTR_ERR(priv->atr);

	i2c_atr_set_driver_data(priv->atr, priv);

	for (i = 0; i < priv->ops->num_links; i++) {
		struct max_des_link *link = &priv->links[i];
		struct max_des_link_data *data = &link->data;

		if (!data->enabled)
			continue;

		ret = i2c_atr_add_adapter(priv->atr, data->index, NULL, NULL);
		if (ret)
			goto err_add_adapters;
	}

	return 0;

err_add_adapters:
	max_des_i2c_atr_deinit(priv);

	return ret;
}
