// SPDX-License-Identifier: GPL-2.0-only
/*
 * UART child driver for the CH32V003 USB multi function device.
 *
 * The firmware runs USART1 on PD0 (TX) and PD1 (RX) with a 64 byte receive ring
 * filled by its interrupt and a 32 byte transmit ring it drains.  This driver
 * puts a TTY in front of it, so the device shows up as /dev/ttyV0 and everything
 * that talks to a serial port works unchanged: termios, the line discipline, cat,
 * stty, pyserial.
 *
 * There is no interrupt from the device to the host - the link is a bit banged
 * low speed USB device that answers control transfers - so the receive path is
 * polled: a delayed work reads V003_UART_GET_STATE, pulls whatever the firmware's
 * ring holds with V003_UART_READ and pushes it into the TTY flip buffer.  How
 * often is derived from the configured baud rate so that the ring cannot fill
 * between two polls, and when the rate is too high for that to be possible the
 * driver says so (see v003_uart_update_polling) instead of quietly losing bytes.
 *
 * The transmit path uses the firmware's own flow control: write() sends what the
 * transmit ring has room for and returns that count; the poll work calls
 * tty_wakeup() once the ring has drained, which makes the TTY layer hand over the
 * rest.  Nothing in this driver sleeps in an atomic context, and no transfer
 * buffer lives on the stack.
 *
 * Modelled on the tty_port based drivers in drivers/tty/ and drivers/usb/serial/.
 *
 * Copyright (C) 2026 embeddedboys
 *
 * Author: Wooden Chair <hua.zheng@embeddedboys.com>
 */

#include <linux/device.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/termios.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/workqueue.h>

#include "usb-mfd.h"

#define DRV_NAME "v003-uart"
#define V003_UART_MINORS 1

/* One UART in the firmware, so one port here. */
#define V003_UART_PORT 0

struct v003_uart {
	struct v003_dev *v003;
	struct device *dev;

	struct tty_driver *tty;
	struct tty_port port;

	/* receive polling */
	struct delayed_work rx_work;
	bool polling;

	/* what the last poll saw, for the cheap questions the TTY layer asks in
	 * contexts where a USB transfer is not allowed */
	u32 state;
	unsigned int poll_ms;

	/* the port is configured and enabled */
	bool enabled;
	unsigned int baud;
	unsigned int actual_baud;
	/* the configured rate cannot be serviced by polling: the ring fills in
	 * less time than a control transfer takes */
	bool lossy;

	unsigned long polls;
	unsigned long rx_bytes;
	unsigned long tx_bytes;
};

/*
 * ---------------------------------------------------------------- termios
 */

static void v003_uart_update_polling(struct v003_uart *u)
{
	unsigned int bytes_per_ms;

	if (!u->baud) {
		u->poll_ms = 10;
		u->lossy = false;
		return;
	}

	/* ten bits on the wire per byte */
	bytes_per_ms = u->baud / 10000;
	if (!bytes_per_ms)
		bytes_per_ms = 1;

	/* half a ring per poll, clamped to what a timer can do and to something
	 * a reader would still call responsive */
	u->poll_ms = (V003_UART_RX_RING_SIZE / 2) / bytes_per_ms;
	if (u->poll_ms < 1)
		u->poll_ms = 1;
	if (u->poll_ms > 10)
		u->poll_ms = 10;

	/* the ring fills in ring_bytes * 10 * 1000 / baud milliseconds; if that
	 * is less than one control transfer (~2 ms) then bytes will be dropped
	 * whatever this driver does, and saying so is more useful than being
	 * quiet.  Writing the comparison as 2000 here made every rate look lossy,
	 * which is the kind of wrong number a log line is supposed to prevent. */
	u->lossy = (V003_UART_RX_RING_SIZE * 10 * 1000 / u->baud) < 2;
}

/* Map termios onto what the firmware can do and tell it.  Called from
 * set_termios() and from the port's activate(), both process context. */
