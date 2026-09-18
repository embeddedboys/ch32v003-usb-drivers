// SPDX-License-Identifier: GPL-2.0-only
/*
 * Core driver for the CH32V003 USB multi function device.
 *
 * Modelled on the Diolan DLN-2 core (drivers/mfd/dln2.c): this file only owns
 * the USB transport, checks that the firmware speaks the expected protocol and
 * registers the MFD cells.  The actual functions live in child drivers that
 * call v003_transfer_out()/v003_transfer_in().
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
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/mfd/core.h>
#include <linux/unaligned.h>

#include "usb-mfd.h"
#include "v003_usb_ids.h" /* the shared USB identity */

#define DRV_NAME "v003-usb-mfd"

/*
 * Unlike dln2 this core serialises transfers with a mutex instead of keeping
 * several URBs and RX slots in flight.  The firmware executes one vendor
 * request at a time in its main loop (the I2C bit banging can take
 * milliseconds), so an async queue would only move the serialisation around;
 * a mutex keeps the core honest about what the device can do.  If the firmware
 * grows a framed protocol on the endpoint data path, the transfer functions
 * below are the only place that has to change.
 */
struct v003_dev {
	struct usb_device *udev;
	struct usb_interface *intf;

	/* what the firmware reported as its capabilities; cleared when it
	 * predates the command */
	struct v003_caps caps;
	bool have_caps;

	/* serialises vendor transfers (all of them can sleep) */
	struct mutex lock;
	/* set on disconnect/suspend so queued calls fail fast */
	bool offline;

	/* framed endpoint transport */
	bool frame_mode;
	u16 frame_echo;
	u8 frame_tx[V003_FRAME_MAX];
	u8 frame_rx[V003_FRAME_RESP];

	/* Data stage bounce buffers.  usb_control_msg() hands the buffer to the
	 * HCD for DMA mapping, which is why it must not live on the stack (the
	 * kernel prints "transfer buffer is on stack" and fails the transfer),
	 * and in kernel i2c/spi clients pass stack buffers all the time.  The
	 * core copies instead of pushing that rule onto every caller. */
	u8 bounce_tx[V003_CTRL_DATA_MAX];
	u8 bounce_rx[V003_CTRL_DATA_MAX];
};

/*
 * Pins the driver owns on top of what the device reports, as a comma separated
 * list of flat pin numbers (`insmod usb-mfd.ko reserved=36` for the chip select
 * line of v003-spi.ko).  Without it userspace can request a line that a child
 * driver is driving, which is the kind of thing that only shows up as a device
 * behaving strangely.
 */
static int v003_reserved[V003_NGPIO];
static int v003_reserved_count;
module_param_array_named(reserved, v003_reserved, int, &v003_reserved_count, 0644);
MODULE_PARM_DESC(reserved, "flat pin numbers the driver owns, e.g. reserved=36,37");

static u64 v003_reserved_pins_param;
static int __init v003_reserved_setup(void)
{
	int i;

	for (i = 0; i < v003_reserved_count; i++) {
		if (v003_reserved[i] < 0 || v003_reserved[i] >= V003_NGPIO) {
			pr_err("v003-usb-mfd: reserved pin %d is out of range\n",
			       v003_reserved[i]);
			return -EINVAL;
		}
		v003_reserved_pins_param |= 1ULL << v003_reserved[i];
	}

	return 0;
}

/* How the child drivers' commands are carried; see V003_TRANSPORT_* in the
 * header for why the default is not simply "the endpoint path". */
unsigned int v003_transport;
module_param_named(transport, v003_transport, uint, 0644);
MODULE_PARM_DESC(v003_transport,
		 "command transport: 0 = control transfers (default), 1 = framed endpoints, 2 = framed writes with control reads");

struct v003_dev *v003_get_dev(struct platform_device *pdev)
{
	return dev_get_drvdata(pdev->dev.parent);
}
EXPORT_SYMBOL_GPL(v003_get_dev);

static int v003_check_offline(struct v003_dev *v003)
{
	return v003->offline ? -ENODEV : 0;
}

