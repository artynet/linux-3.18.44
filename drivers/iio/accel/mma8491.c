/*
 * mma8491.c - Support for Freescale MMA8491Q 3-axis 12-bit accelerometer
 *
 * Copyright 2014 Dog Hunter SA
 * Author: Aurelio Colosimo <aurelio@aureliocolosimo.it>
 *
 * GNU GPLv2 or later
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/buffer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/delay.h>
#include <linux/gpio.h>

#define MMA8491_STATUS 0x00
#define MMA8491_OUT_X 0x01 /* MSB first, 14-bit  */
#define MMA8491_OUT_Y 0x03
#define MMA8491_OUT_Z 0x05
#define MMA8491_STATUS_DRDY (BIT(2) | BIT(1) | BIT(0))

struct mma8491_data {
	struct i2c_client *client;
	struct mutex lock;
	u8 ctrl_reg1;
	u8 data_cfg;
	unsigned rst_gpio;
};

static int mma8491_drdy(struct mma8491_data *data)
{
	int tries = 150;

	while (tries-- > 0) {
		int ret = i2c_smbus_read_byte_data(data->client,
			MMA8491_STATUS);
		if (ret < 0)
			return ret;
		if ((ret & MMA8491_STATUS_DRDY) == MMA8491_STATUS_DRDY)
			return 0;
		msleep(20);
	}

	dev_err(&data->client->dev, "data not ready\n");
	return -EIO;
}

static int mma8491_read(struct mma8491_data *data, __be16 buf[3])
{
	int ret = mma8491_drdy(data);
	if (ret < 0)
		return ret;
	return i2c_smbus_read_i2c_block_data(data->client,
		MMA8491_OUT_X, 3 * sizeof(__be16), (u8 *) buf);
}

static int mma8491_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct mma8491_data *data = iio_priv(indio_dev);
	__be16 buffer[3];
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (iio_buffer_enabled(indio_dev))
			return -EBUSY;

		gpio_set_value_cansleep(data->rst_gpio, 1);
		mutex_lock(&data->lock);
		ret = mma8491_read(data, buffer);
		mutex_unlock(&data->lock);
		gpio_set_value_cansleep(data->rst_gpio, 0);
		if (ret < 0)
			return ret;
		*val = sign_extend32(
			be16_to_cpu(buffer[chan->scan_index]) >> 2, 13);
		return IIO_VAL_INT;
	}
	return -EINVAL;
}

static irqreturn_t mma8491_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct mma8491_data *data = iio_priv(indio_dev);
	u8 buffer[16]; /* 3 16-bit channels + padding + ts */
	int ret;

	ret = mma8491_read(data, (__be16 *) buffer);
	if (ret < 0)
		goto done;

	iio_push_to_buffers_with_timestamp(indio_dev, buffer,
		iio_get_time_ns());

done:
	iio_trigger_notify_done(indio_dev->trig);
	return IRQ_HANDLED;
}

#define MMA8491_CHANNEL(axis, idx) { \
	.type = IIO_ACCEL, \
	.modified = 1, \
	.channel2 = IIO_MOD_##axis, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | \
		BIT(IIO_CHAN_INFO_CALIBBIAS), \
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SAMP_FREQ) | \
		BIT(IIO_CHAN_INFO_SCALE), \
	.scan_index = idx, \
	.scan_type = { \
		.sign = 's', \
		.realbits = 12, \
		.storagebits = 16, \
		.shift = 4, \
		.endianness = IIO_BE, \
	}, \
}

static const struct iio_chan_spec mma8491_channels[] = {
	MMA8491_CHANNEL(X, 0),
	MMA8491_CHANNEL(Y, 1),
	MMA8491_CHANNEL(Z, 2),
	IIO_CHAN_SOFT_TIMESTAMP(3),
};

static const struct iio_info mma8491_info = {
	.read_raw = &mma8491_read_raw,
	.driver_module = THIS_MODULE,
};

static const unsigned long mma8491_scan_masks[] = {0x7, 0};

static int mma8491_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct mma8491_data *data;
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->client = client;
	data->rst_gpio = *((unsigned*)dev_get_platdata(&client->dev));
	mutex_init(&data->lock);

	i2c_set_clientdata(client, indio_dev);
	indio_dev->info = &mma8491_info;
	indio_dev->name = id->name;
	indio_dev->dev.parent = &client->dev;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = mma8491_channels;
	indio_dev->num_channels = ARRAY_SIZE(mma8491_channels);
	indio_dev->available_scan_masks = mma8491_scan_masks;

	ret = devm_gpio_request_one(&client->dev, data->rst_gpio,
				GPIOF_OUT_INIT_LOW,
				"mma8491-reset");

	if (ret) {
		dev_err(&client->dev, "failed to request gpio %d: %d\n",
			data->rst_gpio, ret);
		return ret;
	}

	ret = iio_triggered_buffer_setup(indio_dev, NULL,
		mma8491_trigger_handler, NULL);
	if (ret < 0)
		return ret;

	ret = iio_device_register(indio_dev);
	if (ret < 0)
		goto buffer_cleanup;
	return 0;

buffer_cleanup:
	iio_triggered_buffer_cleanup(indio_dev);
	return ret;
}

static int mma8491_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);

	iio_device_unregister(indio_dev);
	iio_triggered_buffer_cleanup(indio_dev);

	return 0;
}

static const struct i2c_device_id mma8491_id[] = {
	{ "mma8491", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mma8491_id);

static struct i2c_driver mma8491_driver = {
	.driver = {
		.name	= "mma8491",
	},
	.probe = mma8491_probe,
	.remove = mma8491_remove,
	.id_table = mma8491_id,
};
module_i2c_driver(mma8491_driver);

MODULE_AUTHOR("Aurelio Colosimo <aurelio@aureliocolosimo.it>");
MODULE_DESCRIPTION("Freescale MMA8491 accelerometer driver");
MODULE_LICENSE("GPL");