static int v003_uart_configure(struct v003_uart *u, struct tty_struct *tty)
{
	struct v003_uart_cfg cfg = { 0 };
	struct ktermios *termios = &tty->termios;
	unsigned int baud, bits, stop;
	u8 parity;
	int ret;

	baud = tty_termios_baud_rate(termios);
	if (baud < V003_UART_BAUD_MIN)
		baud = 115200; /* B0 means hang up, not "as fast as possible" */

	bits = tty_get_char_size(termios->c_cflag);
	if (bits != 8) {
		/* the firmware converts 8 or 9 bits; anything else is rounded
		 * up and the caller is told through the termios we report back */
		dev_dbg(u->dev, "termios asks for %u data bits, using 8\n", bits);
		bits = 8;
	}

	if (termios->c_cflag & PARENB)
		parity = (termios->c_cflag & PARODD) ? V003_UART_PARITY_ODD
						     : V003_UART_PARITY_EVEN;
	else
		parity = V003_UART_PARITY_NONE;

	stop = (termios->c_cflag & CSTOPB) ? 2 : 1;

	if (termios->c_cflag & CRTSCTS) {
		/* the pins for RTS/CTS are the I2C SCL line and the SPI pins on
		 * this board, so there is no hardware handshake to offer; taking
		 * the flag out is visible to the caller through tcgetattr */
		termios->c_cflag &= ~CRTSCTS;
		dev_info(u->dev, "CRTSCTS is not available, clearing it\n");
	}

	cfg.baud = baud;
	cfg.data_bits = bits;
	cfg.parity = parity;
	cfg.stop_bits = stop;
	cfg.enable = 1;

	ret = v003_data_out(u->v003, V003_UART_CONFIG, 0, &cfg, sizeof(cfg));
	if (ret < 0)
		return ret;
	if (ret != sizeof(cfg))
		return -EIO;

	ret = v003_data_in(u->v003, V003_UART_GET_CFG, 0, &cfg, sizeof(cfg));
	if (ret < 0)
		return ret;
	if (ret != sizeof(cfg))
		return -EIO;

	u->enabled = cfg.enable;
	u->baud = baud;
	u->actual_baud = cfg.actual_baud;
	v003_uart_update_polling(u);

	/* report the rate the hardware really divides to, which is what a
	 * caller reads back with cfgetospeed() - on this chip baud = HCLK / BRR,
	 * so 115200 becomes 115107 and 921600 becomes 923076 */
	tty_termios_encode_baud_rate(termios, u->actual_baud, u->actual_baud);

	dev_info(u->dev, "%u baud (actual %u), %u%c%u, poll %u ms%s\n",
		 baud, u->actual_baud, bits,
		 parity == V003_UART_PARITY_NONE ? 'N'
		 : parity == V003_UART_PARITY_EVEN ? 'E' : 'O',
		 stop, u->poll_ms,
		 u->lossy ? " - the ring cannot keep up at this rate, expect drops"
			  : "");

	return 0;
}

/* ----------------------------------------------------------- receive path */

/*
 * Pull what the firmware holds into the TTY.  V003_UART_READ answers with the
 * contiguous run at the ring's tail, so a wrapped ring needs more than one read;
 * what it publishes is consumed, so the loop can stop when the firmware says
 * there is nothing left.
 */
static int v003_uart_drain(struct v003_uart *u)
{
	u8 buf[V003_UART_RX_RING_SIZE];
	unsigned int rounds = 4;
	int total = 0;

	while (rounds--) {
		unsigned int avail = V003_UART_GET_RX_AVAIL(u->state);
		int got;

		if (!avail)
			break;

		if (avail > sizeof(buf))
			avail = sizeof(buf);

		got = v003_data_in(u->v003, V003_UART_READ, avail, buf, avail);
		if (got <= 0)
			return got ? got : total;

		tty_insert_flip_string(&u->port, buf, got);
		total += got;
		u->rx_bytes += got;

		if (got < (int)avail)
			break;

		/* ask again: the ring may have wrapped or more may have arrived */
		if (v003_cmd_in(u->v003, V003_UART_GET_STATE, 0, &u->state))
			break;
	}

	if (total)
		tty_flip_buffer_push(&u->port);

	return total;
}

