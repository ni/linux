/*
 * GPIO Testing Device Driver
 *
 * Copyright (C) 2014  Kamlakant Patel <kamlakant.patel@broadcom.com>
 * Copyright (C) 2015-2016  Bamvor Jian Zhang <bamvor.zhangjian@linaro.org>
 * Copyright (C) 2017 Bartosz Golaszewski <brgl@bgdev.pl>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/consumer.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irq_sim.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>

#include "gpiolib.h"

#define GPIO_MOCKUP_NAME	"gpio-mockup"
#define	GPIO_MOCKUP_MAX_GC	10
/*
 * We're storing two values per chip: the GPIO base and the number
 * of GPIO lines.
 */
#define GPIO_MOCKUP_MAX_RANGES	(GPIO_MOCKUP_MAX_GC * 2)

enum {
	GPIO_MOCKUP_DIR_IN = 0,
	GPIO_MOCKUP_DIR_OUT = 1,
};

/*
 * struct gpio_pin_status - structure describing a GPIO status
 * @dir:       Configures direction of gpio as "in" or "out", 0=in, 1=out
 * @value:     Configures status of the gpio as 0(low) or 1(high)
 */
struct gpio_mockup_line_status {
	int dir;
	bool value;
};

struct gpio_mockup_chip {
	struct gpio_chip gc;
	struct gpio_mockup_line_status *lines;
	struct irq_sim irqsim;
	struct dentry *dbg_dir;
};

struct gpio_mockup_dbgfs_private {
	struct gpio_mockup_chip *chip;
	struct gpio_desc *desc;
	int offset;
};

static int gpio_mockup_ranges[GPIO_MOCKUP_MAX_RANGES];
static int gpio_mockup_params_nr;
module_param_array(gpio_mockup_ranges, int, &gpio_mockup_params_nr, 0400);

static bool gpio_mockup_named_lines;
module_param_named(gpio_mockup_named_lines,
		   gpio_mockup_named_lines, bool, 0400);

static const char gpio_mockup_name_start = 'A';
static struct dentry *gpio_mockup_dbg_dir;

static int gpio_mockup_get(struct gpio_chip *gc, unsigned int offset)
{
	struct gpio_mockup_chip *chip = gpiochip_get_data(gc);

	return chip->lines[offset].value;
}

static void gpio_mockup_set(struct gpio_chip *gc, unsigned int offset,
			    int value)
{
	struct gpio_mockup_chip *chip = gpiochip_get_data(gc);

	chip->lines[offset].value = !!value;
}

static int gpio_mockup_dirout(struct gpio_chip *gc, unsigned int offset,
			      int value)
{
	struct gpio_mockup_chip *chip = gpiochip_get_data(gc);

	gpio_mockup_set(gc, offset, value);
	chip->lines[offset].dir = GPIO_MOCKUP_DIR_OUT;

	return 0;
}

static int gpio_mockup_dirin(struct gpio_chip *gc, unsigned int offset)
{
	struct gpio_mockup_chip *chip = gpiochip_get_data(gc);

	chip->lines[offset].dir = GPIO_MOCKUP_DIR_IN;

	return 0;
}

static int gpio_mockup_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct gpio_mockup_chip *chip = gpiochip_get_data(gc);

	return !chip->lines[offset].dir;
}

static int gpio_mockup_name_lines(struct device *dev,
				  struct gpio_mockup_chip *chip)
{
	struct gpio_chip *gc = &chip->gc;
	char **names;
	int i;

	names = devm_kcalloc(dev, gc->ngpio, sizeof(char *), GFP_KERNEL);
	if (!names)
		return -ENOMEM;

	for (i = 0; i < gc->ngpio; i++) {
		names[i] = devm_kasprintf(dev, GFP_KERNEL,
					  "%s-%d", gc->label, i);
		if (!names[i])
			return -ENOMEM;
	}

	gc->names = (const char *const *)names;

	return 0;
}

static int gpio_mockup_to_irq(struct gpio_chip *gc, unsigned int offset)
{
	struct gpio_mockup_chip *chip = gpiochip_get_data(gc);

	return irq_sim_irqnum(&chip->irqsim, offset);
}

static ssize_t gpio_mockup_event_write(struct file *file,
				       const char __user *usr_buf,
				       size_t size, loff_t *ppos)
{
	struct gpio_mockup_dbgfs_private *priv;
	struct gpio_mockup_chip *chip;
	struct seq_file *sfile;
	struct gpio_desc *desc;
	int rv, val;

	rv = kstrtoint_from_user(usr_buf, size, 0, &val);
	if (rv)
		return rv;
	if (val != 0 && val != 1)
		return -EINVAL;

	sfile = file->private_data;
	priv = sfile->private;
	desc = priv->desc;
	chip = priv->chip;

	gpiod_set_value_cansleep(desc, val);
	irq_sim_fire(&chip->irqsim, priv->offset);

	return size;
}

static int gpio_mockup_event_open(struct inode *inode, struct file *file)
{
	return single_open(file, NULL, inode->i_private);
}

static const struct file_operations gpio_mockup_event_ops = {
	.owner = THIS_MODULE,
	.open = gpio_mockup_event_open,
	.write = gpio_mockup_event_write,
	.llseek = no_llseek,
};

static void gpio_mockup_debugfs_setup(struct device *dev,
				      struct gpio_mockup_chip *chip)
{
	struct gpio_mockup_dbgfs_private *priv;
	struct dentry *evfile;
	struct gpio_chip *gc;
	char *name;
	int i;

