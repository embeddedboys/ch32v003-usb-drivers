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
#include <linux/fb.h>
#include <linux/usb.h>
#include <linux/usb/input.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/mutex.h>
#include <linux/timer.h>

#include "gpio.h"

#define DRV_NAME "v003-usb-mfd"

#define V003_USB_TIMEOUT 200

struct v003_usb_dev {
	struct usb_device *udev;
	struct usb_interface *intf;

	bool disconnect;
	wait_queue_head_t disconnect_wq;
	spinlock_t disconnect_lock;
};

static struct task_struct *blink_kthread;

static int v003_usb_transfer(struct v003_usb_dev *v003, u16 val, u16 idx)
{
	struct usb_device *udev = v003->udev;

	usb_control_msg(udev, usb_sndctrlpipe(udev, 0x00), 0x00, 0x40, val, idx,
			NULL, 0, V003_USB_TIMEOUT);

	return 0;
}

static int blink_thread_func(void *data)
{
	struct v003_usb_dev *v003 = data;

	while (!kthread_should_stop()) {
		v003_usb_transfer(v003, V003_GPIO_VAL(PC0, 1), V003_GPIO_SET);
		msleep(200);
		v003_usb_transfer(v003, V003_GPIO_VAL(PC0, 0), V003_GPIO_SET);
		msleep(200);
	}

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
	// int ret;

	// ret = usb_find_common_endpoints(intf->cur_altsetting, NULL, NULL,
	// 				&int_in, &int_out);
	// if (ret) {
	// 	dev_err(dev, "failed to get usb common endpoints! ret : %d\n",
	// 		ret);
	// 	return ret;
	// }

	v003 = kzalloc(sizeof(*v003), GFP_KERNEL);
	if (!v003)
		return -ENOMEM;

	v003->udev = usb_get_dev(interface_to_usbdev(intf));
	v003->intf = intf;
	usb_set_intfdata(intf, v003);
	init_waitqueue_head(&v003->disconnect_wq);
	spin_lock_init(&v003->disconnect_lock);

	blink_kthread = kthread_run(blink_thread_func, v003, "blink");
	if (IS_ERR(blink_kthread))
		return PTR_ERR(blink_kthread);

	set_current_state(TASK_INTERRUPTIBLE);
	schedule_timeout(msecs_to_jiffies(2000));
	kthread_stop(blink_kthread);
	blink_kthread = NULL;

	dev_info(dev, "exit\n");
	return -1;
}

static void v003_usb_disconnect(struct usb_interface *intf)
{
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