static void v003_uart_rx_work(struct work_struct *work)
{
	struct v003_uart *u = container_of(to_delayed_work(work),
					   struct v003_uart, rx_work);
	u32 state;
	int ret;

	if (!u->polling)
		return;

	ret = v003_cmd_in(u->v003, V003_UART_GET_STATE, 0, &state);
	if (ret) {
		/* the device is gone: stop, and let the port notice */
		if (ret == -ENODEV) {
			u->polling = false;
			tty_port_tty_hangup(&u->port, false);
			return;
		}
		goto reschedule;
	}

	u->state = state;
	u->polls++;

	v003_uart_drain(u);

	/* the transmit ring may have drained while we were here: the TTY layer
	 * waits on the wakeup to hand over the bytes write() could not take */
	if (V003_UART_GET_TX_QUEUED(state) < V003_UART_TX_RING_HOLDS) {
		struct tty_struct *tty = tty_port_tty_get(&u->port);

		if (tty) {
			tty_wakeup(tty);
			tty_kref_put(tty);
		}
	}

reschedule:
	if (u->polling)
		schedule_delayed_work(&u->rx_work, msecs_to_jiffies(u->poll_ms));
}

/* ----------------------------------------------------------- transmit path */

static ssize_t v003_uart_write(struct tty_struct *tty, const u8 *buf,
			       size_t count)
{
	struct v003_uart *u = tty->driver_data;
	unsigned int room;
	size_t done = 0;
	int ret;

	if (!u || !u->enabled)
		return -EIO;

	/* the firmware's ring is the flow control: send what fits and report it,
	 * the poll work calls tty_wakeup() when it drains */
	room = V003_UART_TX_RING_HOLDS - V003_UART_GET_TX_QUEUED(u->state);
	if (!room)
		return 0;

	if (count > room)
		count = room;
	if (count > V003_CTRL_DATA_MAX)
		count = V003_CTRL_DATA_MAX;

	ret = v003_data_out(u->v003, V003_UART_WRITE, 0, buf, count);
	if (ret < 0)
		return ret;

	done = ret;
	u->tx_bytes += done;

	/* make the next write see the room this one used up */
	v003_cmd_in(u->v003, V003_UART_GET_STATE, 0, &u->state);

	return done;
}

static unsigned int v003_uart_write_room(struct tty_struct *tty)
{
	struct v003_uart *u = tty->driver_data;
	unsigned int room;

	if (!u || !u->enabled)
		return 0;

	room = V003_UART_TX_RING_HOLDS - V003_UART_GET_TX_QUEUED(u->state);

	return room > V003_CTRL_DATA_MAX ? V003_CTRL_DATA_MAX : room;
}

static unsigned int v003_uart_chars_in_buffer(struct tty_struct *tty)
{
	struct v003_uart *u = tty->driver_data;

	if (!u)
		return 0;

	return V003_UART_GET_TX_QUEUED(u->state);
}

/* --------------------------------------------------------------- tty glue */

static int v003_uart_open(struct tty_struct *tty, struct file *filp)
{
	struct v003_uart *u = dev_get_drvdata(tty->dev);
	int ret;

	if (!u)
		return -ENODEV;

	tty->driver_data = u;

	ret = tty_port_open(&u->port, tty, filp);
	if (ret)
		tty->driver_data = NULL;

	return ret;
}

static void v003_uart_close(struct tty_struct *tty, struct file *filp)
{
	struct v003_uart *u = tty->driver_data;
	struct tty_port *port;

	if (!u)
		return;

	port = &u->port;
	tty_port_close(port, tty, filp);

	/* the port is closed: stop polling and give the pins back, which matters
	 * because PD1 is the SWIO debug pin and the firmware releasing PD0/PD1 is
	 * what lets the programmer talk to the chip again */
	tty->driver_data = NULL;
}

