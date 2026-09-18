// SPDX-License-Identifier: GPL-2.0-only
/*
 * GPIO child driver for the CH32V003 USB MFD.
 *
 * This is a platform driver instantiated by the "v003-gpio" MFD cell, exactly
 * like drivers/gpio/gpio-dln2.c hangs off the dln2 core: all it does is turn
 * gpiolib callbacks into v003_cmd_*() calls, which the core maps onto the
 * transport selected by its `transport` parameter.
 *
 * Copyright (C) 2026 embeddedboys
 *
 * Author: Wooden Chair <hua.zheng@embeddedboys.com>
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/gpio/driver.h>
#include <linux/bitmap.h>

#include "usb-mfd.h"

#define DRV_NAME "v003-gpio"

static int v003_gpio_direction_get(struct gpio_chip *gc, unsigned int offset)
{
	struct v003_dev *v003 = gpiochip_get_data(gc);
	u32 state = 0;
	int ret;

	ret = v003_cmd_in(v003, V003_GPIO_GET_DIRECTION,
			       V003_GPIO_VAL(offset, 0), &state);
	if (ret)
		return ret;

	dev_dbg(gc->parent, "%s: offset %u -> %u\n", __func__, offset, state);

	return state > 0 ? GPIO_LINE_DIRECTION_IN : GPIO_LINE_DIRECTION_OUT;
}

static int v003_gpio_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	struct v003_dev *v003 = gpiochip_get_data(gc);

	dev_dbg(gc->parent, "%s: offset %u\n", __func__, offset);

	return v003_cmd_out(v003, V003_GPIO_DIRECTION_INPUT,
				 V003_GPIO_VAL(offset, 0));
}

static int v003_gpio_direction_output(struct gpio_chip *gc, unsigned int offset,
				      int value)
{
	struct v003_dev *v003 = gpiochip_get_data(gc);

	dev_dbg(gc->parent, "%s: offset %u value %d\n", __func__, offset, value);

	return v003_cmd_out(v003, V003_GPIO_DIRECTION_OUTPUT,
				 V003_GPIO_VAL(offset, value));
}

static int v003_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct v003_dev *v003 = gpiochip_get_data(gc);
	u32 state = 0;
	int ret;

	ret = v003_cmd_in(v003, V003_GPIO_GET, V003_GPIO_VAL(offset, 0),
			       &state);
	if (ret)
		return ret;

	dev_dbg(gc->parent, "%s: offset %u -> %u\n", __func__, offset, state);

	return state;
}

static int v003_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct v003_dev *v003 = gpiochip_get_data(gc);

	dev_dbg(gc->parent, "%s: offset %u value %d\n", __func__, offset, value);

	return v003_cmd_out(v003, V003_GPIO_SET,
				 V003_GPIO_VAL(offset, value));
}

static int v003_gpio_request(struct gpio_chip *gc, unsigned int offset)
{
	struct v003_dev *v003 = gpiochip_get_data(gc);

	dev_dbg(gc->parent, "%s: offset %u\n", __func__, offset);

	/* the firmware claims the line and puts it into a safe input mode */
	return v003_cmd_out(v003, V003_GPIO_REQUEST,
				 V003_GPIO_VAL(offset, 0));
}

static void v003_gpio_free(struct gpio_chip *gc, unsigned int offset)
{
	struct v003_dev *v003 = gpiochip_get_data(gc);

	dev_dbg(gc->parent, "%s: offset %u\n", __func__, offset);

	v003_cmd_out(v003, V003_GPIO_FREE, V003_GPIO_VAL(offset, 0));
}

/* Keep the pins the device itself needs (USB D+/D-/DPU and the boot button)
 * out of the gpiochip: driving them from userspace would disturb the very bus
 * the requests travel over. */
static int v003_gpio_init_valid_mask(struct gpio_chip *gc,
				     unsigned long *valid_mask,
				     unsigned int ngpios)
{
	struct v003_dev *v003 = gpiochip_get_data(gc);
	u64 reserved = v003_reserved_pins(v003);
	unsigned int pin;

	/* The device tells us which pins it owns: the USB pins (driving the bus
	 * we are talking over would be rude), the pins the enabled modules use
	 * (an I2C bus or an SPI clock is not a GPIO), and the flat range with no
	 * port behind it.  Userspace must not be able to take those over. */
	for_each_set_bit(pin, (unsigned long *)&reserved, V003_NGPIO)
		bitmap_clear(valid_mask, pin, 1);

	return 0;
}

static int v003_gpio_probe(struct platform_device *pdev)
{
	struct v003_dev *v003 = v003_get_dev(pdev);
	struct gpio_chip *gc;
	int ret;

	if (!v003) {
		dev_err(&pdev->dev, "no MFD parent data\n");
		return -ENODEV;
	}

	gc = devm_kzalloc(&pdev->dev, sizeof(*gc), GFP_KERNEL);
	if (!gc)
		return -ENOMEM;

	gc->label = DRV_NAME;
	gc->parent = &pdev->dev;
	gc->owner = THIS_MODULE;
	gc->base = -1;
	gc->ngpio = V003_NGPIO;
	gc->can_sleep = true;
	/* setting the callback makes gpiolib allocate the mask */
	gc->init_valid_mask = v003_gpio_init_valid_mask;
	gc->request = v003_gpio_request;
	gc->free = v003_gpio_free;
	gc->get_direction = v003_gpio_direction_get;
	gc->direction_input = v003_gpio_direction_input;
	gc->direction_output = v003_gpio_direction_output;
	gc->get = v003_gpio_get;
	gc->set = v003_gpio_set;

	ret = devm_gpiochip_add_data(&pdev->dev, gc, v003);
	if (ret) {
		dev_err(&pdev->dev, "failed to add gpio chip: %d\n", ret);
		return ret;
	}

	dev_info(&pdev->dev, "%u lines, %#llx reserved by the device\n",
		 V003_NGPIO, v003_reserved_pins(v003));

	return 0;
}

static struct platform_driver v003_gpio_driver = {
	.probe = v003_gpio_probe,
	.driver = {
		.name = DRV_NAME,
	},
};
module_platform_driver(v003_gpio_driver);

MODULE_AUTHOR("Wooden Chair <hua.zheng@embeddedboys.com>");
MODULE_DESCRIPTION("GPIO driver for the CH32V003 USB MFD");
MODULE_LICENSE("GPL");
