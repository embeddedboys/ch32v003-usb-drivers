// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Copyright (C) 2026 embeddedboys
 *
 * Author: Wooden Chair <hua.zheng@embeddedboys.com>
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/usb/input.h>
#include <linux/input.h>
#include <linux/mutex.h>
#include <linux/gpio/driver.h>

#include "gpio.h"

#define DRV_NAME "v003-usb-mfd"

#define V003_USB_TIMEOUT 200

struct v003_usb_dev {
	struct usb_device *udev;
	struct usb_interface *intf;

	/* data endpoints, all on interface 0 */
	unsigned int ep1_out;
	unsigned int ep2_out;
	unsigned int ep3_in;

	/* GPIO */
	struct gpio_chip gc;
	u16 ngpio;
};

static int v003_usb_tx(struct v003_usb_dev *v003, u16 val, u16 idx)
{
	struct usb_device *udev = v003->udev;

	return usb_control_msg(udev, usb_sndctrlpipe(udev, 0x00), 0x00, 0x40,
			       val, idx, NULL, 0, V003_USB_TIMEOUT);
}

static int v003_usb_rx(struct v003_usb_dev *v003, u16 val, u16 idx, u32 *out)
{
	struct usb_device *udev = v003->udev;
	u32 read;
	int ret;

	ret = usb_control_msg_recv(udev, usb_rcvctrlpipe(udev, 0x00), 0x00,
				   0xC0, val, idx, &read, sizeof(read),
				   V003_USB_TIMEOUT, GFP_KERNEL);
	if (ret < 0)
		return ret;

	if (out)
		*out = read;
	return 0;
}

static void v003_usb_gpio_set(struct gpio_chip *gc, unsigned int offset,
			      int value)
{
	struct v003_usb_dev *v003 = gpiochip_get_data(gc);

	dev_info(gc->parent, "%s, offset : %d, value : %d\n", __func__, offset,
		 value);

	v003_usb_tx(v003, V003_GPIO_VAL(offset, value), V003_GPIO_SET);
}

static int v003_usb_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct v003_usb_dev *v003 = gpiochip_get_data(gc);
	u32 state = 0;

	dev_info(gc->parent, "%s, offset : %d\n", __func__, offset);
	v003_usb_rx(v003, V003_GPIO_VAL(offset, 0x00), V003_GPIO_GET, &state);

	return state;
}

static int v003_usb_gpio_request(struct gpio_chip *gc, unsigned int offset)
{
	struct v003_usb_dev *v003 = gpiochip_get_data(gc);

	dev_info(gc->parent, "%s, offset : %d\n", __func__, offset);
	v003_usb_tx(v003, V003_GPIO_VAL(offset, 0x00), V003_GPIO_REQUEST);

	return 0;
}

static void v003_usb_gpio_free(struct gpio_chip *gc, unsigned offset)
{
	struct v003_usb_dev *v003 = gpiochip_get_data(gc);

	dev_info(gc->parent, "%s, offset : %d\n", __func__, offset);
	v003_usb_tx(v003, V003_GPIO_VAL(offset, 0x00), V003_GPIO_FREE);
}

static int v003_usb_gpio_get_direction(struct gpio_chip *gc, unsigned offset)
{
	struct v003_usb_dev *v003 = gpiochip_get_data(gc);
	u32 state = 0;

	v003_usb_rx(v003, V003_GPIO_VAL(offset, 0x00),
		    V003_GPIO_GET_DIRECTION, &state);
	dev_info(gc->parent, "%s, offset : %d, value : %d\n", __func__, offset,
		 state);

	return state > 0 ? GPIO_LINE_DIRECTION_IN : GPIO_LINE_DIRECTION_OUT;
}

static int v003_usb_gpio_direction_input(struct gpio_chip *gc, unsigned offset)
{
	struct v003_usb_dev *v003 = gpiochip_get_data(gc);

	dev_info(gc->parent, "%s, offset : %d\n", __func__, offset);
	v003_usb_tx(v003, V003_GPIO_VAL(offset, 0x00),
		    V003_GPIO_DIRECTION_INPUT);

	return 0;
}