static void v003_uart_set_termios(struct tty_struct *tty,
				  const struct ktermios *old)
{
	struct v003_uart *u = tty->driver_data;
	int ret;

	if (!u)
		return;

	ret = v003_uart_configure(u, tty);
	if (ret)
		dev_err(u->dev, "could not configure the port: %d\n", ret);
}

static const struct tty_operations v003_uart_ops = {
	.open = v003_uart_open,
	.close = v003_uart_close,
	.write = v003_uart_write,
	.write_room = v003_uart_write_room,
	.chars_in_buffer = v003_uart_chars_in_buffer,
	.set_termios = v003_uart_set_termios,
};

/* ------------------------------------------------------- tty_port callbacks */

static int v003_uart_activate(struct tty_port *port, struct tty_struct *tty)
{
	struct v003_uart *u = container_of(port, struct v003_uart, port);
	int ret;

	ret = v003_uart_configure(u, tty);
	if (ret)
		return ret;

	/* A session starts with the firmware's counters zeroed.  They are eight
	 * bits wide and saturate - the receiver's interrupt count is at 255 after
	 * a few hundred bytes - so a cumulative value stops meaning anything and
	 * only a difference is useful; resetting here is also what lets a user
	 * who cannot write the root-only stats_reset attribute measure at all. */
	v003_cmd_out(u->v003, V003_UART_CLEAR_STATS, 0);

	u->polling = true;
	schedule_delayed_work(&u->rx_work, msecs_to_jiffies(u->poll_ms));

	return 0;
}

static void v003_uart_shutdown(struct tty_port *port)
{
	struct v003_uart *u = container_of(port, struct v003_uart, port);
	struct v003_uart_cfg cfg = { 0 };
	int ret;

	u->polling = false;
	cancel_delayed_work_sync(&u->rx_work);

	/* disabling the port is what releases PD0 and PD1 in the firmware */
	cfg.baud = u->baud ? u->baud : 115200;
	cfg.data_bits = 8;
	cfg.parity = V003_UART_PARITY_NONE;
	cfg.stop_bits = 1;
	cfg.enable = 0;

	ret = v003_data_out(u->v003, V003_UART_CONFIG, 0, &cfg, sizeof(cfg));
	if (ret < 0 && ret != -ENODEV)
		dev_warn(u->dev, "could not disable the port: %d\n", ret);

	u->enabled = false;
	u->state = 0;
}

/*
 * The device has no modem control lines at all (RTS would be the I2C SCL pin
 * and CTS a SPI pin), so carrier is always there and DTR does nothing.  Saying
 * that is what keeps open() from waiting for a carrier that can never change.
 */
static bool v003_uart_carrier_raised(struct tty_port *port)
{
	return true;
}

static void v003_uart_dtr_rts(struct tty_port *port, bool active)
{
}

static const struct tty_port_operations v003_uart_port_ops = {
	.carrier_raised = v003_uart_carrier_raised,
	.dtr_rts = v003_uart_dtr_rts,
	.activate = v003_uart_activate,
	.shutdown = v003_uart_shutdown,
};

/*
 * ------------------------------------------------------------- diagnostics
 *
 * Everything a host can learn about the link without a protocol decoder: what
 * the firmware reports now (its counters are the truth about the wire) plus what
 * this driver has moved.  One line, so `cat` is enough.
 */