int v003_transfer_out(struct v003_dev *v003, u16 cmd, u16 val)
{
	int ret;

	mutex_lock(&v003->lock);

	ret = v003_check_offline(v003);
	if (ret)
		goto out;

	ret = usb_control_msg(v003->udev, usb_sndctrlpipe(v003->udev, 0x00),
			      0x00, 0x40, val, cmd, NULL, 0, V003_USB_TIMEOUT);
	if (ret < 0) {
		dev_err(&v003->intf->dev, "cmd %#x val %#x failed: %d\n", cmd,
			val, ret);
		goto out;
	}

	ret = 0;
out:
	mutex_unlock(&v003->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(v003_transfer_out);

static bool v003_frame_enabled(struct v003_dev *v003)
{
	return v003->frame_mode;
}

/* --- framed endpoint transport ------------------------------------- */

/* result field of a response frame: the firmware reports 0 for success */
#define V003_FRAME_OK 0

static int v003_frame_exchange(struct v003_dev *v003, u16 handle, u16 id,
			       u16 arg, const void *payload, size_t payload_len,
			       bool want_reply, void *rx, size_t rx_len,
			       size_t *rx_got)
{
	u8 *tx = v003->frame_tx;
	u8 *rbuf = v003->frame_rx;
	unsigned long deadline;
	size_t len;
	u16 echo;
	u16 size;
	int actual = 0;
	int ret;

	len = V003_FRAME_HDR + 2 + payload_len;
	if (len > sizeof(v003->frame_tx))
		return -EINVAL;

	mutex_lock(&v003->lock);

	ret = v003_check_offline(v003);
	if (ret)
		goto out;

	echo = ++v003->frame_echo;
	if (!echo) /* 0 means "no response wanted" to the firmware */
		echo = ++v003->frame_echo;

	put_unaligned_le16(len, tx);
	put_unaligned_le16(id, tx + 2);
	put_unaligned_le16(want_reply ? echo : 0, tx + 4);
	put_unaligned_le16(handle, tx + 6);
	put_unaligned_le16(arg, tx + 8);
	if (payload_len)
		memcpy(tx + V003_FRAME_HDR + 2, payload, payload_len);

	ret = usb_interrupt_msg(v003->udev,
				usb_sndintpipe(v003->udev, V003_EP_OUT), tx, len,
				&actual, V003_USB_TIMEOUT);
	if (ret < 0) {
		dev_err(&v003->intf->dev, "frame %#x/%#x send failed: %d\n",
			handle, id, ret);
		goto out;
	}
	if (actual != len) {
		ret = -EIO;
		goto out;
	}

	if (!want_reply) {
		ret = 0;
		goto out;
	}

	/* The firmware answers every IN token, with an empty packet while it
	 * has nothing queued, so a read can legitimately come back empty and
	 * the exchange only ends when the echo tag comes back. */
	deadline = jiffies + msecs_to_jiffies(V003_FRAME_TIMEOUT);
	for (;;) {
		ret = usb_interrupt_msg(v003->udev,
					usb_rcvintpipe(v003->udev, V003_EP_IN),
					rbuf, sizeof(v003->frame_rx), &actual,
					V003_USB_TIMEOUT);
		if (ret < 0) {
			dev_err(&v003->intf->dev,
				"frame %#x/%#x receive failed: %d\n", handle, id,
				ret);
			goto out;
		}

		if (actual >= V003_FRAME_HDR + 2) {
			size = get_unaligned_le16(rbuf);
			if (size >= V003_FRAME_HDR + 2 && size <= actual &&
			    get_unaligned_le16(rbuf + 2) == id &&
			    get_unaligned_le16(rbuf + 4) == echo &&
			    get_unaligned_le16(rbuf + 6) == handle)
				break; /* ours */
			dev_warn(&v003->intf->dev,
				 "dropping stray frame (size %u, echo %#x)\n",
				 size, get_unaligned_le16(rbuf + 4));
		}

		if (time_after(jiffies, deadline)) {
			ret = -ETIMEDOUT;
			dev_err(&v003->intf->dev, "frame %#x/%#x timed out\n",
				handle, id);
			goto out;
		}
	}

	if (get_unaligned_le16(rbuf + 8) != V003_FRAME_OK) {
		ret = -EIO;
		goto out;
	}

	if (rx) {
		size = get_unaligned_le16(rbuf) - (V003_FRAME_HDR + 2);
		if (size > rx_len)
			size = rx_len;
		memcpy(rx, rbuf + V003_FRAME_HDR + 2, size);
		if (rx_got)
			*rx_got = size;
	}
	ret = 0;

out:
	mutex_unlock(&v003->lock);

	return ret;
}

int v003_frame_send(struct v003_dev *v003, u16 handle, u16 id, u16 arg,
		    const void *payload, size_t payload_len)
{
	return v003_frame_exchange(v003, handle, id, arg, payload, payload_len,
				   false, NULL, 0, NULL);
}
EXPORT_SYMBOL_GPL(v003_frame_send);

int v003_frame_xfer(struct v003_dev *v003, u16 handle, u16 id, u16 arg,
		    const void *payload, size_t payload_len, void *rx,
		    size_t rx_len, size_t *rx_got)
{
	return v003_frame_exchange(v003, handle, id, arg, payload, payload_len,
				   true, rx, rx_len, rx_got);
}
EXPORT_SYMBOL_GPL(v003_frame_xfer);

int v003_frame_mode(struct v003_dev *v003, bool on)
{
	int ret;

	/* the switch itself is a zero length control OUT: the firmware applies
	 * it from its main loop, so there is nothing to wait for beyond the
	 * transfer completing */
	ret = v003_transfer_out(v003, V003_SET_FRAME_MODE, on ? 1 : 0);
	if (ret)
		return ret;

	v003->frame_mode = on;
	v003->frame_echo = 0;

	return 0;
}
EXPORT_SYMBOL_GPL(v003_frame_mode);

int v003_cmd_out(struct v003_dev *v003, u16 cmd, u16 val)
{
	if (!v003_frame_enabled(v003) || v003_transport == V003_TRANSPORT_CONTROL)
		return v003_transfer_out(v003, cmd, val);

	/* no reply wanted: the command costs two packets instead of a whole
	 * control transfer */
	return v003_frame_send(v003, cmd >> 8, cmd & 0xff, val, NULL, 0);
}
EXPORT_SYMBOL_GPL(v003_cmd_out);

int v003_cmd_in(struct v003_dev *v003, u16 cmd, u16 val, u32 *out)
{
	u8 payload[4];
	size_t got = 0;
	int ret;

	if (!v003_frame_enabled(v003) || v003_transport != V003_TRANSPORT_FRAME)
		return v003_transfer_in(v003, cmd, val, out);

	ret = v003_frame_xfer(v003, cmd >> 8, cmd & 0xff, val, NULL, 0, payload,
			      sizeof(payload), &got);
	if (ret)
		return ret;

	if (got < sizeof(payload))
		return -EIO;

	if (out)
		*out = get_unaligned_le32(payload);

	return 0;
}
EXPORT_SYMBOL_GPL(v003_cmd_in);

int v003_transfer_in(struct v003_dev *v003, u16 cmd, u16 val, u32 *out)
{
	u32 data = 0;
	int ret;

	mutex_lock(&v003->lock);

	ret = v003_check_offline(v003);
	if (ret)
		goto out;

	ret = usb_control_msg_recv(v003->udev, usb_rcvctrlpipe(v003->udev, 0x00),
				   0x00, 0xc0, val, cmd, &data, sizeof(data),
				   V003_USB_TIMEOUT, GFP_KERNEL);
	if (ret < 0) {
		dev_err(&v003->intf->dev, "cmd %#x val %#x failed: %d\n", cmd,
			val, ret);
		goto out;
	}

	if (out)
		*out = data;

	ret = 0;
out:
	mutex_unlock(&v003->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(v003_transfer_in);

int v003_data_out(struct v003_dev *v003, u16 cmd, u16 val, const void *buf,
		  size_t len)
{
	int ret;

	if (len > sizeof(v003->bounce_tx))
		return -EINVAL;

	mutex_lock(&v003->lock);

	ret = v003_check_offline(v003);
	if (ret)
		goto out;

	memcpy(v003->bounce_tx, buf, len);

	ret = usb_control_msg(v003->udev, usb_sndctrlpipe(v003->udev, 0x00),
			      0x00, 0x40, val, cmd, v003->bounce_tx, len,
			      V003_USB_TIMEOUT);
	if (ret < 0)
		dev_err(&v003->intf->dev, "cmd %#x val %#x (%zu bytes) failed: %d\n",
			cmd, val, len, ret);
	else if (ret != len)
		ret = -EIO;

out:
	mutex_unlock(&v003->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(v003_data_out);

int v003_data_in(struct v003_dev *v003, u16 cmd, u16 val, void *buf, size_t len)
{
	int ret;

	if (len > sizeof(v003->bounce_rx))
		return -EINVAL;

	mutex_lock(&v003->lock);

	ret = v003_check_offline(v003);
	if (ret)
		goto out;

	/* not usb_control_msg_recv(): the firmware answers with as many bytes as
	 * it has, which can be fewer than the host asked for and is still a
	 * valid short read */
	ret = usb_control_msg(v003->udev, usb_rcvctrlpipe(v003->udev, 0x00),
			      0x00, 0xc0, val, cmd, v003->bounce_rx, len,
			      V003_USB_TIMEOUT);
	if (ret < 0) {
		dev_err(&v003->intf->dev, "cmd %#x val %#x (%zu bytes) failed: %d\n",
			cmd, val, len, ret);
		goto out;
	}

	if (ret > 0)
		memcpy(buf, v003->bounce_rx, ret);

out:
	mutex_unlock(&v003->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(v003_data_in);

/* Ask the firmware for its version: the dln2 core does the same with
 * CMD_GET_DEVICE_VER before it registers anything. */
static int v003_hw_init(struct v003_dev *v003)
{
	u8 uid[V003_DEVICE_UID_SIZE];
	u32 ver = 0;
	u32 sn = 0;
	int ret;

	ret = v003_transfer_in(v003, V003_GET_DEVICE_VER, 0, &ver);
	if (ret)
		return ret;

	if (ver != V003_DEVICE_VER) {
		dev_err(&v003->intf->dev, "device version %#x, expected %#x\n",
			ver, V003_DEVICE_VER);
		return -ENODEV;
	}

	ret = v003_transfer_in(v003, V003_GET_DEVICE_SN, 0, &sn);
	if (ret)
		return ret;

	/* the unique id identifies the board; a device that cannot report it is
	 * still usable, so this only warns */
	ret = v003_data_in(v003, V003_GET_DEVICE_UID, 0, uid, sizeof(uid));
	if (ret == sizeof(uid))
		dev_info(&v003->intf->dev,
			 "firmware %#x, serial %#x, unique id %*phN\n", ver, sn,
			 (int)sizeof(uid), uid);
	else
		dev_warn(&v003->intf->dev,
			 "firmware %#x, serial %#x (no unique id: %d)\n", ver, sn,
			 ret);

	return 0;
}

/*
 * The cells on offer and the capability bit each one needs.  Only the cells the
 * device says it has are added, so a firmware with a module compiled out does
 * not get a child driver probing something that is not there.  The capability
 * is kept next to the name instead of in mfd_cell.id, which is the platform
 * device instance number and would rename the devices.
 */
struct v003_cell_desc {
	const char *name;
	u32 cap;
};

static const struct v003_cell_desc v003_cell_list[] = {
	{ "v003-gpio", V003_CAP_GPIO },
	{ "v003-i2c",  V003_CAP_I2C },
	{ "v003-spi",  V003_CAP_SPI },
	{ "v003-wdt",  V003_CAP_WDG },
	{ "v003-pwm",  V003_CAP_PWM },
	{ "v003-adc",  V003_CAP_ADC },
};

const struct v003_caps *v003_capabilities(struct v003_dev *v003)
{
	return v003->have_caps ? &v003->caps : NULL;
}
EXPORT_SYMBOL_GPL(v003_capabilities);

u64 v003_reserved_pins(struct v003_dev *v003)
{
	u64 mask;

	if (!v003->have_caps)
		mask = V003_RESERVED_PINS_FALLBACK;
	else
		mask = (u64)v003->caps.reserved_hi << 32 | v003->caps.reserved_lo;

	/* pins the child drivers themselves take over: a chip select line the SPI
	 * driver drives, an IRQ pin, anything a board wires to something that is
	 * not a GPIO.  The device cannot know about those, the driver can. */
	return mask | v003_reserved_pins_param;
}
EXPORT_SYMBOL_GPL(v003_reserved_pins);

/*
 * Ask the firmware what it contains.  A device that predates the command is not
 * an error: it is the firmware that had exactly GPIO, I2C and SPI, which is
 * also what the driver assumed before, so fall back to that.
 */
static void v003_read_capabilities(struct v003_dev *v003)
{
	struct v003_caps caps;
	int ret;

	ret = v003_data_in(v003, V003_GET_CAPABILITIES, 0, &caps, sizeof(caps));
	if (ret != sizeof(caps)) {
		dev_warn(&v003->intf->dev,
			 "no capability report (%d), assuming the original GPIO, I2C and SPI\n",
			 ret);
		return;
	}

	v003->caps = caps;
	v003->have_caps = true;

	dev_info(&v003->intf->dev,
		 "capabilities %#x%s%s%s%s%s, %u gpio lines, %u adc, %u pwm, %u uart\n",
		 caps.caps, caps.caps & V003_CAP_GPIO ? " gpio" : "",
		 caps.caps & V003_CAP_SPI ? " spi" : "",
		 caps.caps & V003_CAP_I2C ? " i2c" : "",
		 caps.caps & V003_CAP_WDG ? " wdg" : "",
		 caps.caps & V003_CAP_FRAME ? " frame" : "", caps.ngpio,
		 caps.nadc, caps.npwm, caps.nuart);
}

/* add the cells the firmware reported (or all of them for old firmware) */
static int v003_add_cells(struct v003_dev *v003)
{
	struct mfd_cell *cells;
	unsigned int i, n = 0;

	cells = devm_kcalloc(&v003->intf->dev, ARRAY_SIZE(v003_cell_list),
			     sizeof(*cells), GFP_KERNEL);
	if (!cells)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(v003_cell_list); i++) {
		const struct v003_cell_desc *desc = &v003_cell_list[i];

		if (v003->have_caps && !(v003->caps.caps & desc->cap))
			continue;

		cells[n].name = desc->name;
		n++;
	}

	if (!n)
		return 0;

	return devm_mfd_add_devices(&v003->intf->dev, PLATFORM_DEVID_NONE, cells,
				    n, NULL, 0, NULL);
}

static int v003_probe(struct usb_interface *intf,
		      const struct usb_device_id *usb_id)
{
	struct usb_host_interface *alt = intf->cur_altsetting;
	struct device *dev = &intf->dev;
	struct v003_dev *v003;
	int ret;

	if (alt->desc.bInterfaceNumber != 0)
		return -ENODEV;

	v003 = devm_kzalloc(dev, sizeof(*v003), GFP_KERNEL);
	if (!v003)
		return -ENOMEM;

	v003->udev = usb_get_dev(interface_to_usbdev(intf));
	v003->intf = intf;
	mutex_init(&v003->lock);
	usb_set_intfdata(intf, v003);
	dev_set_drvdata(dev, v003);

	ret = v003_hw_init(v003);
	if (ret) {
		dev_err(dev, "failed to talk to the firmware: %d\n", ret);
		goto err_put;
	}

	v003_read_capabilities(v003);

	if (v003_transport != V003_TRANSPORT_CONTROL) {
		ret = v003_frame_mode(v003, true);
		if (ret) {
			dev_err(dev, "failed to enable the framed transport: %d\n",
				ret);
			goto err_put;
		}
		dev_info(dev, "framed endpoint transport enabled\n");
	}

	ret = v003_add_cells(v003);
	if (ret) {
		dev_err(dev, "failed to add mfd devices: %d\n", ret);
		goto err_put;
	}

	dev_info(dev, "ready\n");

	return 0;

err_put:
	usb_put_dev(v003->udev);
	usb_set_intfdata(intf, NULL);
	return ret;
}

static void v003_disconnect(struct usb_interface *intf)
{
	struct v003_dev *v003 = usb_get_intfdata(intf);

	if (!v003)
		return;

	/* stop new transfers, then let the in-flight one finish */
	mutex_lock(&v003->lock);
	v003->offline = true;
	mutex_unlock(&v003->lock);

	/* leave the device the way the userspace tests expect to find it */
	if (v003->frame_mode)
		v003_frame_mode(v003, false);

	usb_set_intfdata(intf, NULL);
	usb_put_dev(v003->udev);
}

static int v003_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct v003_dev *v003 = usb_get_intfdata(intf);

	if (!v003)
		return 0;

	mutex_lock(&v003->lock);
	v003->offline = true;
	mutex_unlock(&v003->lock);

	return 0;
}

static int v003_resume(struct usb_interface *intf)
{
	struct v003_dev *v003 = usb_get_intfdata(intf);

	if (!v003)
		return 0;

	mutex_lock(&v003->lock);
	v003->offline = false;
	mutex_unlock(&v003->lock);

	return 0;
}

static struct usb_device_id v003_usb_ids[] = {
	{ USB_DEVICE(V003_USB_VID, V003_USB_PID) },
	{ /* KEEP THIS */ },
};
MODULE_DEVICE_TABLE(usb, v003_usb_ids);

static struct usb_driver v003_usb_driver = {
	.name = DRV_NAME,
	.probe = v003_probe,
	.disconnect = v003_disconnect,
	.suspend = v003_suspend,
	.resume = v003_resume,
	.id_table = v003_usb_ids,
};
static int __init v003_init(void)
{
	int ret;

	ret = v003_reserved_setup();
	if (ret)
		return ret;

	return usb_register_driver(&v003_usb_driver, THIS_MODULE, DRV_NAME);
}

static void __exit v003_exit(void)
{
	usb_deregister(&v003_usb_driver);
}

module_init(v003_init);
module_exit(v003_exit);

MODULE_AUTHOR("Wooden Chair <hua.zheng@embeddedboys.com>");
MODULE_DESCRIPTION("CH32V003 USB to GPIO/I2C/SPI multi function device core");
MODULE_LICENSE("GPL");
