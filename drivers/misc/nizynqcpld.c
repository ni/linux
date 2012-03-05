/*
 * Copyright (C) 2012 National Instruments Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/workqueue.h>


#define NIZYNQCPLD_VERSION		0x00
#define NIZYNQCPLD_PRODUCT		0x1D

struct nizynqcpld_desc {
	u8 supported_version;
	u8 supported_product;
};

struct nizynqcpld {
	struct device *dev;
	struct nizynqcpld_desc *desc;
	struct i2c_client *client;
	struct mutex lock;
};

static int nizynqcpld_write(struct nizynqcpld *cpld, u8 reg, u8 data);
static int nizynqcpld_read(struct nizynqcpld *cpld, u8 reg, u8 *data);

static inline void nizynqcpld_lock(struct nizynqcpld *cpld)
{
	mutex_lock(&cpld->lock);
}
static inline void nizynqcpld_unlock(struct nizynqcpld *cpld)
{
	mutex_unlock(&cpld->lock);
}

static int nizynqcpld_write(struct nizynqcpld *cpld, u8 reg, u8 data)
{
	int err;
	u8 tdata[2] = { reg, data };

	/* write reg byte, then data */
	struct i2c_msg msg = {
		.addr = cpld->client->addr,
		.len  = 2,
		.buf  = tdata,
	};

	err = i2c_transfer(cpld->client->adapter, &msg, 1);

	return err == 1 ? 0 : err;
}

static int nizynqcpld_read(struct nizynqcpld *cpld, u8 reg, u8 *data)
{
	int err;

	/* First, write the CPLD register offset, then read the data. */
	struct i2c_msg msgs[] = {
		{
			.addr	= cpld->client->addr,
			.len	= 1,
			.buf	= &reg,
		},
		{
			.addr	= cpld->client->addr,
			.flags	= I2C_M_RD,
			.len	= 1,
			.buf	= data,
		},
	};

	err = i2c_transfer(cpld->client->adapter, msgs, ARRAY_SIZE(msgs));

	return err == ARRAY_SIZE(msgs) ? 0 : err;
}

static struct nizynqcpld_desc nizynqcpld_descs[] = {
	/* DosEquis and myRIO development CPLD */
	{
		.supported_version	= 3,
		.supported_product	= 0,
	},
	/* DosEquis and myRIO development CPLD */
	{
		.supported_version	= 4,
		.supported_product	= 0,
	},
	/* DosEquis and myRIO development CPLD */
	{
		.supported_version	= 5,
		.supported_product	= 0,
	},
	/* DosEquis CPLD */
	{
		.supported_version	= 6,
		.supported_product	= 0,
	},
	/* myRIO CPLD */
	{
		.supported_version	= 6,
		.supported_product	= 1,
	},
};

static int nizynqcpld_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	struct nizynqcpld_desc *desc;
	struct nizynqcpld *cpld;
	u8 version;
	u8 product;
	int err;
	int i;

	cpld = kzalloc(sizeof(*cpld), GFP_KERNEL);
	if (!cpld) {
		err = -ENOMEM;
		dev_err(&client->dev, "could not allocate private data.\n");
		goto err_cpld_alloc;
	}

	cpld->dev = &client->dev;
	cpld->client = client;
	mutex_init(&cpld->lock);

	err = nizynqcpld_read(cpld, NIZYNQCPLD_VERSION,
			      &version);
	if (err) {
		dev_err(cpld->dev, "could not read version cpld version.\n");
		goto err_read_info;
	}

	err = nizynqcpld_read(cpld, NIZYNQCPLD_PRODUCT, &product);
	if (err) {
		dev_err(cpld->dev, "could not read cpld product number.\n");
		goto err_read_info;
	}

	for (i = 0, desc = NULL; i < ARRAY_SIZE(nizynqcpld_descs); i++) {
		if (nizynqcpld_descs[i].supported_version == version &&
		    nizynqcpld_descs[i].supported_product == product) {
			desc = &nizynqcpld_descs[i];
			break;
		}
	}

	if (!desc) {
		err = -ENODEV;
		dev_err(cpld->dev,
			"this driver does not support cpld with version %d and"
			" product %d.\n", version, product);
		goto err_no_version;
	}

	cpld->desc = desc;

	i2c_set_clientdata(client, cpld);

	dev_info(&client->dev,
		 "%s NI Zynq-based target CPLD found.\n",
		 client->name);

	return 0;

err_no_version:
err_read_info:
	kfree(cpld);
err_cpld_alloc:
	return err;
}

static int nizynqcpld_remove(struct i2c_client *client)
{
	struct nizynqcpld *cpld = i2c_get_clientdata(client);

	kfree(cpld);
	return 0;
}

static const struct i2c_device_id nizynqcpld_ids[] = {
	{ .name = "nizynqcpld" },
	{ },
};
MODULE_DEVICE_TABLE(i2c, nizynqcpld_ids);

static struct i2c_driver nizynqcpld_driver = {
	.driver = {
		.name		= "nizynqcpld",
		.owner		= THIS_MODULE,
	},
	.probe		= nizynqcpld_probe,
	.remove		= nizynqcpld_remove,
	.id_table	= nizynqcpld_ids,
};

static int __init nizynqcpld_init(void)
{
	return i2c_add_driver(&nizynqcpld_driver);
}
module_init(nizynqcpld_init);

static void __exit nizynqcpld_exit(void)
{
	i2c_del_driver(&nizynqcpld_driver);
}
module_exit(nizynqcpld_exit);

MODULE_DESCRIPTION("Driver for CPLD on NI's Zynq RIO products");
MODULE_AUTHOR("National Instruments");
MODULE_LICENSE("GPL");