static ssize_t stats_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct v003_uart *u = dev_get_drvdata(dev);
	struct v003_uart_cfg cfg = { 0 };
	u32 state = 0, counts = 0, errors = 0, info = 0;
	int ret;
	int n = 0;

	if (!u)
		return -ENODEV;

	/* a read of this file may sleep, so the firmware can be asked directly
	 * instead of reporting the last poll */
	if (!v003_cmd_in(u->v003, V003_UART_GET_STATE, 0, &state) &&
	    !v003_cmd_in(u->v003, V003_UART_GET_COUNTS, 0, &counts) &&
	    !v003_cmd_in(u->v003, V003_UART_GET_ERRORS, 0, &errors) &&
	    !v003_cmd_in(u->v003, V003_UART_GET_INFO, 0, &info))
		u->state = state;
	else
		state = u->state;

	ret = v003_data_in(u->v003, V003_UART_GET_CFG, 0, &cfg, sizeof(cfg));

	/* sysfs_emit_at(), not sysfs_emit(buf + n): the latter warns and returns
	 * 0 for a buffer that is not page aligned, so every line after the first
	 * silently disappeared */
	n += sysfs_emit_at(buf, n, "driver=%s open=%u baud=%u actual=%u poll_ms=%u lossy=%u\n",
			   DRV_NAME, u->polling ? 1 : 0, u->baud, u->actual_baud,
			   u->poll_ms, u->lossy ? 1 : 0);
	n += sysfs_emit_at(buf, n, "firmware enabled=%u tx_queued=%u rx_available=%u tx_dropped=%u rx_dropped=%u\n",
			   ret == (int)sizeof(cfg) ? cfg.enable : 0,
			   V003_UART_GET_TX_QUEUED(state),
			   V003_UART_GET_RX_AVAIL(state),
			   V003_UART_GET_TX_DROPPED(state),
			   V003_UART_GET_RX_DROPPED(state));
	n += sysfs_emit_at(buf, n, "firmware tx_bytes=%u rx_bytes=%u isr=%u overrun=%u framing=%u parity=%u\n",
			   counts >> 16, counts & 0xffff, errors >> 24,
			   (errors >> 16) & 0xff, (errors >> 8) & 0xff, errors & 0xff);
	n += sysfs_emit_at(buf, n, "driver tx_bytes=%lu rx_bytes=%lu polls=%lu rx_ring=%u\n",
			   u->tx_bytes, u->rx_bytes, u->polls, info >> 16);

	return n;
}
static DEVICE_ATTR_RO(stats);

/*
 * The firmware's counters are cumulative and eight bits wide, so they saturate:
 * after a few hundred bytes the receiver's interrupt count sits at 255 and stops
 * telling anyone anything.  Writing here asks the device to zero them, which is
 * what makes a *difference* measurement possible (and what the userspace test
 * does before each step).
 */
