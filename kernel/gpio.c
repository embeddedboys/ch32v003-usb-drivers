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
#include <linux/timer.h>
#include <linux/kthread.h>
#include <linux/gpio/driver.h>

#include "gpio.h"

#define DRV_NAME "v003-usb-mfd"

#define V003_USB_TIMEOUT 200

struct v003_usb_dev {
	struct usb_device *udev;
	struct usb_interface *intf;

	bool disconnect;
	wait_queue_head_t disconnect_wq;
	spinlock_t disconnect_lock;

	/* GPIO */
	struct gpio_chip gc;
	u16 ngpio;
};

static int v003_usb_tx(struct v003_usb_dev *v003, u16 val, u16 idx)
{
	struct usb_device *udev = v003->udev;

	usb_control_msg(udev, usb_sndctrlpipe(udev, 0x00), 0x00, 0x40, val, idx,
			NULL, 0, V003_USB_TIMEOUT);

	return 0;
}

static u32 v003_usb_rx(struct v003_usb_dev *v003, u16 val, u16 idx)
{
	struct usb_device *udev = v003->udev;
	u32 read;

	usb_control_msg_recv(udev, usb_rcvctrlpipe(udev, 0x00), 0x00, 0xC0, val,
			     idx, &read, sizeof(read), V003_USB_TIMEOUT,
			     GFP_KERNEL);

	return read;
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
	u32 state;

	dev_info(gc->parent, "%s, offset : %d\n", __func__, offset);
	state = v003_usb_rx(v003, V003_GPIO_VAL(offset, 0x00), V003_GPIO_GET);

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
	u32 state;

	state = v003_usb_rx(v003, V003_GPIO_VAL(offset, 0x00),
			    V003_GPIO_GET_DIRECTION);
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
	// struct usb_device *udev = interface_to_usbdev(intf);
	struct device *dev = &intf->dev;
	// struct usb_endpoint_descriptor *endpoint_desc;
	// struct usb_host_interface *interface;
	// struct usb_endpoint_descriptor *int_in;
	// struct usb_endpoint_descriptor *int_out;
	struct v003_usb_dev *v003;
	int ret;

	/* We only care about intf 0 since we don't have any work on intf 1 */
	if (intf->cur_altsetting->desc.bInterfaceNumber != 0)
		return 0;

	// ret = usb_find_common_endpoints(intf->cur_altsetting, NULL, NULL,
	// 				&int_in, &int_out);
	// if (ret) {
	// 	dev_err(dev, "failed to get usb common endpoints! ret : %d\n",
	// 		ret);
	// 	return ret;
	// }

	v003 = devm_kzalloc(dev, sizeof(*v003), GFP_KERNEL);
	if (!v003)
		return -ENOMEM;

	v003->udev = usb_get_dev(interface_to_usbdev(intf));
	v003->intf = intf;
	usb_set_intfdata(intf, v003);
	init_waitqueue_head(&v003->disconnect_wq);
	spin_lock_init(&v003->disconnect_lock);

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
		return ret;
	}

	dev_info(dev, "ready\n");
	return 0;
}

static void v003_usb_disconnect(struct usb_interface *intf)
{
	if (intf->cur_altsetting->desc.bInterfaceNumber != 0)
		return;
}

static int v003_usb_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct v003_usb_dev *v003 = usb_get_intfdata(intf);

	spin_lock(&v003->disconnect_lock);
	v003->disconnect = true;
	spin_unlock(&v003->disconnect_lock);

	return 0;
}

static int v003_usb_resume(struct usb_interface *intf)
{
	struct v003_usb_dev *v003 = usb_get_intfdata(intf);

	v003->disconnect = false;

	return 0;
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
	.suspend = v003_usb_suspend,
	.resume = v003_usb_resume,
};
module_usb_driver(v003_gpio_drv);

MODULE_AUTHOR("Wooden Chair <hua.zheng@embeddedboys.com>");
MODULE_DESCRIPTION("CH32V003 USB to GPIO/I2C/SPI Multi-Func Device Driver");
MODULE_LICENSE("GPL");