static int v003_usb_gpio_direction_output(struct gpio_chip *gc, unsigned offset,
					  int value)
{
	struct v003_usb_dev *v003 = gpiochip_get_data(gc);

	dev_info(gc->parent, "%s, offset : %d, value : %d\n", __func__, offset,
		 value);
	v003_usb_tx(v003, V003_GPIO_VAL(offset, value),
		    V003_GPIO_DIRECTION_OUTPUT);

	return 0;
}

static int v003_usb_probe(struct usb_interface *intf,
			  const struct usb_device_id *id)
{
	struct usb_host_interface *alt = intf->cur_altsetting;
	struct device *dev = &intf->dev;
	struct v003_usb_dev *v003;
	int i, ret;

	v003 = devm_kzalloc(dev, sizeof(*v003), GFP_KERNEL);
	if (!v003)
		return -ENOMEM;

	v003->udev = usb_get_dev(interface_to_usbdev(intf));
	v003->intf = intf;
	usb_set_intfdata(intf, v003);

	/* find our data endpoints on interface 0 */
	for (i = 0; i < alt->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *epd = &alt->endpoint[i].desc;
		unsigned int num = usb_endpoint_num(epd);

		if (num == 1 && usb_endpoint_dir_out(epd))
			v003->ep1_out = num;
		else if (num == 2 && usb_endpoint_dir_out(epd))
			v003->ep2_out = num;
		else if (num == 3 && usb_endpoint_dir_in(epd))
			v003->ep3_in = num;
	}

	if (!v003->ep1_out || !v003->ep2_out || !v003->ep3_in) {
		dev_err(dev, "failed to find data endpoints\n");
		ret = -ENODEV;
		goto err_put;
	}

	v003->gc.label = DRV_NAME;
	v003->gc.parent = dev;
	v003->gc.owner = THIS_MODULE;
	v003->gc.base = -1;
	v003->gc.ngpio = V003_NGPIO;
	v003->gc.can_sleep = true;
	v003->gc.set = v003_usb_gpio_set;
	v003->gc.get = v003_usb_gpio_get;
	v003->gc.request = v003_usb_gpio_request;
	v003->gc.free = v003_usb_gpio_free;
	v003->gc.get_direction = v003_usb_gpio_get_direction;
	v003->gc.direction_input = v003_usb_gpio_direction_input;
	v003->gc.direction_output = v003_usb_gpio_direction_output;

	ret = devm_gpiochip_add_data(dev, &v003->gc, v003);
	if (ret < 0) {
		dev_err(dev, "failed to add gpio chip: %d\n", ret);
		goto err_put;
	}

	dev_info(dev, "ready\n");
	return 0;

err_put:
	usb_put_dev(v003->udev);
	usb_set_intfdata(intf, NULL);
	return ret;
}

static void v003_usb_disconnect(struct usb_interface *intf)
{
	struct v003_usb_dev *v003 = usb_get_intfdata(intf);

	if (!v003)
		return;

	usb_put_dev(v003->udev);
	usb_set_intfdata(intf, NULL);
}

static struct usb_device_id v003_usb_ids[] = {
	{ USB_DEVICE(0x1209, 0xC303) },
	{ /* KEEP THIS */ },
};
MODULE_DEVICE_TABLE(usb, v003_usb_ids);

static struct usb_driver v003_gpio_drv = {
	.name = DRV_NAME,
	.probe = v003_usb_probe,
	.disconnect = v003_usb_disconnect,
	.id_table = v003_usb_ids,
};
module_usb_driver(v003_gpio_drv);

MODULE_AUTHOR("Wooden Chair <hua.zheng@embeddedboys.com>");
MODULE_DESCRIPTION("CH32V003 USB to GPIO/I2C/SPI Multi-Func Device Driver");
MODULE_LICENSE("GPL");