static ssize_t stats_reset_store(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t count)
{
	struct v003_uart *u = dev_get_drvdata(dev);
	int ret;

	if (!u)
		return -ENODEV;

	ret = v003_cmd_out(u->v003, V003_UART_CLEAR_STATS, 0);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_WO(stats_reset);

static struct attribute *v003_uart_attrs[] = {
	&dev_attr_stats.attr,
	&dev_attr_stats_reset.attr,
	NULL,
};
ATTRIBUTE_GROUPS(v003_uart);

/* ------------------------------------------------------------------ probe */

static int v003_uart_probe(struct platform_device *pdev)
{
	struct v003_dev *v003 = v003_get_dev(pdev);
	const struct v003_caps *caps = v003_capabilities(v003);
	struct v003_uart *u;
	struct tty_driver *tty;
	struct device *ttydev;
	u32 info = 0;
	int ret;

	if (caps && !caps->nuart) {
		dev_err(&pdev->dev, "the device reports no UART\n");
		return -ENODEV;
	}

	u = devm_kzalloc(&pdev->dev, sizeof(*u), GFP_KERNEL);
	if (!u)
		return -ENOMEM;

	u->v003 = v003;
	u->dev = &pdev->dev;
	INIT_DELAYED_WORK(&u->rx_work, v003_uart_rx_work);

	/* the firmware reports its receive ring size, which is what the poll
	 * interval is derived from */
	ret = v003_cmd_in(v003, V003_UART_GET_INFO, 0, &info);
	if (ret)
		return ret;

	tty = tty_alloc_driver(V003_UART_MINORS,
			       TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);
	if (IS_ERR(tty))
		return PTR_ERR(tty);

	tty->driver_name = DRV_NAME;
	tty->name = "ttyV";
	tty->major = 0; /* let the tty core pick */
	tty->type = TTY_DRIVER_TYPE_SERIAL;
	tty->subtype = SERIAL_TYPE_NORMAL;
	tty->init_termios = tty_std_termios;
	tty->init_termios.c_cflag = B115200 | CS8 | CREAD | CLOCAL | HUPCL;
	tty_set_operations(tty, &v003_uart_ops);

	tty_port_init(&u->port);
	u->port.ops = &v003_uart_port_ops;
	tty_port_link_device(&u->port, tty, V003_UART_PORT);

	ret = tty_register_driver(tty);
	if (ret) {
		dev_err(&pdev->dev, "failed to register the tty driver: %d\n",
			ret);
		tty_port_destroy(&u->port);
		tty_driver_kref_put(tty);
		return ret;
	}

	u->tty = tty;
	u->baud = 115200;
	v003_uart_update_polling(u);

	platform_set_drvdata(pdev, u);

	ttydev = tty_port_register_device_attr(&u->port, tty, V003_UART_PORT,
					       &pdev->dev, u,
					       v003_uart_groups);
	if (IS_ERR(ttydev)) {
		ret = PTR_ERR(ttydev);
		dev_err(&pdev->dev, "failed to register /dev/ttyV%u: %d\n",
			V003_UART_PORT, ret);
		tty_unregister_driver(tty);
		tty_driver_kref_put(tty);
		tty_port_destroy(&u->port);
		return ret;
	}

	dev_info(&pdev->dev,
		 "ttyV%u registered, %u byte receive ring, poll every %u ms\n",
		 V003_UART_PORT, info >> 16, u->poll_ms);

	return 0;
}

/*
 * Teardown of a tty_port that is **embedded** in this driver's own allocation:
 * tty_port_init() is paired with tty_port_destroy() and there is deliberately no
 * tty_port_put() anywhere in this file.
 *
 * That is not a style choice.  tty_port_put() drops the last reference and the
 * port's destructor ends in
 *
 *	if (port->ops && port->ops->destruct)
 *		port->ops->destruct(port);
 *	else
 *		kfree(port);
 *
 * (drivers/tty/tty_port.c), so putting a port embedded in a larger structure
 * frees a pointer *inside* that structure - an interior free, which corrupts the
 * allocator.  It did: after the first load and unload, the next load found the
 * device model's power management list already broken,
 *
 *   list_add corruption. prev->next should be next (ffffffff8902a7e0),
 *   but was ffff8c26579010f8
 *   ... device_pm_add+0xc9/0x120 <- device_add+0x53f/0x890
 *      <- tty_register_device_attr+0x197/0x2c0 <- v003_uart_probe
 *
 * and systemd-udevd, systemsettings and a kworker all oopsed on the same freed
 * object.  A driver whose port is embedded either destroys the port directly
 * (this one) or provides a ->destruct hook; it must not let the core free it.
 *
 * The device itself still has to be unregistered, which is what
 * tty_unregister_device() below does.  tty_port_unregister_device() would do the
 * same thing, but it is documented for ports registered through the serdev path
 * and this driver does not need it.
 */
static void v003_uart_remove(struct platform_device *pdev)
{
	struct v003_uart *u = platform_get_drvdata(pdev);

	u->polling = false;
	cancel_delayed_work_sync(&u->rx_work);

	tty_unregister_device(u->tty, V003_UART_PORT);
	tty_unregister_driver(u->tty);
	tty_driver_kref_put(u->tty);
	tty_port_destroy(&u->port);
}

static struct platform_driver v003_uart_driver = {
	.driver = {
		.name = DRV_NAME,
	},
	.probe = v003_uart_probe,
	.remove = v003_uart_remove,
};
module_platform_driver(v003_uart_driver);

MODULE_AUTHOR("Wooden Chair <hua.zheng@embeddedboys.com>");
MODULE_DESCRIPTION("CH32V003 USB to UART (TTY)");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