	gc = &chip->gc;

	chip->dbg_dir = debugfs_create_dir(gc->label, gpio_mockup_dbg_dir);
	if (!chip->dbg_dir)
		goto err;

	for (i = 0; i < gc->ngpio; i++) {
		name = devm_kasprintf(dev, GFP_KERNEL, "%d", i);
		if (!name)
			goto err;

		priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
		if (!priv)
			goto err;

		priv->chip = chip;
		priv->offset = i;
		priv->desc = &gc->gpiodev->descs[i];

		evfile = debugfs_create_file(name, 0200, chip->dbg_dir, priv,
					     &gpio_mockup_event_ops);
		if (!evfile)
			goto err;
	}

	return;

err:
	dev_err(dev, "error creating debugfs directory\n");
}

static int gpio_mockup_add(struct device *dev,
			   struct gpio_mockup_chip *chip,
			   const char *name, int base, int ngpio)
{
	struct gpio_chip *gc = &chip->gc;
	int ret;

	gc->base = base;
	gc->ngpio = ngpio;
	gc->label = name;
	gc->owner = THIS_MODULE;
	gc->parent = dev;
	gc->get = gpio_mockup_get;
	gc->set = gpio_mockup_set;
	gc->direction_output = gpio_mockup_dirout;
	gc->direction_input = gpio_mockup_dirin;
	gc->get_direction = gpio_mockup_get_direction;
	gc->to_irq = gpio_mockup_to_irq;

	chip->lines = devm_kcalloc(dev, gc->ngpio,
				   sizeof(*chip->lines), GFP_KERNEL);
	if (!chip->lines)
		return -ENOMEM;

	if (gpio_mockup_named_lines) {
		ret = gpio_mockup_name_lines(dev, chip);
		if (ret)
			return ret;
	}

	ret = devm_irq_sim_init(dev, &chip->irqsim, gc->ngpio);
	if (ret)
		return ret;

	ret = devm_gpiochip_add_data(dev, &chip->gc, chip);
	if (ret)
		return ret;

	if (gpio_mockup_dbg_dir)
		gpio_mockup_debugfs_setup(dev, chip);

	return 0;
}

static int gpio_mockup_probe(struct platform_device *pdev)
{
	int ret, i, base, ngpio, num_chips;
	struct device *dev = &pdev->dev;
	struct gpio_mockup_chip *chips;
	char *chip_name;

	if (gpio_mockup_params_nr < 2 || (gpio_mockup_params_nr % 2))
		return -EINVAL;

	/* Each chip is described by two values. */
	num_chips = gpio_mockup_params_nr / 2;

	chips = devm_kcalloc(dev, num_chips, sizeof(*chips), GFP_KERNEL);
	if (!chips)
		return -ENOMEM;

	platform_set_drvdata(pdev, chips);

	for (i = 0; i < num_chips; i++) {
		base = gpio_mockup_ranges[i * 2];

		if (base == -1)
			ngpio = gpio_mockup_ranges[i * 2 + 1];
		else
			ngpio = gpio_mockup_ranges[i * 2 + 1] - base;

		if (ngpio >= 0) {
			chip_name = devm_kasprintf(dev, GFP_KERNEL,
						   "%s-%c", GPIO_MOCKUP_NAME,
						   gpio_mockup_name_start + i);
			if (!chip_name)
				return -ENOMEM;

			ret = gpio_mockup_add(dev, &chips[i],
					      chip_name, base, ngpio);
		} else {
			ret = -EINVAL;
		}

		if (ret) {
			dev_err(dev,
				"adding gpiochip failed: %d (base: %d, ngpio: %d)\n",
				ret, base, base < 0 ? ngpio : base + ngpio);

			return ret;
		}
	}

	return 0;
}

static struct platform_driver gpio_mockup_driver = {
	.driver = {
		.name = GPIO_MOCKUP_NAME,
	},
	.probe = gpio_mockup_probe,
};

static struct platform_device *pdev;
static int __init mock_device_init(void)
{
	int err;

	gpio_mockup_dbg_dir = debugfs_create_dir("gpio-mockup-event", NULL);
	if (!gpio_mockup_dbg_dir)
		pr_err("%s: error creating debugfs directory\n",
		       GPIO_MOCKUP_NAME);

	pdev = platform_device_alloc(GPIO_MOCKUP_NAME, -1);
	if (!pdev)
		return -ENOMEM;

	err = platform_device_add(pdev);
	if (err) {
		platform_device_put(pdev);
		return err;
	}

	err = platform_driver_register(&gpio_mockup_driver);
	if (err) {
		platform_device_unregister(pdev);
		debugfs_remove_recursive(gpio_mockup_dbg_dir);
		return err;
	}

	return 0;
}

static void __exit mock_device_exit(void)
{
	debugfs_remove_recursive(gpio_mockup_dbg_dir);
	platform_driver_unregister(&gpio_mockup_driver);
	platform_device_unregister(pdev);
}

module_init(mock_device_init);
module_exit(mock_device_exit);

MODULE_AUTHOR("Kamlakant Patel <kamlakant.patel@broadcom.com>");
MODULE_AUTHOR("Bamvor Jian Zhang <bamvor.zhangjian@linaro.org>");
MODULE_AUTHOR("Bartosz Golaszewski <brgl@bgdev.pl>");
MODULE_DESCRIPTION("GPIO Testing driver");
MODULE_LICENSE("GPL v2");
